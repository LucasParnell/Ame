#define eglCreateContext backend_eglCreateContext
#define eglCreatePbufferSurface backend_eglCreatePbufferSurface
#define eglDestroyContext backend_eglDestroyContext
#define eglDestroySurface backend_eglDestroySurface
#define eglMakeCurrent backend_eglMakeCurrent
#define glCreateShader backend_glCreateShader
#define glShaderSource backend_glShaderSource
#define glCompileShader backend_glCompileShader
#define glGetShaderiv backend_glGetShaderiv
#define glDeleteShader backend_glDeleteShader
#define glCreateProgram backend_glCreateProgram
#define glAttachShader backend_glAttachShader
#define glBindAttribLocation backend_glBindAttribLocation
#define glLinkProgram backend_glLinkProgram
#define glGetProgramiv backend_glGetProgramiv
#define glUseProgram backend_glUseProgram
#define glDeleteProgram backend_glDeleteProgram
#define glGenBuffers backend_glGenBuffers
#define glBindBuffer backend_glBindBuffer
#define glBufferData backend_glBufferData
#define glBufferSubData backend_glBufferSubData
#define glDeleteBuffers backend_glDeleteBuffers
#define glGetIntegerv backend_glGetIntegerv
#define glVertexAttribPointer backend_glVertexAttribPointer
#define glVertexAttrib1f backend_glVertexAttrib1f
#define glEnableVertexAttribArray backend_glEnableVertexAttribArray
#define glDisableVertexAttribArray backend_glDisableVertexAttribArray
#define glViewport backend_glViewport
#define glClearColor backend_glClearColor
#define glClear backend_glClear
#define glDrawArrays backend_glDrawArrays
#define glFinish backend_glFinish
#define glGetError backend_glGetError
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include "../linux-artifacts/panfrost-renderer/gles_dispatch.h"
#include "midix/abi/protocols/graphics_stream.h"
static unsigned next_name = 1, buffer_bound, draws, presents;
static std::vector<unsigned char> buffer_bytes;
static std::string shader_text;
static GLfloat angle;
static unsigned context_creates, retire_calls;
static bool fail_retire;
static int retire_display() {
    ++retire_calls;
    if (fail_retire) {
        fail_retire = false;
        return -1;
    }
    return 0;
}
unsigned backend_context_creates() {
    return context_creates;
}
unsigned backend_retire_calls() {
    return retire_calls;
}
void backend_fail_next_retire() {
    fail_retire = true;
}
extern "C" {
EGLContext backend_eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*) {
    ++context_creates;
    return reinterpret_cast<EGLContext>(2);
}
EGLSurface backend_eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*) {
    return reinterpret_cast<EGLSurface>(2);
}
EGLBoolean backend_eglDestroyContext(EGLDisplay, EGLContext) {
    return EGL_TRUE;
}
EGLBoolean backend_eglDestroySurface(EGLDisplay, EGLSurface) {
    return EGL_TRUE;
}
EGLBoolean backend_eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext) {
    return EGL_TRUE;
}
GLuint backend_glCreateShader(GLenum type) {
    assert(type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER);
    return next_name++;
}
void backend_glShaderSource(GLuint, GLsizei n, const char* const* source, const GLint* length) {
    assert(n == 1);
    shader_text.assign(*source, *length);
}
void backend_glCompileShader(GLuint) {
    assert(shader_text.find("void main") != std::string::npos);
}
void backend_glGetShaderiv(GLuint, GLenum p, GLint* v) {
    assert(p == GL_COMPILE_STATUS);
    *v = 1;
}
void backend_glDeleteShader(GLuint) {}
GLuint backend_glCreateProgram() {
    return next_name++;
}
void backend_glAttachShader(GLuint, GLuint) {}
void backend_glBindAttribLocation(GLuint, GLuint, const char* name) {
    assert(!strcmp(name, "position"));
}
void backend_glLinkProgram(GLuint) {}
void backend_glGetProgramiv(GLuint, GLenum p, GLint* v) {
    assert(p == GL_LINK_STATUS);
    *v = 1;
}
void backend_glUseProgram(GLuint name) {
    assert(!(name & 0x80000000U));
}
void backend_glDeleteProgram(GLuint name) {
    assert(!(name & 0x80000000U));
}
void backend_glGenBuffers(GLsizei n, GLuint* v) {
    assert(n == 1);
    *v = next_name++;
}
void backend_glBindBuffer(GLenum t, GLuint v) {
    assert(t == GL_ARRAY_BUFFER);
    buffer_bound = v;
}
void backend_glBufferData(GLenum t, GLsizeiptr n, const void* data, GLenum usage) {
    assert(t == GL_ARRAY_BUFFER && !data && usage == GL_STATIC_DRAW);
    buffer_bytes.assign(n, 0);
}
void backend_glBufferSubData(GLenum t, GLintptr off, GLsizeiptr n, const void* data) {
    assert(t == GL_ARRAY_BUFFER && off >= 0 && n >= 0 &&
           off + n <= static_cast<GLintptr>(buffer_bytes.size()));
    memcpy(buffer_bytes.data() + off, data, n);
}
void backend_glDeleteBuffers(GLsizei, const GLuint*) {}
void backend_glGetIntegerv(GLenum p, GLint* v) {
    assert(p == GL_ARRAY_BUFFER_BINDING);
    *v = buffer_bound;
}
void backend_glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {
    assert(buffer_bound);
}
void backend_glVertexAttrib1f(GLuint index, GLfloat value) {
    assert(index == 2);
    angle = value;
}
void backend_glEnableVertexAttribArray(GLuint) {}
void backend_glDisableVertexAttribArray(GLuint) {}
void backend_glViewport(GLint, GLint, GLsizei w, GLsizei h) {
    assert(w == 512 && h == 512);
}
void backend_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    assert(r == .25f && g == .5f && b == .75f && a == 1.f);
}
void backend_glClear(GLbitfield mask) {
    assert(mask == GL_COLOR_BUFFER_BIT);
}
void backend_glDrawArrays(GLenum mode, GLint first, GLsizei n) {
    assert(mode == GL_TRIANGLES && first == 0 && n == 3);
    ++draws;
}
void backend_glFinish() {}
GLenum backend_glGetError() {
    return GL_NO_ERROR;
}
}
int backend_submit(const void* bytes, unsigned size, unsigned flags, unsigned* value) {
    gles_target_retire = retire_display;
    int status = gles_execute(static_cast<const unsigned char*>(bytes), size, 0x830, value);
    if (!status && (flags & AME_GFX_PRESENT) && !(flags & AME_GFX_PRESENT_RESTORE)) {
        ++presents;
    }
    return status;
}
void backend_check(unsigned expected_draws) {
    assert(draws == expected_draws && presents == expected_draws);
}
void backend_check_angle(GLfloat expected) {
    assert(angle == expected);
}
void backend_check_upload(const void* bytes, unsigned size) {
    assert(buffer_bytes.size() == size && !memcmp(buffer_bytes.data(), bytes, size));
}
void backend_malformed_tests() {
    struct ame_gfx_stream s = {};
    struct ame_gfx_chunk c = {1, 0, 4, {1, 2, 3, 4}};
    assert(ame_gfx_stream_chunk(&s, &c));
    assert(!ame_gfx_stream_chunk(&s, &c)); // duplicate offset poisons batch
    struct ame_gfx_submit r = {1, AME_GFX_VERSION, AME_GFX_API_GLES2, 4, 0};
    assert(!ame_gfx_stream_submit(&s, &r));
    c.id = 2;
    c.offset = 0;
    assert(ame_gfx_stream_chunk(&s, &c));
    r.id = 2;
    r.bytes = 8;
    assert(!ame_gfx_stream_submit(&s, &r));
    c.id = 3;
    c.size = 33;
    assert(!ame_gfx_stream_chunk(&s, &c));
    c.id = 4;
    c.size = 4;
    assert(ame_gfx_stream_chunk(&s, &c));
    r.id = 4;
    r.bytes = 4;
    r.version = 99;
    assert(!ame_gfx_stream_submit(&s, &r));
    unsigned v = 0;
    unsigned create[] = {AME_GL_CREATE_PROGRAM_NAMED, 4, 0x80010000U};
    auto execute = [&](unsigned* words, unsigned bytes) {
        return gles_execute(reinterpret_cast<unsigned char*>(words), bytes, 0x830, &v);
    };
    assert(execute(create, sizeof(create)) == 0);
    assert(execute(create, sizeof(create)) == -1); // duplicate live name
    unsigned wrong_kind[] = {AME_GL_BIND_BUFFER, 8, GL_ARRAY_BUFFER, create[2]};
    assert(execute(wrong_kind, sizeof(wrong_kind)) == -1);
    unsigned raw_name[] = {AME_GL_USE_PROGRAM, 4, 1};
    assert(execute(raw_name, sizeof(raw_name)) == -1); // cannot guess Mesa names
    for (unsigned i = 1; i < 128; ++i) {
        create[2] = 0x80010000U + i;
        assert(execute(create, sizeof(create)) == 0);
    }
    create[2] = 0x80020000U;
    assert(execute(create, sizeof(create)) == -1); // bounded live objects
    unsigned remove[] = {AME_GL_DELETE_PROGRAM, 4, 0x80010000U};
    assert(execute(remove, sizeof(remove)) == 0);
    assert(execute(create, sizeof(create)) == 0); // retired slot reusable
    remove[2] = create[2];
    assert(execute(remove, sizeof(remove)) == 0);
    for (unsigned i = 1; i < 128; ++i) {
        remove[2] = 0x80010000U + i;
        assert(execute(remove, sizeof(remove)) == 0);
    }
    unsigned bad[] = {AME_GL_CLEAR, 4, GL_COLOR_BUFFER_BIT, 9999, 0};
    // A valid clear followed by an invalid opcode must not partially execute.
    assert(!gles_validate(reinterpret_cast<unsigned char*>(bad), sizeof(bad)));
    assert(gles_execute(reinterpret_cast<unsigned char*>(bad), sizeof(bad), 0x830, &v) == -1);
    for (unsigned n = 1; n < sizeof(bad); ++n) {
        if (n != 12) {
            assert(!gles_validate(reinterpret_cast<unsigned char*>(bad), n));
        }
    }
    unsigned scalar[] = {AME_GL_ATTRIB_1F, 8, 2, 0};
    assert(gles_validate(reinterpret_cast<unsigned char*>(scalar), sizeof(scalar)));
    scalar[1] = 4;
    assert(!gles_validate(reinterpret_cast<unsigned char*>(scalar), sizeof(scalar)));
    scalar[1] = 12;
    assert(!gles_validate(reinterpret_cast<unsigned char*>(scalar), sizeof(scalar)));
    unsigned pointer[] = {AME_GL_ATTRIB_POINTER, 24, 0, 2, GL_FLOAT, 0, 8, 0};
    buffer_bound = 0;
    assert(gles_execute(reinterpret_cast<unsigned char*>(pointer), sizeof(pointer), 0x830, &v) ==
           -1);
}
