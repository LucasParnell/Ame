/* SPDX-License-Identifier: MIT */
/* PID 1 bootstrap for the optional read-only compressed renderer root.
 * The loop device refers only to the signed package's initramfs file. */
#include <asm/unistd.h>
#include <linux/fcntl.h>
#include <linux/loop.h>
#include <linux/mount.h>
#include <stddef.h>
static long call(long n,long a,long b,long c,long d,long e,long f) {
    register long x8 __asm__("x8")=n;
    register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");
    return x0;
}
void *memset(void *p,int c,size_t n) {unsigned char *s=p;for(size_t i=0;i<n;i++)s[i]=c;return p;}
void *memcpy(void *p,const void *q,size_t n) {unsigned char *s=p;const unsigned char *t=q;for(size_t i=0;i<n;i++)s[i]=t[i];return p;}
static void say(const char *s) {size_t n=0;while(s[n])n++;(void)call(__NR_write,2,(long)s,n,0,0,0);}
__attribute__((noreturn)) static void fail(const char *stage,long error) {
    say("panvk compressed root: failed ");say(stage);say(" errno=0x");
    char number[17];unsigned long value=error<0 ? -error : error;
    for(unsigned i=0;i<16;i++){number[15-i]="0123456789abcdef"[value&15];value>>=4;}number[16]=0;
    say(number);say("\n");call(__NR_exit_group,127,0,0,0,0,0);for(;;){}
}
static long checked(const char *stage,long result){if(result<0)fail(stage,result);return result;}
static void directory(const char *p) {long r=call(__NR_mkdirat,AT_FDCWD,(long)p,0755,0,0,0);if(r<0 && r!=-17)fail(p,r);}
static long open_file(const char *p,long flags){return checked(p,call(__NR_openat,AT_FDCWD,(long)p,flags,0,0,0));}
__attribute__((noreturn)) void _start(void) {
    checked("devtmpfs",call(__NR_mount,(long)"devtmpfs",(long)"/dev",(long)"devtmpfs",0,0,0));
    long image=open_file("/runtime.sqfs",O_RDONLY|O_CLOEXEC);
    long control=open_file("/dev/loop-control",O_RDWR|O_CLOEXEC);
    long index=checked("free loop",call(__NR_ioctl,control,LOOP_CTL_GET_FREE,0,0,0,0));
    if(index!=0)fail("first private loop index",index);
    long loop=open_file("/dev/loop0",O_RDONLY|O_CLOEXEC);
    struct loop_config config={0};
    config.fd=image;config.block_size=4096;config.info.lo_flags=LO_FLAGS_READ_ONLY|LO_FLAGS_AUTOCLEAR;
    checked("loop configure",call(__NR_ioctl,loop,LOOP_CONFIGURE,(long)&config,0,0,0));
    directory("/root");
    checked("squashfs",call(__NR_mount,(long)"/dev/loop0",(long)"/root",(long)"squashfs",MS_RDONLY|MS_NOSUID|MS_NODEV,0,0));
    checked("root devtmpfs",call(__NR_mount,(long)"devtmpfs",(long)"/root/dev",(long)"devtmpfs",0,0,0));
    checked("tmpfs",call(__NR_mount,(long)"tmpfs",(long)"/root/tmp",(long)"tmpfs",MS_NOSUID|MS_NODEV,(long)"mode=1777,size=16m",0));
    checked("chdir root",call(__NR_chdir,(long)"/root",0,0,0,0,0));
    checked("chroot",call(__NR_chroot,(long)".",0,0,0,0,0));
    checked("chdir slash",call(__NR_chdir,(long)"/",0,0,0,0,0));
    call(__NR_close,loop,0,0,0,0,0);call(__NR_close,control,0,0,0,0,0);call(__NR_close,image,0,0,0,0,0);
    char *argv[]={"/init",0};char *env[]={"LD_LIBRARY_PATH=/usr/lib","PATH=/usr/bin",0};
    say("panvk compressed root: squashfs mounted; executing renderer\n");
    long result=call(__NR_execve,(long)"/init",(long)argv,(long)env,0,0,0);
    fail("exec renderer",result);
}
