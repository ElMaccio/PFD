#include <iostream>
#include <vector>
#include <map>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include "camera.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "stb_easy_font.h"

// ----------------------------------------------------------------------
// Definicje stałych EGL (na wypadek gdyby nie były zdefiniowane w nagłówkach)
#ifndef EGL_NO_IMAGE_KHR
#define EGL_NO_IMAGE_KHR ((EGLImageKHR)0)
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif

#define IMGUI_IMPL_OPENGL_ES2
#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM

// Wskaźniki do funkcji rozszerzeń EGL
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;

// ----------------------------------------------------------------------
// Struktury globalne DRM/KMS
static int drm_fd = -1;
static drmModeConnector *connector = nullptr;
static drmModeEncoder *encoder = nullptr;
static drmModeModeInfo mode;
static uint32_t crtc_id;
static uint32_t connector_id;
static drmModeCrtc *saved_crtc = nullptr;
static struct gbm_device *gbm_dev = nullptr;
static struct gbm_surface *gbm_surface = nullptr;
static uint32_t current_fb_id = 0;

// ----------------------------------------------------------------------
// EGL / GLES
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static int screen_width = 0, screen_height = 0;

static PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR = nullptr;
static PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR = nullptr;
static PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR = nullptr;

// ----------------------------------------------------------------------
// Zasoby kamery// stride w bajtach
MCamera cam;

char text_buffer[10240];
std::vector<GLushort> vertices;
int num_quads = 0;
// Mapa dla buforów DMABUF
struct BufferResources {
    EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
};
std::map<int, BufferResources> buffer_resources;

// ----------------------------------------------------------------------
// Makra kontroli błędów
#define CHECK_DRM(f, msg) do { if ((f) < 0) { std::cerr << msg << " (errno: " << errno << ")" << std::endl; exit(1); } } while(0)
#define CHECK_EGL(f, msg) do { EGLint err = eglGetError(); if ((f) != EGL_TRUE) { std::cerr << msg << " (EGL error: 0x" << std::hex << err << ")" << std::endl; exit(1); } } while(0)
#define CHECK_GL(msg) do { GLenum err = glGetError(); if (err != GL_NO_ERROR) { std::cerr << msg << " (GL error: 0x" << std::hex << err << ")" << std::endl; exit(1); } } while(0)

static struct gbm_bo *displayed_bo = nullptr;
static struct gbm_bo *next_bo = nullptr;
static uint32_t displayed_fb = 0;
static uint32_t next_fb = 0;
static bool flip_pending = false;

static drmEventContext evctx = {
    .version = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = [](int fd, unsigned int sequence,
                            unsigned int tv_sec, unsigned int tv_usec,
                            void *user_data) {
        // user_data to stary bufor (displayed_bo przed flip)
        struct gbm_bo *old_bo = static_cast<struct gbm_bo*>(user_data);

        // Usuń starą framebuffer
        if (displayed_fb) {
            drmModeRmFB(fd, displayed_fb);
            displayed_fb = 0;
        }

        // Zwolnij stary bufor do GBM
        if (old_bo) {
            gbm_surface_release_buffer(gbm_surface, old_bo);
        }

        // Aktualizuj wyświetlany bufor
        displayed_bo = next_bo;
        displayed_fb = next_fb;
        next_bo = nullptr;
        next_fb = 0;
        flip_pending = false;
    }
};

// Znajdowanie urządzenia DRM z podłączonym wyświetlaczem
static int find_drm_device() {
    const char *cards[] = { "/dev/dri/card0", "/dev/dri/card1" };
    for (const char *card : cards) {
        int fd = open(card, O_RDWR);
        if (fd < 0) continue;

        drmModeRes *resources = drmModeGetResources(fd);
        if (!resources) {
            close(fd);
            continue;
        }

        std::cout << "Sprawdzam urządzenie: " << card << std::endl;
        for (int i = 0; i < resources->count_connectors; i++) {
            drmModeConnector *conn = drmModeGetConnector(fd, resources->connectors[i]);
            if (!conn) continue;

            std::cout << "  Złącze " << i << ": id=" << conn->connector_id
                      << " typ=" << conn->connector_type
                      << " połączone=" << (conn->connection == DRM_MODE_CONNECTED ? "tak" : "nie")
                      << " liczba trybów=" << conn->count_modes << std::endl;

            if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
                // Wybieramy pierwszy dostępny tryb
                mode = conn->modes[0];
                std::cout << "    Wybrano tryb: " << mode.hdisplay << "x" << mode.vdisplay << std::endl;

                // Znajdź enkoder
                for (int j = 0; j < resources->count_encoders; j++) {
                    drmModeEncoder *enc = drmModeGetEncoder(fd, resources->encoders[j]);
                    if (enc && enc->encoder_id == conn->encoder_id) {
                        encoder = enc;
                        crtc_id = enc->crtc_id;
                        drmModeFreeEncoder(enc);
                        break;
                    }
                }

                saved_crtc = drmModeGetCrtc(fd, crtc_id);
                connector = conn; // zapamiętujemy (zwolnimy w cleanup)
                connector_id = conn->connector_id;
                drmModeFreeResources(resources);
                return fd;
            }
            drmModeFreeConnector(conn);
        }
        drmModeFreeResources(resources);
        close(fd);
    }
    return -1;
}

// ----------------------------------------------------------------------
// Inicjalizacja wyświetlania (DRM/KMS + GBM + EGL)
void init_display() {
    drm_fd = find_drm_device();
    if (drm_fd < 0) {
        std::cerr << "Nie znaleziono urządzenia DRM z podłączonym wyświetlaczem." << std::endl;
        exit(1);
    }
    std::cout << "Otwarto DRM, rozdzielczość: " << mode.hdisplay << "x" << mode.vdisplay << std::endl;

    gbm_dev = gbm_create_device(drm_fd);
    if (!gbm_dev) {
        std::cerr << "Nie można utworzyć GBM device." << std::endl;
        exit(1);
    }

    gbm_surface = gbm_surface_create(
        gbm_dev,
        mode.hdisplay,
        mode.vdisplay,
        GBM_FORMAT_XRGB8888,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING
    );
    if (!gbm_surface) {
        std::cerr << "Nie można utworzyć GBM surface." << std::endl;
        exit(1);
    }

    egl_display = eglGetDisplay((EGLNativeDisplayType)gbm_dev);
    if (egl_display == EGL_NO_DISPLAY) {
        std::cerr << "eglGetDisplay zwrócił EGL_NO_DISPLAY." << std::endl;
        exit(1);
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        std::cerr << "eglInitialize nie powiodło się." << std::endl;
        exit(1);
    }
    std::cout << "EGL " << major << "." << minor << std::endl;

    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint attribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 16,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(egl_display, attribs, &config, 1, &num_configs)) {
        std::cerr << "eglChooseConfig nie powiodło się." << std::endl;
        exit(1);
    }

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
    CHECK_EGL(egl_context != EGL_NO_CONTEXT, "eglCreateContext");

    egl_surface = eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)gbm_surface, nullptr);
    CHECK_EGL(egl_surface != EGL_NO_SURFACE, "eglCreateWindowSurface");

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        std::cerr << "eglMakeCurrent nie powiodło się." << std::endl;
        exit(1);
    }

    //eglSwapInterval(egl_display, 1);

    // Pobierz wskaźniki do funkcji rozszerzeń
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
    if (!eglCreateImageKHR || !eglDestroyImageKHR) {
        std::cerr << "Brak rozszerzeń EGL_KHR_image." << std::endl;
        exit(1);
    }

    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!glEGLImageTargetTexture2DOES) {
        std::cerr << "Brak rozszerzenia GL_OES_EGL_image." << std::endl;
        exit(1);
    }
    
    eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC) eglGetProcAddress("eglCreateSyncKHR");
    eglClientWaitSyncKHR = (PFNEGLCLIENTWAITSYNCKHRPROC) eglGetProcAddress("eglClientWaitSyncKHR");
    eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC) eglGetProcAddress("eglDestroySyncKHR");

    if (!eglCreateSyncKHR || !eglClientWaitSyncKHR || !eglDestroySyncKHR) {
        std::cerr << "EGL_KHR_fence_sync not supported" << std::endl;
        exit(1);
    }

    screen_width = mode.hdisplay;
    screen_height = mode.vdisplay;
    std::cout << "Display gotowy: " << screen_width << "x" << screen_height << std::endl;
}

// ----------------------------------------------------------------------
// Shadery OpenGL ES 2.0

// Vertex shader dla kamery (pełnoekranowy quad)
static const char *vs_camera = R"(
    precision highp float;
    attribute vec2 a_pos;
    attribute vec2 a_tex;
    varying vec2 v_tex;
    void main() {
        gl_Position = vec4(a_pos, 0.0, 1.0);
        v_tex = a_tex;
    }
)";

// Fragment shader dla kamery (proste mapowanie tekstury)
static const char *fs_camera = R"(
    precision highp float;
    varying vec2 v_tex;
    uniform sampler2D u_tex;
    uniform vec2 u_scale;
    uniform vec2 u_offset;
    void main() {
        gl_FragColor = vec4(texture2D(u_tex, v_tex * u_scale + u_offset).rgb, 1.0);
    }
)";

// Vertex shader dla nakładek 2D (z macierzą projekcji)
static const char *vs_overlay = R"(
    precision highp float;
    attribute vec2 a_pos;
    uniform mat4 u_proj;
    void main() {
        gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
    }
)";

// Fragment shader dla nakładek (stały kolor)
static const char *fs_overlay = R"(
    precision highp float;
    uniform vec4 u_color;
    void main() {
        gl_FragColor = u_color;
    }
)";

static const char *vs_text = R"(
    precision highp float;
    attribute vec2 a_pos;
    attribute vec4 a_color;
    varying vec4 v_color;
    uniform mat4 u_proj;
    
    void main() {
        gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
        v_color = a_color;
    }
)";

static const char *fs_text = R"(
    precision highp float;
    varying vec4 v_color;

    void main() {
        gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
    }
)";

GLuint program_camera = 0;
GLuint program_overlay = 0;
GLuint program_text = 0;
GLint u_tex_loc, u_scale_loc, u_offset_loc;
GLint u_proj_loc, u_color_loc, u_proj_text;
GLuint vbo_camera = 0;       // VBO dla quadu kamery
GLuint vbo_overlay = 0;      // VBO dla rysowania nakładek
GLuint ibo_overlay = 0;
GLuint vbo_text = 0;

// Funkcja kompilująca shader
GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        std::cerr << "Shader compilation error: " << log << std::endl;
        exit(1);
    }
    return sh;
}

// Inicjalizacja OpenGL
void init_gl() {
    // Ustaw viewport na cały ekran
    glViewport(0, 0, screen_width, screen_height);

    // Program kamery
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_camera);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_camera);
    program_camera = glCreateProgram();
    glAttachShader(program_camera, vs);
    glAttachShader(program_camera, fs);
    glLinkProgram(program_camera);
    GLint ok;
    glGetProgramiv(program_camera, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program_camera, sizeof(log), nullptr, log);
        std::cerr << "Program camera link error: " << log << std::endl;
        exit(1);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    u_tex_loc = glGetUniformLocation(program_camera, "u_tex");
    u_scale_loc = glGetUniformLocation(program_camera, "u_scale");
    u_offset_loc = glGetUniformLocation(program_camera, "u_offset");

    // VBO dla kamery (pełnoekranowy quad)
    float cam_vertices[] = {
        -1, -1,   0, 0,
         1, -1,   1, 0,
         1,  1,   1, 1,
        -1,  1,   0, 1
    };
    glGenBuffers(1, &vbo_camera);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_camera);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cam_vertices), cam_vertices, GL_STATIC_DRAW);

    // Program dla nakładek
    vs = compile_shader(GL_VERTEX_SHADER, vs_overlay);
    fs = compile_shader(GL_FRAGMENT_SHADER, fs_overlay);
    program_overlay = glCreateProgram();
    glAttachShader(program_overlay, vs);
    glAttachShader(program_overlay, fs);
    glLinkProgram(program_overlay);
    glGetProgramiv(program_overlay, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program_overlay, sizeof(log), nullptr, log);
        std::cerr << "Program overlay link error: " << log << std::endl;
        exit(1);
    }

    //Program dla tekstu
    vs = compile_shader(GL_VERTEX_SHADER, vs_text);
    fs = compile_shader(GL_FRAGMENT_SHADER, fs_text);
    program_text = glCreateProgram();
    glAttachShader(program_text, vs);
    glAttachShader(program_text, fs);
    glLinkProgram(program_text);
    glGetProgramiv(program_text, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program_text, sizeof(log), nullptr, log);
        std::cerr << "Program text link error: " << log << std::endl;
        exit(1);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    u_proj_loc = glGetUniformLocation(program_overlay, "u_proj");
    u_color_loc = glGetUniformLocation(program_overlay, "u_color");
    u_proj_text = glGetUniformLocation(program_text, "u_proj");

    // VBO dla nakładek (będzie aktualizowane dynamicznie)
    glGenBuffers(1, &vbo_overlay);
    glGenBuffers(1, &vbo_text);
    glGenBuffers(1, &ibo_overlay);
    
    CHECK_GL("init_gl");

    num_quads = stb_easy_font_print(10, 10, "HELLO WORLD", NULL, text_buffer, sizeof(text_buffer));
    vertices.reserve(num_quads*6);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_text);
    glBufferData(GL_ARRAY_BUFFER, num_quads * 4 * 8, text_buffer, GL_DYNAMIC_DRAW);
    
    for(int i = 0; i < num_quads; ++i)
    {
        GLushort v0 = i * 4;
        GLushort v1 = i * 4 + 1;
        GLushort v2 = i * 4 + 2;
        GLushort v3 = i * 4 + 3;

        vertices.push_back(v0);
        vertices.push_back(v1);
        vertices.push_back(v2);
       
        vertices.push_back(v0);
        vertices.push_back(v2);
        vertices.push_back(v3);
    }


    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_overlay);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, vertices.size() * sizeof(GLushort), vertices.data(), GL_STATIC_DRAW);
}

void draw_pfd() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    // Macierz projekcji: odwrócona oś Y (bo współrzędne ekranu mają Y w dół)
    float proj[16] = {
        2.0f/screen_width, 0, 0, 0,
        0, -2.0f/screen_height, 0, 0,
        0, 0, 1, 0,
        -1, 1, 0, 1
    };

    glUseProgram(program_overlay);
    glUniformMatrix4fv(u_proj_loc, 1, GL_FALSE, proj);

    GLint a_pos = glGetAttribLocation(program_overlay, "a_pos");
    glBindBuffer(GL_ARRAY_BUFFER, vbo_overlay);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(a_pos);

    // Linia horyzontu (zielona)
    float horizon_y = screen_height / 2;
    float horizon_x = screen_width / 2;
    GLfloat vertical_horizon_lines[] = {
        //LEFT
        200.0f, 125.0f,
        200.0f, (float)(screen_height-125),
        //RIGHT
        (float)(screen_width - 200), 125.0f,
        (float)(screen_width - 200), (float)(screen_height-125)
    };

    GLfloat horizontal_horizon_lines[] = {
        //HORIZONTAL LEFT
        200.0f, 125.0f,
        150.0f, 125.0f,

        200.0f, (((float)horizon_y-125.0f)/3.0f) + 125.0f,
        175.0f, (((float)horizon_y-125.0f)/3.0f) + 125.0f,

        200.0f, (2.0f *((float)horizon_y-125.0f)/3.0f) + 125.0f,
        175.0f, (2.0f *((float)horizon_y-125.0f)/3.0f) + 125.0f,

        200.0f, (float)(horizon_y),
        150.0f, (float)(horizon_y),

        200.0f, (((float)horizon_y-(float)(screen_height-125))/3.0f) + (float)(screen_height-125),
        175.0f, (((float)horizon_y-(float)(screen_height-125))/3.0f) + (float)(screen_height-125),

        200.0f, (2.0f *((float)horizon_y-(float)(screen_height-125))/3.0f) + (float)(screen_height-125),
        175.0f, (2.0f *((float)horizon_y-(float)(screen_height-125))/3.0f) + (float)(screen_height-125),

        200.0f, (float)(screen_height-125),
        150.0f, (float)(screen_height-125)
    };

    GLfloat crosshair[] = {
        (float)(horizon_x - 15), (float)(horizon_y - 10),
        (float)(horizon_x), (float)(horizon_y + 10),
        (float)(horizon_x + 15), (float)(horizon_y - 10)
    };

    //HORIZON_LINE
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertical_horizon_lines), vertical_horizon_lines, GL_DYNAMIC_DRAW);
    glUniform4f(u_color_loc, 0.0f, 1.0f, 0.0f, 1.0f);
    glLineWidth(2);
    glDrawArrays(GL_LINES, 0, 4);

    glBufferData(GL_ARRAY_BUFFER, sizeof(horizontal_horizon_lines), horizontal_horizon_lines, GL_DYNAMIC_DRAW);
    glLineWidth(3);
    glDrawArrays(GL_LINES, 0, 14);

    //CROSSHAIR
    glLineWidth(4);
    glBufferData(GL_ARRAY_BUFFER, sizeof(crosshair), crosshair, GL_STATIC_DRAW);
    glDrawArrays(GL_LINE_STRIP, 0, 3);

    //TEXT
    glBindBuffer(GL_ARRAY_BUFFER, vbo_text);

    glUseProgram(program_text);

    glUniformMatrix4fv(u_proj_text, 1, GL_FALSE, proj);

    glBufferData(GL_ARRAY_BUFFER, num_quads * 4 * 8, text_buffer, GL_DYNAMIC_DRAW);
    a_pos = glGetAttribLocation(program_text, "a_pos");
    glVertexAttribPointer(a_pos, 2, GL_SHORT, GL_FALSE, 8, (void*)0);
    glEnableVertexAttribArray(a_pos);

    GLuint a_color = glGetAttribLocation(program_text, "a_color");
    glVertexAttribPointer(a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, 8, (void*)4);
    glEnableVertexAttribArray(a_color);


    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_overlay);

    glDrawElements(GL_TRIANGLES, vertices.size(), GL_UNSIGNED_SHORT, 0);

    glDisableVertexAttribArray(a_pos);
    glDisableVertexAttribArray(a_color);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}

// ----------------------------------------------------------------------
// Import bufora DMABUF do EGLImage i tekstury
// ----------------------------------------------------------------------
void import_dmabuf(const FrameBuffer *buffer, BufferResources &res) {
    const FrameBuffer::Plane &plane = buffer->planes()[0];
    int fd = plane.fd.get();
    off_t offset = plane.offset;
    unsigned int stride = cam.stride_value;
    unsigned int width = cam.stream_size.width;
    unsigned int height = cam.stream_size.height;

    std::cout << "Import DMABUF: fd=" << fd
              << " offset=" << offset
              << " stride=" << stride
              << " width=" << width
              << " height=" << height << std::endl;

    // Format – XRGB8888
    unsigned int fourcc = DRM_FORMAT_XRGB8888;

    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_NONE
    };

    std::cout << "Atrybuty EGL: ";
    for (int i = 0; attrs[i] != EGL_NONE; i += 2) {
        std::cout << "0x" << std::hex << attrs[i] << "=" << std::dec << attrs[i+1] << " ";
    }
    std::cout << std::endl;

    res.egl_image = eglCreateImageKHR(egl_display, EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (res.egl_image == EGL_NO_IMAGE_KHR) {
        EGLint err = eglGetError();
        std::cerr << "eglCreateImageKHR nie powiodło się, błąd EGL: 0x" << std::hex << err << std::endl;
        exit(1);
    }

    glGenTextures(1, &res.texture);
    glBindTexture(GL_TEXTURE_2D, res.texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)res.egl_image);
    CHECK_GL("glEGLImageTargetTexture2DOES");

    // Ustaw parametry tekstury
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    CHECK_GL("Texture parameters");
}

// ----------------------------------------------------------------------
// Prezentacja bufora na ekranie przez DRM
void present_frame() {
    // Pobierz nowy bufor z GBM (ten, który właśnie został wyrenderowany)
    struct gbm_bo *new_bo = gbm_surface_lock_front_buffer(gbm_surface);
    if (!new_bo) {
        std::cerr << "Nie można zablokować front buffer GBM." << std::endl;
        return;
    }

    // Utwórz nowy framebuffer DRM
    uint32_t new_fb;
    int ret = drmModeAddFB(drm_fd,
                           mode.hdisplay,
                           mode.vdisplay,
                           24, 32,
                           gbm_bo_get_stride(new_bo),
                           gbm_bo_get_handle(new_bo).u32,
                           &new_fb);
    if (ret) {
        std::cerr << "drmModeAddFB nie powiodło się." << std::endl;
        gbm_surface_release_buffer(gbm_surface, new_bo);
        return;
    }

    // Pierwsza klatka – ustaw CRTC bezpośrednio
    if (!displayed_bo) {
        ret = drmModeSetCrtc(drm_fd, crtc_id, new_fb, 0, 0,
                             &connector_id, 1, &mode);
        if (ret) {
            std::cerr << "drmModeSetCrtc nie powiodło się." << std::endl;
            drmModeRmFB(drm_fd, new_fb);
            gbm_surface_release_buffer(gbm_surface, new_bo);
            return;
        }
        displayed_bo = new_bo;
        displayed_fb = new_fb;
        return;   // nowy bufor jest wyświetlany
    }

    // Oczekuj na zakończenie poprzedniego flipa, jeśli jest w toku
    if (flip_pending) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(drm_fd, &fds);
        while (flip_pending) {
            int ret = select(drm_fd + 1, &fds, nullptr, nullptr, nullptr);
            if (ret < 0) break;
            drmHandleEvent(drm_fd, &evctx);
        }
    }

    // Wykonaj page flip, przekazując stary bufor do zwolnienia po zakończeniu
    ret = drmModePageFlip(drm_fd, crtc_id, new_fb,
                          DRM_MODE_PAGE_FLIP_EVENT, displayed_bo);
    if (ret) {
        std::cerr << "drmModePageFlip nie powiodło się." << std::endl;
        drmModeRmFB(drm_fd, new_fb);
        gbm_surface_release_buffer(gbm_surface, new_bo);
        return;
    }

    // Zapamiętaj nowy bufor jako oczekujący na flip
    next_bo = new_bo;
    next_fb = new_fb;
    flip_pending = true;
}

// ----------------------------------------------------------------------
// Główna pętla programu
void run() {
    // Queue all requests initially
    for (auto &req : cam.requests)
        cam.camera->queueRequest(req.get());
    EGLSyncKHR rendering_fence = EGL_NO_SYNC_KHR;

    while (true) {
        // Wait for completed request
        std::unique_lock<std::mutex> lock(MCamera::request_mutex);
        MCamera::request_cv.wait(lock, []{ return !MCamera::completed_requests.empty(); });
        Request *completed = MCamera::completed_requests.front();
        MCamera::completed_requests.pop();
        lock.unlock();

        // Wait for previous rendering to finish before reusing the buffer
        if (rendering_fence != EGL_NO_SYNC_KHR) {
            eglClientWaitSyncKHR(egl_display, rendering_fence,
                                 EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                 EGL_FOREVER_KHR);
            eglDestroySyncKHR(egl_display, rendering_fence);
            rendering_fence = EGL_NO_SYNC_KHR;
        }

        const FrameBuffer *buffer = completed->findBuffer(cam.video_stream);
        if (!buffer) continue;

        int fd = buffer->planes()[0].fd.get();
        auto it = buffer_resources.find(fd);
        if (it == buffer_resources.end()) {
            BufferResources res;
            import_dmabuf(buffer, res);
            buffer_resources[fd] = res;
            it = buffer_resources.find(fd);
        }

        // Render camera and OSD
        glUseProgram(program_camera);
        glBindTexture(GL_TEXTURE_2D, it->second.texture);
        glUniform2f(u_scale_loc, 1.0f, 1.0f);
        glUniform2f(u_offset_loc, 0.0f, 0.0f);

        // Bind the camera VBO and set up the vertex attributes
        glBindBuffer(GL_ARRAY_BUFFER, vbo_camera);
        GLint a_pos = glGetAttribLocation(program_camera, "a_pos");
        GLint a_tex = glGetAttribLocation(program_camera, "a_tex");
        glEnableVertexAttribArray(a_pos);
        glEnableVertexAttribArray(a_tex);
        glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
        glVertexAttribPointer(a_tex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (void*)(2 * sizeof(float)));

        // Draw the quad as a triangle fan
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

        draw_pfd();   // ensure this uses a quad, not a line

        // Swap and present
        eglSwapBuffers(egl_display, egl_surface);
        present_frame();

        // Create a fence for this frame's rendering
        rendering_fence = eglCreateSyncKHR(egl_display, EGL_SYNC_FENCE_KHR, nullptr);

        // Reuse the request (buffer will be filled by camera again)
        completed->reuse();
        completed->addBuffer(cam.video_stream, cam.fd_to_buffer[fd]);
        cam.camera->queueRequest(completed);
    }
}

// ----------------------------------------------------------------------
// Sprzątanie zasobów
void cleanup() {
    cam.camera->stop();
    cam.allocator->free(cam.video_stream);
    cam.camera->release();
    cam.cm->stop();

    for (auto &pair : buffer_resources) {
        if (pair.second.texture)
            glDeleteTextures(1, &pair.second.texture);
        if (pair.second.egl_image != EGL_NO_IMAGE_KHR)
            eglDestroyImageKHR(egl_display, pair.second.egl_image);
    }

    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(egl_display, egl_context);
        if (egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(egl_display, egl_surface);
        eglTerminate(egl_display);
    }

        // Poczekaj na ewentualny oczekujący flip
    if (flip_pending) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(drm_fd, &fds);
        select(drm_fd + 1, &fds, nullptr, nullptr, nullptr);
        drmHandleEvent(drm_fd, &evctx);
    }

    // Zwolnij bufory GBM
    if (displayed_bo) {
        gbm_surface_release_buffer(gbm_surface, displayed_bo);
    }
    if (next_bo) {
        gbm_surface_release_buffer(gbm_surface, next_bo);
    }
    if (displayed_fb) {
        drmModeRmFB(drm_fd, displayed_fb);
    }
    if (next_fb) {
        drmModeRmFB(drm_fd, next_fb);
    }

    if (gbm_surface)
        gbm_surface_destroy(gbm_surface);
    if (gbm_dev)
        gbm_device_destroy(gbm_dev);

    if (saved_crtc) {
        drmModeSetCrtc(drm_fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
                       saved_crtc->x, saved_crtc->y, &connector_id, 1, &saved_crtc->mode);
        drmModeFreeCrtc(saved_crtc);
    }
    if (current_fb_id)
        drmModeRmFB(drm_fd, current_fb_id);
    if (connector)
        drmModeFreeConnector(connector);
    if (encoder)
        drmModeFreeEncoder(encoder);
    if (drm_fd >= 0)
        close(drm_fd);

    std::cout << "Zasoby zwolnione." << std::endl;
}

int main() {
    init_display();

    init_gl();

    run();  // nigdy nie wraca (chyba że przerwane)

    cleanup();

    return 0;
}
