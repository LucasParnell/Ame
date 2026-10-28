#pragma once
#include "midix/abi/protocols/graphics.h"
#include "midix/abi/protocols/gles2.h"

static EGLDisplay native_display;
static EGLConfig native_config;
static EGLContext native_context = EGL_NO_CONTEXT;
static EGLSurface native_surface = EGL_NO_SURFACE;
static int native_bound;
static void (*gles_target_bind)(void);
static void (*gles_target_release)(void);
static int (*gles_target_retire)(void);
static EGLContext diagnostic_context;
static EGLSurface diagnostic_surface;

enum { GLES_NAME_SHADER = 1, GLES_NAME_PROGRAM, GLES_NAME_BUFFER };
struct gles_name {
    unsigned client, driver, kind;
};
static struct gles_name gles_names[128];
static int gles_named_mode, gles_legacy_mode;
static unsigned gles_resolve(unsigned name, unsigned kind) {
    // Preserve the original create/query protocol for older clients.
    if (!(name & 0x80000000U)) {
        return gles_named_mode ? 0 : name;
    }
    for (unsigned i = 0; i < 128; ++i) {
        if (gles_names[i].client == name && gles_names[i].kind == kind) {
            return gles_names[i].driver;
        }
    }
    return 0;
}
static struct gles_name* gles_name_slot(unsigned name) {
    if (!(name & 0x80000000U) || name == 0x80000000U) {
        return NULL;
    }
    struct gles_name* empty = NULL;
    for (unsigned i = 0; i < 128; ++i) {
        if (gles_names[i].client == name) {
            return NULL;
        }
        if (!gles_names[i].client && !empty) {
            empty = &gles_names[i];
        }
    }
    return empty;
}
static void gles_forget(unsigned name, unsigned kind) {
    for (unsigned i = 0; i < 128; ++i) {
        if (gles_names[i].client == name && gles_names[i].kind == kind) {
            memset(&gles_names[i], 0, sizeof(gles_names[i]));
        }
    }
}

// Validate the entire batch before executing any commands. All upload bytes
// belong to the bounded stream; no host or guest address crosses this ABI.
static int gles_validate(const unsigned char* data, unsigned size) {
    for (unsigned at = 0; at < size;) {
        if (size - at < 8) {
            return 0;
        }
        struct ame_gles_command h;
        memcpy(&h, data + at, 8);
        if (h.bytes > size - at - 8) {
            return 0;
        }
        unsigned n = h.bytes, fixed = ~0U;
        const unsigned char* p = data + at + 8;
        uint32_t words[4] = {0};
        memcpy(words, p, n < sizeof(words) ? n : sizeof(words));
        switch (h.op) {
        case AME_GL_HELLO:
        case AME_GL_CREATE_PROGRAM:
        case AME_GL_GEN_BUFFER:
        case AME_GL_FINISH:
        case AME_GL_GET_ERROR:
        case AME_GL_CONTEXT_CREATE:
        case AME_GL_CONTEXT_BIND:
        case AME_GL_CONTEXT_UNBIND:
        case AME_GL_CONTEXT_DESTROY:
            fixed = 0;
            break;
        case AME_GL_CREATE_SHADER:
        case AME_GL_CREATE_PROGRAM_NAMED:
        case AME_GL_GEN_BUFFER_NAMED:
        case AME_GL_COMPILE_SHADER:
        case AME_GL_DELETE_SHADER:
        case AME_GL_LINK_PROGRAM:
        case AME_GL_USE_PROGRAM:
        case AME_GL_DELETE_PROGRAM:
        case AME_GL_DELETE_BUFFER:
        case AME_GL_ENABLE_ATTRIB:
        case AME_GL_DISABLE_ATTRIB:
        case AME_GL_CLEAR:
            fixed = 4;
            break;
        case AME_GL_GET_SHADER:
        case AME_GL_ATTACH_SHADER:
        case AME_GL_GET_PROGRAM:
        case AME_GL_BIND_BUFFER:
        case AME_GL_ATTRIB_1F:
        case AME_GL_CREATE_SHADER_NAMED:
            fixed = 8;
            break;
        case AME_GL_DRAW_ARRAYS:
            fixed = 12;
            break;
        case AME_GL_VIEWPORT:
        case AME_GL_CLEAR_COLOR:
            fixed = 16;
            break;
        case AME_GL_ATTRIB_POINTER:
            fixed = 24;
            break;
        case AME_GL_SHADER_SOURCE:
            if (n < 5 || p[n - 1] != 0) {
                return 0;
            }
            break;
        case AME_GL_BIND_ATTRIB:
            if (n < 9 || p[n - 1] != 0) {
                return 0;
            }
            break;
        case AME_GL_BUFFER_DATA:
            if (n != 16 || words[1] > 16 * 1024 * 1024 || words[3]) {
                return 0;
            }
            break;
        case AME_GL_BUFFER_SUB_DATA:
            if (n < 12 || words[2] != n - 12 || words[1] > 0x7fffffffU - words[2]) {
                return 0;
            }
            break;
        default:
            return 0;
        }
        if (fixed != ~0U && fixed != n) {
            return 0;
        }
        unsigned total = (8 + n + 3) & ~3U;
        if (total > size - at) {
            return 0;
        }
        at += total;
    }
    return 1;
}

static int gles_execute(const unsigned char* data, unsigned size, unsigned gpu, unsigned* value) {
    if (!gles_validate(data, size)) {
        return -1;
    }
    for (unsigned at = 0; at < size;) {
        struct ame_gles_command h;
        memcpy(&h, data + at, 8);
        const unsigned char* p = data + at + 8;
        uint32_t a[6] = {0};
        memcpy(a, p, h.bytes < sizeof(a) ? h.bytes : sizeof(a));
        GLint result = 0;
        if (h.op != AME_GL_HELLO &&
            (h.op < AME_GL_CONTEXT_CREATE || h.op > AME_GL_CONTEXT_DESTROY) && !native_bound) {
            return -1;
        }
        switch (h.op) {
        case AME_GL_HELLO:
            *value = gpu;
            break;
        case AME_GL_CONTEXT_CREATE: {
            if (native_context != EGL_NO_CONTEXT) {
                return -1;
            }
            if (gles_target_retire && gles_target_retire()) {
                return -1;
            }
            const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
            const EGLint sa[] = {EGL_WIDTH, 512, EGL_HEIGHT, 512, EGL_NONE};
            native_context = eglCreateContext(native_display, native_config, EGL_NO_CONTEXT, ca);
            native_surface = eglCreatePbufferSurface(native_display, native_config, sa);
            if (native_context == EGL_NO_CONTEXT || native_surface == EGL_NO_SURFACE) {
                if (native_context != EGL_NO_CONTEXT) {
                    eglDestroyContext(native_display, native_context);
                }
                if (native_surface != EGL_NO_SURFACE) {
                    eglDestroySurface(native_display, native_surface);
                }
                native_context = EGL_NO_CONTEXT;
                native_surface = EGL_NO_SURFACE;
                return -1;
            }
            *value = 1;
            break;
        }
        case AME_GL_CONTEXT_BIND:
            if (native_context == EGL_NO_CONTEXT ||
                !eglMakeCurrent(native_display, native_surface, native_surface, native_context)) {
                return -1;
            }
            native_bound = 1;
            if (gles_target_bind) {
                gles_target_bind();
            }
            *value = 1;
            break;
        case AME_GL_CONTEXT_UNBIND:
            if (!eglMakeCurrent(native_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
                return -1;
            }
            native_bound = 0;
            *value = 1;
            break;
        case AME_GL_CONTEXT_DESTROY:
            if (native_bound || native_context == EGL_NO_CONTEXT) {
                return -1;
            }
            if (gles_target_retire && gles_target_retire()) {
                return -1;
            }
            if (gles_target_release) {
                if (!eglMakeCurrent(native_display, native_surface, native_surface,
                                    native_context)) {
                    return -1;
                }
                gles_target_release();
                if (!eglMakeCurrent(native_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                    EGL_NO_CONTEXT)) {
                    return -1;
                }
            }
            if (!eglDestroyContext(native_display, native_context) ||
                !eglDestroySurface(native_display, native_surface)) {
                return -1;
            }
            native_context = EGL_NO_CONTEXT;
            native_surface = EGL_NO_SURFACE;
            memset(gles_names, 0, sizeof(gles_names));
            gles_named_mode = gles_legacy_mode = 0;
            *value = 1;
            break;
        case AME_GL_CREATE_SHADER:
            if (gles_named_mode) {
                return -1;
            }
            gles_legacy_mode = 1;
            *value = glCreateShader(a[0]);
            break;
        case AME_GL_CREATE_SHADER_NAMED:
        case AME_GL_CREATE_PROGRAM_NAMED:
        case AME_GL_GEN_BUFFER_NAMED: {
            struct gles_name* slot = gles_name_slot(a[0]);
            if (!slot || gles_legacy_mode) {
                return -1;
            }
            unsigned driver = 0, kind;
            if (h.op == AME_GL_CREATE_SHADER_NAMED) {
                if (a[1] != GL_VERTEX_SHADER && a[1] != GL_FRAGMENT_SHADER) {
                    return -1;
                }
                kind = GLES_NAME_SHADER;
                driver = glCreateShader(a[1]);
            } else if (h.op == AME_GL_CREATE_PROGRAM_NAMED) {
                kind = GLES_NAME_PROGRAM;
                driver = glCreateProgram();
            } else {
                kind = GLES_NAME_BUFFER;
                glGenBuffers(1, &driver);
            }
            if (!driver) {
                return -1;
            }
            gles_named_mode = 1;
            *slot = (struct gles_name){a[0], driver, kind};
            break;
        }
        case AME_GL_SHADER_SOURCE: {
            a[0] = gles_resolve(a[0], GLES_NAME_SHADER);
            if (!a[0]) {
                return -1;
            }
            const char* s = (const char*)p + 4;
            GLint len = h.bytes - 5;
            glShaderSource(a[0], 1, &s, &len);
            break;
        }
        case AME_GL_COMPILE_SHADER:
            a[0] = gles_resolve(a[0], GLES_NAME_SHADER);
            if (!a[0]) {
                return -1;
            }
            glCompileShader(a[0]);
            break;
        case AME_GL_GET_SHADER:
            a[0] = gles_resolve(a[0], GLES_NAME_SHADER);
            if (!a[0]) {
                return -1;
            }
            glGetShaderiv(a[0], a[1], &result);
            *value = result;
            break;
        case AME_GL_DELETE_SHADER:
            glDeleteShader(gles_resolve(a[0], GLES_NAME_SHADER));
            gles_forget(a[0], GLES_NAME_SHADER);
            break;
        case AME_GL_CREATE_PROGRAM:
            if (gles_named_mode) {
                return -1;
            }
            gles_legacy_mode = 1;
            *value = glCreateProgram();
            break;
        case AME_GL_ATTACH_SHADER:
            a[0] = gles_resolve(a[0], GLES_NAME_PROGRAM);
            a[1] = gles_resolve(a[1], GLES_NAME_SHADER);
            if (!a[0] || !a[1]) {
                return -1;
            }
            glAttachShader(a[0], a[1]);
            break;
        case AME_GL_BIND_ATTRIB:
            a[0] = gles_resolve(a[0], GLES_NAME_PROGRAM);
            if (!a[0]) {
                return -1;
            }
            glBindAttribLocation(a[0], a[1], (const char*)p + 8);
            break;
        case AME_GL_LINK_PROGRAM:
            a[0] = gles_resolve(a[0], GLES_NAME_PROGRAM);
            if (!a[0]) {
                return -1;
            }
            glLinkProgram(a[0]);
            break;
        case AME_GL_GET_PROGRAM:
            a[0] = gles_resolve(a[0], GLES_NAME_PROGRAM);
            if (!a[0]) {
                return -1;
            }
            glGetProgramiv(a[0], a[1], &result);
            *value = result;
            break;
        case AME_GL_USE_PROGRAM:
            if (a[0] && !gles_resolve(a[0], GLES_NAME_PROGRAM)) {
                return -1;
            }
            a[0] = gles_resolve(a[0], GLES_NAME_PROGRAM);
            glUseProgram(a[0]);
            break;
        case AME_GL_DELETE_PROGRAM:
            glDeleteProgram(gles_resolve(a[0], GLES_NAME_PROGRAM));
            gles_forget(a[0], GLES_NAME_PROGRAM);
            break;
        case AME_GL_GEN_BUFFER:
            if (gles_named_mode) {
                return -1;
            }
            gles_legacy_mode = 1;
            glGenBuffers(1, value);
            break;
        case AME_GL_BIND_BUFFER:
            if (a[1] && !gles_resolve(a[1], GLES_NAME_BUFFER)) {
                return -1;
            }
            a[1] = gles_resolve(a[1], GLES_NAME_BUFFER);
            glBindBuffer(a[0], a[1]);
            break;
        case AME_GL_BUFFER_DATA:
            glBufferData(a[0], a[1], NULL, a[2]);
            break;
        case AME_GL_BUFFER_SUB_DATA:
            glBufferSubData(a[0], a[1], a[2], p + 12);
            break;
        case AME_GL_DELETE_BUFFER: {
            unsigned driver = gles_resolve(a[0], GLES_NAME_BUFFER);
            glDeleteBuffers(1, &driver);
            gles_forget(a[0], GLES_NAME_BUFFER);
        } break;
        case AME_GL_ATTRIB_POINTER: {
            GLint bound = 0;
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound);
            if (!bound) {
                return -1; // never interpret an offset as a guest pointer
            }
            glVertexAttribPointer(a[0], a[1], a[2], a[3], a[4], (void*)(uintptr_t)a[5]);
            break;
        }
        case AME_GL_ATTRIB_1F: {
            float value;
            memcpy(&value, p + 4, sizeof(value));
            glVertexAttrib1f(a[0], value);
            break;
        }
        case AME_GL_ENABLE_ATTRIB:
            glEnableVertexAttribArray(a[0]);
            break;
        case AME_GL_DISABLE_ATTRIB:
            glDisableVertexAttribArray(a[0]);
            break;
        case AME_GL_VIEWPORT:
            glViewport((int)a[0], (int)a[1], (int)a[2], (int)a[3]);
            break;
        case AME_GL_CLEAR_COLOR: {
            float f[4];
            memcpy(f, a, 16);
            glClearColor(f[0], f[1], f[2], f[3]);
            break;
        }
        case AME_GL_CLEAR:
            glClear(a[0]);
            break;
        case AME_GL_DRAW_ARRAYS:
            glDrawArrays(a[0], (int)a[1], (int)a[2]);
            break;
        case AME_GL_FINISH:
            glFinish();
            break;
        case AME_GL_GET_ERROR:
            *value = glGetError();
            break;
        }
        at += (8 + h.bytes + 3) & ~3U;
    }
    return 0;
}
