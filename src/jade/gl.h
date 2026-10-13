// Jade's native layer: OpenGL as it is used here.
//
// The system's own header declares OpenGL as it was in 1997. Everything later (shaders,
// buffers, framebuffers) is fetched from the driver by name once a context exists; the
// functions used are listed here and nothing else of OpenGL is touched. No loader library.
#pragma once

#ifdef _WIN32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#ifndef APIENTRY
#define APIENTRY
#endif
#endif

#include <cstddef>

namespace astra::jade {

const GLenum kFragmentShader = 0x8B30, kVertexShader = 0x8B31, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82;
const GLenum kArrayBuffer = 0x8892, kStreamDraw = 0x88E0, kTexture0 = 0x84C0, kClampToEdge = 0x812F;
const GLenum kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5;
const GLenum kR8 = 0x8229, kRed = 0x1903, kRgba8 = 0x8058, kTriangleStrip = 0x0005;

struct GlApi {
    GLuint(APIENTRY* CreateShader)(GLenum);
    void(APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
    void(APIENTRY* CompileShader)(GLuint);
    void(APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
    GLuint(APIENTRY* CreateProgram)();
    void(APIENTRY* AttachShader)(GLuint, GLuint);
    void(APIENTRY* LinkProgram)(GLuint);
    void(APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* UseProgram)(GLuint);
    GLint(APIENTRY* GetUniformLocation)(GLuint, const char*);
    void(APIENTRY* Uniform1i)(GLint, GLint);
    void(APIENTRY* Uniform1f)(GLint, GLfloat);
    void(APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat);
    void(APIENTRY* GenVertexArrays)(GLsizei, GLuint*);
    void(APIENTRY* BindVertexArray)(GLuint);
    void(APIENTRY* GenBuffers)(GLsizei, GLuint*);
    void(APIENTRY* BindBuffer)(GLenum, GLuint);
    void(APIENTRY* BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
    void(APIENTRY* EnableVertexAttribArray)(GLuint);
    void(APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void(APIENTRY* VertexAttribDivisor)(GLuint, GLuint);
    void(APIENTRY* DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
    void(APIENTRY* ActiveTexture)(GLenum);
    void(APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
    void(APIENTRY* BindFramebuffer)(GLenum, GLuint);
    void(APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum(APIENTRY* CheckFramebufferStatus)(GLenum);
    void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
    void(APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
};

extern GlApi gl;

// Fills `gl` through a function that finds an OpenGL function by name (the window system's
// own). False, with the name of the first one missing, if the driver lacks any.
bool loadGl(void* (*find)(const char* name), const char*& missing);

}  // namespace astra::jade
