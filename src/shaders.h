#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <string>


#define CHECK_GL(msg) do { \
    GLenum err = glGetError(); \
    if (err != GL_NO_ERROR) { \
        std::cerr << msg << " (GL error: 0x" << std::hex << err << ")" << std::endl; \
        exit(1); \
    } \
} while(0)


class Shaders {
public:
    Shaders();
    ~Shaders();

    // Prevent copying
    Shaders(const Shaders&) = delete;
    Shaders& operator=(const Shaders&) = delete;

    // Use a specific shader program
    void useCamera() const;
    void useOverlay() const;
    void useText() const;

    // Uniform setters for camera shader
    void setCameraTexture(GLuint textureUnit) const;
    void setCameraScale(float sx, float sy) const;
    void setCameraOffset(float ox, float oy) const;

    // Uniform setters for overlay shader
    void setOverlayProjection(const float matrix[16]) const;
    void setOverlayColor(float r, float g, float b, float a) const;

    // Uniform setters for text shader
    void setTextProjection(const float matrix[16]) const;
    void setTextureUnit(GLuint unit) const;      // for text shader
    void setTextColor(float r, float g, float b, float a) const;

    // Attribute locations (for binding VBOs)
    GLuint getCameraPosAttr() const { return attr_camera_pos; }
    GLuint getCameraTexAttr() const { return attr_camera_tex; }
    GLuint getOverlayPosAttr() const { return attr_overlay_pos; }
    GLuint getTextPosAttr() const { return attr_text_pos; }
    GLuint getTextTexAttr() const { return attr_text_tex; }

    // Program handles (if needed directly)
    GLuint getCameraProgram() const { return prog_camera; }
    GLuint getOverlayProgram() const { return prog_overlay; }
    GLuint getTextProgram() const { return prog_text; }

private:
    GLuint compileShader(GLenum type, const char* source) const;
    GLuint linkProgram(GLuint vs, GLuint fs) const;

    GLuint prog_camera;
    GLuint prog_overlay;
    GLuint prog_text;

    // Camera shader uniforms & attributes
    GLuint unif_camera_tex;
    GLuint unif_camera_scale;
    GLuint unif_camera_offset;
    GLuint attr_camera_pos;
    GLuint attr_camera_tex;

    // Overlay shader uniforms & attributes
    GLuint unif_overlay_proj;
    GLuint unif_overlay_color;
    GLuint attr_overlay_pos;

    // Text shader uniforms & attributes
    GLuint unif_text_proj;
    GLuint unif_text_tex;
    GLuint unif_text_color;
    GLuint attr_text_pos;
    GLuint attr_text_tex;
};
