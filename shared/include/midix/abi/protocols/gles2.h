#pragma once
/* GLES-specific encoding, independent of the transport and presentation. Each
 * record is header + payload, padded to four bytes. Scalar payloads use u32
 * words (float bits preserved). Data is copied, never a native pointer. */
enum ame_gles_op {
    AME_GL_HELLO = 1,
    AME_GL_CREATE_SHADER,
    AME_GL_SHADER_SOURCE,
    AME_GL_COMPILE_SHADER,
    AME_GL_GET_SHADER,
    AME_GL_DELETE_SHADER,
    AME_GL_CREATE_PROGRAM,
    AME_GL_ATTACH_SHADER,
    AME_GL_BIND_ATTRIB,
    AME_GL_LINK_PROGRAM,
    AME_GL_GET_PROGRAM,
    AME_GL_USE_PROGRAM,
    AME_GL_DELETE_PROGRAM,
    AME_GL_GEN_BUFFER,
    AME_GL_BIND_BUFFER,
    AME_GL_BUFFER_DATA,
    AME_GL_DELETE_BUFFER,
    AME_GL_ATTRIB_POINTER,
    AME_GL_ENABLE_ATTRIB,
    AME_GL_DISABLE_ATTRIB,
    AME_GL_VIEWPORT,
    AME_GL_CLEAR_COLOR,
    AME_GL_CLEAR,
    AME_GL_DRAW_ARRAYS,
    AME_GL_FINISH,
    AME_GL_GET_ERROR,
    AME_GL_CONTEXT_CREATE,
    AME_GL_CONTEXT_BIND,
    AME_GL_CONTEXT_UNBIND,
    AME_GL_CONTEXT_DESTROY,
    AME_GL_BUFFER_SUB_DATA,
    AME_GL_ATTRIB_1F,
    // Client names are mapped to private Mesa names by the decoder. Creates
    // can share a batch with setup instead of requiring a synchronous reply.
    AME_GL_CREATE_SHADER_NAMED,
    AME_GL_CREATE_PROGRAM_NAMED,
    AME_GL_GEN_BUFFER_NAMED
};
struct ame_gles_command {
    unsigned int op, bytes;
};
