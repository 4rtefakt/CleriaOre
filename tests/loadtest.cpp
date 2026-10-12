// ore-loadtest: stands in for yso_win.exe. Lays a cleriaore/ folder with one sample mod next to this
// exe, loads the built dinput8.dll from the same folder and checks that
//   - DirectInput8Create still reaches the system DLL (the proxy is transparent);
//   - the mod is found, active, and its Lua script starts (cleriaore.log says so).
// argv[1]: a mod folder whose mod.ini has Id=sample.hello (CleriaCore's samples/mods/hello).
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: ore-loadtest <sample mod folder>\n"); return 2; }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path here = fs::path(exe).parent_path(), ore = here / "cleriaore";

    fs::remove_all(ore);
    fs::create_directories(ore / "mods");
    fs::copy(argv[1], ore / "mods" / "hello", fs::copy_options::recursive);
    std::ofstream(ore / "cleriaore.ini", std::ios::binary) << "[cleria.mods]\r\nActive=\"sample.hello\"\r\n";

    int failed = 0;
    auto check = [&](const char* what, bool ok) {
        std::printf("[load] %-70s %s\n", what, ok ? "OK" : "FAILED");
        if (!ok) ++failed;
    };

    const HMODULE dll = LoadLibraryW((here / "dinput8.dll").c_str());
    check("dinput8.dll loads", dll != nullptr);
    if (!dll) return 1;

    typedef HRESULT(WINAPI* Create)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    const Create create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(dll, "DirectInput8Create")));
    IDirectInput8W* di = nullptr;
    check("DirectInput8Create is forwarded to the system DLL",
          create && SUCCEEDED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                     reinterpret_cast<void**>(&di), nullptr)) && di);
    if (di) di->Release();

    // The loader runs on its own thread: wait for its line about the mod's script.
    std::string text;
    for (int i = 0; i < 100 && text.find("[lua] Hello Lua:") == std::string::npos; ++i) {
        Sleep(100);
        std::ifstream f(ore / "cleriaore.log", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    std::printf("---- cleriaore.log\n%s----\n", text.c_str());
    check("the mod is found and active", text.find("1 installed, active: Hello Lua 1.0.0") != std::string::npos);
    check("its Lua script starts", text.find("[lua] Hello Lua: started (main.lua)") != std::string::npos);

    if (failed) std::printf("%d FAILED\n", failed);
    else std::printf("all passed (0 failures)\n");
    return failed ? 1 : 0;
}
