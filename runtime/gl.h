// Minimal OpenGL ES 3.0 declarations, loaded at runtime through SDL_GL_GetProcAddress (the
// same code on Windows, where SDL creates an ES 3 context through WGL, and on Android/EGL).
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(_WIN64)
#define GLAPIENTRY __stdcall
#else
#define GLAPIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef struct __GLsync* GLsync;
typedef uint64_t GLuint64;

enum : GLenum {
    GL_NO_ERROR = 0,
    GL_FALSE_ = 0, GL_TRUE_ = 1,
    GL_TRIANGLES = 0x0004,
    GL_ONE = 1, GL_ZERO = 0,
    GL_SRC_ALPHA = 0x0302,
    GL_CONSTANT_ALPHA = 0x8003, GL_SRC1_COLOR = 0x88F9,  // (EXT_blend_func_extended)
    GL_FUNC_ADD = 0x8006, GL_FUNC_REVERSE_SUBTRACT = 0x800B,
    GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11, GL_DEPTH_TEST = 0x0B71, GL_CULL_FACE = 0x0B44,
    GL_DITHER = 0x0BD0,
    GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406,
    GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_R16UI = 0x8234, GL_RED_INTEGER = 0x8D94,
    GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_EXTENSIONS = 0x1F03,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F,
    GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0,
    GL_COLOR_BUFFER_BIT = 0x4000,
    GL_ARRAY_BUFFER = 0x8892, GL_STREAM_DRAW = 0x88E0, GL_DYNAMIC_DRAW = 0x88E8,
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31,
    GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_INFO_LOG_LENGTH = 0x8B84,
    GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9,
    GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_MAX_TEXTURE_SIZE = 0x0D33,
    GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117, GL_SYNC_FLUSH_COMMANDS_BIT = 0x1,
};

namespace psx {

// X(return type, name, parameters)
#define GL_FUNCS(X)                                                                                      \
    X(const GLubyte*, glGetString, (GLenum name))                                                        \
    X(void, glGetIntegerv, (GLenum pname, GLint* data))                                                  \
    X(GLenum, glGetError, (void))                                                                        \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                        \
    X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                         \
    X(void, glEnable, (GLenum cap))                                                                      \
    X(void, glDisable, (GLenum cap))                                                                     \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                  \
    X(void, glClear, (GLbitfield mask))                                                                  \
    X(void, glBlendFuncSeparate, (GLenum sc, GLenum dc, GLenum sa, GLenum da))                           \
    X(void, glBlendEquationSeparate, (GLenum c, GLenum a))                                               \
    X(void, glBlendColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                  \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                           \
    X(void, glGenTextures, (GLsizei n, GLuint* t))                                                       \
    X(void, glDeleteTextures, (GLsizei n, const GLuint* t))                                              \
    X(void, glBindTexture, (GLenum target, GLuint t))                                                    \
    X(void, glActiveTexture, (GLenum unit))                                                              \
    X(void, glTexStorage2D, (GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h))           \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,        \
                              GLenum fmt, GLenum type, const void* data))                                \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint v))                                     \
    X(void, glPixelStorei, (GLenum pname, GLint v))                                                      \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* f))                                                   \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* f))                                          \
    X(void, glBindFramebuffer, (GLenum target, GLuint f))                                                \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum tt, GLuint t, GLint level))       \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                                 \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,         \
                                GLint dx1, GLint dy1, GLbitfield mask, GLenum filter))                   \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* data)) \
    X(void, glGenBuffers, (GLsizei n, GLuint* b))                                                        \
    X(void, glBindBuffer, (GLenum target, GLuint b))                                                     \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))              \
    X(void, glGenVertexArrays, (GLsizei n, GLuint* a))                                                   \
    X(void, glBindVertexArray, (GLuint a))                                                               \
    X(void, glEnableVertexAttribArray, (GLuint i))                                                       \
    X(void, glVertexAttribPointer, (GLuint i, GLint n, GLenum type, GLboolean norm, GLsizei stride,      \
                                    const void* off))                                                    \
    X(void, glVertexAttribIPointer, (GLuint i, GLint n, GLenum type, GLsizei stride, const void* off))  \
    X(GLuint, glCreateShader, (GLenum type))                                                             \
    X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar* const* src, const GLint* len))           \
    X(void, glCompileShader, (GLuint s))                                                                 \
    X(void, glGetShaderiv, (GLuint s, GLenum pname, GLint* v))                                           \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei* len, GLchar* log))                      \
    X(void, glDeleteShader, (GLuint s))                                                                  \
    X(GLuint, glCreateProgram, (void))                                                                   \
    X(void, glAttachShader, (GLuint p, GLuint s))                                                        \
    X(void, glLinkProgram, (GLuint p))                                                                   \
    X(void, glGetProgramiv, (GLuint p, GLenum pname, GLint* v))                                          \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei* len, GLchar* log))                     \
    X(void, glUseProgram, (GLuint p))                                                                    \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar* name))                                       \
    X(void, glUniform1i, (GLint loc, GLint v))                                                           \
    X(void, glUniform2i, (GLint loc, GLint a, GLint b))                                                  \
    X(void, glUniform2f, (GLint loc, GLfloat a, GLfloat b))                                              \
    X(void, glUniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                        \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei n))                                         \
    X(void, glFlush, (void))                                                                             \
    X(void, glFinish, (void))                                                                            \
    X(GLsync, glFenceSync, (GLenum cond, GLbitfield flags))                                              \
    X(GLenum, glClientWaitSync, (GLsync s, GLbitfield flags, GLuint64 timeout))                          \
    X(void, glDeleteSync, (GLsync s))

#define GL_DECLARE(ret, name, args) typedef ret(GLAPIENTRY* PFN_##name) args; extern PFN_##name name;
GL_FUNCS(GL_DECLARE)
#undef GL_DECLARE

bool gl_load_functions();  // after a context is current; false if any is missing

}  // namespace psx
