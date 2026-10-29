#pragma once
#include <EGL/egl.h>
/* Temporary display-service window token: fixed 512x512 overlay, five-second
 * hold and console restoration. It is explicitly distinct from a future
 * general native window handle. Pbuffer surfaces do not use this token. */
#define AME_EGL_DIAGNOSTIC_WINDOW ((EGLNativeWindowType)1)

#ifdef __cplusplus
extern "C" {
#endif
/* Present one animation frame without a hold; finish restores the console.
 * Only valid for AME_EGL_DIAGNOSTIC_WINDOW. A stalled client restores after 5s. */
EGLBoolean ameEglSwapDiagnosticFrame(EGLDisplay display, EGLSurface surface, EGLBoolean finish);
#ifdef __cplusplus
}
#endif
