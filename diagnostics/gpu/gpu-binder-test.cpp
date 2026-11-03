// SPDX-License-Identifier: Apache-2.0
// Explicit bounded Binder FD + standard Android allocator/mapper interoperability.
// No global HAL service replacement, SurfaceFlinger switch or framebuffer write.
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <binder/Binder.h>
#include <atomic>
#include <algorithm>
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/Parcel.h>
#include <android-base/unique_fd.h>
#include <hardware/gralloc.h>
#include <sync/sync.h>
#include "superman_graphics_hal.h"
#include "gpu-native-target.h"
extern char** environ;
using namespace android;
namespace M=android::hardware::graphics::mapper::V2_0;
namespace A=android::hardware::graphics::allocator::V2_0;
using android::hardware::hidl_handle;
using Error=M::Error;
static constexpr int width=720,height=1280,frames=6;
static constexpr uint64_t usage=GRALLOC_USAGE_HW_TEXTURE|GRALLOC_USAGE_HW_RENDER|GRALLOC_USAGE_SW_READ_OFTEN;
static const unsigned char colors[frames][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255},{0,255,255,255},{255,0,255,255}};
static double now(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static const hw_module_t* module(const char* path){
    void* lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);return lib?reinterpret_cast<const hw_module_t*>(dlsym(lib,HAL_MODULE_INFO_SYM_AS_STR)):nullptr;
}
struct Handle {
    native_handle_t* value=nullptr;
    ~Handle(){if(value){native_handle_close(value);native_handle_delete(value);}}
};
static void* import(const sp<M::IMapper>& mapper,const native_handle_t* raw){
    void* buffer=nullptr;hidl_handle handle;handle.setTo(const_cast<native_handle_t*>(raw),false);
    auto ret=mapper->importBuffer(handle,[&](Error error,void* result){if(error==Error::NONE)buffer=result;});
    return ret.isOk()?buffer:nullptr;
}
static bool freeImported(const sp<M::IMapper>& mapper,void* buffer){auto ret=mapper->freeBuffer(buffer);return ret.isOk()&&ret==Error::NONE;}
class Service:public BBinder {
    sp<M::IMapper> mapper;
    sp<A::IAllocator> allocator;
    const char* mesaPath;
    NativeTarget consumer;
    void* target=nullptr;
    int targetStride=0;
    pid_t child=0;
    void* allocateTarget(int format) {
        M::IMapper::BufferDescriptorInfo info={width,height,1,static_cast<hardware::graphics::common::V1_0::PixelFormat>(format),usage};
        M::BufferDescriptor descriptor;Error status=Error::BAD_DESCRIPTOR;
        auto created=mapper->createDescriptor(info,[&](Error e,const auto& d){status=e;descriptor=d;});
        if(!created.isOk()||status!=Error::NONE)return nullptr;
        void* buffer=nullptr;
        auto allocated=allocator->allocate(descriptor,1,[&](Error e,uint32_t stride,const auto& buffers){if(e==Error::NONE&&buffers.size()==1){targetStride=stride;buffer=import(mapper,buffers[0].getNativeHandle());}});
        return allocated.isOk()?buffer:nullptr;
    }
    std::atomic<int> completed{0};
public:
    Service(const hw_module_t* mod,const char* mesa):mapper(supermanCreateMapper(mod)),allocator(supermanCreateAllocator(mod)),mesaPath(mesa){}
    bool valid()const{return mapper&&allocator;}
    void setChild(pid_t pid){child=pid;}
    int frameCount()const{return completed.load();}
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags)override{
        if(code!=1&&code!=2)return BBinder::onTransact(code,data,reply,flags);
        if(IPCThreadState::self()->getCallingPid()!=child)return PERMISSION_DENIED;
        if(code==1){
            const int format=data.readInt32();if(format!=HAL_PIXEL_FORMAT_RGBA_8888&&format!=HAL_PIXEL_FORMAT_BGRA_8888)return BAD_VALUE;
            M::IMapper::BufferDescriptorInfo info={width,height,1,static_cast<hardware::graphics::common::V1_0::PixelFormat>(format),usage};
            M::BufferDescriptor descriptor;Error status=Error::BAD_DESCRIPTOR;
            auto created=mapper->createDescriptor(info,[&](Error e,const auto& d){status=e;descriptor=d;});
            if(!created.isOk()||status!=Error::NONE)return BAD_VALUE;
            status_t out=BAD_VALUE;
            auto allocated=allocator->allocate(descriptor,1,[&](Error e,uint32_t stride,const auto& buffers){
                if(e!=Error::NONE||buffers.size()!=1)return;
                out=reply->writeInt32(stride);if(!out)out=reply->writeNativeHandle(buffers[0].getNativeHandle());
            });
            std::printf("BINDER allocate format=%d status=%d parent_pid=%d\n",format,out,getpid());
            return allocated.isOk()?out:BAD_VALUE;
        }
        int frame=data.readInt32(),format=data.readInt32(),stride=data.readInt32();
        if(frame!=completed||frame>=frames||stride<width||stride>8192||
           (format!=HAL_PIXEL_FORMAT_RGBA_8888&&format!=HAL_PIXEL_FORMAT_BGRA_8888))return BAD_VALUE;
        Handle raw;raw.value=data.readNativeHandle();int fence=data.readFileDescriptor();
        if(!raw.value||fence<0||sync_wait(fence,5000))return BAD_VALUE;
        void* imported=import(mapper,raw.value);if(!imported)return BAD_VALUE;
        if(frame%3==0){
            target=allocateTarget(format);
            if(!target||!consumer.initialize(mesaPath)||!consumer.bind(static_cast<buffer_handle_t>(target),width,height,format,targetStride,usage))return BAD_VALUE;
        }
        base::unique_fd composed(consumer.compose(static_cast<buffer_handle_t>(imported),width,height,format,stride,usage));
        if(composed<0||!freeImported(mapper,imported))return BAD_VALUE;
        imported=target;stride=targetStride;
        Handle fenceHandle;fenceHandle.value=native_handle_create(1,0);if(!fenceHandle.value)return NO_MEMORY;
        fenceHandle.value->data[0]=dup(composed.get());if(fenceHandle.value->data[0]<0)return BAD_VALUE;
        hidl_handle acquire;acquire.setTo(fenceHandle.value,false);void* pixels=nullptr;Error lockError=Error::BAD_VALUE;
        auto locked=mapper->lock(imported,GRALLOC_USAGE_SW_READ_OFTEN,{0,0,width,height},acquire,[&](Error e,void* ptr){lockError=e;pixels=ptr;});
        if(!locked.isOk()||lockError!=Error::NONE||!pixels)return BAD_VALUE;
        bool ok=true;unsigned char expected[4];std::memcpy(expected,colors[frame],4);
        if(format==HAL_PIXEL_FORMAT_BGRA_8888)std::swap(expected[0],expected[2]);
        for(int quadrant=0;quadrant<4;++quadrant){
            int x=(quadrant%2)*width/2+width/4,y=(quadrant/2)*height/2+height/4;
            const auto* pixel=static_cast<const unsigned char*>(pixels)+(size_t(y)*stride+x)*4;
            if(std::memcmp(pixel,expected,4)){std::fprintf(stderr,"BINDER pixel_mismatch frame=%d format=%d quadrant=%d bytes=%u,%u,%u,%u expected=%u,%u,%u,%u\n",frame,format,quadrant,pixel[0],pixel[1],pixel[2],pixel[3],expected[0],expected[1],expected[2],expected[3]);ok=false;}
        }
        Error unlockError=Error::BAD_VALUE;
        auto unlocked=mapper->unlock(imported,[&](Error e,const hidl_handle& h){unlockError=e;if(h.getNativeHandle()&&h.getNativeHandle()->numFds==1&&sync_wait(h.getNativeHandle()->data[0],5000))ok=false;});
        ok=unlocked.isOk()&&unlockError==Error::NONE&&ok;
        if(frame%3==2){ok=consumer.close()&&freeImported(mapper,target)&&ok;target=nullptr;}
        if(!ok)return BAD_VALUE;++completed;
        std::printf("BINDER verified frame=%d format=%d stride=%d distinct_pids=true native_fence=true sparse_pixels=4 consumer_GPU_composition=true CPU_frame_copy=false\n",frame,format,stride);
        return reply->writeInt32(0);
    }
};
static int client(const char* name,const char* mesa,const char* hal){
    alarm(25);ProcessState::self()->startThreadPool();sp<IBinder> remote;
    for(int attempt=0;attempt<100&&!remote;++attempt){remote=defaultServiceManager()->checkService(String16(name));if(!remote)usleep(50000);}
    const hw_module_t* mod=module(hal);sp<M::IMapper> mapper=supermanCreateMapper(mod);
    if(!remote||!mapper)return 1;
    NativeTarget gl;if(!gl.initialize(mesa))return 1;
    for(int group=0;group<2;++group){
        int format=group?HAL_PIXEL_FORMAT_BGRA_8888:HAL_PIXEL_FORMAT_RGBA_8888;
        Parcel request,response;request.writeInt32(format);
        if(remote->transact(1,request,&response))return 1;
        int stride=response.readInt32();Handle raw;raw.value=response.readNativeHandle();if(!raw.value||stride<width)return 1;
        void* imported=import(mapper,raw.value);if(!imported)return 1;
        // Mapper clones the incoming Binder handle; the caller can close its copy.
        native_handle_close(raw.value);native_handle_delete(raw.value);raw.value=nullptr;
        if(!gl.bind(static_cast<buffer_handle_t>(imported),width,height,format,stride,usage))return 1;
        for(int n=0;n<3;++n){
            int frame=group*3+n;double begin=now();base::unique_fd fence(gl.render(frame));if(fence<0)return 1;
            Parcel check,answer;check.writeInt32(frame);check.writeInt32(format);check.writeInt32(stride);
            if(check.writeNativeHandle(static_cast<buffer_handle_t>(imported))||check.writeDupFileDescriptor(fence.get())||remote->transact(2,check,&answer)||answer.readInt32())return 1;
            std::printf("BINDER frame=%d format=%d GPU_Binder_map_verify_ms=%.3f child_pid=%d\n",frame,format,now()-begin,getpid());
        }
        if(!gl.releaseTarget()||!freeImported(mapper,imported))return 1;
        auto twice=mapper->freeBuffer(imported);if(!twice.isOk()||twice!=Error::BAD_BUFFER)return 1;
    }
    if(!gl.close())return 1;
    std::printf("BINDER client_cleanup=PASS\n");return 0;
}
int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==5&&!std::strcmp(argv[1],"--client"))return client(argv[2],argv[3],argv[4]);
    if(argc!=6||std::strcmp(argv[1],"--run-six-frames")||std::strcmp(argv[2],"--mesa-library")||std::strcmp(argv[4],"--gralloc-library")||std::strncmp(argv[3],"/data/local/tmp/",16)||std::strncmp(argv[5],"/data/local/tmp/",16)){std::fprintf(stderr,"Usage: --run-six-frames --mesa-library /data/local/tmp/... --gralloc-library /data/local/tmp/...\n");return 2;}
    alarm(30);auto* mod=module(argv[5]);sp<Service> service=new Service(mod,argv[3]);if(!service->valid())return 1;
    // One explicit pool thread owns the consumer EGL context; no extra driver threads.
    if(ProcessState::self()->setThreadPoolMaxThreadCount(0))return 1;
    std::string name="superman.gpu.p28."+std::to_string(getpid());pid_t child;
    char* childArgs[]={argv[0],const_cast<char*>("--client"),const_cast<char*>(name.c_str()),argv[3],argv[5],nullptr};
    int spawned=posix_spawn(&child,argv[0],nullptr,nullptr,childArgs,environ);if(spawned)return 1;
    service->setChild(child);
    if(defaultServiceManager()->addService(String16(name.c_str()),service)){kill(child,SIGTERM);waitpid(child,nullptr,0);return 1;}
    std::printf("BINDER service=%s parent_pid=%d child_pid=%d\n",name.c_str(),getpid(),child);
    ProcessState::self()->startThreadPool();int status=0;if(waitpid(child,&status,0)!=child)return 1;
    bool passed=WIFEXITED(status)&&WEXITSTATUS(status)==0&&service->frameCount()==frames;
    std::printf("BINDER result=%s frames=%d standard_IAllocator_IMapper=true Binder_FD_transport=true cross_process=true consumer_GPU_composition=true SurfaceFlinger=false scanout=false sustained_fps_claim=false\n",passed?"PASS":"FAIL",service->frameCount());
    return passed?0:1;
}
