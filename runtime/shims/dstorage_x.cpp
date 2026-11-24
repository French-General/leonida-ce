// HACK (local, exploratory): stand-in for the Xbox DirectStorage factory (xcrdapi.dll!DStorageGetFactory).
// The Xbox interface layout is not known, so every object handed out is a "probe" whose vtable slots log their slot
// number and first six arguments and return S_OK. Slots 0-2 behave as IUnknown. The log shows what the game calls,
// and real behaviour is filled in slot by slot (see kKnown below).
#include <windows.h>

#include <array>
#include <atomic>
#include <utility>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"
#include "../src/shader_library.h"
#include <vector>

namespace {

constexpr int kSlots = 96;

struct Probe;
using Slot = uintptr_t(__stdcall*)(Probe*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct Probe {
    const Slot* vtbl;
    const char* name;
    std::atomic<long> refs{1};
    HANDLE file = INVALID_HANDLE_VALUE;  // "file" probes: the opened package file
    wchar_t path[260] = {};
};

void print_guid(const void* p, char* out, size_t n) {
    GUID g{};
    if (!p || IsBadReadPtr(p, sizeof g)) { std::snprintf(out, n, "(unreadable)"); return; }
    std::memcpy(&g, p, sizeof g);
    std::snprintf(out, n, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0],
                  g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

Probe* make_probe(const char* name);

void hexdump(const char* tag, const void* p, size_t n) {
    if (!p || IsBadReadPtr(p, n)) { std::fprintf(stderr, "[dstorage]   %s: (unreadable %p)\n", tag, p); return; }
    const auto* w = static_cast<const uint32_t*>(p);
    std::fprintf(stderr, "[dstorage]   %s:", tag);
    for (size_t i = 0; i < n / 4; ++i) std::fprintf(stderr, " %08x", w[i]);
    std::fprintf(stderr, "\n");
}

// HACK: known shapes (PC IDStorageFactory order where it matches the title's calls).
bool special(Probe* self, int slot, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t, uintptr_t,
             uintptr_t* result) {
    const bool factory = std::strcmp(self->name, "factory") == 0;
    if (factory && slot == 4) {  // OpenFile(path, riid, ppv)
        static std::atomic<int> n{0};
        const wchar_t* path = reinterpret_cast<const wchar_t*>(a);
        if (n++ < 40) {
            std::fprintf(stderr, "[dstorage] OpenFile(%ls)\n", path && !IsBadReadPtr(path, 2) ? path : L"?");
            std::fflush(stderr);
        }
        Probe* f = make_probe("file");
        if (path && !IsBadReadPtr(path, 2)) {
            // G:\x -> x, relative to the working directory (the package folder: gta6rt.sh / leonida-try.sh cd there)
            const wchar_t* rel = path;
            if (rel[0] && rel[1] == L':') rel += 2;
            while (*rel == L'\\' || *rel == L'/') ++rel;
            wcsncpy_s(f->path, rel, _TRUNCATE);
            f->file = CreateFileW(f->path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f->file == INVALID_HANDLE_VALUE) {
                std::fprintf(stderr, "[dstorage] OpenFile(%ls): open failed, error %lu\n", path, GetLastError());
                std::fflush(stderr);
            }
        }
        if (c) *reinterpret_cast<void**>(c) = f;
        *result = (f->file != INVALID_HANDLE_VALUE) ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return true;
    }
    if (factory && slot == 5) {  // CreateStatusArray(capacity, name, riid, ppv)
        std::fprintf(stderr, "[dstorage] CreateStatusArray(capacity=%llu, name=%s)\n", (unsigned long long)a,
                     b && !IsBadReadPtr(reinterpret_cast<void*>(b), 1) ? reinterpret_cast<const char*>(b) : "?");
        std::fflush(stderr);
        if (d) *reinterpret_cast<void**>(d) = make_probe("status");
        *result = S_OK;
        return true;
    }
    if (factory && slot == 10) {  // console CreateQueue-like (desc, riid, ppv)
        std::fprintf(stderr, "[dstorage] factory slot 10 (queue?) desc=%p\n", reinterpret_cast<void*>(a));
        hexdump("queue desc", reinterpret_cast<void*>(a), 48);
        std::fflush(stderr);
        if (c) *reinterpret_cast<void**>(c) = make_probe("queue");
        *result = S_OK;
        return true;
    }
    if (std::strcmp(self->name, "status") == 0 && slot == 3) {  // IsComplete(index) -> TRUE (HACK)
        *result = 1;
        return true;
    }
    if (std::strcmp(self->name, "status") == 0 && slot == 4) {  // GetHResult(index) -> S_OK
        *result = S_OK;
        return true;
    }
    if (std::strcmp(self->name, "queue") == 0 && slot == 3) {  // EnqueueRequest(request*): synchronous read
        // Console request layout (measured): +0 options (low byte = compression: 0 none, 1 zlib), +8 destination,
        // +0x18 destination size, +0x20 IDStorageFile*, +0x28 file offset, +0x30 source size.
        const auto* rq = reinterpret_cast<const uint8_t*>(a);
        const uint32_t comp = rq[0];
        auto* dst = *reinterpret_cast<uint8_t* const*>(rq + 0x08);
        const uint32_t dst_size = *reinterpret_cast<const uint32_t*>(rq + 0x18);
        auto* f = *reinterpret_cast<Probe* const*>(rq + 0x20);
        const uint64_t off = *reinterpret_cast<const uint64_t*>(rq + 0x28);
        const uint32_t src_size = *reinterpret_cast<const uint32_t*>(rq + 0x30);
        static std::atomic<uint64_t> total{0}, fails{0};
        bool ok = false;
        std::vector<uint8_t> src(src_size);
        if (f && f->file != INVALID_HANDLE_VALUE && dst) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD got = 0;
            if (ReadFile(f->file, src.data(), src_size, &got, &ov) && got == src_size) {
                if (comp == 0) {
                    std::memcpy(dst, src.data(), (std::min)(src_size, dst_size));
                    ok = true;
                } else if (comp == 1) {
                    std::vector<uint8_t> out;
                    if (leonida::zlib_inflate(std::span<const uint8_t>(src.data(), src.size()), out, dst_size)) {
                        std::memcpy(dst, out.data(), (std::min)(out.size(), static_cast<size_t>(dst_size)));
                        ok = true;
                    }
                }
            }
        }
        total += src_size;
        if (!ok) {
            if (fails++ < 12) {
                std::fprintf(stderr, "[dstorage] request FAILED: comp=%u file=%ls off=0x%llx size=%u dst=%p/%u first=%02x %02x %02x %02x\n",
                             comp, f ? f->path : L"?", (unsigned long long)off, src_size, (void*)dst, dst_size,
                             src_size > 0 ? src[0] : 0, src_size > 1 ? src[1] : 0, src_size > 2 ? src[2] : 0,
                             src_size > 3 ? src[3] : 0);
                std::fflush(stderr);
            }
        } else {
            static std::atomic<int> n{0};
            const int k = n++;
            if (k < 3 || (k % 2000) == 0) {
                std::fprintf(stderr, "[dstorage] request #%d ok: comp=%u %ls @0x%llx %u -> %u (total %llu MiB read)\n",
                             k, comp, f->path, (unsigned long long)off, src_size, dst_size,
                             (unsigned long long)(total.load() >> 20));
                std::fflush(stderr);
            }
        }
        *result = S_OK;
        return true;
    }
    return false;
}

template <int N>
uintptr_t __stdcall slot_fn(Probe* self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
    void* ret = _ReturnAddress();
    if constexpr (N == 0) {  // QueryInterface(riid, ppv)
        char g[64];
        print_guid(reinterpret_cast<void*>(a), g, sizeof g);
        std::fprintf(stderr, "[dstorage] %s::QueryInterface(%s) -> self\n", self->name, g);
        if (b) *reinterpret_cast<void**>(b) = self;
        self->refs++;
        return S_OK;
    } else if constexpr (N == 1) {
        return static_cast<uintptr_t>(++self->refs);
    } else if constexpr (N == 2) {
        return static_cast<uintptr_t>(--self->refs);
    } else {
        uintptr_t result = S_OK;
        if (special(self, N, a, b, c, d, e, f, &result)) return result;
        std::fprintf(stderr,
                     "[dstorage] %s slot %d called from %p: a=0x%llx b=0x%llx c=0x%llx d=0x%llx e=0x%llx f=0x%llx\n",
                     self->name, N, ret, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
                     (unsigned long long)d, (unsigned long long)e, (unsigned long long)f);
        std::fflush(stderr);
        return S_OK;
    }
}

template <int... I>
constexpr auto make_vtbl(std::integer_sequence<int, I...>) {
    return std::array<Slot, sizeof...(I)>{&slot_fn<I>...};
}
const auto kVtbl = make_vtbl(std::make_integer_sequence<int, kSlots>{});

Probe* make_probe(const char* name) {
    auto* p = new Probe;
    p->vtbl = kVtbl.data();
    p->name = name;
    return p;
}

// HRESULT DStorageGetFactory(REFIID riid, void** ppv)
HRESULT WINAPI DStorageGetFactory(const GUID* riid, void** ppv) {
    char g[64];
    print_guid(riid, g, sizeof g);
    std::fprintf(stderr, "[dstorage] DStorageGetFactory(%s, %p) -> probe factory\n", g, (void*)ppv);
    std::fflush(stderr);
    // HACK: switch off the title hang watchdog (1.0.2.1: bit 0 of the byte at 0x1471b6340 makes its thread exit).
    // Pipeline creation here takes far longer than on the console, and the watchdog kills the process after 30 s.
    {
        auto* flag = reinterpret_cast<unsigned char*>(0x1471b6340ull);
        DWORD old_protect = 0;
        if (VirtualProtect(flag, 1, PAGE_READWRITE, &old_protect)) {
            *reinterpret_cast<volatile unsigned char*>(flag) |= 1;
            std::fprintf(stderr, "[hack] hang watchdog disabled\n");
        }
    }
    if (!ppv) return E_POINTER;
    static Probe* factory = make_probe("factory");
    factory->refs++;
    *ppv = factory;
    return S_OK;
}

}  // namespace

SHIM_TABLE(dstorage_x,
    {"xcrdapi.dll", "DStorageGetFactory", reinterpret_cast<void*>(&DStorageGetFactory)},
);
