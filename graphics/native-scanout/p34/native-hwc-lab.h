// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
// Private headless-test ABI; counts composition objects, never kernel pins.
struct superman_hwc_lab_state {
    uint32_t size;
    uint32_t live_attempts;
    uint32_t latched_frames;
    uint32_t retired_compositions;
    uint32_t explicit_acquire_inputs;
    uint32_t active_composition;
    uint32_t restore_confirmed;
};
