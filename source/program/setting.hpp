#pragma once

#include "common.hpp"

#define EXL_MODULE_NAME "exlaunch"

#define EXL_DEBUG
#define EXL_USE_FAKEHEAP

/*
#define EXL_SUPPORTS_REBOOTPAYLOAD
*/

namespace exl::setting {
    /* How large the fake .bss heap will be. */
    constexpr size_t HeapSize = 0x5000;

    /* How large the JIT area will be for hooks. */
    /* v287: the hook library carves trampolines out of this pool, and each
     * one costs 5*10 u32 = 200 bytes, so 0x1000 was only 20 hooks.  This mod
     * installs more than that, and the 21st one wrote past the end of the
     * array (see the off-by-one in AllocForTrampoline) -- the console refused
     * to boot.  0x4000 gives 81 slots. */
    constexpr size_t JitSize = 0x4000;

    /* How large the area will be inline hook pool. */
    constexpr size_t InlinePoolSize = 0x1000;

    /* How large the formatting buffer should be for logging. The buffer will be on the stack. */
    constexpr size_t LogBufferSize = 512;

    /* Sanity checks. */
    static_assert(ALIGN_UP(JitSize, PAGE_SIZE) == JitSize, "");
    static_assert(ALIGN_UP(InlinePoolSize, PAGE_SIZE) == InlinePoolSize, "");
}
