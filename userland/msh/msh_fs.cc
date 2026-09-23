// userland/msh/msh_fs.cc

#include "msh.h"

#include "libk/bytes.h"

// Filesystem builtins: io.fs path resolution and pwd/cd/ls/cat/mkdir/rm/touch.
// Paths are normalised into components, resolved one component at a time with
// FS_OPEN, and every intermediate handle is closed on the way out.

int cmd_pwd(int, char**) {
    putln(g.cwd);
    return 0;
}

static bool fs_open_name_at(fs_handle_t parent, const char* name, mx_u32_t flags, FsOpenReply* out) {
    if (!g.have_fs) return false;
    return g.fs.wait_open(parent, name, flags, out);
}

static bool path_push(char parts[][kPathNameMax], uint32_t* count, const char* s, uint32_t len) {
    if (!parts || !count || len == 0) return true;
    if (len == 1 && s[0] == '.') return true;
    if (len == 2 && s[0] == '.' && s[1] == '.') {
        if (*count > 0) --*count;
        return true;
    }
    if (*count >= kPathDepthMax || len >= kPathNameMax) return false;
    (void)libk::copy_n(libk::as_bytes(parts[*count], len), libk::as_bytes(s, len), len);
    parts[*count][len] = '\0';
    ++*count;
    return true;
}

static bool path_collect(const char* path, char parts[][kPathNameMax], uint32_t* count) {
    if (!path || !parts || !count) return false;
    uint32_t i = 0;
    while (path[i]) {
        while (path[i] == '/') ++i;
        const uint32_t start = i;
        while (path[i] && path[i] != '/') ++i;
        if (i > start && !path_push(parts, count, path + start, i - start)) return false;
    }
    return true;
}

static bool normalize_path(const char* input, char* out, uint32_t out_cap) {
    if (!input || !input[0]) input = ".";
    if (!out || out_cap == 0) return false;
    char parts[kPathDepthMax][kPathNameMax]{};
    uint32_t count = 0;
    if (input[0] != '/') {
        if (!path_collect(g.cwd, parts, &count)) return false;
    }
    if (!path_collect(input, parts, &count)) return false;
    uint32_t n = 0;
    append_char(out, out_cap, &n, '/');
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0) append_char(out, out_cap, &n, '/');
        append_str(out, out_cap, &n, parts[i]);
    }
    return n < out_cap;
}

static bool split_parent_name(const char* path, char* parent, uint32_t parent_cap,
                              char* name, uint32_t name_cap) {
    char norm[kPathMax];
    if (!normalize_path(path, norm, sizeof(norm))) return false;
    if (streq(norm, "/")) return false;
    uint32_t last = 0;
    for (uint32_t i = 0; norm[i]; ++i) if (norm[i] == '/') last = i;
    if (last == 0) {
        copy_str(parent, parent_cap, "/");
        copy_str(name, name_cap, norm + 1);
    } else {
        uint32_t n = 0;
        for (uint32_t i = 0; i < last && n + 1 < parent_cap; ++i) parent[n++] = norm[i];
        parent[n] = '\0';
        copy_str(name, name_cap, norm + last + 1);
    }
    return name[0] != '\0';
}

struct ResolvedPath {
    fs_handle_t handle;
    mx_u64_t inode;
    mx_u16_t type;
    mx_u64_t size;
    char normalized[kPathMax];
    bool borrowed_root;
};

static void close_resolved(const ResolvedPath& r) {
    if (!r.borrowed_root && r.handle != 0 && r.handle != g.cwd_handle) {
        (void)g.fs.try_close(r.handle);
    }
}

static bool resolve_normalized(const char* normalized, mx_u32_t flags, ResolvedPath* out) {
    if (!normalized || !out || !g.have_fs) return false;
    *out = {};
    copy_str(out->normalized, sizeof(out->normalized), normalized);
    if (streq(normalized, "/")) {
        out->handle = kRootHandle;
        out->inode = kRootHandle;
        out->type = FS_INODE_DIR;
        out->size = 0;
        out->borrowed_root = true;
        return true;
    }

    fs_handle_t parent = kRootHandle;
    bool parent_owned = false;
    uint32_t i = 1;
    while (normalized[i]) {
        char component[kPathNameMax]{};
        uint32_t n = 0;
        while (normalized[i] && normalized[i] != '/') {
            if (n + 1 >= sizeof(component)) {
                if (parent_owned) (void)g.fs.try_close(parent);
                return false;
            }
            component[n++] = normalized[i++];
        }
        component[n] = '\0';
        while (normalized[i] == '/') ++i;
        const bool final = normalized[i] == '\0';
        FsOpenReply open{};
        const mx_u32_t open_flags = final ? flags : FS_OPEN_READ;
        if (!fs_open_name_at(parent, component, open_flags, &open) || open.status != MX_OK) {
            if (parent_owned) (void)g.fs.try_close(parent);
            return false;
        }
        if (parent_owned) (void)g.fs.try_close(parent);
        if (!final && open.type != FS_INODE_DIR) {
            (void)g.fs.try_close(open.handle);
            return false;
        }
        if (final) {
            out->handle = open.handle;
            out->inode = open.inode_number;
            out->type = open.type;
            out->size = open.size;
            out->borrowed_root = false;
            return true;
        }
        parent = open.handle;
        parent_owned = true;
    }
    return false;
}

static bool resolve_path(const char* path, mx_u32_t flags, ResolvedPath* out) {
    char norm[kPathMax];
    if (!normalize_path(path, norm, sizeof(norm))) return false;
    return resolve_normalized(norm, flags, out);
}

static bool wait_status_reply(mx_u32_t type, mx_i32_t* out) {
    RingSlot slot{};
    while (g.fs.ep.wait_recv(&slot)) {
        if (slot.type == type && slot.length >= sizeof(mx_i32_t)) {
            mx_i32_t status = MX_ERR_BAD_PROTOCOL;
            mochi::fs::copy_bytes(&status, slot.payload, sizeof(status));
            if (out) *out = status;
            return true;
        }
        if (slot.type == FS_ERROR && slot.length >= sizeof(mx_i32_t)) {
            mx_i32_t status = MX_ERR_BAD_PROTOCOL;
            mochi::fs::copy_bytes(&status, slot.payload, sizeof(status));
            if (out) *out = status;
            return true;
        }
    }
    return false;
}

int cmd_cd(int argc, char** argv) {
    if (!g.have_fs) return notimpl("cd io.fs");
    const char* dst = argc > 1 ? argv[1] : "/";
    ResolvedPath r{};
    if (!resolve_path(dst, FS_OPEN_READ, &r)) {
        put("msh: cd: no such directory: "); putln(dst);
        return 1;
    }
    if (r.type != FS_INODE_DIR) {
        put("msh: cd: not a directory: "); putln(dst);
        close_resolved(r);
        return 1;
    }
    if (g.cwd_handle != 1 && g.cwd_handle != r.handle) (void)g.fs.try_close(g.cwd_handle);
    copy_str(g.cwd, sizeof(g.cwd), r.normalized);
    g.cwd_handle = r.handle;
    set_var("PWD", g.cwd);
    return 0;
}

int cmd_ls(int argc, char** argv) {
    if (!g.have_fs) return notimpl("ls io.fs");
    const char* name = argc > 1 ? argv[1] : ".";
    ResolvedPath r{};
    if (!resolve_path(name, FS_OPEN_READ, &r)) {
        put("msh: ls: cannot access: "); putln(name);
        return 1;
    }
    if (r.type != FS_INODE_DIR) {
        const char* display = name;
        for (uint32_t i = 0; name[i]; ++i) if (name[i] == '/') display = name + i + 1;
        putln(display);
        close_resolved(r);
        return 0;
    }
    mx_u32_t cookie = 0;
    bool any = false;
    for (;;) {
        FsReaddirRequest req{};
        req.handle = r.handle;
        req.cookie = cookie;
        if (!g.fs.ep.send_spinning(FS_READDIR, &req, sizeof(req))) {
            close_resolved(r);
            return 1;
        }
        RingSlot slot{};
        FsReaddirReply reply{};
        bool got = false;
        while (g.fs.ep.wait_recv(&slot)) {
            if (slot.type == FS_READDIR_REPLY && slot.length >= sizeof(FsReaddirReply)) {
                mochi::fs::copy_bytes(&reply, slot.payload, sizeof(reply));
                got = true;
                break;
            }
            if (slot.type == FS_ERROR) break;
        }
        if (!got || reply.status != MX_OK) {
            put("msh: ls: "); put_status(reply.status); put("\n");
            close_resolved(r);
            return 1;
        }
        if (reply.flags & FS_READDIR_HAS_ENTRY) {
            if (reply.type == FS_INODE_DIR) put("d "); else put("- ");
            put_bytes(reply.name, reply.name_len);
            if (reply.flags & FS_READDIR_TRUNCATED) put("…");
            put("\n");
            any = true;
        }
        if (reply.flags & FS_READDIR_EOF) break;
        cookie = reply.next_cookie;
    }
    if (!any) putln(".");
    close_resolved(r);
    return 0;
}

int cmd_cat(int argc, char** argv) {
    if (argc < 2) { putln("usage: cat FILE"); return 1; }
    if (!g.have_fs) return notimpl("cat io.fs");
    ResolvedPath r{};
    if (!resolve_path(argv[1], FS_OPEN_READ, &r)) {
        put("msh: cat: open failed: "); putln(argv[1]);
        return 1;
    }
    if (r.type != FS_INODE_FILE) {
        put("msh: cat: not a file: "); putln(argv[1]);
        close_resolved(r);
        return 1;
    }
    mx_u64_t off = 0;
    while (true) {
        FsReadReply read{};
        if (!g.fs.try_read(r.handle, off, FS_INLINE_READ_MAX, &read)) {
            putln("msh: cat: read protocol failed");
            close_resolved(r);
            return 1;
        }
        if (read.status != MX_OK) {
            put("msh: cat: "); put_status(read.status); put("\n");
            close_resolved(r);
            return 1;
        }
        if (read.length_read == 0) break;
        put_bytes(reinterpret_cast<const char*>(read.inline_data), read.length_read);
        off += read.length_read;
        if (read.length_read < FS_INLINE_READ_MAX) break;
    }
    close_resolved(r);
    return 0;
}

int cmd_mkdir(int argc, char** argv) {
    if (argc < 2) { putln("usage: mkdir DIR"); return 1; }
    if (!g.have_fs) return notimpl("mkdir io.fs");
    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        char parent_path[kPathMax]{};
        char name[kPathNameMax]{};
        if (!split_parent_name(argv[a], parent_path, sizeof(parent_path), name, sizeof(name))) {
            put("mkdir: bad path: "); putln(argv[a]); rc = 1; continue;
        }
        ResolvedPath parent{};
        if (!resolve_path(parent_path, FS_OPEN_READ, &parent) || parent.type != FS_INODE_DIR) {
            put("mkdir: parent missing: "); putln(parent_path); rc = 1; continue;
        }
        FsMkdirRequest req{};
        req.parent_inode = parent.handle;
        req.mode = 0755;
        uint32_t n = slen(name);
        if (n > sizeof(req.name)) n = sizeof(req.name);
        req.name_len = static_cast<mx_u8_t>(n);
        (void)libk::copy_n(libk::as_bytes(req.name, n), libk::as_bytes(name, n), n);
        mx_i32_t status = MX_ERR_BAD_PROTOCOL;
        if (!g.fs.ep.send_spinning(FS_MKDIR, &req, sizeof(req)) || !wait_status_reply(FS_MKDIR_REPLY, &status)) {
            status = MX_ERR_BAD_PROTOCOL;
        }
        close_resolved(parent);
        if (status != MX_OK) { put("mkdir: "); put_status(status); put(" "); putln(argv[a]); rc = 1; }
    }
    return rc;
}

int cmd_rm(int argc, char** argv) {
    if (argc < 2) { putln("usage: rm PATH"); return 1; }
    if (!g.have_fs) return notimpl("rm io.fs");
    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        char parent_path[kPathMax]{};
        char name[kPathNameMax]{};
        if (!split_parent_name(argv[a], parent_path, sizeof(parent_path), name, sizeof(name))) {
            put("rm: bad path: "); putln(argv[a]); rc = 1; continue;
        }
        ResolvedPath parent{};
        if (!resolve_path(parent_path, FS_OPEN_READ, &parent) || parent.type != FS_INODE_DIR) {
            put("rm: parent missing: "); putln(parent_path); rc = 1; continue;
        }
        FsUnlinkRequest req{};
        req.parent_inode = parent.handle;
        uint32_t n = slen(name);
        if (n > sizeof(req.name)) n = sizeof(req.name);
        req.name_len = static_cast<mx_u8_t>(n);
        (void)libk::copy_n(libk::as_bytes(req.name, n), libk::as_bytes(name, n), n);
        mx_i32_t status = MX_ERR_BAD_PROTOCOL;
        if (!g.fs.ep.send_spinning(FS_UNLINK, &req, sizeof(req)) || !wait_status_reply(FS_UNLINK_REPLY, &status)) {
            status = MX_ERR_BAD_PROTOCOL;
        }
        close_resolved(parent);
        if (status != MX_OK) { put("rm: "); put_status(status); put(" "); putln(argv[a]); rc = 1; }
    }
    return rc;
}

int cmd_touch(int argc, char** argv) {
    if (argc < 2) { putln("usage: touch FILE"); return 1; }
    if (!g.have_fs) return notimpl("touch io.fs");
    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        char parent_path[kPathMax]{};
        char name[kPathNameMax]{};
        if (!split_parent_name(argv[a], parent_path, sizeof(parent_path), name, sizeof(name))) {
            put("touch: bad path: "); putln(argv[a]); rc = 1; continue;
        }
        ResolvedPath parent{};
        if (!resolve_path(parent_path, FS_OPEN_READ, &parent) || parent.type != FS_INODE_DIR) {
            put("touch: parent missing: "); putln(parent_path); rc = 1; continue;
        }
        FsOpenReply open{};
        if (!fs_open_name_at(parent.handle, name, FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE, &open) || open.status != MX_OK) {
            put("touch: "); put_status(open.status); put(" "); putln(argv[a]); rc = 1;
        } else {
            (void)g.fs.try_close(open.handle);
        }
        close_resolved(parent);
    }
    return rc;
}

