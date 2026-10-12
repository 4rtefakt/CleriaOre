// DLL entry: load the real dinput8 (so the proxy is transparent), hook the game before its first
// instruction runs, and start the mod loader on a worker thread (doing that inside DllMain is unsafe).
#include <windows.h>

#include "ore.hpp"

void proxy_load_real();

static DWORD WINAPI init_thread(LPVOID hooked) {
    ore::start(hooked != nullptr);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        proxy_load_real();
        CreateThread(nullptr, 0, init_thread, ore::install_hooks() ? mod : nullptr, 0, nullptr);
    }
    return TRUE;
}
