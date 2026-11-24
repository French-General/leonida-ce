// HACK (Leonida CE boot): app-lifecycle (PLM) notifications.
//
// GTA VI registers for app-state and app-constrained notifications (api-ms-win-core-psm-appnotify) and its main thread
// waits, before it ever starts its boot state machine, until its lifecycle byte (0x1471b9048) reads 2 ("running, not
// constrained"): 0x1421b8a3e..0x1421b8a56 pump, check, Sleep(1). The byte starts at 2 and only these callbacks move it
// (constrained -> 8, quiesce/resume -> 0x80; only constrained=FALSE goes back to 2). Without a shim the host answered
// (a desktop process is not a console app), and the title never left that loop: black frames, no boot. On the console a
// running title is simply not notified; so are we: the registration is accepted and the callback is never called.
#include <windows.h>

#include <atomic>
#include <cstdio>

#include "shims.h"

namespace {

struct Registration { void* routine; void* context; };

ULONG WINAPI CeRegisterAppStateChange(void* routine, void* context, void** registration) {
    static std::atomic<int> told{0};
    if (told++ < 2) std::fprintf(stderr, "[rt] plm: RegisterAppStateChangeNotification(%p) accepted; never called\n", routine);
    if (registration) *registration = new Registration{routine, context};
    return ERROR_SUCCESS;
}

void WINAPI CeUnregister(void* registration) { delete static_cast<Registration*>(registration); }

ULONG WINAPI CeRegisterAppConstrainedChange(void* routine, void* context, void** registration) {
    static std::atomic<int> told{0};
    if (told++ < 2)
        std::fprintf(stderr, "[rt] plm: RegisterAppConstrainedChangeNotification(%p) accepted; never called\n", routine);
    if (registration) *registration = new Registration{routine, context};
    return ERROR_SUCCESS;
}

}  // namespace

SHIM_TABLE(plm_ce,
    {"api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification", reinterpret_cast<void*>(&CeRegisterAppStateChange)},
    {"api-ms-win-core-psm-appnotify-l1-1-0.dll", "UnregisterAppStateChangeNotification", reinterpret_cast<void*>(&CeUnregister)},
    {"api-ms-win-core-psm-appnotify-l1-1-1.dll", "RegisterAppConstrainedChangeNotification", reinterpret_cast<void*>(&CeRegisterAppConstrainedChange)},
    {"api-ms-win-core-psm-appnotify-l1-1-1.dll", "UnregisterAppConstrainedChangeNotification", reinterpret_cast<void*>(&CeUnregister)},
);
