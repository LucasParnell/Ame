#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <linux/dma-buf.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <xf86drm.h>
#include <panfrost_drm.h>
#include "midix/abi/dango_channel_guest.h"
#include "midix/abi/dango_shm_guest.h"
#include <drm/drm_fourcc.h>
#include "midix/abi/protocols/gpu_diag.h"
#include "midix/abi/protocols/graphics_stream.h"
#include "gles_dispatch.h"
#ifdef AME_ENABLE_PANVK
#include "vulkan_rpc.h"
#endif

static const char* volatile render_phase = "startup";
struct direct_target {
    EGLImageKHR image;
    GLuint texture, fbo;
    int dma_fd;
};
static struct direct_target direct_targets[AME_GPU_DIAG_TARGETS];
static unsigned direct_active, direct_last;
static int direct_has_last;
static int direct_attached;
static int direct_tried;

static void fatal_signal(int signal, siginfo_t* info, void* context) {
    const ucontext_t* registers = context;
    const uintptr_t pc = registers->uc_mcontext.pc;
    Dl_info module = {0};
    (void)dladdr((void*)pc, &module);
    char line[512];
    int length = snprintf(
        line, sizeof(line),
        "panfrost: signal=%d phase=%s address=%p pc=%lx lr=%lx module=%s offset=%lx\n", signal,
        render_phase, info->si_addr, pc, (uintptr_t)registers->uc_mcontext.regs[30],
        module.dli_fname ? module.dli_fname : "unknown", pc - (uintptr_t)module.dli_fbase);
    if (length > 0) {
        (void)write(STDERR_FILENO, line,
                    (size_t)length < sizeof(line) ? (size_t)length : sizeof(line));
    }
    // PID 1 must stay alive so the native host can collect the failure.
    for (;;) {
        pause();
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#include "shader_cache.h"
#include "display_sync.h"
#include "vulkan_diag.h"

static GLuint shader(GLenum type, const char* source) {
    render_phase = type == GL_VERTEX_SHADER ? "vertex create" : "fragment create";
    GLuint s = glCreateShader(type);
    render_phase = type == GL_VERTEX_SHADER ? "vertex source" : "fragment source";
    glShaderSource(s, 1, &source, NULL);
    render_phase = type == GL_VERTEX_SHADER ? "vertex compile" : "fragment compile";
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "panfrost: shader compilation failed: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint initialize_gles(void) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay display = platform_display ? platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                                             EGL_DEFAULT_DISPLAY, NULL)
                                          : EGL_NO_DISPLAY;
    EGLint major, minor;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        return 0;
    }
    shader_cache_install(display);
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        return 0;
    }
    const EGLint config_attribs[] = {EGL_SURFACE_TYPE,
                                     EGL_PBUFFER_BIT,
                                     EGL_RENDERABLE_TYPE,
                                     EGL_OPENGL_ES2_BIT,
                                     EGL_RED_SIZE,
                                     8,
                                     EGL_GREEN_SIZE,
                                     8,
                                     EGL_BLUE_SIZE,
                                     8,
                                     EGL_ALPHA_SIZE,
                                     8,
                                     EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attribs, &config, 1, &count) || count != 1) {
        return 0;
    }
    const EGLint surface_attribs[] = {EGL_WIDTH, AME_GPU_DIAG_WIDTH, EGL_HEIGHT,
                                      AME_GPU_DIAG_HEIGHT, EGL_NONE};
    native_display = display;
    native_config = config;
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attribs);
    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) {
        return 0;
    }
    diagnostic_context = context;
    diagnostic_surface = surface;
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    printf("panfrost: GL_RENDERER=%s EGL=%d.%d\n", renderer ? renderer : "(null)", major, minor);
    // This appliance ships only the Panfrost hardware driver. Never accept a
    // software fallback or report a different GPU as a successful T830 test.
    if (!renderer || !strstr(renderer, "Mali-T830")) {
        return 0;
    }
    GLuint vs =
        shader(GL_VERTEX_SHADER, "attribute vec2 position; attribute vec3 color; varying vec3 rgb;"
                                 "void main(){gl_Position=vec4(position,0.0,1.0);rgb=color;}");
    GLuint fs = shader(GL_FRAGMENT_SHADER, "precision mediump float; varying vec3 rgb;"
                                           "void main(){gl_FragColor=vec4(rgb,1.0);}");
    if (!vs || !fs) {
        return 0;
    }
    GLuint program = glCreateProgram();
    render_phase = "shader link";
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "color");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!linked) {
        return 0;
    }
    glUseProgram(program);
    static const GLfloat vertices[] = {-.85f, -.75f, 1, 0,    0, .85f, -.75f, 0,
                                       1,     0,     0, .85f, 0, 0,    1};
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                          (void*)(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    return program;
}

// Direct presentation: import the SharedGpu pixel arena as an EGLImage and use
// it as the render target. Panfrost writes the shared pixels directly;
// the native framebuffer service still copies them into the panel buffer.
//
// Returns 1 when a direct target is installed and the caller must skip the
// readback, 0 when the caller must fall back to the pbuffer + readback path.
static int direct_target_attach(unsigned index, unsigned width, unsigned height) {
    int shm = open(DANGO_DEV_SHM, O_RDWR | O_CLOEXEC);
    if (shm < 0) {
        printf("panfrost direct: shm-open errno=%d\n", errno);
        return 0;
    }
    struct dango_shm_query query;
    memset(&query, 0, sizeof(query));
    query.index = index;
    if (ioctl(shm, DANGO_SHM_QUERY, &query) < 0 || !query.count) {
        printf("panfrost direct: query errno=%d\n", errno);
        close(shm);
        return 0;
    }
    if (query.region.flags & DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU) {
        const unsigned long long need = (unsigned long long)width * height * 4ULL;
        if (query.region.size < need) {
            printf("panfrost direct: region too small (%llu < %llu)\n",
                   (unsigned long long)query.region.size, need);
            close(shm);
            return 0;
        }
        struct dango_shm_export export_req;
        memset(&export_req, 0, sizeof(export_req));
        export_req.id = query.region.id;
        export_req.boot_generation = query.boot_generation;
        if (ioctl(shm, DANGO_SHM_EXPORT_DMABUF, &export_req) < 0) {
            printf("panfrost direct: export errno=%d\n", errno);
            close(shm);
            return 0;
        }
        // Declared locally: the vendored EGL/GLES headers do not pull in the
        // OES typedefs, and these two entry points are all this path needs.
        typedef EGLImageKHR (*CreateImageFn)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer,
                                             const EGLint*);
        typedef void (*TargetTextureFn)(GLenum, void*);
        CreateImageFn create_image = (CreateImageFn)eglGetProcAddress("eglCreateImageKHR");
        TargetTextureFn target_texture =
            (TargetTextureFn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (create_image && target_texture) {
            const EGLint attrs[] = {EGL_WIDTH,
                                    (EGLint)width,
                                    EGL_HEIGHT,
                                    (EGLint)height,
                                    EGL_LINUX_DRM_FOURCC_EXT,
                                    (EGLint)DRM_FORMAT_ARGB8888,
                                    EGL_DMA_BUF_PLANE0_FD_EXT,
                                    export_req.fd,
                                    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                    0,
                                    EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                    (EGLint)(width * 4),
                                    EGL_NONE};
            EGLImageKHR image =
                create_image(native_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
            if (image != EGL_NO_IMAGE_KHR) {
                GLuint texture = 0, fbo = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                target_texture(GL_TEXTURE_2D, (void*)image);
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture,
                                       0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                    direct_targets[index] =
                        (struct direct_target){image, texture, fbo, export_req.fd};
                    printf("panfrost direct: shared target attached %ux%u\n", width, height);
                    close(shm);
                    return 1;
                }
                printf("panfrost direct: framebuffer incomplete\n");
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &texture);
                PFNEGLDESTROYIMAGEKHRPROC destroy_image =
                    (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
                if (destroy_image) {
                    destroy_image(native_display, image);
                }
            } else {
                printf("panfrost direct: eglCreateImage errno=0x%x\n", (unsigned)eglGetError());
            }
        } else {
            printf("panfrost direct: EGLImage entry points unavailable\n");
        }
        close(export_req.fd);
    }
    close(shm);
    return 0;
}

static void direct_release(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    for (unsigned i = 0; i < AME_GPU_DIAG_TARGETS; ++i) {
        struct direct_target* target = &direct_targets[i];
        if (!target->fbo) {
            continue;
        }
        glDeleteFramebuffers(1, &target->fbo);
        glDeleteTextures(1, &target->texture);
        if (destroy_image) {
            destroy_image(native_display, target->image);
        }
        close(target->dma_fd);
        memset(target, 0, sizeof(*target));
    }
    direct_attached = direct_tried = direct_has_last = 0;
    direct_active = direct_last = 0;
}
static void direct_bind(void) {
    if (!direct_tried) {
        int ready = 1;
        for (unsigned i = 0; i < AME_GPU_DIAG_TARGETS; ++i) {
            if (!direct_target_attach(i, AME_GPU_DIAG_WIDTH, AME_GPU_DIAG_HEIGHT)) {
                ready = 0;
                break;
            }
        }
        if (!ready) {
            direct_release();
        }
        direct_tried = 1;
        direct_attached = ready;
    }
    if (direct_attached) {
        glBindFramebuffer(GL_FRAMEBUFFER, direct_targets[direct_active].fbo);
    }
}
// A completed GPU render is synchronized before the guest and native display
// read the imported pages. This avoids glReadPixels and channel conversion;
// DSS can scan the same pages after the manager's presentation handshake.
static int direct_present_checksum(unsigned slot, uint32_t* pixels, uint32_t* checksum,
                                   int hash_frame) {
    struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (ioctl(direct_targets[slot].dma_fd, DMA_BUF_IOCTL_SYNC, &sync)) {
        return -1;
    }
    // Streaming needs coherency for display DMA or the CPU fallback. Hash once
    // on explicit restore (or a static diagnostic), outside the frame cadence.
    if (hash_frame) {
        uint32_t hash = 2166136261U;
        for (unsigned i = 0; i < AME_GPU_DIAG_WIDTH * AME_GPU_DIAG_HEIGHT; ++i) {
            hash = (hash ^ (pixels[i] | 0xff000000U)) * 16777619U;
        }
        *checksum = hash;
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    return ioctl(direct_targets[slot].dma_fd, DMA_BUF_IOCTL_SYNC, &sync) ? -1 : 0;
}

static int draw_frame(void) {
    glViewport(0, 0, AME_GPU_DIAG_WIDTH, AME_GPU_DIAG_HEIGHT);
    glClearColor(.02f, .03f, .06f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return 0;
}

static void report_memory(void) {
    const char* paths[] = {"/proc/meminfo", "/proc/self/status"};
    for (unsigned i = 0; i < 2; ++i) {
        FILE* file = fopen(paths[i], "r");
        if (!file) {
            continue;
        }
        char line[160];
        while (fgets(line, sizeof(line), file)) {
            if (!strncmp(line, "MemTotal:", 9) || !strncmp(line, "MemAvailable:", 13) ||
                !strncmp(line, "VmRSS:", 6) || !strncmp(line, "VmPeak:", 7)) {
                printf("panfrost memory: %s", line);
            }
        }
        fclose(file);
    }
}
static int read_frame(uint32_t* pixels, uint32_t* checksum, int triangle, int hash_frame) {
    const uint64_t start = now_ns();
    // Readback is also the fence: no frame is exposed to Ame before the GPU
    // work has completed. It writes directly into the shared CPU-only arena.
    glReadPixels(0, 0, AME_GPU_DIAG_WIDTH, AME_GPU_DIAG_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (glGetError() != GL_NO_ERROR) {
        return -1;
    }
    const uint64_t readback_done = now_ns();
    uint32_t hash = 2166136261U;
    for (uint32_t i = 0; i < AME_GPU_DIAG_WIDTH * AME_GPU_DIAG_HEIGHT; ++i) {
        uint32_t p = pixels[i];
        p = 0xff000000U | ((p & 255) << 16) | (p & 0xff00) | ((p >> 16) & 255);
        pixels[i] = p;
        if (hash_frame) {
            hash = (hash ^ p) * 16777619U;
        }
    }
    // A completed submission alone is insufficient: reject an unchanged or
    // cleared buffer by checking interpolated color at three interior points.
    const uint32_t r = pixels[100 * AME_GPU_DIAG_WIDTH + 100];
    const uint32_t g = pixels[100 * AME_GPU_DIAG_WIDTH + 410];
    const uint32_t b = pixels[380 * AME_GPU_DIAG_WIDTH + 256];
    if (triangle && (((r >> 16) & 255) <= ((r >> 8) & 255) ||
                     ((g >> 8) & 255) <= ((g >> 16) & 255) || (b & 255) <= ((b >> 16) & 255))) {
        return -1;
    }
    *checksum = hash_frame ? hash : 0;
    if (hash_frame) {
        printf("panfrost frame: readback_us=%llu conversion_us=%llu\n",
               (unsigned long long)((readback_done - start) / 1000),
               (unsigned long long)((now_ns() - readback_done) / 1000));
    }
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    struct sigaction action = {.sa_sigaction = fatal_signal, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    (void)mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    (void)mount("proc", "/proc", "proc", 0, NULL);
    (void)mount("sysfs", "/sys", "sysfs", 0, NULL);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "panfrost", 1);
    setenv("LIBGL_DRIVERS_PATH", "/usr/lib/dri", 1);
    // Disable Mesa's Linux disk backend. EGL blob callbacks are independently
    // enabled and persist through the native manager's libmochi FS session.
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    setenv("EGL_PLATFORM", "surfaceless", 1);
    int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct drm_panfrost_get_param parameter = {.param = DRM_PANFROST_PARAM_GPU_PROD_ID};
    uint32_t gpu_id = 0;
    if (drm >= 0 && !ioctl(drm, DRM_IOCTL_PANFROST_GET_PARAM, &parameter)) {
        gpu_id = parameter.value;
    }
    if (drm >= 0) {
        close(drm); // only one GPU address space is advertised
    }
    GLuint program = gpu_id == 0x830 ? initialize_gles() : 0;
    if (argc == 2 && !strcmp(argv[1], "--compile-only")) {
        printf("panfrost: shader compiler check %s\n", program ? "passed" : "FAILED");
        return program ? 0 : 1;
    }
    printf("panfrost: hardware renderer %s gpu_id=%x\n", program ? "ready" : "FAILED", gpu_id);
    gles_target_bind = direct_bind;
    gles_target_release = direct_release;
    report_memory();
    int fd = open(DANGO_DEV_CHANNEL, O_RDWR | O_CLOEXEC);
    struct dango_channel_layout layout;
    if (fd < 0 || ioctl(fd, DANGO_CH_QUERY, &layout) || layout.desc.magic != DANGO_CH_MAGIC ||
        layout.desc.ring_order != DANGO_CH_DEFAULT_RING_ORDER ||
        layout.desc.shared_staging_size !=
            AME_GPU_DIAG_SHARED_BYTES + AME_GFX_COMMAND_BYTES + AME_GFX_CACHE_TRANSFER) {
        goto failed;
    }
    size_t offset = DANGO_CH_STAGING_OFF(layout.desc.ring_order);
    void* mapping = mmap(
        NULL, offset + AME_GPU_DIAG_SHARED_BYTES + AME_GFX_COMMAND_BYTES + AME_GFX_CACHE_TRANSFER,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        goto failed;
    }
    struct dango_ring_hdr* rx = mapping;
    struct dango_ring_hdr* tx = (void*)((char*)mapping + layout.ring_bytes);
    uint32_t* pixels = (void*)((char*)mapping + offset);
    struct ame_gfx_stream stream = {0};
    struct shader_cache_channel cache_channel = {
        .fd = fd,
        .rx = rx,
        .tx = tx,
        .transfer = (unsigned char*)pixels + AME_GPU_DIAG_SHARED_BYTES + AME_GFX_COMMAND_BYTES};
    display_channel = &cache_channel;
    gles_target_retire = display_retire;
    const uint64_t mask = DANGO_RING_SLOTS(DANGO_CH_DEFAULT_RING_ORDER) - 1;
    for (;;) {
        uint64_t tail = __atomic_load_n(&rx->tail, __ATOMIC_RELAXED);
        if (tail == __atomic_load_n(&rx->head, __ATOMIC_ACQUIRE)) {
            struct pollfd wait = {.fd = fd, .events = POLLIN};
            if (poll(&wait, 1, -1) < 0 && errno != EINTR) {
                goto failed;
            }
            continue;
        }
        struct dango_ring_slot slot = *((struct dango_ring_slot*)(rx + 1) + (tail & mask));
        __atomic_store_n(&rx->tail, tail + 1, __ATOMIC_RELEASE);
        if (slot.type == AME_GFX_CHUNK) {
            struct ame_gfx_chunk chunk;
            if (slot.length == sizeof(chunk)) {
                memcpy(&chunk, slot.payload, sizeof(chunk));
                (void)ame_gfx_stream_chunk(&stream, &chunk);
            } else {
                stream.valid = 0;
            }
            continue;
        }
        if ((slot.type == AME_GFX_SUBMIT && slot.length == sizeof(struct ame_gfx_submit)) ||
            (slot.type == AME_GFX_SHARED && slot.length == sizeof(struct ame_gfx_shared_submit))) {
            struct ame_gfx_submit request;
            unsigned response_slot=AME_GFX_COMMAND_SLOTS;
            memcpy(&request, slot.payload, sizeof(request));
            if (slot.type == AME_GFX_SHARED) {
                struct ame_gfx_shared_submit shared;
                memcpy(&shared, slot.payload, sizeof(shared));
                if (!shared.reserved && shared.slot < AME_GFX_COMMAND_SLOTS &&
                    request.bytes <= AME_GFX_BATCH_BYTES && request.id > stream.id) {
                    memcpy(stream.data,
                           (char*)pixels + AME_GPU_DIAG_SHARED_BYTES +
                               shared.slot * AME_GFX_BATCH_BYTES,
                           request.bytes);
                    response_slot=shared.slot;
                    stream.id = request.id;
                    stream.used = request.bytes;
                    stream.valid = 1;
                } else {
                    stream.valid = 0;
                }
            }
            struct ame_gfx_reply reply = {.id = request.id, .status = -1, .gpu_id = gpu_id};
            // Persist asynchronously compiled blobs between animations, rather
            // than adding filesystem/channel round trips to streaming frames.
            if (!(request.flags & AME_GFX_PRESENT_STREAM)) {
                shader_cache_checkpoint(&cache_channel);
            }
            uint64_t start = now_ns();
            const int valid_submit = ame_gfx_stream_submit(&stream, &request);
#ifdef AME_ENABLE_PANVK
            if(valid_submit && request.api==AME_GFX_API_VULKAN && response_slot<AME_GFX_COMMAND_SLOTS) {
                setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER","1",1);
                setenv("PANVK_MIDGARD_EXPERIMENTAL","1",1);
#ifdef AME_PANVK_SUITE_TRACE
                setenv("PANVK_DEBUG","startup,trace_fault",1);
                setenv("AME_VK_RPC_TRACE","1",1);
#else
                setenv("PANVK_DEBUG","startup",1);
#endif
                reply.status=vulkan_rpc_execute(&request,stream.data,pixels,&reply);
                memcpy((char*)pixels+AME_GPU_DIAG_SHARED_BYTES+response_slot*AME_GFX_BATCH_BYTES,
                    stream.data,request.bytes);
            }
#endif
            if (program && valid_submit && request.api == AME_GFX_API_GLES2) {
#ifdef AME_ENABLE_PANVK
                vulkan_in_use = 0;
#endif
                reply.status = gles_execute(stream.data, request.bytes, gpu_id, &reply.value);
                if (!reply.status && (request.flags & AME_GFX_PRESENT)) {
                    if (!native_bound) {
                        reply.status = -1;
                    } else if (direct_attached) {
                        const int restoring = request.flags & AME_GFX_PRESENT_RESTORE;
                        const unsigned slot =
                            restoring && direct_has_last ? direct_last : direct_active;
                        // The GPU already wrote shared pixels. Ordered wait, no
                        // readback: the checksum is computed from the same pages
                        // the host will display, so it still proves real pixels.
                        glFinish();
                        reply.status =
                            glGetError() == GL_NO_ERROR
                                ? direct_present_checksum(
                                      slot, pixels + slot * (AME_GPU_DIAG_BYTES / 4),
                                      &reply.checksum, !(request.flags & AME_GFX_PRESENT_STREAM))
                                : -1;
                        reply.present_target = slot;
                        if (!reply.status && !restoring) {
                            direct_last = slot;
                            direct_has_last = 1;
                            // The manager waits for display retirement before
                            // forwarding another batch. Only then can this
                            // previous front buffer receive new GPU writes.
                            direct_active = slot ^ 1;
                            glBindFramebuffer(GL_FRAMEBUFFER, direct_targets[direct_active].fbo);
                        }
                    } else {
                        // Native presentation requires two imported targets.
                        // A CPU readback into slot 0 could overwrite a live
                        // scanout and has no exporter cache-ownership bracket.
                        reply.status = -1;
                    }
                }
            }
            reply.render_ns = now_ns() - start;
            // Persist asynchronously compiled blobs between animations, rather
            // than adding filesystem/channel round trips to streaming frames.
            if (!(request.flags & AME_GFX_PRESENT_STREAM)) {
                shader_cache_checkpoint(&cache_channel);
            }
            if ((request.flags & AME_GFX_PRESENT) && !(request.flags & AME_GFX_PRESENT_STREAM)) {
                pthread_mutex_lock(&shader_cache_mutex);
                printf("panfrost cache: hits=%llu misses=%llu generation=%llu saved=%llu\n",
                       (unsigned long long)shader_blobs.hits,
                       (unsigned long long)shader_blobs.misses,
                       (unsigned long long)shader_blobs.generation,
                       (unsigned long long)shader_saved_generation);
                pthread_mutex_unlock(&shader_cache_mutex);
            }
            uint64_t head = __atomic_load_n(&tx->head, __ATOMIC_RELAXED);
            while (head - __atomic_load_n(&tx->tail, __ATOMIC_ACQUIRE) > mask) {
                usleep(1000);
            }
            struct dango_ring_slot* out = (struct dango_ring_slot*)(tx + 1) + (head & mask);
            *out = (struct dango_ring_slot){
                .type = AME_GFX_REPLY, .length = sizeof(reply), .seq = head};
            memcpy(out->payload, &reply, sizeof(reply));
            __atomic_store_n(&tx->head, head + 1, __ATOMIC_RELEASE);
            if (ioctl(fd, DANGO_CH_NOTIFY, 0)) {
                goto failed;
            }
            continue;
        }
        if (slot.type != AME_GPU_DIAG_TRIANGLE ||
            slot.length != sizeof(struct ame_gpu_diag_request)) {
            continue;
        }
        struct ame_gpu_diag_request request;
        memcpy(&request, slot.payload, sizeof(request));
        struct ame_gpu_diag_reply reply = {.id = request.id, .status = -1, .gpu_id = gpu_id};
        if (program && native_context == EGL_NO_CONTEXT &&
            eglMakeCurrent(native_display, diagnostic_surface, diagnostic_surface,
                           diagnostic_context) &&
            request.width == AME_GPU_DIAG_WIDTH && request.height == AME_GPU_DIAG_HEIGHT &&
            request.duration_ms == AME_GPU_DIAG_DURATION_MS && !request.reserved) {
            uint64_t start = now_ns();
            reply.status = draw_frame();
            if (!reply.status) {
                reply.status = read_frame(pixels, &reply.checksum, 1, 1);
            }
            reply.render_ns = now_ns() - start;
            if (!reply.status) {
                reply.flags = AME_GPU_DIAG_HARDWARE;
            }
            printf("panfrost: triangle status=%d gpu=%x render_us=%llu checksum=%x\n", reply.status,
                   gpu_id, reply.render_ns / 1000, reply.checksum);
        }
        uint64_t head = __atomic_load_n(&tx->head, __ATOMIC_RELAXED);
        while (head - __atomic_load_n(&tx->tail, __ATOMIC_ACQUIRE) > mask) {
            usleep(1000);
        }
        struct dango_ring_slot* out = (struct dango_ring_slot*)(tx + 1) + (head & mask);
        *out = (struct dango_ring_slot){
            .type = AME_GPU_DIAG_REPLY, .length = sizeof(reply), .seq = head};
        memcpy(out->payload, &reply, sizeof(reply));
        __atomic_store_n(&tx->head, head + 1, __ATOMIC_RELEASE);
        if (ioctl(fd, DANGO_CH_NOTIFY, 0)) {
            goto failed;
        }
    }
failed:
    perror("panfrost renderer");
    for (;;) {
        pause();
    }
}
