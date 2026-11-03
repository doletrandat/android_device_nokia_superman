#include <jni.h>
#include <android/native_window_jni.h>
#include <android/hardware_buffer.h>
#include <system/window.h>
#include <gralloc_handle.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "direct-buffer-layout.h"

namespace {
struct Capture {
    ANativeWindow* window = nullptr;
    AHardwareBuffer* buffer = nullptr;
    int fence = -1;
    ~Capture() {
        if (fence >= 0) close(fence);
        if (buffer) AHardwareBuffer_release(buffer);
        if (window) ANativeWindow_release(window);
    }
};
int waitFence(int fd) {
    if (fd < 0) return 0;
    pollfd p{fd,POLLIN,0};
    int r=poll(&p,1,500);
    if (r<=0) return r==0 ? -ETIMEDOUT : -errno;
    return p.revents==POLLIN ? 0 : -EIO;
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_superman_diagnostics_hwui_DirectBuffer_capture(JNIEnv* env,jclass,jobject surface,jstring filename) {
    Capture c;
    const char* stage="window";
    int rc=-EINVAL, unlockRc=0;
    int64_t nextBefore=-1,nextAfter=-1;
    uint64_t device=0,inode=0,size=0;
    AHardwareBuffer_Desc d{};
    float matrix[16]{};
    unsigned char* copy=nullptr;
    size_t bytes=0;
    const char* path=nullptr;
    do {
        c.window=ANativeWindow_fromSurface(env,surface);
        if (!c.window) break;
        stage="next-before";
        rc=c.window->perform(c.window,NATIVE_WINDOW_GET_NEXT_FRAME_ID,&nextBefore);
        if (rc || nextBefore<=0) { if (!rc) rc=-EINVAL; break; }
        stage="last-queued";
        rc=ANativeWindow_getLastQueuedBuffer(c.window,&c.buffer,&c.fence,matrix);
        if (rc || !c.buffer) { if (!rc) rc=-ENODATA; break; }
        AHardwareBuffer_describe(c.buffer,&d);
        stage="description"; rc=-EINVAL;
        if (!d.width || d.width>1080 || !d.height || d.height>2000 || d.layers!=1 ||
            (d.format!=1 && d.format!=5) || d.stride<d.width || d.stride>2048 ||
            (d.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT)) break;
        for(float f:matrix) if (!isfinite(f)) { rc=-EDOM; break; }
        if (rc==-EDOM) break;
        stage="handle-accessor";
        // This platform-only accessor is read-only; no namespace-policy bypass.
        void* library=dlopen("libnativewindow.so",RTLD_NOW|RTLD_LOCAL);
        if (!library) { rc=-ENOSYS; break; }
        using GetHandle=const native_handle_t* (*)(const AHardwareBuffer*);
        auto getHandle=reinterpret_cast<GetHandle>(dlsym(library,"AHardwareBuffer_getNativeHandle"));
        const native_handle_t* n=getHandle ? getHandle(c.buffer) : nullptr;
        // The actual libnativewindow remains loaded through the application's dependencies.
        if (!n || n->version!=sizeof(native_handle_t) || n->numFds!=GRALLOC_HANDLE_NUM_FDS || n->numInts!=GRALLOC_HANDLE_NUM_INTS) {
            dlclose(library); rc=-EINVAL; break;
        }
        const auto* h=reinterpret_cast<const gralloc_handle_t*>(n);
        stage="linear-gbm-layout";
        bool valid=h->magic==GRALLOC_HANDLE_MAGIC && h->version==GRALLOC_HANDLE_VERSION &&
            h->width==d.width && h->height==d.height && h->format==d.format &&
            h->stride==uint64_t(d.stride)*4 && h->modifier==0 && h->prime_fd>=0;
        struct stat st{};
        int statRc=valid ? fstat(h->prime_fd,&st) : -1;
        dlclose(library);
        if (!valid || statRc || st.st_size<0 || !directLayout(d.width,d.height,d.stride,st.st_size)) { rc=-EINVAL; break; }
        device=st.st_dev;inode=st.st_ino;size=st.st_size;
        stage="acquire-fence";rc=waitFence(c.fence);if(rc)break;
        if(c.fence>=0) { close(c.fence);c.fence=-1; }
        bytes=size_t(d.width)*4*d.height;
        copy=static_cast<unsigned char*>(malloc(bytes));
        if(!copy){stage="allocation";rc=-ENOMEM;break;}
        void* address=nullptr;
        stage="cpu-read-lock";
        // Mapper2 cannot implement lockAndGetInfo. The exact GBM handle and
        // selected mapper's checked map-stride contract supply the pitch here.
        rc=AHardwareBuffer_lock(c.buffer,AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,-1,nullptr,&address);
        if(rc)break;
        bool packed=directPack(copy,bytes,address,size,d.width,d.height,d.stride);
        int release=-1;
        unlockRc=AHardwareBuffer_unlock(c.buffer,&release);
        int waitRc=waitFence(release);if(release>=0)close(release);
        stage="unlock";
        if(!packed || unlockRc || waitRc){rc=unlockRc ? unlockRc : waitRc ? waitRc : -EFAULT;break;}
        stage="next-after";
        rc=c.window->perform(c.window,NATIVE_WINDOW_GET_NEXT_FRAME_ID,&nextAfter);
        if(rc || nextBefore!=nextAfter){if(!rc)rc=-ESTALE;break;}
        path=env->GetStringUTFChars(filename,nullptr);
        if(!path){rc=-ENOMEM;break;}
        stage="write";
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0){rc=-errno;break;}
        size_t done=0;
        while(done<bytes){ssize_t nwrite=write(fd,copy+done,bytes-done);if(nwrite<=0){rc=nwrite<0 ? -errno : -EIO;break;}done+=nwrite;}
        if(done==bytes)rc=fsync(fd)==0 ? 0 : -errno;
        close(fd);
        if(!rc)stage="complete";
    } while(false);
    if(path)env->ReleaseStringUTFChars(filename,path);
    free(copy);
    char m[1024];size_t used=0;
    for(int i=0;i<16;i++)used+=snprintf(m+used,sizeof(m)-used,"%s%.9g",i ? "," : "",isfinite(matrix[i]) ? matrix[i] : 0.0f);
    char result[2048];
    snprintf(result,sizeof(result),"{\"rc\":%d,\"stage\":\"%s\",\"nextBefore\":%lld,\"nextAfter\":%lld,\"width\":%u,\"height\":%u,\"stridePixels\":%u,\"format\":%u,\"usage\":%llu,\"device\":%llu,\"inode\":%llu,\"allocationBytes\":%llu,\"packedBytes\":%zu,\"unlockRc\":%d,\"textureMatrix\":[%s]}",rc,stage,(long long)nextBefore,(long long)nextAfter,d.width,d.height,d.stride,d.format,(unsigned long long)d.usage,(unsigned long long)device,(unsigned long long)inode,(unsigned long long)size,bytes,unlockRc,m);
    return env->NewStringUTF(result);
}
