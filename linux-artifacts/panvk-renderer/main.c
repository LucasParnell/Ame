/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "renderer.h"
#include "midix/abi/protocols/gpu_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static uint32_t pixels[AME_GPU_DIAG_WIDTH*AME_GPU_DIAG_HEIGHT];
static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (uint64_t)ts.tv_sec*1000000000u+ts.tv_nsec;
}
int main(int argc,char **argv) {
    int host=0, compile_only=0, record_only=0, load_test=0, record_load=0;
    const char *ppm=NULL;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--host")) host=1;
        else if (!strcmp(argv[i],"--load-test")) load_test=1;
        else if (!strcmp(argv[i],"--record-load-only")) record_load=1;
        else if (!strcmp(argv[i],"--record-only")) record_only=1;
        else if (!strcmp(argv[i],"--compile-only")) compile_only=1;
        else if (!strcmp(argv[i],"--ppm") && i+1<argc) ppm=argv[++i];
        else { fprintf(stderr,"usage: %s [--host] [--compile-only | --record-only | --record-load-only | --load-test] [--ppm output.ppm]\n",argv[0]); return 2; }
    }
    const char *library=host ? "libvulkan.so.1" : "libvulkan_panfrost.so";
    struct panvk_triangle *t=panvk_triangle_create(library,!host);
    if (!t) return 1;
    printf("Vulkan triangle: device=%s mode=%s\n",panvk_triangle_device(t),
           host ? "host client validation" : "T830 hardware required");
    if (record_only || record_load) {
        int result=record_load ? panvk_triangle_record_load(t) : panvk_triangle_record(t,0);
        printf("Vulkan triangle: command recording %s; no GPU execution verified\n",result?"FAILED":"passed");
        panvk_triangle_destroy(t); return result?1:0;
    }
    if (compile_only) {
        printf("Vulkan triangle: pipeline compiled; no GPU execution verified\n");
        panvk_triangle_destroy(t); return 0;
    }
    uint32_t checksum=0;
    if (load_test) {
        int result=panvk_triangle_verify_load(t,pixels,&checksum);
        printf("Vulkan triangle: LOAD preservation %s checksum=%08x\n",result?"FAILED":"passed",checksum);
        panvk_triangle_destroy(t);
        return result ? 1 : 0;
    }
    if (panvk_triangle_frame(t,0,pixels,&checksum) || !panvk_triangle_verify_pixels(pixels)) {
        fprintf(stderr,"Vulkan triangle: static pixel verification FAILED\n");
        panvk_triangle_destroy(t); return 1;
    }
    printf("Vulkan triangle: static pixel verification passed checksum=%08x\n",checksum);
    if (ppm) {
        FILE *file=fopen(ppm,"wb");
        if (!file) { perror(ppm); panvk_triangle_destroy(t); return 1; }
        int ok=fprintf(file,"P6\n512 512\n255\n")>0;
        for (unsigned i=0;ok && i<512*512;i++) {
            unsigned char rgb[]={pixels[i]>>16,pixels[i]>>8,pixels[i]};
            ok=fwrite(rgb,1,3,file)==3;
        }
        if (fclose(file) || !ok) { panvk_triangle_destroy(t); return 1; }
    }
    uint64_t start=now_ns(); unsigned frames=0; int changed=0;
    do {
        float angle=(float)(now_ns()-start)*(6.2831853f/2500000000.0f);
        uint32_t next;
        if (panvk_triangle_frame(t,angle,pixels,&next)) {
            panvk_triangle_destroy(t); return 1;
        }
        changed |= next!=checksum;
        frames++;
    } while (now_ns()-start<UINT64_C(5000000000));
    printf("Vulkan triangle: %u frames in five seconds; animation=%s\n",frames,changed?"changed":"FAILED");
    panvk_triangle_destroy(t);
    return changed ? 0 : 1;
}
