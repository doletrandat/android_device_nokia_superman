// SPDX-License-Identifier: Apache-2.0
// Exercises the actual adapter hooks with a memory-only fake framebuffer.
#include <hwc2onfbadapter/HWC2OnFbAdapter.h>
#include <hardware/fb.h>
#include <system/graphics.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
static int posts=0,postError=0;
static int post(framebuffer_device_t*,buffer_handle_t){++posts;return postError;}
static int closeFb(hw_device_t*){return 0;}
int main(int argc,char** argv){
    if(argc!=2||std::strcmp(argv[1],"--run-memory-only"))return 2;
    alarm(10);std::setvbuf(stdout,nullptr,_IONBF,0);bool pass=true;
    const framebuffer_device_t desc={.common={.tag=HARDWARE_DEVICE_TAG,.version=0,.module=nullptr,.close=closeFb},.flags=0,.width=720,.height=1280,.stride=720,.format=HAL_PIXEL_FORMAT_BGRA_8888,.xdpi=320,.ydpi=320,.fps=60,.minSwapInterval=0,.maxSwapInterval=1,.numFramebuffers=1,.post=post};
    auto check=[&](bool ok,const char* name){std::printf("HWC_BRIDGE %s=%s\n",name,ok?"PASS":"FAIL");pass&=ok;};
    for(bool bounded:{false,true}){
        framebuffer_device_t fb=desc;android::HWC2OnFbAdapter adapter(&fb,bounded);hwc2_device_t* dev=&adapter;
        auto support=reinterpret_cast<HWC2_PFN_GET_CLIENT_TARGET_SUPPORT>(dev->getFunction(dev,HWC2_FUNCTION_GET_CLIENT_TARGET_SUPPORT));
        auto target=reinterpret_cast<HWC2_PFN_SET_CLIENT_TARGET>(dev->getFunction(dev,HWC2_FUNCTION_SET_CLIENT_TARGET));
        auto present=reinterpret_cast<HWC2_PFN_PRESENT_DISPLAY>(dev->getFunction(dev,HWC2_FUNCTION_PRESENT_DISPLAY));
        if(!support||!target||!present)return 1;auto display=adapter.getDisplayId();
        check(support(dev,display,720,1280,HAL_PIXEL_FORMAT_BGRA_8888,HAL_DATASPACE_UNKNOWN)==HWC2_ERROR_NONE,"unknown_dataspace");
        check(support(dev,display,720,1280,HAL_PIXEL_FORMAT_BGRA_8888,HAL_DATASPACE_V0_SRGB)==(bounded?HWC2_ERROR_NONE:HWC2_ERROR_UNSUPPORTED),"gated_v0_srgb");
        check(support(dev,display,720,1280,HAL_PIXEL_FORMAT_BGRA_8888,HAL_DATASPACE_SRGB)==(bounded?HWC2_ERROR_NONE:HWC2_ERROR_UNSUPPORTED),"gated_legacy_srgb");
        check(support(dev,display,720,1280,HAL_PIXEL_FORMAT_BGRA_8888,HAL_DATASPACE_DISPLAY_P3)==HWC2_ERROR_UNSUPPORTED,"reject_wide_color");
        check(support(dev,display,720,1280,HAL_PIXEL_FORMAT_RGBA_8888,HAL_DATASPACE_UNKNOWN)==HWC2_ERROR_UNSUPPORTED,"reject_wrong_format");
        hwc_region_t damage{};native_handle_t raw={.version=sizeof(native_handle_t),.numFds=0,.numInts=0};
        check(target(dev,display,&raw,-1,bounded?HAL_DATASPACE_V0_SRGB:HAL_DATASPACE_UNKNOWN,damage)==HWC2_ERROR_NONE,"set_target");
        int fence=-2;adapter.setState(android::HWC2OnFbAdapter::State::VALIDATED);postError=-EIO;int previous=posts;
        check(present(dev,display,&fence)==(bounded?HWC2_ERROR_NO_RESOURCES:HWC2_ERROR_NONE)&&posts==previous+1&&fence==-1,"gated_post_error");
        postError=0;check(present(dev,display,&fence)==HWC2_ERROR_NONE,"post_success");
        if(bounded){
            int invalid=open("/dev/null",O_RDONLY|O_CLOEXEC);if(invalid<0)return 1;
            auto began=std::chrono::steady_clock::now();int result=target(dev,display,&raw,invalid,HAL_DATASPACE_V0_SRGB,damage);
            auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count();
            check(result==HWC2_ERROR_NO_RESOURCES&&ms<3500,"invalid_fence_fails_finitely");errno=0;check(fcntl(invalid,F_GETFD)==-1&&errno==EBADF,"fence_fd_closed");
        }
        adapter.close();
    }
    std::printf("HWC_BRIDGE result=%s memory_only=true physical_display_write=false\n",pass?"PASS":"FAIL");return pass?0:1;
}
