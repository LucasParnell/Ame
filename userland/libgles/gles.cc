#include "client.h"
namespace ame::gles {
bool current;
static unsigned used;
alignas(8) static unsigned char batch[AME_GFX_BATCH_BYTES];
static GLenum failure;
static unsigned next_name;
static GLuint create_named(unsigned op, unsigned argument = 0) {
    if (next_name == 0x7fffffffU) {
        error(GL_OUT_OF_MEMORY);
        return 0;
    }
    const unsigned data[] = {0x80000000U | ++next_name, argument};
    return command(op, data, op == AME_GL_CREATE_SHADER_NAMED ? 8 : 4) ? data[0] : 0;
}
void error(GLenum code) {
    if (!failure) {
        failure = code;
    }
}
bool flush(unsigned flags) {
    ame_gfx_reply reply{};
    if (!ame::graphics::submit(batch, used, AME_GFX_API_GLES2, flags, reply)) {
        used = 0;
        current = false;
        error(GL_INVALID_OPERATION);
        return false;
    }
    used = 0;
    return true;
}
bool flush_async() {
    if (!used) {
        return true;
    }
    mx_u64_t ticket = 0;
    if (!ame::graphics::enqueue(batch, used, AME_GFX_API_GLES2, 0, ticket) &&
        (!ame::graphics::wait_all() ||
         !ame::graphics::enqueue(batch, used, AME_GFX_API_GLES2, 0, ticket))) {
        used = 0;
        current = false;
        error(GL_INVALID_OPERATION);
        return false;
    }
    used = 0;
    return true;
}
bool command(unsigned op, const void* data, unsigned size) {
    if (!current && op != AME_GL_HELLO &&
        (op < AME_GL_CONTEXT_CREATE || op > AME_GL_CONTEXT_DESTROY)) {
        error(GL_INVALID_OPERATION);
        return false;
    }
    const unsigned total = (sizeof(ame_gles_command) + size + 3) & ~3u;
    const unsigned limit = ame::graphics::batch_limit();
    if (total > limit) {
        error(GL_OUT_OF_MEMORY);
        return false;
    }
    if (used + total > limit && !flush_async()) {
        return false;
    }
    const ame_gles_command h{op, size};
    const auto* header = reinterpret_cast<const unsigned char*>(&h);
    for (unsigned i = 0; i < sizeof(h); ++i) {
        batch[used + i] = header[i];
    }
    for (unsigned i = 0; i < size; ++i) {
        batch[used + sizeof(h) + i] = static_cast<const unsigned char*>(data)[i];
    }
    for (unsigned i = sizeof(h) + size; i < total; ++i) {
        batch[used + i] = 0;
    }
    used += total;
    return true;
}
unsigned query(unsigned op, const void* data, unsigned size) {
    return command(op, data, size) && flush() ? ame::graphics::completion().value : 0;
}
} // namespace ame::gles
using namespace ame::gles;
extern "C" {
GLuint glCreateShader(GLenum type) {
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        error(GL_INVALID_ENUM);
        return 0;
    }
    return create_named(AME_GL_CREATE_SHADER_NAMED, type);
}
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings,
                    const GLint* lengths) {
    alignas(4) unsigned char data[1400]{};
    if (count < 0 || (count && !strings)) {
        error(GL_INVALID_VALUE);
        return;
    }
    *reinterpret_cast<unsigned*>(data) = shader;
    unsigned n = 4;
    for (int i = 0; i < count; ++i) {
        if (!strings[i]) {
            error(GL_INVALID_VALUE);
            return;
        }
        int len = lengths ? lengths[i] : -1;
        for (int j = 0; (len < 0 ? strings[i][j] != 0 : j < len); ++j) {
            if (n + 1 >= sizeof(data)) {
                error(GL_OUT_OF_MEMORY);
                return;
            }
            data[n++] = strings[i][j];
        }
    }
    data[n++] = 0;
    command(AME_GL_SHADER_SOURCE, data, n);
}
void glCompileShader(GLuint s) {
    command(AME_GL_COMPILE_SHADER, &s, 4);
}
void glGetShaderiv(GLuint s, GLenum pname, GLint* p) {
    unsigned a[] = {s, pname};
    if (p) {
        *p = query(AME_GL_GET_SHADER, a, 8);
    } else {
        error(GL_INVALID_VALUE);
    }
}
void glDeleteShader(GLuint s) {
    command(AME_GL_DELETE_SHADER, &s, 4);
}
GLuint glCreateProgram(void) {
    return create_named(AME_GL_CREATE_PROGRAM_NAMED);
}
void glAttachShader(GLuint p, GLuint s) {
    unsigned a[] = {p, s};
    command(AME_GL_ATTACH_SHADER, a, 8);
}
void glBindAttribLocation(GLuint p, GLuint index, const GLchar* name) {
    alignas(4) unsigned char data[264]{};
    if (!name) {
        error(GL_INVALID_VALUE);
        return;
    }
    reinterpret_cast<unsigned*>(data)[0] = p;
    reinterpret_cast<unsigned*>(data)[1] = index;
    unsigned n = 0;
    while (name[n] && n < 255) {
        data[8 + n] = name[n];
        ++n;
    }
    if (name[n]) {
        error(GL_INVALID_VALUE);
        return;
    }
    command(AME_GL_BIND_ATTRIB, data, 9 + n);
}
void glLinkProgram(GLuint p) {
    command(AME_GL_LINK_PROGRAM, &p, 4);
}
void glGetProgramiv(GLuint p, GLenum pname, GLint* v) {
    unsigned a[] = {p, pname};
    if (v) {
        *v = query(AME_GL_GET_PROGRAM, a, 8);
    } else {
        error(GL_INVALID_VALUE);
    }
}
void glUseProgram(GLuint p) {
    command(AME_GL_USE_PROGRAM, &p, 4);
}
void glDeleteProgram(GLuint p) {
    command(AME_GL_DELETE_PROGRAM, &p, 4);
}
void glGenBuffers(GLsizei n, GLuint* buffers) {
    if (n < 0 || (n && !buffers)) {
        error(GL_INVALID_VALUE);
        return;
    }
    for (int i = 0; i < n; ++i) {
        buffers[i] = create_named(AME_GL_GEN_BUFFER_NAMED);
    }
}
void glBindBuffer(GLenum target, GLuint b) {
    unsigned a[] = {target, b};
    command(AME_GL_BIND_BUFFER, a, 8);
}
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    if (offset < 0 || size < 0 || offset > 0x7fffffff || size > 0x7fffffff - offset ||
        (size && !data)) {
        error(GL_INVALID_VALUE);
        return;
    }
    alignas(4) unsigned char bytes[1452]{};
    const auto* input = static_cast<const unsigned char*>(data);
    while (size) {
        const unsigned n = size > 1440 ? 1440 : static_cast<unsigned>(size);
        auto* a = reinterpret_cast<unsigned*>(bytes);
        a[0] = target;
        a[1] = offset;
        a[2] = n;
        for (unsigned i = 0; i < n; ++i) {
            bytes[12 + i] = input[i];
        }
        if (!command(AME_GL_BUFFER_SUB_DATA, bytes, 12 + n)) {
            return;
        }
        input += n;
        offset += n;
        size -= n;
    }
}
void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    if (size < 0) {
        error(GL_INVALID_VALUE);
        return;
    }
    if (size > 16 * 1024 * 1024) {
        error(GL_OUT_OF_MEMORY);
        return;
    }
    unsigned a[] = {target, static_cast<unsigned>(size), usage, 0};
    if (command(AME_GL_BUFFER_DATA, a, sizeof(a)) && data) {
        glBufferSubData(target, 0, size, data);
    }
}
void glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    if (n < 0 || (n && !buffers)) {
        error(GL_INVALID_VALUE);
        return;
    }
    for (int i = 0; i < n; ++i) {
        command(AME_GL_DELETE_BUFFER, buffers + i, 4);
    }
}
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const void* offset) {
    if (reinterpret_cast<__UINTPTR_TYPE__>(offset) > 0xffffffffu) {
        error(GL_INVALID_VALUE);
        return;
    }
    unsigned a[] = {index,
                    static_cast<unsigned>(size),
                    type,
                    normalized,
                    static_cast<unsigned>(stride),
                    static_cast<unsigned>(reinterpret_cast<__UINTPTR_TYPE__>(offset))};
    command(AME_GL_ATTRIB_POINTER, a, sizeof(a));
}
void glVertexAttrib1f(GLuint index, GLfloat value) {
    static_assert(sizeof(value) == sizeof(unsigned));
    unsigned a[] = {index, 0};
    auto* bytes = reinterpret_cast<unsigned char*>(&a[1]);
    const auto* source = reinterpret_cast<const unsigned char*>(&value);
    for (unsigned i = 0; i < sizeof(value); ++i) {
        bytes[i] = source[i];
    }
    command(AME_GL_ATTRIB_1F, a, sizeof(a));
}
void glEnableVertexAttribArray(GLuint i) {
    command(AME_GL_ENABLE_ATTRIB, &i, 4);
}
void glDisableVertexAttribArray(GLuint i) {
    command(AME_GL_DISABLE_ATTRIB, &i, 4);
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    int a[] = {x, y, w, h};
    command(AME_GL_VIEWPORT, a, 16);
}
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    float v[] = {r, g, b, a};
    command(AME_GL_CLEAR_COLOR, v, 16);
}
void glClear(GLbitfield mask) {
    command(AME_GL_CLEAR, &mask, 4);
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    unsigned a[] = {mode, static_cast<unsigned>(first), static_cast<unsigned>(count)};
    command(AME_GL_DRAW_ARRAYS, a, 12);
}
void glFlush(void) {
    flush_async();
}
void glFinish(void) {
    command(AME_GL_FINISH);
    flush();
}
GLenum glGetError(void) {
    if (failure) {
        GLenum e = failure;
        failure = 0;
        return e;
    }
    const GLenum result = query(AME_GL_GET_ERROR);
    if (failure) {
        GLenum e = failure;
        failure = 0;
        return e;
    }
    return result;
}
const GLubyte* glGetString(GLenum name) {
    if (!current) {
        error(GL_INVALID_OPERATION);
        return nullptr;
    }
    switch (name) {
    case GL_VENDOR:
        return reinterpret_cast<const GLubyte*>("Ame / Mesa");
    case GL_RENDERER:
        return reinterpret_cast<const GLubyte*>("Mali-T830 (Panfrost via Dango)");
    case GL_VERSION:
        return reinterpret_cast<const GLubyte*>("OpenGL ES 2.0 Ame experimental subset");
    case GL_SHADING_LANGUAGE_VERSION:
        return reinterpret_cast<const GLubyte*>("OpenGL ES GLSL ES 1.00");
    case GL_EXTENSIONS:
        return reinterpret_cast<const GLubyte*>("");
    default:
        error(GL_INVALID_ENUM);
        return nullptr;
    }
}
}
