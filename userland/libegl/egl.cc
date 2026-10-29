#include "../libgles/client.h"
#include "ame/egl.h"
namespace {
EGLint failure = EGL_SUCCESS;
bool initialized, context_live, surface_live;
EGLSurface bound_surface;
const EGLDisplay display = reinterpret_cast<EGLDisplay>(1);
const EGLConfig config = reinterpret_cast<EGLConfig>(1);
const EGLContext context = reinterpret_cast<EGLContext>(1);
const EGLSurface surface = reinterpret_cast<EGLSurface>(1);
bool window_surface;
EGLBoolean reject(EGLint code) {
    failure = code;
    return EGL_FALSE;
}
bool valid_display(EGLDisplay d) {
    if (d != display) {
        reject(EGL_BAD_DISPLAY);
        return false;
    }
    if (!initialized) {
        reject(EGL_NOT_INITIALIZED);
        return false;
    }
    return true;
}
bool attributes(const EGLint* attrs, bool is_context) {
    if (!attrs) {
        return true;
    }
    for (unsigned i = 0; i < 32; i += 2) {
        if (attrs[i] == EGL_NONE) {
            return true;
        }
        if (is_context) {
            if (attrs[i] != EGL_CONTEXT_CLIENT_VERSION || attrs[i + 1] != 2) {
                return false;
            }
        } else if ((attrs[i] != EGL_WIDTH && attrs[i] != EGL_HEIGHT) || attrs[i + 1] != 512) {
            return false;
        }
    }
    return false;
}
bool same(const char* a, const char* b) {
    if (!a) {
        return false;
    }
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
} // namespace
extern "C" {
EGLDisplay eglGetDisplay(EGLNativeDisplayType native) {
    if (native != EGL_DEFAULT_DISPLAY) {
        reject(EGL_BAD_PARAMETER);
        return EGL_NO_DISPLAY;
    }
    return display;
}
EGLBoolean eglInitialize(EGLDisplay d, EGLint* major, EGLint* minor) {
    if (d != display) {
        return reject(EGL_BAD_DISPLAY);
    }
    const unsigned gpu = ame::gles::query(AME_GL_HELLO);
    if (gpu != 0x830) {
        return reject(EGL_NOT_INITIALIZED);
    }
    initialized = true;
    if (major) {
        *major = 1;
    }
    if (minor) {
        *minor = 4;
    }
    return EGL_TRUE;
}
EGLBoolean eglBindAPI(EGLenum api) {
    return api == EGL_OPENGL_ES_API ? EGL_TRUE : reject(EGL_BAD_PARAMETER);
}
EGLenum eglQueryAPI(void) {
    return EGL_OPENGL_ES_API;
}
EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig* configs, EGLint size, EGLint* count) {
    if (!valid_display(d)) {
        return EGL_FALSE;
    }
    if (!count || size < 0) {
        return reject(EGL_BAD_PARAMETER);
    }
    *count = (!configs || size > 0) ? 1 : 0;
    if (configs && size > 0) {
        configs[0] = config;
    }
    return EGL_TRUE;
}
EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr, EGLint* value) {
    if (!valid_display(d)) {
        return EGL_FALSE;
    }
    if (c != config) {
        return reject(EGL_BAD_CONFIG);
    }
    if (!value) {
        return reject(EGL_BAD_PARAMETER);
    }
    switch (attr) {
    case EGL_CONFIG_ID:
        *value = 1;
        break;
    case EGL_RED_SIZE:
    case EGL_GREEN_SIZE:
    case EGL_BLUE_SIZE:
    case EGL_ALPHA_SIZE:
        *value = 8;
        break;
    case EGL_BUFFER_SIZE:
        *value = 32;
        break;
    case EGL_DEPTH_SIZE:
    case EGL_STENCIL_SIZE:
    case EGL_SAMPLES:
    case EGL_SAMPLE_BUFFERS:
        *value = 0;
        break;
    case EGL_SURFACE_TYPE:
        *value = EGL_PBUFFER_BIT | EGL_WINDOW_BIT;
        break;
    case EGL_RENDERABLE_TYPE:
    case EGL_CONFORMANT:
        *value = EGL_OPENGL_ES2_BIT;
        break;
    default:
        return reject(EGL_BAD_ATTRIBUTE);
    }
    return EGL_TRUE;
}
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint* attrs, EGLConfig* configs, EGLint size,
                           EGLint* count) {
    if (!valid_display(d)) {
        return EGL_FALSE;
    }
    if (!count || size < 0) {
        return reject(EGL_BAD_PARAMETER);
    }
    bool match = true, ended = !attrs;
    for (unsigned i = 0; attrs && i < 64; i += 2) {
        if (attrs[i] == EGL_NONE) {
            ended = true;
            break;
        }
        EGLint value = 0;
        if (!eglGetConfigAttrib(d, config, attrs[i], &value)) {
            return EGL_FALSE;
        }
        if (attrs[i + 1] == EGL_DONT_CARE) {
            continue;
        }
        if (attrs[i] == EGL_SURFACE_TYPE || attrs[i] == EGL_RENDERABLE_TYPE ||
            attrs[i] == EGL_CONFORMANT) {
            match &= (value & attrs[i + 1]) == attrs[i + 1];
        } else if (attrs[i] == EGL_CONFIG_ID) {
            match &= value == attrs[i + 1];
        } else {
            match &= value >= attrs[i + 1];
        }
    }
    if (!ended) {
        return reject(EGL_BAD_ATTRIBUTE);
    }
    *count = match && (!configs || size > 0) ? 1 : 0;
    if (match && configs && size > 0) {
        configs[0] = config;
    }
    return EGL_TRUE;
}
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext shared, const EGLint* attrs) {
    if (!valid_display(d)) {
        return EGL_NO_CONTEXT;
    }
    if (c != config) {
        reject(EGL_BAD_CONFIG);
        return EGL_NO_CONTEXT;
    }
    if (shared != EGL_NO_CONTEXT || context_live) {
        reject(EGL_BAD_ALLOC);
        return EGL_NO_CONTEXT;
    }
    if (!attributes(attrs, true)) {
        reject(EGL_BAD_ATTRIBUTE);
        return EGL_NO_CONTEXT;
    }
    if (!ame::gles::query(AME_GL_CONTEXT_CREATE)) {
        reject(EGL_BAD_ALLOC);
        return EGL_NO_CONTEXT;
    }
    context_live = true;
    return context;
}
EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint* attrs) {
    if (!valid_display(d)) {
        return EGL_NO_SURFACE;
    }
    if (c != config) {
        reject(EGL_BAD_CONFIG);
        return EGL_NO_SURFACE;
    }
    if (surface_live) {
        reject(EGL_BAD_ALLOC);
        return EGL_NO_SURFACE;
    }
    if (!attributes(attrs, false)) {
        reject(EGL_BAD_ATTRIBUTE);
        return EGL_NO_SURFACE;
    }
    surface_live = true;
    window_surface = false;
    return surface;
}
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType native,
                                  const EGLint* attrs) {
    // Window token 1 is the provisioned 512x512 diagnostic overlay. Real window
    // objects will be supplied by the native display service in a later revision.
    if (native != AME_EGL_DIAGNOSTIC_WINDOW) {
        reject(EGL_BAD_NATIVE_WINDOW);
        return EGL_NO_SURFACE;
    }
    EGLSurface s = eglCreatePbufferSurface(d, c, attrs);
    if (s) {
        window_surface = true;
    }
    return s;
}
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c) {
    if (!valid_display(d)) {
        return EGL_FALSE;
    }
    if (c == EGL_NO_CONTEXT && draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE) {
        if (ame::gles::current &&
            !ame::gles::flush(window_surface ? AME_GFX_PRESENT | AME_GFX_PRESENT_RESTORE : 0)) {
            return reject(EGL_CONTEXT_LOST);
        }
        if (ame::gles::current && !ame::gles::query(AME_GL_CONTEXT_UNBIND)) {
            return reject(EGL_CONTEXT_LOST);
        }
        ame::gles::current = false;
        bound_surface = EGL_NO_SURFACE;
        return EGL_TRUE;
    }
    if (!context_live || c != context) {
        return reject(EGL_BAD_CONTEXT);
    }
    if (!surface_live || draw != surface || read != surface) {
        return reject(EGL_BAD_SURFACE);
    }
    if (!ame::gles::query(AME_GL_CONTEXT_BIND)) {
        return reject(EGL_CONTEXT_LOST);
    }
    ame::gles::current = true;
    bound_surface = surface;
    return EGL_TRUE;
}
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!valid_display(d)) {
        return EGL_FALSE;
    }
    if (!surface_live || s != bound_surface || !ame::gles::current) {
        return reject(EGL_BAD_SURFACE);
    }
    return ame::gles::flush(window_surface ? AME_GFX_PRESENT : 0) ? EGL_TRUE
                                                                  : reject(EGL_CONTEXT_LOST);
}
EGLBoolean ameEglSwapDiagnosticFrame(EGLDisplay d, EGLSurface s, EGLBoolean finish) {
    if (!valid_display(d)) {
        return EGL_FALSE;
    }
    if (!surface_live || s != bound_surface || !ame::gles::current || !window_surface) {
        return reject(EGL_BAD_SURFACE);
    }
    const unsigned flags =
        AME_GFX_PRESENT | (finish ? AME_GFX_PRESENT_RESTORE : AME_GFX_PRESENT_STREAM);
    return ame::gles::flush(flags) ? EGL_TRUE : reject(EGL_CONTEXT_LOST);
}
EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) {
    if (!valid_display(d)) {
        return EGL_FALSE;
    }
    if (!surface_live || s != surface) {
        return reject(EGL_BAD_SURFACE);
    }
    if (bound_surface == s) {
        return reject(EGL_BAD_ACCESS);
    }
    surface_live = false;
    return EGL_TRUE;
}
EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c) {
    if (!valid_display(d)) {
        return EGL_FALSE;
    }
    if (!context_live || c != context) {
        return reject(EGL_BAD_CONTEXT);
    }
    if (ame::gles::current) {
        return reject(EGL_BAD_ACCESS);
    }
    if (!ame::gles::query(AME_GL_CONTEXT_DESTROY)) {
        return reject(EGL_CONTEXT_LOST);
    }
    context_live = false;
    return EGL_TRUE;
}
EGLBoolean eglTerminate(EGLDisplay d) {
    if (!valid_display(d)) {
        return EGL_FALSE;
    }
    if (context_live || surface_live) {
        return reject(EGL_BAD_ACCESS);
    }
    initialized = false;
    return EGL_TRUE;
}
EGLDisplay eglGetCurrentDisplay(void) {
    return ame::gles::current ? display : EGL_NO_DISPLAY;
}
EGLContext eglGetCurrentContext(void) {
    return ame::gles::current ? context : EGL_NO_CONTEXT;
}
EGLSurface eglGetCurrentSurface(EGLint which) {
    if (which != EGL_DRAW && which != EGL_READ) {
        reject(EGL_BAD_PARAMETER);
        return EGL_NO_SURFACE;
    }
    return bound_surface;
}
EGLint eglGetError(void) {
    EGLint e = failure;
    failure = EGL_SUCCESS;
    return e;
}
const char* eglQueryString(EGLDisplay d, EGLint name) {
    if (!valid_display(d)) {
        return nullptr;
    }
    switch (name) {
    case EGL_VENDOR:
        return "Ame";
    case EGL_VERSION:
        return "1.4 Ame experimental subset";
    case EGL_CLIENT_APIS:
        return "OpenGL_ES";
    case EGL_EXTENSIONS:
        return "";
    default:
        reject(EGL_BAD_PARAMETER);
        return nullptr;
    }
}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
#define PROC(f)                                                                                    \
    if (same(name, #f))                                                                            \
    return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(f)
    PROC(eglGetDisplay);
    PROC(eglInitialize);
    PROC(eglBindAPI);
    PROC(eglQueryAPI);
    PROC(eglChooseConfig);
    PROC(eglGetConfigs);
    PROC(eglGetConfigAttrib);
    PROC(eglCreateContext);
    PROC(eglCreatePbufferSurface);
    PROC(eglCreateWindowSurface);
    PROC(eglMakeCurrent);
    PROC(eglSwapBuffers);
    PROC(ameEglSwapDiagnosticFrame);
    PROC(eglDestroyContext);
    PROC(eglDestroySurface);
    PROC(eglTerminate);
    PROC(eglGetError);
    PROC(eglQueryString);
    PROC(eglGetProcAddress);
    PROC(eglGetCurrentDisplay);
    PROC(eglGetCurrentContext);
    PROC(eglGetCurrentSurface);
    PROC(glCreateShader);
    PROC(glShaderSource);
    PROC(glCompileShader);
    PROC(glGetShaderiv);
    PROC(glDeleteShader);
    PROC(glCreateProgram);
    PROC(glAttachShader);
    PROC(glBindAttribLocation);
    PROC(glLinkProgram);
    PROC(glGetProgramiv);
    PROC(glUseProgram);
    PROC(glDeleteProgram);
    PROC(glGenBuffers);
    PROC(glBindBuffer);
    PROC(glBufferData);
    PROC(glBufferSubData);
    PROC(glDeleteBuffers);
    PROC(glVertexAttribPointer);
    PROC(glVertexAttrib1f);
    PROC(glEnableVertexAttribArray);
    PROC(glDisableVertexAttribArray);
    PROC(glViewport);
    PROC(glClearColor);
    PROC(glClear);
    PROC(glDrawArrays);
    PROC(glFlush);
    PROC(glFinish);
    PROC(glGetError);
    PROC(glGetString);
#undef PROC
    return nullptr;
}
}
