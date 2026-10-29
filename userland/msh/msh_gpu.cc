#include "msh.h"
#include "ame/graphics.h"
#include "ame/egl.h"
#include <GLES2/gl2.h>
#include "libk/printf.h"
#include "../../linux-artifacts/panvk-renderer/renderer.h"
#include "midix/abi/protocols/gpu_diag.h"
#include "midix/vulkan/cts.h"
#include "../../linux-artifacts/panvk-renderer/suite.h"
extern "C" void ame_native_vk_set_logger(void (*logger)(const char*,unsigned));

namespace {
GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    // Link status checks both compilations in one reply after the whole setup
    // batch. No per-shader compile-status round trip is needed here.
    return shader;
}
} // namespace
namespace {
struct TriangleSession {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    GLuint program, buffer;
    bool ready;
} session{};
static constexpr GLfloat vertices[] = {-.69282f, -.4f, 1, 0,   0, .69282f, -.4f, 0,
                                       1,        0,    0, .8f, 0, 0,       1};
bool upload_geometry(bool large) {
    glBindBuffer(GL_ARRAY_BUFFER, session.buffer);
    unsigned offset = 0;
    if (large) {
        // Exceed both command slots and exercise reuse under backpressure.
        // Place the vertices
        // at the end so a truncated transfer cannot still draw the triangle.
        static unsigned char bytes[49152];
        for (unsigned i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = static_cast<unsigned char>(i * 7);
        }
        offset = sizeof(bytes) - sizeof(vertices);
        const auto* source = reinterpret_cast<const unsigned char*>(vertices);
        for (unsigned i = 0; i < sizeof(vertices); ++i) {
            bytes[offset + i] = source[i];
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof(bytes), bytes, GL_STATIC_DRAW);
    } else {
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    }
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                          reinterpret_cast<void*>(static_cast<uintptr_t>(offset)));
    glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
        reinterpret_cast<void*>(static_cast<uintptr_t>(offset + 2 * sizeof(GLfloat))));
    return glGetError() == GL_NO_ERROR;
}
void release_session() {
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        if (session.buffer) {
            glDeleteBuffers(1, &session.buffer);
        }
        glUseProgram(0);
        if (session.program) {
            glDeleteProgram(session.program);
        }
        (void)eglMakeCurrent(session.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (session.surface) {
        (void)eglDestroySurface(session.display, session.surface);
    }
    if (session.context) {
        (void)eglDestroyContext(session.display, session.context);
    }
    if (session.display) {
        (void)eglTerminate(session.display);
    }
    session = {};
}
bool initialize_session() {
    auto& display = session.display;
    auto& context = session.context;
    auto& surface = session.surface;
    auto& program = session.program;
    auto& buffer = session.buffer;
    session.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, nullptr, nullptr)) {
        putln("GPU: EGL initialization failed");
        return false;
    }
    const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                                   EGL_OPENGL_ES2_BIT, EGL_NONE};
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attrs, &config, 1, &count) || !count) {
        return false;
    }
    session.context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    session.surface = eglCreateWindowSurface(display, config, AME_EGL_DIAGNOSTIC_WINDOW, nullptr);
    if (!context || !surface || !eglMakeCurrent(display, surface, surface, context)) {
        putln("GPU: EGL context/surface creation failed");
        return false;
    }
    GLuint vs = compile(GL_VERTEX_SHADER,
                        "attribute vec2 position; attribute vec3 color; attribute float angle;"
                        "varying vec3 rgb; void main(){"
                        "float cx=cos(angle*0.7),sx=sin(angle*0.7);"
                        "float cy=cos(angle),sy=sin(angle);"
                        "vec3 p=vec3(position.x,position.y*cx,position.y*sx);"
                        "p=vec3(cy*p.x+sy*p.z,p.y,-sy*p.x+cy*p.z);"
                        "float d=2.5-p.z;"
                        "gl_Position=vec4(2.5*p.xy,1.020202*d-0.2020202,d);"
                        "rgb=color;}");
    GLuint fs = compile(GL_FRAGMENT_SHADER, "precision mediump float; varying vec3 rgb;"
                                            "void main(){gl_FragColor=vec4(rgb,1.0);}");
    session.program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "color");
    glBindAttribLocation(program, 2, "angle");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glUseProgram(program);
    glGenBuffers(1, &buffer);
    // Immutable geometry: changing the angle never allocates/uploads a VBO.
    if (!upload_geometry(false)) {
        return false;
    }
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glVertexAttrib1f(2, 0);
    if (!vs || !fs || !program || !linked || glGetError() != GL_NO_ERROR) {
        return false;
    }
    session.ready = true;
    return true;
}
} // namespace
int cmd_diag_gpu(bool reset, bool upload) {
    if (reset) {
        release_session();
        putln("GPU: cached context and resources released");
        return 0;
    }
    if (!ame::graphics::connect(g.boot)) {
        putln("GPU renderer unavailable in this image");
        return 1;
    }
    const bool warm = session.ready;
    const auto setup_start = mx_clock_ns();
    if (!session.ready && !initialize_session()) {
        release_session();
        return 1;
    }
    if (upload && !upload_geometry(true)) {
        release_session();
        putln("GPU: large upload failed");
        return 1;
    }
    const auto setup_ns = mx_clock_ns() - setup_start;
    if (upload) {
        putln("GPU: 49152-byte VBO uploaded; drawing vertices from its final chunk");
    }
    putln(warm ? "GPU: cached native EGL/GLES 3D rotating triangle (5 seconds)"
               : "GPU: native EGL/GLES 3D rotating triangle (5 seconds)");
    glViewport(0, 0, 512, 512);
    glClearColor(.02f, .03f, .06f, 1);
    const auto start = mx_clock_ns();
    const auto end = start + 5000000000ull;
    auto next_frame = start;
    mx_u64_t submit_ns = 0, render_ns = 0, max_submit_ns = 0;
    unsigned frames = 0;
    do {
        const auto now = mx_clock_ns();
        // Y turns every 2.5 seconds; X turns at 70% of that rate.
        const GLfloat angle = static_cast<GLfloat>(now - start) * (6.2831853f / 2500000000.0f);
        glVertexAttrib1f(2, angle);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        const auto submitted = mx_clock_ns();
        if (!ameEglSwapDiagnosticFrame(session.display, session.surface, EGL_FALSE)) {
            const auto failed = ame::graphics::completion();
            char failure[160];
            const int length = libk::snprintf(
                failure, sizeof(failure),
                "GPU: native EGL/GLES render failed: EGL=%x status=%d batch=%llu\r\n",
                eglGetError(), failed.status, failed.id);
            if (length > 0 && static_cast<unsigned>(length) < sizeof(failure)) {
                put_bytes(failure, length);
            }
            release_session();
            return 1;
        }
        const auto rendered = mx_clock_ns();
        const auto latency = rendered - submitted;
        submit_ns += latency;
        render_ns += ame::graphics::completion().render_ns;
        if (latency > max_submit_ns) {
            max_submit_ns = latency;
        }
        ++frames;
        // Direct scanout already waits for the next panel boundary. An
        // additional software deadline can miss that boundary on the next
        // draw. Pace only the CPU presentation path, without consuming input.
        if (ame::graphics::completion().scanout_count) {
            continue;
        }
        next_frame += 1000000000ull / 60;
        if (next_frame < rendered) {
            next_frame = rendered;
        }
        const auto deadline = next_frame < end ? next_frame : end;
        while (mx_clock_ns() < deadline) {
            const auto wake = g.term.endpoint.rx_doorbell;
            const auto expected = mx_doorbell_read(wake);
            if (mx_doorbell_wait_until(wake, expected, deadline) != MX_OK) {
                (void)ameEglSwapDiagnosticFrame(session.display, session.surface, EGL_TRUE);
                release_session();
                putln("GPU: animation wait failed");
                return 1;
            }
        }
    } while (mx_clock_ns() < end);
    if (!ameEglSwapDiagnosticFrame(session.display, session.surface, EGL_TRUE)) {
        release_session();
        putln("GPU: console restoration failed");
        return 1;
    }
    const auto frame = ame::graphics::completion();
    char line[256];
    int length = libk::snprintf(
        line, sizeof(line),
        "GPU: Mali-T830 frames=%u avg_submit=%llu us max_submit=%llu us avg_render=%llu us "
        "setup=%llu us scanout_flips=%u checksum=%x; display restored\r\n",
        frames, static_cast<unsigned long long>(submit_ns / frames / 1000),
        static_cast<unsigned long long>(max_submit_ns / 1000),
        static_cast<unsigned long long>(render_ns / frames / 1000),
        static_cast<unsigned long long>(setup_ns / 1000), frame.scanout_count, frame.checksum);
    if (length > 0 && static_cast<unsigned>(length) < sizeof(line)) {
        put_bytes(line, length);
    }
    return 0;
}

int cmd_diag_gpu_vulkan() {
    release_session();
    if(!ame::graphics::connect(g.boot)) {putln("GPU: Vulkan renderer unavailable");return 1;}
    putln("GPU: native Vulkan frontend / PanVK T830 triangle (5 seconds)");
    ame_native_vk_set_logger(put_bytes);
    if(panvk_suite_run(nullptr,0)) {putln("GPU: native Vulkan pixel suite failed");return 1;}
    panvk_triangle* triangle=panvk_triangle_create(nullptr,1);
    if(!triangle) {putln("GPU: native Vulkan resource creation failed");return 1;}
    static uint32_t pixels[AME_GPU_DIAG_WIDTH*AME_GPU_DIAG_HEIGHT];
    uint32_t checksum=0;
    bool ok=panvk_triangle_frame(triangle,0,pixels,&checksum)==0 && panvk_triangle_verify_pixels(pixels);
    char line[192];
    if(ok) {
        int n=libk::snprintf(line,sizeof(line),"panvk: T830 static triangle pixels verified checksum=%08x\r\n",checksum);
        if(n>0&&static_cast<unsigned>(n)<sizeof(line))put_bytes(line,n);
        ok=panvk_triangle_verify_load(triangle,pixels,&checksum)==0;
        if(ok) {
            n=libk::snprintf(line,sizeof(line),"panvk: T830 LOAD preservation verified checksum=%08x\r\n",checksum);
            if(n>0&&static_cast<unsigned>(n)<sizeof(line))put_bytes(line,n);
        }
    }
    auto start=mx_clock_ns();
    unsigned frames=0;
    while(ok && (!frames || mx_clock_ns()-start<5000000000ull)) {
        const float angle=frames ? static_cast<float>(mx_clock_ns()-start)*(6.2831853f/2500000000.0f) : 0;
        ok=panvk_triangle_present_frame(triangle,angle,&checksum)==0;
        if(ok){if(!frames)start=mx_clock_ns();++frames;}
    }
    const bool restored=frames && panvk_triangle_present_finish()==0;
    panvk_triangle_destroy(triangle);
    const int length=libk::snprintf(line,sizeof(line),
        "GPU: Vulkan status=%d frames=%u checksum=%x display_restored=%u\r\n",
        ok ? 0 : -1,frames,checksum,restored ? 1u : 0u);
    if(length>0&&static_cast<unsigned>(length)<sizeof(line))put_bytes(line,length);
    return ok && frames && restored ? 0 : 1;
}

int cmd_diag_gpu_cts(const char* pattern,bool log_only) {
    using namespace ame::vk;
    release_session();
    if(!ame::graphics::connect(g.boot))return 1;
    alignas(16) static unsigned char packet[AME_VK_WIRE_BYTES];
    auto* request=reinterpret_cast<CTSRequest*>(packet);
    *request={};
    auto exchange=[&](uint32_t op,unsigned bytes) {
        request->header={AME_VK_WIRE_VERSION,op,bytes,0,0,0};
        ame_gfx_reply reply{};
        return ame::graphics::exchange(packet,bytes,AME_GFX_API_VULKAN,0,reply) && request->header.result==0;
    };
    if(log_only) {
        for(unsigned offset=0;;) {
            request->log_offset=offset;request->log_bytes=4096;
            if(!exchange(CTSLog,sizeof(*request)+4096)) {putln("GPU: CTS log unavailable");return 1;}
            if(!request->log_bytes)break;
            if(request->log_bytes>4096 || offset>16*1024*1024) return 1;
            put_bytes(reinterpret_cast<const char*>(request+1),request->log_bytes);
            offset+=request->log_bytes;
        }
        putln("GPU: CTS log end");return 0;
    }
    unsigned length=0;
    while(pattern[length] && length+1<sizeof(request->pattern)) {
        request->pattern[length]=pattern[length];++length;
    }
    if(pattern[length])return 1;
    if(!exchange(CTSStart,sizeof(*request))) {putln("GPU: CTS unavailable or already running");return 1;}
    const auto started=mx_clock_ns();
    do {
        const auto until=mx_clock_ns()+100000000ull;
        const auto wake=g.term.endpoint.rx_doorbell;
        (void)mx_doorbell_wait_until(wake,mx_doorbell_read(wake),until);
        if(!exchange(CTSPoll,sizeof(*request)))return 1;
        if(mx_clock_ns()-started>190000000000ull) {putln("GPU: CTS polling timeout");return 1;}
    } while(request->running);
    char line[128];
    int n=libk::snprintf(line,sizeof(line),"GPU: CTS exit=%u elapsed_ms=%llu\r\n",request->exit_status,(mx_clock_ns()-started)/1000000ull);
    if(n>0 && static_cast<unsigned>(n)<sizeof(line))put_bytes(line,n);
    return request->exit_status ? 1 : 0;
}
