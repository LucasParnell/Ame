#include <EGL/egl.h>
#include "ame/egl.h"
#include <GLES2/gl2.h>
#include "ame/graphics.h"
#include "midix/abi/protocols/graphics_stream.h"
#include "midix/abi/protocols/graphics_cache.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
static_assert(
    [] {
        constexpr std::array types{AME_GFX_CHUNK,         AME_GFX_SHARED,
                                   AME_GFX_SUBMIT,        AME_GFX_REPLY,
                                   AME_GFX_CACHE_REQUEST, AME_GFX_CACHE_REPLY,
                                   AME_GFX_DISPLAY_DRAIN, AME_GFX_DISPLAY_DRAIN_REPLY};
        for (unsigned i = 0; i < types.size(); ++i) {
            for (unsigned j = i + 1; j < types.size(); ++j) {
                if (types[i] == types[j]) {
                    return false;
                }
            }
        }
        return true;
    }(),
    "graphics channel message types must be distinct");
int backend_submit(const void*, unsigned, unsigned, unsigned*);
void backend_check(unsigned);
void backend_check_angle(GLfloat);
void backend_check_upload(const void*, unsigned);
void backend_malformed_tests();
unsigned backend_context_creates();
unsigned backend_retire_calls();
void backend_fail_next_retire();
static bool fail_next;
static unsigned last_flags;
static unsigned submissions;
namespace ame::graphics {
mx_u32_t batch_limit() {
    return AME_GFX_BATCH_BYTES;
}
static ame_gfx_reply last;
static unsigned long long sequence;
bool connect(const MxBootInfo*, const char*, mx_u64_t) {
    return true;
}
const ame_gfx_reply& completion() {
    return last;
}
bool submit(const void* data, mx_u32_t size, mx_u32_t api, mx_u32_t flags, ame_gfx_reply& reply) {
    if (fail_next) {
        fail_next = false;
        return false;
    }
    assert(api == AME_GFX_API_GLES2);
    ++submissions;
    last_flags = flags;
    const auto* bytes = static_cast<const unsigned char*>(data);
    ame_gfx_stream stream{};
    const auto id = ++sequence;
    for (unsigned offset = 0; offset < size;) {
        ame_gfx_chunk chunk{id, offset, size - offset > 32 ? 32 : size - offset, {}};
        memcpy(chunk.data, bytes + offset, chunk.size);
        assert(ame_gfx_stream_chunk(&stream, &chunk));
        offset += chunk.size;
    }
    ame_gfx_submit request{id, AME_GFX_VERSION, api, size, flags};
    assert(ame_gfx_stream_submit(&stream, &request));
    assert(!size || !memcmp(stream.data, data, size));
    unsigned result = 0;
    reply = {id, backend_submit(stream.data, size, flags, &result), result, 0, 0, 0x830, 0, 0};
    last = reply;
    return !reply.status;
}
} // namespace ame::graphics
namespace ame::graphics {
static ame_gfx_reply queued[AME_GFX_COMMAND_SLOTS];
bool enqueue(const void* data, mx_u32_t size, mx_u32_t api, mx_u32_t flags, mx_u64_t& ticket) {
    ame_gfx_reply reply{};
    if (!submit(data, size, api, flags, reply)) {
        return false;
    }
    ticket = reply.id;
    queued[ticket % AME_GFX_COMMAND_SLOTS] = reply;
    return true;
}
bool wait(mx_u64_t ticket, ame_gfx_reply& reply) {
    reply = queued[ticket % AME_GFX_COMMAND_SLOTS];
    return reply.id == ticket && !reply.status;
}
bool wait_all() {
    return true;
}
} // namespace ame::graphics
int main() {
    assert(glGetError() == GL_INVALID_OPERATION); // no current context
    assert(!eglGetProcAddress("glMadeUp"));
    assert(eglGetProcAddress("glDrawArrays"));
    assert(eglGetProcAddress("ameEglSwapDiagnosticFrame"));
    assert(!eglBindAPI(EGL_OPENGL_API));
    assert(eglGetError() == EGL_BAD_PARAMETER);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(eglInitialize(d, nullptr, nullptr));
    EGLConfig config{};
    EGLint count = 0;
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    assert(eglChooseConfig(d, attrs, &config, 1, &count) && count == 1);
    for (unsigned iteration = 1; iteration <= 3; ++iteration) {
        const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        EGLContext c = eglCreateContext(d, config, EGL_NO_CONTEXT, ca);
        EGLSurface s =
            eglCreateWindowSurface(d, config, reinterpret_cast<EGLNativeWindowType>(1), nullptr);
        assert(c && s && eglMakeCurrent(d, s, s, c));
        assert(!eglCreateContext(d, config, EGL_NO_CONTEXT, ca));
        assert(eglGetError() == EGL_BAD_ALLOC);
        const auto before_create = submissions;
        assert(!glCreateShader(0) && glGetError() == GL_INVALID_ENUM);
        // Invalid enum is local; no synchronous query was sent.
        GLuint shader = glCreateShader(GL_VERTEX_SHADER);
        assert(shader & 0x80000000U);
        assert(submissions == before_create);
        const char* sources[] = {"void main", "(){gl_Position=vec4(0.0);}"};
        glShaderSource(shader, 2, sources, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        assert(ok);
        const auto before_program = submissions;
        GLuint p = glCreateProgram();
        assert(submissions == before_program);
        glAttachShader(p, shader);
        glBindAttribLocation(p, 0, "position");
        glLinkProgram(p);
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        assert(ok);
        glUseProgram(p);
        GLuint buffer = 0;
        const auto before_buffer = submissions;
        glGenBuffers(1, &buffer);
        assert(submissions == before_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        unsigned char upload[49152];
        for (unsigned i = 0; i < sizeof(upload); ++i) {
            upload[i] = i * 7;
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof(upload), upload, GL_STATIC_DRAW);
        glFinish();
        backend_check_upload(upload, sizeof(upload));
        glBufferData(GL_ARRAY_BUFFER, -1, nullptr, GL_STATIC_DRAW);
        assert(glGetError() == GL_INVALID_VALUE);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
        glEnableVertexAttribArray(0);
        glViewport(0, 0, 512, 512);
        glClearColor(.25, .5, .75, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glVertexAttrib1f(2, -0.5f * iteration);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (iteration == 2) {
            assert(ameEglSwapDiagnosticFrame(d, s, EGL_FALSE));
            assert(last_flags == (AME_GFX_PRESENT | AME_GFX_PRESENT_STREAM));
            assert(ameEglSwapDiagnosticFrame(d, s, EGL_TRUE));
            assert(last_flags == (AME_GFX_PRESENT | AME_GFX_PRESENT_RESTORE));
        } else {
            assert(eglSwapBuffers(d, s));
            assert(last_flags == AME_GFX_PRESENT);
        }
        backend_check(iteration);
        backend_check_angle(-0.5f * iteration);
        assert(glGetError() == GL_NO_ERROR);
        glDeleteBuffers(1, &buffer);
        glDeleteShader(shader);
        glDeleteProgram(p);
        assert(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        assert(eglDestroySurface(d, s));
        assert(eglDestroyContext(d, c));
    }
    // Validate hostile/truncated stream input independently of frontend checks.
    const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const auto prior_creates = backend_context_creates();
    assert(backend_retire_calls() == 6);
    backend_fail_next_retire();
    assert(!eglCreateContext(d, config, EGL_NO_CONTEXT, ca));
    assert(eglGetError() == EGL_BAD_ALLOC);
    assert(glGetError() == GL_INVALID_OPERATION);
    assert(backend_context_creates() == prior_creates); // no GPU writes before retirement
    assert(backend_retire_calls() == 7);
    auto c = eglCreateContext(d, config, EGL_NO_CONTEXT, ca);
    auto s = eglCreatePbufferSurface(d, config, nullptr);
    assert(eglMakeCurrent(d, s, s, c));
    assert(!ameEglSwapDiagnosticFrame(d, s, EGL_FALSE));
    assert(eglGetError() == EGL_BAD_SURFACE);
    for (unsigned flags = 0; flags < 16; ++flags) {
        ame_gfx_stream stream{};
        ame_gfx_submit request{1, AME_GFX_VERSION, AME_GFX_API_GLES2, 0, flags};
        assert(bool(ame_gfx_stream_submit(&stream, &request)) ==
               (flags == 0 || flags == 1 || flags == 3 || flags == 5));
    }
    backend_malformed_tests();
    fail_next = true;
    assert(glGetError() == GL_INVALID_OPERATION); // transport failure must surface
    puts("Native EGL/GLES encoder, decoder and lifecycle probe passed");
}
