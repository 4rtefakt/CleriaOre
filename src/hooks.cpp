// The hooks into yso_win.exe. Every address is of ONE build (the Steam exe linked 2024-01-08, "v1.1.1.0"),
// so nothing is hooked unless the process is that exe: its link time and the first bytes of each hooked
// function are checked first.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "files.hpp"
#include "ore.hpp"

namespace ore {

namespace {

constexpr std::uint32_t kImageBase = 0x400000;      // no ASLR: the addresses below are absolute
constexpr std::uint32_t kLinkTime = 0x659ca02d;     // the PE header's TimeDateStamp

// ---- the archive reader (files.hpp) ----------------------------------------------------------------
struct Reader {
    files::TocEntry* entry;
    std::uint32_t offset;
};
using LookupFn = files::TocEntry*(__thiscall*)(void* archive, const char* path);                     // FUN_005defa0
using ReadFn = int(__thiscall*)(Reader* r, void* dst, std::uint32_t n, std::uint32_t* got);          // FUN_005df220
constexpr std::uintptr_t kLookup = 0x5defa0, kRead = 0x5df220;
constexpr std::uint8_t kLookupBytes[] = {0x81, 0xec, 0x84, 0x01, 0x00, 0x00, 0x8b, 0x94, 0x24, 0x88, 0x01, 0x00, 0x00, 0x53, 0x55, 0x56};
constexpr std::uint8_t kReadBytes[] = {0x83, 0xec, 0x14, 0x53, 0x8b, 0xd9, 0x57, 0x33, 0xff, 0x8b, 0x0b, 0x85, 0xc9, 0x0f, 0x84, 0x48};

LookupFn g_lookup = nullptr;
ReadFn g_read = nullptr;

// __thiscall as __fastcall with an unused EDX: `this` in ECX, the rest on the stack, the callee pops.
files::TocEntry* __fastcall hook_lookup(void* archive, void*, const char* path) {
    wait_started();   // the first lookups come before the mods are loaded
    if (files::TocEntry* e = files::lookup(path)) return e;
    return g_lookup(archive, path);
}

int __fastcall hook_read(Reader* r, void*, void* dst, std::uint32_t n, std::uint32_t* got) {
    std::uint32_t done = 0;
    if (!r->entry || !files::read(r->entry, r->offset, dst, n, done)) return g_read(r, dst, n, got);
    if (got) *got = done;
    return 0;
}

// ---- the game loop (FUN_00410440 runs FUN_004106e0 once per 60 Hz step, then renders) --------------
using StepFn = void(__fastcall*)(char active, char arg);   // FUN_004106e0: `active` = a real step, 0 = an idle one
using FrameFn = int(__cdecl*)();                           // FUN_00410440
constexpr std::uintptr_t kStep = 0x4106e0, kFrame = 0x410440;
constexpr std::uint8_t kStepBytes[] = {0x83, 0xec, 0x30, 0x88, 0x0d, 0xf0, 0xc0, 0x74, 0x00, 0x88, 0x15, 0xf1, 0xc0, 0x74, 0x00, 0x0f};
constexpr std::uint8_t kFrameBytes[] = {0xa1, 0xc0, 0x51, 0x76, 0x00, 0x50, 0x8b, 0x08, 0xff, 0x51, 0x0c, 0x85, 0xc0, 0x79, 0x1e, 0x3d};

StepFn g_step = nullptr;
FrameFn g_frame = nullptr;
bool g_hooked = false;

void __fastcall hook_step(char active, char arg) {
    g_step(active, arg);
    if (active) on_tick();
}

int __cdecl hook_frame() {
    const int hr = g_frame();
    on_frame();
    return hr;
}

bool is_the_build() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (reinterpret_cast<std::uintptr_t>(base) != kImageBase) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return nt->FileHeader.TimeDateStamp == kLinkTime &&
           std::memcmp(reinterpret_cast<const void*>(kLookup), kLookupBytes, sizeof kLookupBytes) == 0 &&
           std::memcmp(reinterpret_cast<const void*>(kRead), kReadBytes, sizeof kReadBytes) == 0 &&
           std::memcmp(reinterpret_cast<const void*>(kStep), kStepBytes, sizeof kStepBytes) == 0 &&
           std::memcmp(reinterpret_cast<const void*>(kFrame), kFrameBytes, sizeof kFrameBytes) == 0;
}

}  // namespace

bool install_hooks() {
    if (!is_the_build()) return false;
    g_hooked = MH_Initialize() == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(kLookup), reinterpret_cast<void*>(&hook_lookup), reinterpret_cast<void**>(&g_lookup)) == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(kRead), reinterpret_cast<void*>(&hook_read), reinterpret_cast<void**>(&g_read)) == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(kStep), reinterpret_cast<void*>(&hook_step), reinterpret_cast<void**>(&g_step)) == MH_OK &&
               MH_CreateHook(reinterpret_cast<void*>(kFrame), reinterpret_cast<void*>(&hook_frame), reinterpret_cast<void**>(&g_frame)) == MH_OK &&
               MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    return g_hooked;
}

bool hooked() { return g_hooked; }

}  // namespace ore
