// HACK (Leonida CE boot): ReadFile into the title's console memory on native Windows.
//
// XMem maps the title's physical pages with commit-on-first-touch (a vectored handler commits a page of the backing
// section when the CPU first touches it). The Windows kernel never raises that fault: a ReadFile straight into such
// a view fails with ERROR_NOACCESS (998), and the title's positional read loop (+0x314200) retries forever with
// Sleep(1) while holding its I/O lock (+0x1478f5e00), so loading stops (no world data, no draws). Wine copies in
// user mode, where the handler runs, so it never showed up there. Commit the destination first.
#include <windows.h>

#include <cstdio>

#include "shims.h"
#include "xmem.h"

namespace {

using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);

BOOL WINAPI CeReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped) {
    static const auto real = reinterpret_cast<ReadFileFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ReadFile"));
    if (buffer && bytes) rt::xmem::commit_physical_range(buffer, bytes);
    const BOOL ok = real(file, buffer, bytes, read, overlapped);
    if (!ok && GetLastError() == ERROR_NOACCESS) {
        static LONG told = 0;
        if (InterlockedIncrement(&told) <= 4)
            std::fprintf(stderr, "[rt] readfile: ERROR_NOACCESS for %p + %lu after committing the range\n", buffer, bytes);
        SetLastError(ERROR_NOACCESS);
    }
    return ok;
}

}  // namespace

SHIM_TABLE(readfile_ce,
    {"api-ms-win-core-file-l1-1-0.dll", "ReadFile", reinterpret_cast<void*>(&CeReadFile)},
    {"kernel32.dll", "ReadFile", reinterpret_cast<void*>(&CeReadFile)},
);
