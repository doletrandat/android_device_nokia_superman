// SPDX-License-Identifier: Apache-2.0
// Calls the original dequeue once. Never waits/closes/duplicates the fence or
// maps/writes/locks a buffer. Handle decoding uses the actual selected ABI.
struct BufferAgreement {
    unsigned dequeues=0;
    bool valid=false;
    bool failed=false;
    bool ready() const { return dequeues>0 && valid && !failed; }
};
static int observe_dequeue(ANativeWindow* window, ANativeWindow_dequeueBufferFn next,
                           void* data, ANativeWindowBuffer** buffer, int* fence) {
    auto* agreement=static_cast<BufferAgreement*>(data);
    const auto start=probe_ns();const int ret=next(window,buffer,fence);
    if(agreement) { ++agreement->dequeues;agreement->valid=false; }
    if(ret || !buffer || !*buffer) {
        if(agreement) agreement->failed=true;
        std::printf("NATIVE_SF_BUFFER dequeue_status=%d buffer_present=%d wall_ns=%" PRIu64 "\n",ret,buffer && *buffer,probe_ns()-start);return ret;
    }
    const ANativeWindowBuffer* b=*buffer;const auto* h=b->handle;
    std::printf("NATIVE_SF_BUFFER width=%d height=%d pixel_stride=%d format=%d usage=0x%" PRIx64 " acquire_fd=%d wall_ns=%" PRIu64 "\n",b->width,b->height,b->stride,b->format,static_cast<uint64_t>(b->usage),fence?*fence:-2,probe_ns()-start);
    if(!h || h->version!=sizeof(native_handle_t) || h->numFds!=GRALLOC_HANDLE_NUM_FDS || h->numInts!=GRALLOC_HANDLE_NUM_INTS) {
        if(agreement) agreement->failed=true;
        std::printf("NATIVE_SF_HANDLE recognized=false reason=ABI_shape diagnostic_only=true\n");return ret;
    }
    const auto* g=reinterpret_cast<const gralloc_handle_t*>(h);
    if(g->magic!=GRALLOC_HANDLE_MAGIC || g->version!=GRALLOC_HANDLE_VERSION) {
        if(agreement) agreement->failed=true;
        std::printf("NATIVE_SF_HANDLE recognized=false magic=0x%x version=%u diagnostic_only=true\n",g->magic,g->version);return ret;
    }
    struct stat info{};const bool sized=g->prime_fd>=0 && fstat(g->prime_fd,&info)==0;
    const uint64_t extent=static_cast<uint64_t>(g->stride)*g->height;
    std::printf("NATIVE_SF_HANDLE recognized=true magic=0x%x version=%u width=%u height=%u Android_format=%u byte_stride=%u modifier=0x%" PRIx64 " DMA_BUF_size=%" PRId64 " extent=%" PRIu64 " sized=%d metadata_match=%d diagnostic_only=true\n",g->magic,g->version,g->width,g->height,g->format,g->stride,g->modifier,static_cast<int64_t>(info.st_size),extent,sized,g->width==static_cast<unsigned>(b->width) && g->height==static_cast<unsigned>(b->height) && g->format==static_cast<unsigned>(b->format) && g->stride==static_cast<unsigned>(b->stride)*4);
    const bool matches=b->width==720 && b->height==1280 && b->stride>=720 &&
        b->format==5 && g->width==720 && g->height==1280 && g->format==5 &&
        g->stride==static_cast<uint64_t>(b->stride)*4 && g->modifier==0 &&
        sized && info.st_size>0 && extent<=static_cast<uint64_t>(info.st_size);
    if(agreement) { agreement->valid=matches;agreement->failed|=!matches; }
    std::printf("NATIVE_SF_BUFFER_AGREEMENT expected_visual=5 geometry=720x1280 match=%d dequeues=%u queue_allowed=%d fence_unchanged=true\n",matches,agreement?agreement->dequeues:0,agreement && agreement->ready());
    return ret;
}
