/*
 * fpv_osd.cpp – FPV OSD z przetwarzaniem obrazu na GPU (DRM/KMS + GBM + libcamera + OpenGL ES)
 * Kompilacja: patrz CMakeLists.txt na końcu pliku.
 */

#include <iostream>
#include <vector>
#include <memory>
#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <libcamera/libcamera.h>

using namespace libcamera;

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

// ----------------------------------------------------------------------
// Zasoby kamery
std::unique_ptr<CameraManager> cm;
std::shared_ptr<Camera> camera;
std::unique_ptr<FrameBufferAllocator> allocator;
Stream *video_stream = nullptr;
std::vector<std::unique_ptr<Request>> requests;
Size stream_size;
unsigned int stride_value = 0;   // stride w bajtach

// Kolejka gotowych żądań (do asynchronicznej obsługi)
std::queue<Request*> completed_requests;
std::mutex request_mutex;
std::condition_variable request_cv;

// Mapa dla buforów DMABUF
struct BufferResources {
    EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
};
std::map<int, BufferResources> buffer_resources;

// Mapa FD -> nie-const FrameBuffer* do ponownego dodania bufora po reuse()
std::map<int, FrameBuffer*> fd_to_buffer;

// ----------------------------------------------------------------------
// Makra kontroli błędów
#define CHECK_DRM(f, msg) do { if ((f) < 0) { std::cerr << msg << " (errno: " << errno << ")" << std::endl; exit(1); } } while(0)
#define CHECK_EGL(f, msg) do { EGLint err = eglGetError(); if ((f) != EGL_TRUE) { std::cerr << msg << " (EGL error: 0x" << std::hex << err << ")" << std::endl; exit(1); } } while(0)
#define CHECK_GL(msg) do { GLenum err = glGetError(); if (err != GL_NO_ERROR) { std::cerr << msg << " (GL error: 0x" << std::hex << err << ")" << std::endl; exit(1); } } while(0)

// ----------------------------------------------------------------------
// Funkcja wywoływana po zakończeniu żądania (callback)
void requestComplete(Request *request) {
    // Debug: licznik klatek – odkomentuj, jeśli chcesz zobaczyć, czy kamera działa
    // static int frame_count = 0;
    // std::cout << "Klatka " << ++frame_count << " zakończona" << std::endl;

    std::lock_guard<std::mutex> lock(request_mutex);
    completed_requests.push(request);
    request_cv.notify_one();
}

// ----------------------------------------------------------------------
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

    screen_width = mode.hdisplay;
    screen_height = mode.vdisplay;
    std::cout << "Display gotowy: " << screen_width << "x" << screen_height << std::endl;
}

// ----------------------------------------------------------------------
// Inicjalizacja kamery libcamera
void init_camera() {
    cm = std::make_unique<CameraManager>();
    cm->start();

    auto cameras = cm->cameras();
    if (cameras.empty()) {
        std::cerr << "Nie znaleziono kamery." << std::endl;
        exit(1);
    }
    camera = cameras[0];
    camera->acquire();

    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration({ StreamRole::VideoRecording });
    StreamConfiguration &stream_cfg = config->at(0);
    stream_cfg.pixelFormat = PixelFormat::fromString("XRGB8888");
    stream_cfg.size = Size(1280, 720);   // przykładowy rozmiar, można zmienić
    config->validate();
    camera->configure(config.get());

    video_stream = config->at(0).stream();
    stream_size = config->at(0).size;
    stride_value = config->at(0).stride;   // zapamiętaj stride

    allocator = std::make_unique<FrameBufferAllocator>(camera);
    int ret = allocator->allocate(video_stream);
    if (ret < 0) {
        std::cerr << "Nie udało się zaalokować buforów kamery." << std::endl;
        exit(1);
    }

    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = allocator->buffers(video_stream);
    for (unsigned int i = 0; i < buffers.size(); ++i) {
        std::unique_ptr<Request> req = camera->createRequest();
        req->addBuffer(video_stream, buffers[i].get());
        requests.push_back(std::move(req));

        // Wypełnij mapę fd -> FrameBuffer*
        int fd = buffers[i]->planes()[0].fd.get();
        fd_to_buffer[fd] = buffers[i].get();
    }

    // Podłącz callback zakończenia żądania
    camera->requestCompleted.connect(requestComplete);

    camera->start();
    std::cout << "Kamera uruchomiona, użyto " << buffers.size() << " buforów." << std::endl;
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

GLuint program_camera = 0;
GLuint program_overlay = 0;
GLint u_tex_loc, u_scale_loc, u_offset_loc;
GLint u_proj_loc, u_color_loc;
GLuint vbo_camera = 0;       // VBO dla quadu kamery
GLuint vbo_overlay = 0;      // VBO dla rysowania nakładek

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
    float vertices[] = {
        -1, -1,   0, 0,
         1, -1,   1, 0,
         1,  1,   1, 1,
        -1,  1,   0, 1
    };
    glGenBuffers(1, &vbo_camera);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_camera);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

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
    glDeleteShader(vs);
    glDeleteShader(fs);

    u_proj_loc = glGetUniformLocation(program_overlay, "u_proj");
    u_color_loc = glGetUniformLocation(program_overlay, "u_color");

    // VBO dla nakładek (będzie aktualizowane dynamicznie)
    glGenBuffers(1, &vbo_overlay);
    
    CHECK_GL("init_gl");
}

// ----------------------------------------------------------------------
// Import bufora DMABUF do EGLImage i tekstury
// ----------------------------------------------------------------------
void import_dmabuf(const FrameBuffer *buffer, BufferResources &res) {
    const FrameBuffer::Plane &plane = buffer->planes()[0];
    int fd = plane.fd.get();
    off_t offset = plane.offset;
    unsigned int stride = stride_value;
    unsigned int width = stream_size.width;
    unsigned int height = stream_size.height;

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
// Rysowanie nakładek PFD (przykład)
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
    GLfloat line_horizon[] = {
        100.0f, horizon_y,
        (float)(screen_width - 100), horizon_y
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(line_horizon), line_horizon, GL_STATIC_DRAW);
    glUniform4f(u_color_loc, 0.0f, 1.0f, 0.0f, 1.0f);
    glLineWidth(15);
    glDrawArrays(GL_LINES, 0, 2);

    glDisableVertexAttribArray(a_pos);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}

// ----------------------------------------------------------------------
// Prezentacja bufora na ekranie przez DRM
void present_frame() {
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);
    if (!bo) {
        std::cerr << "Nie można zablokować front buffer GBM." << std::endl;
        return;
    }

    uint32_t fb_id;
    int ret = drmModeAddFB(drm_fd,
                           mode.hdisplay,
                           mode.vdisplay,
                           24,
                           32,
                           gbm_bo_get_stride(bo),
                           gbm_bo_get_handle(bo).u32,
                           &fb_id);
    if (ret) {
        std::cerr << "drmModeAddFB nie powiodło się." << std::endl;
        gbm_surface_release_buffer(gbm_surface, bo);
        return;
    }

    ret = drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &connector_id, 1, &mode);
    if (ret) {
        std::cerr << "drmModeSetCrtc nie powiodło się." << std::endl;
    }

    if (current_fb_id) {
        drmModeRmFB(drm_fd, current_fb_id);
    }

    if (fb_id > 1) {
        fb_id--;
    }

    current_fb_id = fb_id;

    gbm_surface_release_buffer(gbm_surface, bo);
}

// ----------------------------------------------------------------------
// Główna pętla programu
void run() {

    int running = 0;
    // Kolejkujemy wszystkie żądania na start
    for (auto &req : requests)
        camera->queueRequest(req.get());

    while (true) {
        // Czekaj na zakończenie żądania (blokująco)
        std::unique_lock<std::mutex> lock(request_mutex);
        request_cv.wait(lock, []{ return !completed_requests.empty(); });
        Request *completed = completed_requests.front();
        completed_requests.pop();
        lock.unlock();

        const FrameBuffer *buffer = completed->findBuffer(video_stream);
        if (!buffer)
            continue;

        int fd = buffer->planes()[0].fd.get();
        auto it = buffer_resources.find(fd);
        if (it == buffer_resources.end()) {
            BufferResources res;
            import_dmabuf(buffer, res);
            buffer_resources[fd] = res;
            it = buffer_resources.find(fd);
        }

        // Rysowanie obrazu z kamery
        glUseProgram(program_camera);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, it->second.texture);
        glUniform1i(u_tex_loc, 0);
        glUniform2f(u_scale_loc, 1.0f, 1.0f);
        glUniform2f(u_offset_loc, 0.0f, 0.0f);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_camera);
        GLint a_pos_cam = glGetAttribLocation(program_camera, "a_pos");
        GLint a_tex_cam = glGetAttribLocation(program_camera, "a_tex");
        glVertexAttribPointer(a_pos_cam, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
        glVertexAttribPointer(a_tex_cam, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
        glEnableVertexAttribArray(a_pos_cam);
        glEnableVertexAttribArray(a_tex_cam);

        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        CHECK_GL("glDrawArrays camera");

        // Rysowanie nakładek
        draw_pfd();

        // Wyłącz atrybuty kamery
        glDisableVertexAttribArray(a_pos_cam);
        glDisableVertexAttribArray(a_tex_cam);

        eglSwapBuffers(egl_display, egl_surface);
        CHECK_EGL(EGL_TRUE, "eglSwapBuffers");

        present_frame();

        if (running < 3) {
            running++;
            continue;
        }

        // Ponowne użycie żądania – najpierw reuse, potem dodajemy bufor z powrotem
        completed->reuse();
        FrameBuffer *nonconst_buffer = fd_to_buffer[fd];
        completed->addBuffer(video_stream, nonconst_buffer);
        camera->queueRequest(completed);
    }
}

// ----------------------------------------------------------------------
// Sprzątanie zasobów
void cleanup() {
    camera->stop();
    allocator->free(video_stream);
    camera->release();
    cm->stop();

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

// ----------------------------------------------------------------------
int main() {
    init_display();
    init_gl();
    init_camera();

    run();  // nigdy nie wraca (chyba że przerwane)

    cleanup();
    return 0;
}
