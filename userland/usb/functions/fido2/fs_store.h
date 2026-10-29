#pragma once
#include "store.h"
#include "mochi/fs.h"
namespace mochi::fido2 {
struct FileStore {
    mochi::fs::Client fs;
    const mx_u8_t* master;
    SealedStore sealed;
    fs_handle_t directory=0;
    bool map(const MxBootInfo* boot,const mx_u8_t key[32]) {
        master=key;
        fs.ep=mochi::boot::map_endpoint(boot,
            {"fs.cmd.ring",mochi::va::native_driver::kDevicePayload,"fs.cmd.doorbell"},
            {"fs.reply.ring",mochi::va::native_driver::kDevicePayload+0x10000,"fs.reply.doorbell"});
        if (!fs.valid() || !fs.map_write_arena(boot,mochi::va::native_driver::kDevicePayload+0x20000)) return false;
        FsOpenReply opened{};
        if (!fs.wait_open(1,"data",FS_OPEN_READ|FS_OPEN_WRITE,&opened) || opened.status!=MX_OK) return false;
        directory=opened.handle;return true;
    }
    bool load(Store& store) {
        store.context=this;store.persist=save_callback;
        FsOpenReply file{};
        if (!fs.wait_open(directory,".fido2",FS_OPEN_READ,&file)) return false;
        if (file.status==MX_ERR_NOT_FOUND) {
            store.fresh();store.available=true;return store.save();
        }
        if (file.status!=MX_OK) return false;
        mx_u32_t received=0;
        const bool valid=file.size==sizeof(sealed) && fs.read_bytes(file.handle,0,&sealed,sizeof(sealed),&received) &&
            received==sizeof(sealed) && open_store(store.data,sealed,master);
        (void)fs.try_close(file.handle);crypto_wipe(&sealed,sizeof(sealed));
        store.available=valid;return valid;
    }
    static bool save_callback(void* context,const Stored& state) {
        auto& self=*static_cast<FileStore*>(context);
        seal_store(self.sealed,state,self.master);
        FsOpenReply file{};
        if (!self.fs.wait_open(self.directory,".fido2",FS_OPEN_READ|FS_OPEN_WRITE|FS_OPEN_CREATE,&file) ||
            file.status!=MX_OK) return false;
        FsWriteReply write{};mx_i32_t sync=MX_ERR_FAULT;
        const bool valid=self.fs.try_write(file.handle,0,&self.sealed,sizeof(self.sealed),&write) &&
            write.status==MX_OK && write.bytes_written==sizeof(self.sealed) &&
            self.fs.try_sync(&sync) && sync==MX_OK;
        (void)self.fs.try_close(file.handle);crypto_wipe(&self.sealed,sizeof(self.sealed));
        return valid;
    }
};
} // namespace mochi::fido2
