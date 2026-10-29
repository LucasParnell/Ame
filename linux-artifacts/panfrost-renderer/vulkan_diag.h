#pragma once
/* Native Vulkan's guest presentation boundary; rendering is recorded by apps. */
#ifdef AME_ENABLE_PANVK
static int vulkan_dma[AME_GPU_DIAG_TARGETS]={-1,-1};
static unsigned vulkan_active,vulkan_last;
static int vulkan_has_last,vulkan_in_use;
static uint32_t vulkan_pixels[AME_GPU_DIAG_WIDTH*AME_GPU_DIAG_HEIGHT];
static int vulkan_fail(const char* stage) {
    fprintf(stderr,"panvk renderer: %s failed\n",stage);return -1;
}
static int vulkan_export(unsigned index) {
    int fd = open(DANGO_DEV_SHM, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct dango_shm_query query = {.index = index};
    struct dango_shm_export export_req = {0};
    int ok = !ioctl(fd, DANGO_SHM_QUERY, &query) && query.count &&
        (query.region.flags & DANGO_SHM_REGION_FLAG_DMA_SHARED_GPU) &&
        query.region.size >= AME_GPU_DIAG_BYTES;
    if (ok) {
        export_req.id = query.region.id;
        export_req.boot_generation = query.boot_generation;
        ok = !ioctl(fd, DANGO_SHM_EXPORT_DMABUF, &export_req);
    }
    close(fd);
    return ok ? export_req.fd : -1;
}
static int vulkan_rpc_execute(const struct ame_gfx_submit* request,void* data,
                              uint32_t* shared,struct ame_gfx_reply* reply) {
    if(request->bytes<8)return vulkan_fail("RPC header");
    if(((const uint32_t*)data)[1]!=AME_VK_RPC_PRESENT)
        return request->flags ? vulkan_fail("RPC flags") : ame_vulkan_rpc(data,request->bytes);
    int finish=0;
    if(ame_vulkan_rpc_present(data,request->bytes,vulkan_pixels,&reply->checksum,&finish) ||
       ((int32_t*)data)[4])return vulkan_fail("native image readback");
    if(finish) {
        if(request->flags!=(AME_GFX_PRESENT|AME_GFX_PRESENT_RESTORE) || !vulkan_has_last)return vulkan_fail("native restore");
        vulkan_in_use=0;reply->present_target=vulkan_last;return 0;
    }
    if(request->flags!=(AME_GFX_PRESENT|AME_GFX_PRESENT_STREAM))return vulkan_fail("native present flags");
    if(!vulkan_in_use) {
        if(display_retire())return vulkan_fail("native display retire");
        vulkan_active=0;
    }
    const unsigned target=vulkan_active;
    if(vulkan_dma[target]<0)vulkan_dma[target]=vulkan_export(target);
    if(vulkan_dma[target]<0)return vulkan_fail("native DMA export");
    struct dma_buf_sync sync={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE};
    if(ioctl(vulkan_dma[target],DMA_BUF_IOCTL_SYNC,&sync))return vulkan_fail("native DMA sync");
    memcpy(shared+target*(AME_GPU_DIAG_BYTES/4),vulkan_pixels,AME_GPU_DIAG_BYTES);
    sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE;
    if(ioctl(vulkan_dma[target],DMA_BUF_IOCTL_SYNC,&sync))return vulkan_fail("native DMA sync");
    reply->present_target=target;
    vulkan_in_use=1;vulkan_has_last=1;vulkan_last=target;vulkan_active^=1;
    return 0;
}
#endif
