// HACK (Leonida CE boot): the processors the title is told about.
//
// GTA VI sizes its job system from GetLogicalProcessorInformation (and pins workers with Get/SetThreadGroupAffinity).
// Under Wine that call describes every host processor (128 on the test machine), so the title runs far more job
// threads at once than on the console's 8 cores / 16 threads, and a race in one of its arena allocators (two owners
// for one block, then a crash at 120-140 s; hacks/BOOT_NOTES.md) shows up. The answer is cut to the first
// LEONIDA_CE_CPUS logical processors (off by default; 16 is the console count): entries whose mask lies
// wholly above are dropped and the others keep only the bits below.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "shims.h"

namespace {

unsigned ce_cpu_limit() {
    static const unsigned limit = [] {
        char v[16] = {};
        if (!GetEnvironmentVariableA("LEONIDA_CE_CPUS", v, sizeof(v))) return 0u;
        return static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    }();
    return limit;
}

using GlpiFn = BOOL(WINAPI*)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);

BOOL WINAPI CeGetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer, PDWORD length) {
    static const auto real =
        reinterpret_cast<GlpiFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLogicalProcessorInformation"));
    if (!real || !length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const unsigned limit = ce_cpu_limit();
    if (limit == 0 || limit >= 64) return real(buffer, length);

    DWORD bytes = 0;
    real(nullptr, &bytes);
    if (!bytes) return real(buffer, length);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> all(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
    DWORD got = static_cast<DWORD>(all.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!real(all.data(), &got)) return FALSE;
    all.resize(got / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));

    const ULONG_PTR keep = (ULONG_PTR(1) << limit) - 1;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> cut;
    for (const auto& e : all) {
        if (!(e.ProcessorMask & keep)) continue;
        auto c = e;
        c.ProcessorMask &= keep;
        cut.push_back(c);
    }
    static bool told = false;
    if (!told) {
        told = true;
        std::fprintf(stderr, "[rt] sysinfo: GetLogicalProcessorInformation cut to %u logical processors (%zu of %zu entries)\n",
                     limit, cut.size(), all.size());
    }
    const DWORD need = static_cast<DWORD>(cut.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!buffer || *length < need) {
        *length = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(buffer, cut.data(), need);
    *length = need;
    return TRUE;
}

}  // namespace

SHIM_TABLE(sysinfo_ce,
    {"api-ms-win-core-sysinfo-l1-1-0.dll", "GetLogicalProcessorInformation",
     reinterpret_cast<void*>(&CeGetLogicalProcessorInformation)},
    {"kernel32.dll", "GetLogicalProcessorInformation", reinterpret_cast<void*>(&CeGetLogicalProcessorInformation)},
);
