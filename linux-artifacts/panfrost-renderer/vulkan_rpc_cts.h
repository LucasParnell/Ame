#pragma once
#include "midix/vulkan/cts.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static pid_t cts_child=-1;
static int cts_exit;
static time_t cts_started;
static VkResult cts_dispatch(ame::vk::CTSRequest* r,unsigned bytes) {
    using namespace ame::vk;
    if(bytes<sizeof(*r))return VK_ERROR_INITIALIZATION_FAILED;
    if(r->header.command==CTSStart) {
        if(cts_child>0)return VK_NOT_READY;
        bool terminated=false;
        for(unsigned i=0;i<sizeof(r->pattern);i++) {
            unsigned char c=r->pattern[i];if(!c){terminated=true;break;}
            if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='.' || c=='_' || c=='*' || c=='-'))
                return VK_ERROR_INITIALIZATION_FAILED;
        }
        if(!terminated || strncmp(r->pattern,"dEQP-VK.",8) || access("/usr/bin/deqp-vk",X_OK))return VK_ERROR_FEATURE_NOT_PRESENT;
        if(unlink("/tmp/cts.qpa") && errno!=ENOENT)return VK_ERROR_INITIALIZATION_FAILED;
        /* PID 1 also hosts Mesa worker threads. Prepare the environment and
         * file actions here, before posix_spawn, instead of calling libc setup
         * routines in a forked child with inherited worker-thread locks. */
        extern char **environ;
        char *environment[128];
        unsigned environment_count=0;
        const char *overrides[]={"LD_LIBRARY_PATH=/usr/lib",
            "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/panfrost_icd.aarch64.json",
            "PANVK_DEBUG=startup,trace_fault", "PANDECODE_DUMP_FILE=stderr"};
        for(char **entry=environ;*entry;entry++) {
            bool replaced=false;
            for(const char *override:overrides) {
                size_t name_bytes=strchr(override,'=')-override+1;
                if(!strncmp(*entry,override,name_bytes)) {replaced=true;break;}
            }
            if(!replaced) {
                if(environment_count>=122)return VK_ERROR_OUT_OF_HOST_MEMORY;
                environment[environment_count++]=*entry;
            }
        }
        for(const char *override:overrides)environment[environment_count++]=const_cast<char*>(override);
        environment[environment_count]=nullptr;
        char pattern[sizeof(r->pattern)+32];
        snprintf(pattern,sizeof(pattern),"--deqp-case=%s",r->pattern);
        const char *arguments[]={"deqp-vk",pattern,
            "--deqp-vk-library-path=/usr/lib/libvulkan.so.1",
            "--deqp-archive-dir=/usr/share/deqp","--deqp-log-filename=/tmp/cts.qpa",
            "--deqp-watchdog=disable",nullptr};
        posix_spawn_file_actions_t actions;
        int error=posix_spawn_file_actions_init(&actions);
        if(error)return VK_ERROR_OUT_OF_HOST_MEMORY;
        error=posix_spawn_file_actions_addclosefrom_np(&actions,3);
        if(!error)error=posix_spawn(&cts_child,"/usr/bin/deqp-vk",&actions,nullptr,
            const_cast<char* const*>(arguments),environment);
        posix_spawn_file_actions_destroy(&actions);
        if(error) {
            cts_child=-1;
            fprintf(stderr,"CTS spawn failed errno=%d (%s)\n",error,strerror(error));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        struct rlimit core{0,0},cpu{120,125};
        (void)prlimit(cts_child,RLIMIT_CORE,&core,nullptr);
        (void)prlimit(cts_child,RLIMIT_CPU,&cpu,nullptr);
        cts_started=time(nullptr);cts_exit=0;
    } else if(r->header.command==CTSPoll) {
        if(cts_child>0) {
            if(time(nullptr)-cts_started>180)kill(cts_child,SIGKILL);
            int status=0;pid_t done=waitpid(cts_child,&status,WNOHANG);
            if(done==cts_child) {cts_exit=WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);cts_child=-1;}
            else if(done<0) {cts_exit=255;cts_child=-1;}
        }
    } else if(r->header.command==CTSLog) {
        if(cts_child>0 || r->log_bytes>CTSLogPayload || r->log_bytes>bytes-sizeof(*r))return VK_NOT_READY;
        int fd=open("/tmp/cts.qpa",O_RDONLY|O_CLOEXEC);
        if(fd<0){r->log_bytes=0;return VK_ERROR_INITIALIZATION_FAILED;}
        ssize_t n=pread(fd,r+1,r->log_bytes,r->log_offset);close(fd);
        if(n<0)return VK_ERROR_INITIALIZATION_FAILED;
        r->log_bytes=n;
    } else return VK_ERROR_FEATURE_NOT_PRESENT;
    r->running=cts_child>0;r->exit_status=cts_exit;
    return VK_SUCCESS;
}
