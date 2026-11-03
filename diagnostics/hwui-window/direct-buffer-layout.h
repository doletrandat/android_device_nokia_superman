#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
inline bool directLayout(uint32_t width,uint32_t height,uint32_t stride,uint64_t allocation) {
    return width && width<=1080 && height && height<=2000 && stride>=width && stride<=2048 &&
        uint64_t(stride)*4*height<=allocation;
}
inline bool directPack(void* destination,size_t capacity,const void* source,uint64_t allocation,
                       uint32_t width,uint32_t height,uint32_t stride) {
    if (!destination || !source || !directLayout(width,height,stride,allocation) ||
        uint64_t(width)*4*height>capacity) return false;
    for(uint32_t y=0;y<height;y++)
        memcpy(static_cast<unsigned char*>(destination)+size_t(y)*width*4,
               static_cast<const unsigned char*>(source)+size_t(y)*stride*4,width*4);
    return true;
}
