#pragma once
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "ame/graphics.h"
#include "midix/abi/protocols/gles2.h"
namespace ame::gles {
extern bool current;
void error(GLenum code);
bool command(unsigned op, const void* data = nullptr, unsigned size = 0);
bool flush(unsigned flags = 0);
unsigned query(unsigned op, const void* data = nullptr, unsigned size = 0);
} // namespace ame::gles
