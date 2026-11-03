// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <android/hardware/graphics/allocator/2.0/IAllocator.h>
#include <android/hardware/graphics/mapper/2.0/IMapper.h>
#include <hardware/hardware.h>
// Explicit module injection reuses Android's standard gralloc0 adapters. No
// service registration or module selection occurs in these constructors.
android::hardware::graphics::mapper::V2_0::IMapper*
supermanCreateMapper(const hw_module_t* module);
android::hardware::graphics::allocator::V2_0::IAllocator*
supermanCreateAllocator(const hw_module_t* module);
