/* GL calls through the render thread's queue (native_glq.c).
 *
 * Included by the GL backend (host_gl_webgl.c, host_gl_clientarrays.c; patch
 * 0006) after <GLES3/gl3.h>: every GLES call those files make becomes the
 * glq_ wrapper of the same name. The macros are function-like so a bare
 * function name (UMAT's `fn` argument, an imp_opengl32__ token paste) is left
 * alone and only a call is redirected. */
#ifndef NATIVE_GLQ_H
#define NATIVE_GLQ_H
#include <GLES3/gl3.h>
#include <stddef.h>
#include <stdint.h>

/* 1 while the render thread owns the context */
int isaac_glq_active(void);
/* fn(ctx) on the render thread; returns once it has run */
void isaac_glq_run(void (*fn)(void *), void *ctx);
/* fn(copy of data) on the render thread, after everything queued before it */
void isaac_glq_post(void (*fn)(const void *), const void *data, size_t bytes);
/* a command written in place: room for `bytes` of arguments, queued (or, with
 * the queue off, run) by end; fn gets the bytes. No other glq_ call between. */
void *isaac_glq_begin(void (*fn)(const void *), size_t bytes);
void isaac_glq_end(void);
/* the queue position after the last command, and a wait until the render
 * thread has run everything before such a position */
uint64_t isaac_glq_mark(void);
void isaac_glq_wait(uint64_t mark);

#define GLQ_FUNCS(X) \
    X(glActiveTexture) X(glAttachShader) X(glBindBuffer) X(glBindFramebuffer) X(glBindRenderbuffer) \
    X(glBindTexture) X(glBlendEquation) X(glBlendFuncSeparate) X(glBufferData) X(glBufferSubData) \
    X(glCheckFramebufferStatus) X(glClear) X(glClearColor) X(glClearDepthf) X(glClientWaitSync) \
    X(glCompileShader) X(glCreateProgram) X(glCreateShader) X(glCullFace) X(glDeleteBuffers) \
    X(glDeleteFramebuffers) X(glDeleteProgram) X(glDeleteRenderbuffers) X(glDeleteShader) \
    X(glDeleteSync) X(glDeleteTextures) X(glDepthFunc) X(glDisable) X(glDisableVertexAttribArray) \
    X(glDrawArrays) X(glDrawArraysInstanced) X(glDrawElements) X(glEnable) \
    X(glEnableVertexAttribArray) X(glFenceSync) X(glFramebufferRenderbuffer) \
    X(glFramebufferTexture2D) X(glGenBuffers) X(glGenFramebuffers) X(glGenRenderbuffers) \
    X(glGenTextures) X(glGetAttribLocation) X(glGetError) X(glGetIntegerv) X(glGetProgramInfoLog) \
    X(glGetProgramiv) X(glGetRenderbufferParameteriv) X(glGetShaderInfoLog) X(glGetShaderiv) \
    X(glGetString) X(glGetStringi) X(glGetUniformLocation) X(glLinkProgram) X(glMapBufferRange) \
    X(glReadPixels) X(glRenderbufferStorage) X(glShaderSource) X(glTexImage2D) X(glTexParameteri) \
    X(glTexSubImage2D) X(glUniform1fv) X(glUniform1i) X(glUniform1iv) X(glUniform1uiv) \
    X(glUniform2fv) X(glUniform2iv) X(glUniform2uiv) X(glUniform3fv) X(glUniform3iv) \
    X(glUniform3uiv) X(glUniform4fv) X(glUniform4iv) X(glUniform4uiv) X(glUniformMatrix2fv) \
    X(glUniformMatrix2x3fv) X(glUniformMatrix2x4fv) X(glUniformMatrix3fv) X(glUniformMatrix3x2fv) \
    X(glUniformMatrix3x4fv) X(glUniformMatrix4fv) X(glUniformMatrix4x2fv) X(glUniformMatrix4x3fv) \
    X(glUnmapBuffer) X(glUseProgram) X(glVertexAttribPointer) X(glViewport)

/* the wrappers have the GLES prototypes under a glq_ name */
#define GLQ_DECLARE(name) extern __typeof__(name) glq_##name;
GLQ_FUNCS(GLQ_DECLARE)
#undef GLQ_DECLARE

#ifndef NATIVE_GLQ_IMPL
#define glActiveTexture(...) glq_glActiveTexture(__VA_ARGS__)
#define glAttachShader(...) glq_glAttachShader(__VA_ARGS__)
#define glBindBuffer(...) glq_glBindBuffer(__VA_ARGS__)
#define glBindFramebuffer(...) glq_glBindFramebuffer(__VA_ARGS__)
#define glBindRenderbuffer(...) glq_glBindRenderbuffer(__VA_ARGS__)
#define glBindTexture(...) glq_glBindTexture(__VA_ARGS__)
#define glBlendEquation(...) glq_glBlendEquation(__VA_ARGS__)
#define glBlendFuncSeparate(...) glq_glBlendFuncSeparate(__VA_ARGS__)
#define glBufferData(...) glq_glBufferData(__VA_ARGS__)
#define glBufferSubData(...) glq_glBufferSubData(__VA_ARGS__)
#define glCheckFramebufferStatus(...) glq_glCheckFramebufferStatus(__VA_ARGS__)
#define glClear(...) glq_glClear(__VA_ARGS__)
#define glClearColor(...) glq_glClearColor(__VA_ARGS__)
#define glClearDepthf(...) glq_glClearDepthf(__VA_ARGS__)
#define glClientWaitSync(...) glq_glClientWaitSync(__VA_ARGS__)
#define glCompileShader(...) glq_glCompileShader(__VA_ARGS__)
#define glCreateProgram(...) glq_glCreateProgram(__VA_ARGS__)
#define glCreateShader(...) glq_glCreateShader(__VA_ARGS__)
#define glCullFace(...) glq_glCullFace(__VA_ARGS__)
#define glDeleteBuffers(...) glq_glDeleteBuffers(__VA_ARGS__)
#define glDeleteFramebuffers(...) glq_glDeleteFramebuffers(__VA_ARGS__)
#define glDeleteProgram(...) glq_glDeleteProgram(__VA_ARGS__)
#define glDeleteRenderbuffers(...) glq_glDeleteRenderbuffers(__VA_ARGS__)
#define glDeleteShader(...) glq_glDeleteShader(__VA_ARGS__)
#define glDeleteSync(...) glq_glDeleteSync(__VA_ARGS__)
#define glDeleteTextures(...) glq_glDeleteTextures(__VA_ARGS__)
#define glDepthFunc(...) glq_glDepthFunc(__VA_ARGS__)
#define glDisable(...) glq_glDisable(__VA_ARGS__)
#define glDisableVertexAttribArray(...) glq_glDisableVertexAttribArray(__VA_ARGS__)
#define glDrawArrays(...) glq_glDrawArrays(__VA_ARGS__)
#define glDrawArraysInstanced(...) glq_glDrawArraysInstanced(__VA_ARGS__)
#define glDrawElements(...) glq_glDrawElements(__VA_ARGS__)
#define glEnable(...) glq_glEnable(__VA_ARGS__)
#define glEnableVertexAttribArray(...) glq_glEnableVertexAttribArray(__VA_ARGS__)
#define glFenceSync(...) glq_glFenceSync(__VA_ARGS__)
#define glFramebufferRenderbuffer(...) glq_glFramebufferRenderbuffer(__VA_ARGS__)
#define glFramebufferTexture2D(...) glq_glFramebufferTexture2D(__VA_ARGS__)
#define glGenBuffers(...) glq_glGenBuffers(__VA_ARGS__)
#define glGenFramebuffers(...) glq_glGenFramebuffers(__VA_ARGS__)
#define glGenRenderbuffers(...) glq_glGenRenderbuffers(__VA_ARGS__)
#define glGenTextures(...) glq_glGenTextures(__VA_ARGS__)
#define glGetAttribLocation(...) glq_glGetAttribLocation(__VA_ARGS__)
#define glGetError(...) glq_glGetError(__VA_ARGS__)
#define glGetIntegerv(...) glq_glGetIntegerv(__VA_ARGS__)
#define glGetProgramInfoLog(...) glq_glGetProgramInfoLog(__VA_ARGS__)
#define glGetProgramiv(...) glq_glGetProgramiv(__VA_ARGS__)
#define glGetRenderbufferParameteriv(...) glq_glGetRenderbufferParameteriv(__VA_ARGS__)
#define glGetShaderInfoLog(...) glq_glGetShaderInfoLog(__VA_ARGS__)
#define glGetShaderiv(...) glq_glGetShaderiv(__VA_ARGS__)
#define glGetString(...) glq_glGetString(__VA_ARGS__)
#define glGetStringi(...) glq_glGetStringi(__VA_ARGS__)
#define glGetUniformLocation(...) glq_glGetUniformLocation(__VA_ARGS__)
#define glLinkProgram(...) glq_glLinkProgram(__VA_ARGS__)
#define glMapBufferRange(...) glq_glMapBufferRange(__VA_ARGS__)
#define glReadPixels(...) glq_glReadPixels(__VA_ARGS__)
#define glRenderbufferStorage(...) glq_glRenderbufferStorage(__VA_ARGS__)
#define glShaderSource(...) glq_glShaderSource(__VA_ARGS__)
#define glTexImage2D(...) glq_glTexImage2D(__VA_ARGS__)
#define glTexParameteri(...) glq_glTexParameteri(__VA_ARGS__)
#define glTexSubImage2D(...) glq_glTexSubImage2D(__VA_ARGS__)
#define glUniform1fv(...) glq_glUniform1fv(__VA_ARGS__)
#define glUniform1i(...) glq_glUniform1i(__VA_ARGS__)
#define glUniform1iv(...) glq_glUniform1iv(__VA_ARGS__)
#define glUniform1uiv(...) glq_glUniform1uiv(__VA_ARGS__)
#define glUniform2fv(...) glq_glUniform2fv(__VA_ARGS__)
#define glUniform2iv(...) glq_glUniform2iv(__VA_ARGS__)
#define glUniform2uiv(...) glq_glUniform2uiv(__VA_ARGS__)
#define glUniform3fv(...) glq_glUniform3fv(__VA_ARGS__)
#define glUniform3iv(...) glq_glUniform3iv(__VA_ARGS__)
#define glUniform3uiv(...) glq_glUniform3uiv(__VA_ARGS__)
#define glUniform4fv(...) glq_glUniform4fv(__VA_ARGS__)
#define glUniform4iv(...) glq_glUniform4iv(__VA_ARGS__)
#define glUniform4uiv(...) glq_glUniform4uiv(__VA_ARGS__)
#define glUniformMatrix2fv(...) glq_glUniformMatrix2fv(__VA_ARGS__)
#define glUniformMatrix2x3fv(...) glq_glUniformMatrix2x3fv(__VA_ARGS__)
#define glUniformMatrix2x4fv(...) glq_glUniformMatrix2x4fv(__VA_ARGS__)
#define glUniformMatrix3fv(...) glq_glUniformMatrix3fv(__VA_ARGS__)
#define glUniformMatrix3x2fv(...) glq_glUniformMatrix3x2fv(__VA_ARGS__)
#define glUniformMatrix3x4fv(...) glq_glUniformMatrix3x4fv(__VA_ARGS__)
#define glUniformMatrix4fv(...) glq_glUniformMatrix4fv(__VA_ARGS__)
#define glUniformMatrix4x2fv(...) glq_glUniformMatrix4x2fv(__VA_ARGS__)
#define glUniformMatrix4x3fv(...) glq_glUniformMatrix4x3fv(__VA_ARGS__)
#define glUnmapBuffer(...) glq_glUnmapBuffer(__VA_ARGS__)
#define glUseProgram(...) glq_glUseProgram(__VA_ARGS__)
#define glVertexAttribPointer(...) glq_glVertexAttribPointer(__VA_ARGS__)
#define glViewport(...) glq_glViewport(__VA_ARGS__)
#endif
#endif
