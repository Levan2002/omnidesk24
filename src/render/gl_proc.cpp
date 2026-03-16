#include "render/gl_proc.h"

#ifdef _WIN32

#include <GLFW/glfw3.h>

// Define all function pointer globals
PFNGLACTIVETEXTUREPROC            glActiveTexture           = nullptr;
PFNGLGENBUFFERSPROC               glGenBuffers              = nullptr;
PFNGLDELETEBUFFERSPROC            glDeleteBuffers           = nullptr;
PFNGLBINDBUFFERPROC               glBindBuffer              = nullptr;
PFNGLBUFFERDATAPROC               glBufferData              = nullptr;
PFNGLCREATESHADERPROC             glCreateShader            = nullptr;
PFNGLSHADERSOURCEPROC             glShaderSource            = nullptr;
PFNGLCOMPILESHADERPROC            glCompileShader           = nullptr;
PFNGLGETSHADERIVPROC              glGetShaderiv             = nullptr;
PFNGLGETSHADERINFOLOGPROC         glGetShaderInfoLog        = nullptr;
PFNGLDELETESHADERPROC             glDeleteShader            = nullptr;
PFNGLCREATEPROGRAMPROC            glCreateProgram           = nullptr;
PFNGLATTACHSHADERPROC             glAttachShader            = nullptr;
PFNGLLINKPROGRAMPROC              glLinkProgram             = nullptr;
PFNGLGETPROGRAMIVPROC             glGetProgramiv            = nullptr;
PFNGLGETPROGRAMINFOLOGPROC        glGetProgramInfoLog       = nullptr;
PFNGLDELETEPROGRAMPROC            glDeleteProgram           = nullptr;
PFNGLUSEPROGRAMPROC               glUseProgram              = nullptr;
PFNGLGETUNIFORMLOCATIONPROC       glGetUniformLocation      = nullptr;
PFNGLUNIFORM1IPROC                glUniform1i               = nullptr;
PFNGLUNIFORM1FPROC                glUniform1f               = nullptr;
PFNGLUNIFORM2FPROC                glUniform2f               = nullptr;
PFNGLUNIFORM4FPROC                glUniform4f               = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC      glVertexAttribPointer     = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC  glEnableVertexAttribArray = nullptr;
PFNGLGENVERTEXARRAYSPROC          glGenVertexArrays         = nullptr;
PFNGLBINDVERTEXARRAYPROC          glBindVertexArray         = nullptr;
PFNGLDELETEVERTEXARRAYSPROC       glDeleteVertexArrays      = nullptr;
PFNGLGENFRAMEBUFFERSPROC          glGenFramebuffers         = nullptr;
PFNGLBINDFRAMEBUFFERPROC          glBindFramebuffer         = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC     glFramebufferTexture2D    = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC       glDeleteFramebuffers      = nullptr;

#define LOAD(name) name = reinterpret_cast<decltype(name)>(glfwGetProcAddress(#name))

namespace omnidesk {

void loadGLProcs() {
    LOAD(glActiveTexture);
    LOAD(glGenBuffers);
    LOAD(glDeleteBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferData);
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glDeleteShader);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glDeleteProgram);
    LOAD(glUseProgram);
    LOAD(glGetUniformLocation);
    LOAD(glUniform1i);
    LOAD(glUniform1f);
    LOAD(glUniform2f);
    LOAD(glUniform4f);
    LOAD(glVertexAttribPointer);
    LOAD(glEnableVertexAttribArray);
    LOAD(glGenVertexArrays);
    LOAD(glBindVertexArray);
    LOAD(glDeleteVertexArrays);
    LOAD(glGenFramebuffers);
    LOAD(glBindFramebuffer);
    LOAD(glFramebufferTexture2D);
    LOAD(glDeleteFramebuffers);
}

}  // namespace omnidesk

#undef LOAD

#endif  // _WIN32
