// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "SupermanGraphicsHal"
#include "superman_graphics_hal.h"
#include <mapper-passthrough/2.0/GrallocLoader.h>
#include <allocator-passthrough/2.0/GrallocLoader.h>
#include <cstring>
#include <hardware/gralloc.h>
static bool valid(const hw_module_t* module) {
    if (!module || module->tag != HARDWARE_MODULE_TAG || !module->id ||
        std::strcmp(module->id, GRALLOC_HARDWARE_MODULE_ID) ||
        (module->module_api_version >> 8) != 0 || !module->methods || !module->methods->open) return false;
    auto* gralloc = reinterpret_cast<const gralloc_module_t*>(module);
    return gralloc->registerBuffer && gralloc->unregisterBuffer && gralloc->lock && gralloc->unlock;
}
android::hardware::graphics::mapper::V2_0::IMapper*
supermanCreateMapper(const hw_module_t* module) {
    if (!valid(module)) return nullptr;
    using Loader = android::hardware::graphics::mapper::V2_0::passthrough::GrallocLoader;
    auto hal = Loader::createHal(module);
    return hal ? Loader::createMapper(std::move(hal)) : nullptr;
}
android::hardware::graphics::allocator::V2_0::IAllocator*
supermanCreateAllocator(const hw_module_t* module) {
    if (!valid(module)) return nullptr;
    using Loader = android::hardware::graphics::allocator::V2_0::passthrough::GrallocLoader;
    auto hal = Loader::createHal(module);
    return hal ? Loader::createAllocator(std::move(hal)) : nullptr;
}
