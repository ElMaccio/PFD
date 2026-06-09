#include "shaders.h"
#include <iostream>

// ----------------------------------------------------------------------
// Shader source strings (copied from your original code)
static const char* vs_camera = R"(
    precision highp float;
    attribute vec2 a_pos;
    attribute vec2 a_tex;
    varying vec2 v_tex;
    void main() {
        gl_Position = vec4(a_pos, 0.0, 1.0);
        v_tex = a_tex;
    }
)";

static const char* fs_camera = R"(
    precision highp float;
    varying vec2 v_tex;
    uniform sampler2D u_tex;
    uniform vec2 u_scale;
    uniform vec2 u_offset;
    void main() {
        gl_FragColor = vec4(texture2D(u_tex, v_tex * u_scale + u_offset).rgb, 1.0);
    }
)";

static const char* vs_overlay = R"(
    precision highp float;
    attribute vec2 a_pos;
    uniform mat4 u_proj;
    void main() {
        gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
    }
)";

static const char* fs_overlay = R"(
    precision highp float;
    uniform vec4 u_color;
    void main() {
        gl_FragColor = u_color;
    }
)";

static const char* vs_text = R"(
    precision highp float;
    attribute vec2 a_pos;
    attribute vec2 a_tex;
    varying vec2 v_tex;
    uniform mat4 u_proj;
    void main() {
        gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
        v_tex = a_tex;
    }
)";

static const char* fs_text = R"(
    precision mediump float;
    varying vec2 v_tex;
    uniform sampler2D u_tex;
    uniform vec4 u_color;
    void main() {
        float alpha = texture2D(u_tex, v_tex).a;
        gl_FragColor = vec4(u_color.rgb, u_color.a * alpha);
    }
)";

// ----------------------------------------------------------------------
GLuint Shaders::compileShader(GLenum type, const char* source) const {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &source, nullptr);
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

GLuint Shaders::linkProgram(GLuint vs, GLuint fs) const {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "Program link error: " << log << std::endl;
        exit(1);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

Shaders::Shaders() {
    // Compile camera shader
    GLuint vs_cam = compileShader(GL_VERTEX_SHADER, vs_camera);
    GLuint fs_cam = compileShader(GL_FRAGMENT_SHADER, fs_camera);
    prog_camera = linkProgram(vs_cam, fs_cam);

    // Get camera uniforms and attributes
    unif_camera_tex = glGetUniformLocation(prog_camera, "u_tex");
    unif_camera_scale = glGetUniformLocation(prog_camera, "u_scale");
    unif_camera_offset = glGetUniformLocation(prog_camera, "u_offset");
    attr_camera_pos = glGetAttribLocation(prog_camera, "a_pos");
    attr_camera_tex = glGetAttribLocation(prog_camera, "a_tex");

    // Compile overlay shader
    GLuint vs_ov = compileShader(GL_VERTEX_SHADER, vs_overlay);
    GLuint fs_ov = compileShader(GL_FRAGMENT_SHADER, fs_overlay);
    prog_overlay = linkProgram(vs_ov, fs_ov);

    unif_overlay_proj = glGetUniformLocation(prog_overlay, "u_proj");
    unif_overlay_color = glGetUniformLocation(prog_overlay, "u_color");
    attr_overlay_pos = glGetAttribLocation(prog_overlay, "a_pos");

    // Compile text shader
    GLuint vs_txt = compileShader(GL_VERTEX_SHADER, vs_text);
    GLuint fs_txt = compileShader(GL_FRAGMENT_SHADER, fs_text);
    prog_text = linkProgram(vs_txt, fs_txt);

    unif_text_proj = glGetUniformLocation(prog_text, "u_proj");
    unif_text_tex = glGetUniformLocation(prog_text, "u_tex");
    unif_text_color = glGetUniformLocation(prog_text, "u_color");
    attr_text_pos = glGetAttribLocation(prog_text, "a_pos");
    attr_text_tex = glGetAttribLocation(prog_text, "a_tex");

    CHECK_GL("Shaders constructor");
}

Shaders::~Shaders() {
    if (prog_camera) glDeleteProgram(prog_camera);
    if (prog_overlay) glDeleteProgram(prog_overlay);
    if (prog_text) glDeleteProgram(prog_text);
}

// ----------------------------------------------------------------------
void Shaders::useCamera() const {
    glUseProgram(prog_camera);
}

void Shaders::useOverlay() const {
    glUseProgram(prog_overlay);
}

void Shaders::useText() const {
    glUseProgram(prog_text);
}

// Camera uniforms
void Shaders::setCameraTexture(GLuint textureUnit) const {
    glUniform1i(unif_camera_tex, textureUnit);
}

void Shaders::setCameraScale(float sx, float sy) const {
    glUniform2f(unif_camera_scale, sx, sy);
}

void Shaders::setCameraOffset(float ox, float oy) const {
    glUniform2f(unif_camera_offset, ox, oy);
}

// Overlay uniforms
void Shaders::setOverlayProjection(const float matrix[16]) const {
    glUniformMatrix4fv(unif_overlay_proj, 1, GL_FALSE, matrix);
}

void Shaders::setOverlayColor(float r, float g, float b, float a) const {
    glUniform4f(unif_overlay_color, r, g, b, a);
}

// Text uniforms
void Shaders::setTextProjection(const float matrix[16]) const {
    glUniformMatrix4fv(unif_text_proj, 1, GL_FALSE, matrix);
}

void Shaders::setTextureUnit(GLuint unit) const {
    glUniform1i(unif_text_tex, unit);
}

void Shaders::setTextColor(float r, float g, float b, float a) const {
    glUniform4f(unif_text_color, r, g, b, a);
}
