// OpenGL 3.3 core entry points used by the host renderer, loaded through SDL (works on Linux
// and Windows without a separate loader library).
#ifndef NBA_GL_FUNCS_H
#define NBA_GL_FUNCS_H

#include <SDL3/SDL_opengl.h>

/* GL 1.0/1.1 entry points have no PFN typedefs in glext.h */
typedef void(APIENTRYP PFNGLGENTEXTURESPROC)(GLsizei n, GLuint* textures);
typedef void(APIENTRYP PFNGLDELETETEXTURESPROC)(GLsizei n, const GLuint* textures);
typedef void(APIENTRYP PFNGLBINDTEXTUREPROC)(GLenum target, GLuint texture);
typedef void(APIENTRYP PFNGLTEXIMAGE2DPROC)(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                           GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
typedef void(APIENTRYP PFNGLTEXPARAMETERIPROC)(GLenum target, GLenum pname, GLint param);
typedef void(APIENTRYP PFNGLTEXPARAMETERFPROC)(GLenum target, GLenum pname, GLfloat param);
typedef void(APIENTRYP PFNGLDRAWARRAYSPROC)(GLenum mode, GLint first, GLsizei count);
typedef void(APIENTRYP PFNGLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void(APIENTRYP PFNGLSCISSORPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void(APIENTRYP PFNGLENABLEPROC)(GLenum cap);
typedef void(APIENTRYP PFNGLDISABLEPROC)(GLenum cap);
typedef void(APIENTRYP PFNGLDEPTHFUNCPROC)(GLenum func);
typedef void(APIENTRYP PFNGLDEPTHMASKPROC)(GLboolean flag);
typedef void(APIENTRYP PFNGLCOLORMASKPROC)(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
typedef void(APIENTRYP PFNGLCLEARCOLORPROC)(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
typedef void(APIENTRYP PFNGLCLEARDEPTHPROC)(GLdouble depth);
typedef void(APIENTRYP PFNGLCLEARPROC)(GLbitfield mask);
typedef GLenum(APIENTRYP PFNGLGETERRORPROC)(void);
typedef void(APIENTRYP PFNGLPIXELSTOREIPROC)(GLenum pname, GLint param);
typedef void(APIENTRYP PFNGLREADPIXELSPROC)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                           GLenum type, void* pixels);
typedef const GLubyte*(APIENTRYP PFNGLGETSTRINGPROC)(GLenum name);

#define NBA_GL_FUNCS(X)                                                                          \
  X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                           \
  X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                           \
  X(PFNGLBUFFERDATAPROC, glBufferData)                                                           \
  X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                 \
  X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                 \
  X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                                         \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                 \
  X(PFNGLCREATESHADERPROC, glCreateShader)                                                       \
  X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                       \
  X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                     \
  X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                         \
  X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                               \
  X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                     \
  X(PFNGLATTACHSHADERPROC, glAttachShader)                                                       \
  X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                         \
  X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                       \
  X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                             \
  X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                           \
  X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                           \
  X(PFNGLUNIFORM1IPROC, glUniform1i)                                                             \
  X(PFNGLUNIFORM1UIPROC, glUniform1ui)                                                           \
  X(PFNGLUNIFORM1FPROC, glUniform1f)                                                             \
  X(PFNGLUNIFORM2FPROC, glUniform2f)                                                             \
  X(PFNGLUNIFORM4FPROC, glUniform4f)                                                             \
  X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                                 \
  X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                 \
  X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                       \
  X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                   \
  X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)                                                 \
  X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                                     \
  X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                             \
  X(PFNGLGENTEXTURESPROC, glGenTextures)                                                         \
  X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                                   \
  X(PFNGLBINDTEXTUREPROC, glBindTexture)                                                         \
  X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                                           \
  X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                                     \
  X(PFNGLTEXPARAMETERFPROC, glTexParameterf)                                                     \
  X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                           \
  X(PFNGLVIEWPORTPROC, glViewport)                                                               \
  X(PFNGLSCISSORPROC, glScissor)                                                                 \
  X(PFNGLENABLEPROC, glEnable)                                                                   \
  X(PFNGLDISABLEPROC, glDisable)                                                                 \
  X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                                             \
  X(PFNGLDEPTHMASKPROC, glDepthMask)                                                             \
  X(PFNGLCOLORMASKPROC, glColorMask)                                                             \
  X(PFNGLCLEARCOLORPROC, glClearColor)                                                           \
  X(PFNGLCLEARDEPTHPROC, glClearDepth)                                                           \
  X(PFNGLCLEARPROC, glClear)                                                                     \
  X(PFNGLGETERRORPROC, glGetError)                                                               \
  X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                                         \
  X(PFNGLREADPIXELSPROC, glReadPixels)                                                           \
  X(PFNGLGETSTRINGPROC, glGetString)

#define NBA_GL_DECLARE(type, name) extern type nba_##name;
NBA_GL_FUNCS(NBA_GL_DECLARE)
#undef NBA_GL_DECLARE

/* Call through the loaded pointers. */
#define glGenBuffers nba_glGenBuffers
#define glBindBuffer nba_glBindBuffer
#define glBufferData nba_glBufferData
#define glGenVertexArrays nba_glGenVertexArrays
#define glBindVertexArray nba_glBindVertexArray
#define glVertexAttribPointer nba_glVertexAttribPointer
#define glEnableVertexAttribArray nba_glEnableVertexAttribArray
#define glCreateShader nba_glCreateShader
#define glShaderSource nba_glShaderSource
#define glCompileShader nba_glCompileShader
#define glGetShaderiv nba_glGetShaderiv
#define glGetShaderInfoLog nba_glGetShaderInfoLog
#define glCreateProgram nba_glCreateProgram
#define glAttachShader nba_glAttachShader
#define glLinkProgram nba_glLinkProgram
#define glGetProgramiv nba_glGetProgramiv
#define glGetProgramInfoLog nba_glGetProgramInfoLog
#define glUseProgram nba_glUseProgram
#define glGetUniformLocation nba_glGetUniformLocation
#define glUniform1i nba_glUniform1i
#define glUniform1ui nba_glUniform1ui
#define glUniform1f nba_glUniform1f
#define glUniform2f nba_glUniform2f
#define glUniform4f nba_glUniform4f
#define glGenFramebuffers nba_glGenFramebuffers
#define glBindFramebuffer nba_glBindFramebuffer
#define glFramebufferTexture2D nba_glFramebufferTexture2D
#define glCheckFramebufferStatus nba_glCheckFramebufferStatus
#define glBlitFramebuffer nba_glBlitFramebuffer
#define glActiveTexture nba_glActiveTexture
#define glBlendFuncSeparate nba_glBlendFuncSeparate
#define glGenTextures nba_glGenTextures
#define glDeleteTextures nba_glDeleteTextures
#define glBindTexture nba_glBindTexture
#define glTexImage2D nba_glTexImage2D
#define glTexParameteri nba_glTexParameteri
#define glTexParameterf nba_glTexParameterf
#define glDrawArrays nba_glDrawArrays
#define glViewport nba_glViewport
#define glScissor nba_glScissor
#define glEnable nba_glEnable
#define glDisable nba_glDisable
#define glDepthFunc nba_glDepthFunc
#define glDepthMask nba_glDepthMask
#define glColorMask nba_glColorMask
#define glClearColor nba_glClearColor
#define glClearDepth nba_glClearDepth
#define glClear nba_glClear
#define glGetError nba_glGetError
#define glPixelStorei nba_glPixelStorei
#define glReadPixels nba_glReadPixels
#define glGetString nba_glGetString

/* Returns 0 on success, or -1 naming the first missing function. */
int nba_gl_load(void);

#endif
