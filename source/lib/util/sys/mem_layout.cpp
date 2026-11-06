#define SDV_SOFT_REGIONS 1
#define SDV_SKIP_LOGLayout 1
#include "mem_layout.hpp"

#include <lib/log/logger_mgr.hpp>
#include <lib/util/strings.hpp>
#include <program/loggers.hpp>
#include <program/setting.hpp>


/* Provided by linkerscript, the start of our executable. */
extern "C" {
    extern char __module_start;
}

namespace exl::util {

    #if defined EXL_AS_MODULE
    namespace mem_layout {
        ModuleIndex s_SelfModuleIdx = ModuleIndex::End;
    }
    #endif

    namespace impl::mem_layout {
        std::array<ModuleInfo, static_cast<int>(ModuleIndex::End)> s_ModuleInfos;
        std::bitset<static_cast<int>(ModuleIndex::End)> s_ModuleBitset;
    }

    static void FindModules() {
        /* Setup loop, starting address zero. */
        MemoryInfo meminfo { };
        u32 pageinfo;

        auto& moduleInfos = impl::mem_layout::s_ModuleInfos;
        auto& moduleBitset = impl::mem_layout::s_ModuleBitset;

        enum class State {
            LookingForCodeStatic,
            ExpectingRodata,
            ExpectingData,
        } state = State::LookingForCodeStatic;
        int nextModIdx = static_cast<int>(ModuleIndex::Start);

        uintptr_t offset = 0;
        uintptr_t prevOffset = 0;
        /* Bound the scan.  On hardware the walk ends when the address space
         * wraps, but an emulator's svcQueryMemory can return a zero-sized
         * region at the top, so addr+size stops advancing and the loop never
         * terminates.  Hitting the cap is harmless: the module list is already
         * complete long before it. */
        int scanned = 0;
        util::ModuleInfo curModInfo {};

        /* Utility to reset state.  */
        auto reset = [&curModInfo, &state] {
            curModInfo = {};
            state =  State::LookingForCodeStatic;
        };

        do {
            /* Ensure we don't find too many modules. */
            if(static_cast<int>(util::ModuleIndex::End) <= nextModIdx)
                R_ABORT_UNLESS(result::TooManyStaticModules);

            prevOffset = offset;

            /* Query next range. */
            R_ABORT_UNLESS(svcQueryMemory(&meminfo, &pageinfo, meminfo.addr + meminfo.size));

            /* Setup variables for state machine. */
            u32 memtype = meminfo.type & MemState_Type;
            offset = meminfo.addr;

            switch (state) {
                case State::LookingForCodeStatic: {
                    if(memtype != MemType_CodeStatic || meminfo.perm != Perm_Rx) {
                        /* No module here, keep going... */
                        continue;
                    }

                    /* Store text/total info and move to next state. */
                    curModInfo.m_Total.m_Start = meminfo.addr;
                    curModInfo.m_Text.m_Start = meminfo.addr;
                    curModInfo.m_Text.m_Size = meminfo.size;
                    state =  State::ExpectingRodata;
                    break;
                }
                case State::ExpectingRodata: {
                    if(memtype != MemType_CodeStatic || meminfo.perm != Perm_R) {
                        /* Not a proper module, reset. */
                        reset();
                        continue;
                    }

                    /* Store rodata range and move to next state. */
                    curModInfo.m_Rodata.m_Start = meminfo.addr;
                    curModInfo.m_Rodata.m_Size = meminfo.size;
                    state =  State::ExpectingData;
                    break;
                }
                case State::ExpectingData: {
                    if(memtype != MemType_CodeMutable || meminfo.perm != Perm_Rw) {
                        /* Not a proper module, reset. */
                        reset();
                        continue;
                    }

                    auto& total = curModInfo.m_Total;
                    auto& data = curModInfo.m_Data;

                    /* Assign the data range and finish total range. */
                    data.m_Start = meminfo.addr;
                    data.m_Size = meminfo.size;
                    total.m_Size = data.GetEnd() - total.m_Start;

                    /* Get the MOD0 pointer. */
                    struct {
                        std::uint32_t m_EntrypointInstruction;
                        std::uint32_t m_Mod0Offset;
                    }* header = reinterpret_cast<decltype(header)>(curModInfo.m_Text.m_Start);
                    curModInfo.m_Mod = reinterpret_cast<const Mod0*>(curModInfo.m_Text.m_Start + header->m_Mod0Offset);

                    /* Store built module info. */
                    moduleInfos[nextModIdx] = curModInfo;
                    moduleBitset[nextModIdx] = true;
                    nextModIdx++;

                    /* Back to initial state. */
                    reset();
                    break;
                }
                EXL_UNREACHABLE_DEFAULT_CASE();
            }

        /* Exit once we've wrapped the address space. */
        } while(offset >= prevOffset && meminfo.size != 0 && ++scanned < 0x10000);

        Logging.Log(EXL_LOG_PREFIX "mem layout: %d regions, %d modules", scanned, nextModIdx);

        /* Diagnostic encoding: a failed check freezes the process instead of
         * aborting, so the console shows "stuck on launching" rather than the
         * error dialog, and the two failure modes stay distinguishable. */
        #define SDV_DIAG_STUCK() do { for (;;) { __asm__ volatile("yield"); } } while (0)

        /* We must find at least one module. */
        if (!moduleBitset[static_cast<int>(ModuleIndex::Rtld)]) SDV_DIAG_STUCK();

        #ifdef EXL_AS_MODULE
        /* There also must be a main module. */
        if (!moduleBitset[static_cast<int>(ModuleIndex::Main)]) SDV_DIAG_STUCK();
        /* Move the last module found to the sdk index, if it's not there already. */
        if(nextModIdx != static_cast<int>(ModuleIndex::End)) {
            moduleInfos[static_cast<int>(ModuleIndex::Sdk)] = moduleInfos[nextModIdx-1];
            moduleInfos[nextModIdx-1] = {};
            moduleBitset[static_cast<int>(ModuleIndex::Sdk)] = true;
            moduleBitset[nextModIdx-1] = false;
        }

        /* Note where our current module is. */
        size_t i = 0;
        for(const auto& info : moduleInfos) {
            /* Ignore if this module is not found. */
            if(!moduleBitset[i]) {
                i++;
                continue;
            }

            /* If the start of the module is the start of us, we found ourselves. */
            if(info.m_Total.m_Start == reinterpret_cast<uintptr_t>(&__module_start)) {
                mem_layout::s_SelfModuleIdx = static_cast<ModuleIndex>(i);
                break;
            }
            i++;
        }

        /* Ensure we found a valid self index. */
        if (mem_layout::s_SelfModuleIdx >= ModuleIndex::End) SDV_DIAG_STUCK();
        if (!moduleBitset[static_cast<int>(mem_layout::s_SelfModuleIdx)]) SDV_DIAG_STUCK();
        #endif
    }

    static Result TryGetAddressFromInfo(InfoType type, uintptr_t* ptr) {
        return svcGetInfo(static_cast<u64*>(ptr), type, CUR_PROCESS_HANDLE, 0);
    }

    [[maybe_unused]] static uintptr_t GetAddressFromInfo(InfoType type) {
        uintptr_t addr;
        R_ABORT_UNLESS(TryGetAddressFromInfo(type, &addr));
        return addr;
    }

    [[maybe_unused]] static Result InferAslrAndStack() {
        Result rc = svcUnmapMemory((void*)0xFFFFFFFFFFFFE000UL, (void*)0xFFFFFE000UL, 0x1000);
        if (R_VALUE(rc) == KERNELRESULT(InvalidMemoryState)) {
            // Invalid src-address error means that a valid 36-bit address was rejected.
            // Thus we are 32-bit.
            util::mem_layout::s_Aslr    = util::Range(0x200000ull, 0x100000000ull);
            util::mem_layout::s_Stack   = util::Range(0x200000ull, 0x40000000ull);
        }
        else if (R_VALUE(rc) == KERNELRESULT(InvalidMemoryRange)) {
            // Invalid dst-address error means our 36-bit src-address was valid.
            // Thus we are 36-bit.
            util::mem_layout::s_Aslr    = util::Range(0x8000000ull, 0x1000000000ull);
            util::mem_layout::s_Stack   = util::Range(0x8000000ull, 0x80000000ull);
        }
        else {
            // Wat.
            return MAKERESULT(Module_Libnx, LibnxError_WeirdKernel);
        }

        return result::Success;
    }

    static void FindRegions() {
#ifdef SDV_SOFT_REGIONS
        /* Same tolerance as virtmem: if the kernel refuses these queries, fall
         * back to the 36-bit layout instead of aborting the process. */
        uintptr_t a = 0, b = 0;
        if (R_SUCCEEDED(TryGetAddressFromInfo(InfoType_AliasRegionAddress, &a)) &&
            R_SUCCEEDED(TryGetAddressFromInfo(InfoType_AliasRegionSize,    &b)))
            util::mem_layout::s_Alias = util::Range(a, b);
        else
            util::mem_layout::s_Alias = util::Range(0, 0);

        if (R_SUCCEEDED(TryGetAddressFromInfo(InfoType_HeapRegionAddress, &a)) &&
            R_SUCCEEDED(TryGetAddressFromInfo(InfoType_HeapRegionSize,    &b)))
            util::mem_layout::s_Heap = util::Range(a, b);
        else
            util::mem_layout::s_Heap = util::Range(0, 0);

        mem_layout::s_Aslr  = util::Range(0x8000000ull, 0x1000000000ull);
        mem_layout::s_Stack = util::Range(0x8000000ull, 0x80000000ull);
#else
        util::mem_layout::s_Alias = util::Range(
            GetAddressFromInfo(InfoType_AliasRegionAddress),
            GetAddressFromInfo(InfoType_AliasRegionSize)
        );

        util::mem_layout::s_Heap = util::Range(
            GetAddressFromInfo(InfoType_HeapRegionAddress),
            GetAddressFromInfo(InfoType_HeapRegionSize)
        );

        if(R_FAILED(TryGetAddressFromInfo(InfoType_AslrRegionAddress,   &mem_layout::s_Aslr.m_Start)) ||
           R_FAILED(TryGetAddressFromInfo(InfoType_AslrRegionSize,      &mem_layout::s_Aslr.m_Size)) ||
           R_FAILED(TryGetAddressFromInfo(InfoType_StackRegionAddress,  &mem_layout::s_Stack.m_Start)) ||
           R_FAILED(TryGetAddressFromInfo(InfoType_StackRegionSize,     &mem_layout::s_Stack.m_Size))
        )
        {
            R_ASSERT(InferAslrAndStack());
        }
#endif
    }

    [[maybe_unused]] static void LogLayout() {
        Logging.Log(EXL_LOG_PREFIX "Static module layout:");

        /* Collect up the module names and measure the longest length. */
        std::string_view moduleNames[static_cast<size_t>(ModuleIndex::End)];
        size_t maxModuleName = 0;
        for(size_t i = 0; i < static_cast<size_t>(ModuleIndex::End); i++) {
            /* Ignore if this module is not found. */
            if(!impl::mem_layout::s_ModuleBitset[i])
                continue;

            const auto& mod = GetModuleInfo(static_cast<ModuleIndex>(i));
            moduleNames[i] = mod.GetModuleName();
            maxModuleName = std::max(maxModuleName, moduleNames[i].size());
        }

        /* Print the module names. */
        for(size_t i = 0; i < static_cast<size_t>(ModuleIndex::End); i++) {
            /* Ignore if this module is not found. */
            if(!impl::mem_layout::s_ModuleBitset[i])
                continue;

            const auto& mod = GetModuleInfo(static_cast<ModuleIndex>(i));

            /* Copy name into it's own buffer so it can be null terminated. */
            char nameBuffer[util::ModuleInfo::s_ModulePathLengthMax+1];
            util::CopyString(nameBuffer, moduleNames[i]);

            Logging.Log(EXL_LOG_PREFIX "[ %-*s ]: \t%016lx-%016lx", maxModuleName, nameBuffer, mod.m_Total.m_Start, mod.m_Total.GetEnd());
        }

        Logging.Log("");
        Logging.Log(EXL_LOG_PREFIX "Alias: \t%016lx-%016lx", mem_layout::s_Alias.m_Start,    mem_layout::s_Alias.GetEnd());
        Logging.Log(EXL_LOG_PREFIX "Heap: \t%016lx-%016lx",  mem_layout::s_Heap.m_Start,     mem_layout::s_Heap.GetEnd());
        Logging.Log(EXL_LOG_PREFIX "Aslr: \t%016lx-%016lx",  mem_layout::s_Aslr.m_Start,     mem_layout::s_Aslr.GetEnd());
        Logging.Log(EXL_LOG_PREFIX "Stack: \t%016lx-%016lx", mem_layout::s_Stack.m_Start,    mem_layout::s_Stack.GetEnd());
    }

    void impl::InitMemLayout() {
        FindModules();
        FindRegions();

#ifndef SDV_SKIP_LOGLayout
        if(Logging.IsEnabled())
            LogLayout();
#endif
    }
};