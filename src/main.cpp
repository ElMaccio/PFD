#include <iostream>
#include <iomanip>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <cmath>
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
#include "pixelfont.h"
#include "octafont-regular.h"
#include "shaders.h"

//DEBUG ONLY
float counter = 0.0f;

// ----------------------------------------------------------------------
// EGL extension definitions (if not already defined)
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

// ----------------------------------------------------------------------
// Function pointers for EGL extensions
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;

// ----------------------------------------------------------------------
// DRM/KMS globals
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
// EGL / GLES globals
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static int screen_width = 0, screen_height = 0;

static PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR = nullptr;
static PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR = nullptr;
static PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR = nullptr;

// ----------------------------------------------------------------------
// Camera globals
MCamera cam;

// ----------------------------------------------------------------------
// DMABUF buffer resources
struct BufferResources {
    EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
};
std::map<int, BufferResources> buffer_resources;

// ----------------------------------------------------------------------
// Page flip state
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
        struct gbm_bo *old_bo = static_cast<struct gbm_bo*>(user_data);
        if (displayed_fb) {
            drmModeRmFB(fd, displayed_fb);
            displayed_fb = 0;
        }
        if (old_bo) {
            gbm_surface_release_buffer(gbm_surface, old_bo);
        }
        displayed_bo = next_bo;
        displayed_fb = next_fb;
        next_bo = nullptr;
        next_fb = 0;
        flip_pending = false;
    }
};

struct GlyphInfo {
    float s0, t0, s1, t1;
    int width;
};

std::map<char, GlyphInfo> pixel_glyph_map;
GLuint pixel_font_atlas = 0;
int pixel_atlas_width = 0, pixel_atlas_height = 0;
const int PIXEL_FONT_HEIGHT = 8;

OctafontRegular& getFont() {
    static OctafontRegular font;
    return font;
}

// ----------------------------------------------------------------------
// Global shaders instance (for easy access)
static Shaders* g_shaders = nullptr;

// VBOs
GLuint vbo_camera = 0;
GLuint vbo_overlay = 0;
GLuint vbo_text = 0;

// ----------------------------------------------------------------------
// Build texture atlas (unchanged)
void buildPixelFontAtlas() {
    const char* chars = "0123456789ABCDEF.";
    const int count = strlen(chars);
    int total_width = 0;
    int max_width = 0;

    for (int i = 0; i < count; ++i) {
        int w = getFont().get_width(chars[i]);
        total_width += w + 1;
        if (w > max_width) max_width = w;
    }
    total_width -= 1;

    pixel_atlas_width = 1;
    while (pixel_atlas_width < total_width) pixel_atlas_width <<= 1;
    pixel_atlas_height = 1;
    while (pixel_atlas_height < PIXEL_FONT_HEIGHT) pixel_atlas_height <<= 1;

    std::vector<unsigned char> bitmap(pixel_atlas_width * pixel_atlas_height, 0);

    int x_offset = 0;
    for (int i = 0; i < count; ++i) {
        char c = chars[i];
        int w = getFont().get_width(c) + 1;
        for (int col = 0; col < w; ++col) {
            uint8_t column = getFont().get_octet(c, col);
            for (int row = 0; row < PIXEL_FONT_HEIGHT; ++row) {
                int bit = (column >> row) & 1;
                if (bit) {
                    int idx = row * pixel_atlas_width + (x_offset + col);
                    bitmap[idx] = 255;
                }
            }
        }

        GlyphInfo info;
        info.width = w;
        info.s0 = (float)x_offset / pixel_atlas_width;
        info.s1 = (float)(x_offset + w) / pixel_atlas_width;
        info.t0 = 1.0f - (float)PIXEL_FONT_HEIGHT / pixel_atlas_height;
        info.t1 = 1.0f;
        pixel_glyph_map[c] = info;

        x_offset += w + 1;
    }

    glGenTextures(1, &pixel_font_atlas);
    glBindTexture(GL_TEXTURE_2D, pixel_font_atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, pixel_atlas_width, pixel_atlas_height, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, bitmap.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    CHECK_GL("buildPixelFontAtlas");

    std::cout << "Pixel font atlas created: " << pixel_atlas_width << "x" << pixel_atlas_height
              << " containing " << count << " characters." << std::endl;
}

float getPixelTextWidth(const std::string& text, float scale) {
    float width = 0.0f;
    for (char c : text) {
        auto it = pixel_glyph_map.find(c);
        if (it != pixel_glyph_map.end())
            width += it->second.width * scale;
    }
    return width;
}

void drawPixelText(const std::string& text, float x, float y, float scale,
                   float r, float g, float b, float a) {
    if (text.empty() || pixel_font_atlas == 0 || !g_shaders) return;

    std::vector<float> vertices;
    float cursor_x = x;

    for (char character : text) {
        auto it = pixel_glyph_map.find(character);
        if (it == pixel_glyph_map.end()) continue;
        const GlyphInfo& glyph = it->second;

        float glyphWidth  = glyph.width * scale;
        float glyphHeight = PIXEL_FONT_HEIGHT * scale;

        float leftX   = cursor_x;
        float topY    = y;
        float rightX  = cursor_x + glyphWidth;
        float bottomY = y + glyphHeight;

        // Triangle 1
        vertices.push_back(leftX);  vertices.push_back(topY);
        vertices.push_back(glyph.s0); vertices.push_back(glyph.t0);
        vertices.push_back(rightX); vertices.push_back(topY);
        vertices.push_back(glyph.s1); vertices.push_back(glyph.t0);
        vertices.push_back(leftX);  vertices.push_back(bottomY);
        vertices.push_back(glyph.s0); vertices.push_back(glyph.t1);
        // Triangle 2
        vertices.push_back(rightX); vertices.push_back(topY);
        vertices.push_back(glyph.s1); vertices.push_back(glyph.t0);
        vertices.push_back(rightX); vertices.push_back(bottomY);
        vertices.push_back(glyph.s1); vertices.push_back(glyph.t1);
        vertices.push_back(leftX);  vertices.push_back(bottomY);
        vertices.push_back(glyph.s0); vertices.push_back(glyph.t1);

        cursor_x += glyphWidth;
    }

    if (vertices.empty()) return;

    glBindBuffer(GL_ARRAY_BUFFER, vbo_text);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.data(), GL_DYNAMIC_DRAW);

    g_shaders->useText();

    float proj[16] = {
        2.0f / screen_width, 0, 0, 0,
        0, -2.0f / screen_height, 0, 0,
        0, 0, 1, 0,
        -1, 1, 0, 1
    };
    g_shaders->setTextProjection(proj);
    g_shaders->setTextureUnit(0);
    g_shaders->setTextColor(r, g, b, a);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pixel_font_atlas);

    GLint a_pos = g_shaders->getTextPosAttr();
    GLint a_tex = g_shaders->getTextTexAttr();
    glEnableVertexAttribArray(a_pos);
    glEnableVertexAttribArray(a_tex);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(a_tex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, vertices.size() / 4);

    glDisableVertexAttribArray(a_pos);
    glDisableVertexAttribArray(a_tex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

void drawPixelTextWithOutline(const std::string& text, float x, float y, float scale, float r, float g, float b, float a, float outline_r, float outline_g, float outline_b, float outline_a, int outline_thickness = 1) {
    std::vector<std::pair<float, float>> offsets;
    for (int dx = -outline_thickness; dx <= outline_thickness; ++dx) {
        for (int dy = -outline_thickness; dy <= outline_thickness; ++dy) {
            if (dx == 0 && dy == 0) continue;
            if (abs(dx) + abs(dy) <= outline_thickness * 2) {
                offsets.emplace_back(dx, dy);
            }
        }
    }
    // Draw outline passes
    for (auto& off : offsets) {
        drawPixelText(text, x + off.first, y + off.second, scale,
                      outline_r, outline_g, outline_b, outline_a);
    }
    // Draw main text
    drawPixelText(text, x, y, scale, r, g, b, a);
}

std::string formatNumber(double value, int int_precision, int decimal_precision) {
    // Round to avoid floating point artifacts
    double factor = std::pow(10.0, decimal_precision);
    value = std::round(value * factor) / factor;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(decimal_precision) << value;
    std::string num_str = ss.str();

    // Count digits before decimal (including sign if negative)
    std::string::size_type dot_pos = num_str.find('.');
    int digits_before = (dot_pos == std::string::npos) ? num_str.length() : dot_pos;
    if (value < 0) digits_before--; // minus sign doesn't count as digit for padding

    int leading_zeros_needed = int_precision - digits_before;
    if (leading_zeros_needed > 0) {
        std::string leading = std::string(leading_zeros_needed, '0');
        if (value < 0)
            num_str = "-" + leading + num_str.substr(1);
        else
            num_str = leading + num_str;
    }
    return num_str;
}

// ----------------------------------------------------------------------
// DRM, display init (unchanged except no shader init here)
static int find_drm_device() {
    const char *cards[] = { "/dev/dri/card0", "/dev/dri/card1" };
    for (const char *card : cards) {
        int fd = open(card, O_RDWR);
        if (fd < 0) continue;
        drmModeRes *resources = drmModeGetResources(fd);
        if (!resources) { close(fd); continue; }
        for (int i = 0; i < resources->count_connectors; i++) {
            drmModeConnector *conn = drmModeGetConnector(fd, resources->connectors[i]);
            if (!conn) continue;
            if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
                mode = conn->modes[0];
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
                connector = conn;
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

#define CHECK_DRM(f, msg) do { if ((f) < 0) { std::cerr << msg << " (errno: " << errno << ")" << std::endl; exit(1); } } while(0)
#define CHECK_EGL(f, msg) do { EGLint err = eglGetError(); if ((f) != EGL_TRUE) { std::cerr << msg << " (EGL error: 0x" << std::hex << err << ")" << std::endl; exit(1); } } while(0)

void init_display() {
    drm_fd = find_drm_device();
    if (drm_fd < 0) { std::cerr << "No DRM device with connected display found." << std::endl; exit(1); }
    std::cout << "DRM opened, resolution: " << mode.hdisplay << "x" << mode.vdisplay << std::endl;

    gbm_dev = gbm_create_device(drm_fd);
    if (!gbm_dev) { std::cerr << "Cannot create GBM device." << std::endl; exit(1); }

    gbm_surface = gbm_surface_create(gbm_dev, mode.hdisplay, mode.vdisplay,
                                     GBM_FORMAT_XRGB8888,
                                     GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!gbm_surface) { std::cerr << "Cannot create GBM surface." << std::endl; exit(1); }

    egl_display = eglGetDisplay((EGLNativeDisplayType)gbm_dev);
    if (egl_display == EGL_NO_DISPLAY) { std::cerr << "eglGetDisplay returned EGL_NO_DISPLAY." << std::endl; exit(1); }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) { std::cerr << "eglInitialize failed." << std::endl; exit(1); }
    std::cout << "EGL " << major << "." << minor << std::endl;

    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint attribs[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                               EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 16,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                               EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(egl_display, attribs, &config, 1, &num_configs)) {
        std::cerr << "eglChooseConfig failed." << std::endl;
        exit(1);
    }

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
    CHECK_EGL(egl_context != EGL_NO_CONTEXT, "eglCreateContext");

    egl_surface = eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)gbm_surface, nullptr);
    CHECK_EGL(egl_surface != EGL_NO_SURFACE, "eglCreateWindowSurface");

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        std::cerr << "eglMakeCurrent failed." << std::endl;
        exit(1);
    }

    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
    if (!eglCreateImageKHR || !eglDestroyImageKHR) {
        std::cerr << "Missing EGL_KHR_image extension." << std::endl;
        exit(1);
    }

    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!glEGLImageTargetTexture2DOES) {
        std::cerr << "Missing GL_OES_EGL_image extension." << std::endl;
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
    std::cout << "Display ready: " << screen_width << "x" << screen_height << std::endl;
}

// ----------------------------------------------------------------------
// OpenGL init (creates shaders and VBOs)
void init_gl() {
    glViewport(0, 0, screen_width, screen_height);

    g_shaders = new Shaders();   // create shader programs

    // Camera VBO
    float cam_vertices[] = { -1,-1,0,0, 1,-1,1,0, 1,1,1,1, -1,1,0,1 };
    glGenBuffers(1, &vbo_camera);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_camera);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cam_vertices), cam_vertices, GL_STATIC_DRAW);

    // Overlay VBO (dynamic)
    glGenBuffers(1, &vbo_overlay);
    // Text VBO (dynamic)
    glGenBuffers(1, &vbo_text);

    buildPixelFontAtlas();

    CHECK_GL("init_gl");
}

// ----------------------------------------------------------------------
// Draw overlay (uses Shaders)
void draw_pfd() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    float proj[16] = {
        2.0f/screen_width, 0, 0, 0,
        0, -2.0f/screen_height, 0, 0,
        0, 0, 1, 0,
        -1, 1, 0, 1
    };

    g_shaders->useOverlay();
    g_shaders->setOverlayProjection(proj);

    GLint a_pos = g_shaders->getOverlayPosAttr();
    glBindBuffer(GL_ARRAY_BUFFER, vbo_overlay);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(a_pos);

    float horizon_y = screen_height / 2;
    float horizon_x = screen_width / 2;

    GLfloat vertical_horizon_lines[] = {
        200.0f, 125.0f,
        200.0f, (float)(screen_height-125),
        (float)(screen_width - 200), 125.0f,
        (float)(screen_width - 200), (float)(screen_height-125)
    };
    GLfloat crosshair[] = {
        (float)(horizon_x - 15), (float)(horizon_y - 10),
        (float)(horizon_x), (float)(horizon_y + 10),
        (float)(horizon_x + 15), (float)(horizon_y - 10)
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof(vertical_horizon_lines), vertical_horizon_lines, GL_DYNAMIC_DRAW);
    g_shaders->setOverlayColor(0.0f, 1.0f, 0.0f, 1.0f);
    glLineWidth(2);
    glDrawArrays(GL_LINES, 0, 4);

    std::vector<GLfloat> horiz_lines;
    auto add_segment = [&](float x1, float y1, float x2, float y2) {
        horiz_lines.push_back(x1); horiz_lines.push_back(y1);
        horiz_lines.push_back(x2); horiz_lines.push_back(y2);
        horiz_lines.push_back(screen_width - x1); horiz_lines.push_back(y1);
        horiz_lines.push_back(screen_width - x2); horiz_lines.push_back(y2);
    };
    add_segment(200.0f, 125.0f, 150.0f, 125.0f);
    float y_mid1 = ((horizon_y - 125.0f) / 3.0f) + 125.0f;
    add_segment(200.0f, y_mid1, 175.0f, y_mid1);
    float y_mid2 = (2.0f * (horizon_y - 125.0f) / 3.0f) + 125.0f;
    add_segment(200.0f, y_mid2, 175.0f, y_mid2);
    add_segment(200.0f, horizon_y, 150.0f, horizon_y);
    float y_bot1 = ((horizon_y - (screen_height - 125)) / 3.0f) + (screen_height - 125);
    add_segment(200.0f, y_bot1, 175.0f, y_bot1);
    float y_bot2 = (2.0f * (horizon_y - (screen_height - 125)) / 3.0f) + (screen_height - 125);
    add_segment(200.0f, y_bot2, 175.0f, y_bot2);
    add_segment(200.0f, screen_height - 125, 150.0f, screen_height - 125);

    glBufferData(GL_ARRAY_BUFFER, horiz_lines.size() * sizeof(GLfloat), horiz_lines.data(), GL_DYNAMIC_DRAW);
    glLineWidth(3);
    glDrawArrays(GL_LINES, 0, horiz_lines.size() / 2);

    glLineWidth(4);
    glBufferData(GL_ARRAY_BUFFER, sizeof(crosshair), crosshair, GL_STATIC_DRAW);
    glDrawArrays(GL_LINE_STRIP, 0, 3);

    glDisableVertexAttribArray(a_pos);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

    // Draw text
    /*float scale = 4.0f;
    std::string number = "1234ABCDEF.";
    float text_width = getPixelTextWidth(number, scale);
    float margin = 10.0f;
    float x = screen_width - 200 - text_width - margin;
    float y = (screen_height - PIXEL_FONT_HEIGHT * scale) / 2.0f;
    drawPixelText(number, x, y, scale, 1.0f, 1.0f, 1.0f, 1.0f);
    drawPixelTextWithOutline(number, x, y + 250, scale, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);*/

    counter += 0.001f;

    std::string text = formatNumber(counter, 3, 2);
    drawPixelTextWithOutline(text, screen_width / 2, screen_height / 2, 4.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 3);

    glDisable(GL_BLEND);
}

// ----------------------------------------------------------------------
// Import DMABUF (unchanged)
void import_dmabuf(const FrameBuffer *buffer, BufferResources &res) {
    const FrameBuffer::Plane &plane = buffer->planes()[0];
    int fd = plane.fd.get();
    off_t offset = plane.offset;
    unsigned int stride = cam.stride_value;
    unsigned int width = cam.stream_size.width;
    unsigned int height = cam.stream_size.height;

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
    res.egl_image = eglCreateImageKHR(egl_display, EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (res.egl_image == EGL_NO_IMAGE_KHR) {
        EGLint err = eglGetError();
        std::cerr << "eglCreateImageKHR failed, EGL error: 0x" << std::hex << err << std::endl;
        exit(1);
    }
    glGenTextures(1, &res.texture);
    glBindTexture(GL_TEXTURE_2D, res.texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)res.egl_image);
    CHECK_GL("glEGLImageTargetTexture2DOES");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    CHECK_GL("Texture parameters");
}

// ----------------------------------------------------------------------
// Present frame (unchanged)
void present_frame() {
    struct gbm_bo *new_bo = gbm_surface_lock_front_buffer(gbm_surface);
    if (!new_bo) { std::cerr << "Cannot lock front buffer." << std::endl; return; }
    uint32_t new_fb;
    int ret = drmModeAddFB(drm_fd, mode.hdisplay, mode.vdisplay, 24, 32,
                           gbm_bo_get_stride(new_bo), gbm_bo_get_handle(new_bo).u32, &new_fb);
    if (ret) { std::cerr << "drmModeAddFB failed." << std::endl; gbm_surface_release_buffer(gbm_surface, new_bo); return; }
    if (!displayed_bo) {
        ret = drmModeSetCrtc(drm_fd, crtc_id, new_fb, 0, 0, &connector_id, 1, &mode);
        if (ret) { std::cerr << "drmModeSetCrtc failed." << std::endl; drmModeRmFB(drm_fd, new_fb); gbm_surface_release_buffer(gbm_surface, new_bo); return; }
        displayed_bo = new_bo; displayed_fb = new_fb; return;
    }
    if (flip_pending) {
        fd_set fds; FD_ZERO(&fds); FD_SET(drm_fd, &fds);
        while (flip_pending) { int ret = select(drm_fd+1, &fds, nullptr, nullptr, nullptr); if (ret < 0) break; drmHandleEvent(drm_fd, &evctx); }
    }
    ret = drmModePageFlip(drm_fd, crtc_id, new_fb, DRM_MODE_PAGE_FLIP_EVENT, displayed_bo);
    if (ret) { std::cerr << "drmModePageFlip failed." << std::endl; drmModeRmFB(drm_fd, new_fb); gbm_surface_release_buffer(gbm_surface, new_bo); return; }
    next_bo = new_bo; next_fb = new_fb; flip_pending = true;
}

// ----------------------------------------------------------------------
// Main loop
void run() {
    for (auto &req : cam.requests) cam.camera->queueRequest(req.get());
    EGLSyncKHR rendering_fence = EGL_NO_SYNC_KHR;

    while (true) {
        std::unique_lock<std::mutex> lock(MCamera::request_mutex);
        MCamera::request_cv.wait(lock, []{ return !MCamera::completed_requests.empty(); });
        Request *completed = MCamera::completed_requests.front();
        MCamera::completed_requests.pop();
        lock.unlock();

        if (rendering_fence != EGL_NO_SYNC_KHR) {
            eglClientWaitSyncKHR(egl_display, rendering_fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
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

        // Render camera using shaders
        g_shaders->useCamera();
        glBindTexture(GL_TEXTURE_2D, it->second.texture);
        g_shaders->setCameraTexture(0);
        g_shaders->setCameraScale(1.0f, 1.0f);
        g_shaders->setCameraOffset(0.0f, 0.0f);

        glBindBuffer(GL_ARRAY_BUFFER, vbo_camera);
        GLint a_pos = g_shaders->getCameraPosAttr();
        GLint a_tex = g_shaders->getCameraTexAttr();
        glEnableVertexAttribArray(a_pos);
        glEnableVertexAttribArray(a_tex);
        glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
        glVertexAttribPointer(a_tex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              (void*)(2 * sizeof(float)));
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

        draw_pfd();

        eglSwapBuffers(egl_display, egl_surface);
        present_frame();

        rendering_fence = eglCreateSyncKHR(egl_display, EGL_SYNC_FENCE_KHR, nullptr);

        completed->reuse();
        completed->addBuffer(cam.video_stream, cam.fd_to_buffer[fd]);
        cam.camera->queueRequest(completed);
    }
}

// ----------------------------------------------------------------------
// Cleanup
void cleanup() {
    cam.camera->stop();
    cam.allocator->free(cam.video_stream);
    cam.camera->release();
    cam.cm->stop();

    for (auto &pair : buffer_resources) {
        if (pair.second.texture) glDeleteTextures(1, &pair.second.texture);
        if (pair.second.egl_image != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(egl_display, pair.second.egl_image);
    }
    if (pixel_font_atlas) glDeleteTextures(1, &pixel_font_atlas);

    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
        if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
        eglTerminate(egl_display);
    }

    if (flip_pending) {
        fd_set fds; FD_ZERO(&fds); FD_SET(drm_fd, &fds);
        select(drm_fd+1, &fds, nullptr, nullptr, nullptr);
        drmHandleEvent(drm_fd, &evctx);
    }
    if (displayed_bo) gbm_surface_release_buffer(gbm_surface, displayed_bo);
    if (next_bo) gbm_surface_release_buffer(gbm_surface, next_bo);
    if (displayed_fb) drmModeRmFB(drm_fd, displayed_fb);
    if (next_fb) drmModeRmFB(drm_fd, next_fb);
    if (gbm_surface) gbm_surface_destroy(gbm_surface);
    if (gbm_dev) gbm_device_destroy(gbm_dev);
    if (saved_crtc) {
        drmModeSetCrtc(drm_fd, saved_crtc->crtc_id, saved_crtc->buffer_id,
                       saved_crtc->x, saved_crtc->y, &connector_id, 1, &saved_crtc->mode);
        drmModeFreeCrtc(saved_crtc);
    }
    if (current_fb_id) drmModeRmFB(drm_fd, current_fb_id);
    if (connector) drmModeFreeConnector(connector);
    if (encoder) drmModeFreeEncoder(encoder);
    if (drm_fd >= 0) close(drm_fd);

    delete g_shaders;
    std::cout << "Resources released." << std::endl;
}

// ----------------------------------------------------------------------
// Main
int main() {
    init_display();
    init_gl();
    std::cout << formatNumber(1.1f, 3, 1) << "\n";
    std::cout << formatNumber(1.1567f, 3, 1) << "\n";
    std::cout << formatNumber(100.25f, 3, 1) << "\n";
    std::cout << formatNumber(15.0f, 3, 1) << "\n"; 
    run();
    cleanup();
    return 0;
}
