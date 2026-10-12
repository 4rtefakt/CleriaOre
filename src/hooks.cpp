// The hooks into yso_win.exe. Every address is of ONE build (the Steam exe linked 2024-01-08, "v1.1.1.0"),
// so nothing is hooked unless the process is that exe: its link time and the first bytes of each hooked
// hooked site are checked first.
#include <windows.h>

#include <cstdint>
#include <cctype>
#include <cstring>
#include <string>

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

// ---- the script VM's grants (FUN_00566350 is a GoScript's tick; its frame keeps the script at [ebp-0x14]) ----
//   0x567d17  mov [eax], ecx    op 0x64 Flag_SetInt: eax = the cell, ecx = the value
//   0x567db3  add [esi], ecx    op 0x67 Flag_AddInt: esi = the cell, ecx = the amount
//   FUN_00573210(item, flag, mesh)          op 0x116 Effect_GetItem: the icon that rises from a chest
//   FUN_00574410(script; a, item, text)     op 0xD5 Window_Treasure: the "Acquired" box
// Each asks on_grant() first, as CleriaCore's VM asks its grant filter (cleria::GrantQuery).
constexpr std::uintptr_t kFlags = 0x76b91c;
constexpr std::uintptr_t kStore = 0x567d17, kAdd = 0x567db3, kIcon = 0x573210, kWindow = 0x574410;
constexpr std::uint8_t kStoreBytes[] = {0x89, 0x08, 0xe9, 0xab, 0xe7, 0xff, 0xff};
constexpr std::uint8_t kAddBytes[] = {0x01, 0x0e, 0xe9, 0x0f, 0xe7, 0xff, 0xff};
constexpr std::uint8_t kIconBytes[] = {0x55, 0x8b, 0xec, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0xc8, 0x72, 0x67, 0x00};
constexpr std::uint8_t kWindowBytes[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0x58, 0x73, 0x67, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00};

using IconFn = void(__stdcall*)(int item, int flag, const char* mesh);
using WindowFn = void(__thiscall*)(void* script, int a, int item, const char* text);
void* g_store = nullptr;      // the trampolines of the two instruction hooks
void* g_add = nullptr;
IconFn g_icon = nullptr;
WindowFn g_window = nullptr;
std::int32_t g_sink = 0;      // where a store that must not happen is written
std::int32_t g_amount = 0;    // hook_add: the amount to add after all

cleria::GrantQuery query(cleria::GrantQuery::Kind kind, int sub, int index, const void* script) {
    cleria::GrantQuery q;
    q.kind = kind;
    q.sub = sub;
    q.index = index;
    if (script) {   // GoScript +0x124: its name; lower-case as CleriaCore names scripts ("s_box01")
        const char* name = static_cast<const char*>(script) + 0x124;
        for (int i = 0; i < 64 && name[i]; ++i) q.script.push_back(char(std::tolower(static_cast<unsigned char>(name[i]))));
    }
    return q;
}

// The cell's index in g_flags, or -1: a script's own variable, not a flag.
int flag_index(const std::int32_t* cell) {
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(cell);
    return a >= kFlags && a < kFlags + 0x200 * 4 ? int((a - kFlags) / 4) : -1;
}

// A decision on the store of `value` into flag `index`: true = the store must not happen (a Replace has
// then written its own cell here).
bool store_decided(int sub, int index, std::int32_t old_value, std::int32_t value, const void* script) {
    cleria::GrantQuery q = query(cleria::GrantQuery::Kind::Store, sub, index, script);
    q.old_value = old_value;
    q.value = value;
    const cleria::GrantDecision d = on_grant(q);
    if (d.action == cleria::GrantDecision::Action::Pass) return false;
    if (d.action == cleria::GrantDecision::Action::Replace && d.index >= 0 && d.index < 0x200)
        reinterpret_cast<std::int32_t*>(kFlags)[d.index] = d.value;
    return true;
}

// The cell `mov [eax], ecx` writes after all.
std::int32_t* __cdecl decide_store(std::int32_t* cell, std::int32_t value, const void* script) {
    const int index = flag_index(cell);
    if (index < 0 || !store_decided(0x64, index, *cell, value, script)) return cell;
    return &g_sink;
}

// The amount `add [esi], ecx` adds after all.
std::int32_t __cdecl decide_add(std::int32_t* cell, std::int32_t amount, const void* script) {
    const int index = flag_index(cell);
    return index >= 0 && store_decided(0x67, index, *cell, *cell + amount, script) ? 0 : amount;
}

__declspec(naked) void hook_store() {
    __asm {
        pushfd
        push edx
        push ecx                        // the value, kept
        push dword ptr [ebp - 0x14]     // the script
        push ecx
        push eax
        call decide_store               // eax = the cell to write
        add  esp, 12
        pop  ecx
        pop  edx
        popfd
        jmp  dword ptr [g_store]        // mov [eax], ecx ; jmp to the VM's loop
    }
}

__declspec(naked) void hook_add() {
    __asm {
        pushfd
        pushad
        push dword ptr [ebp - 0x14]     // the script
        push ecx
        push esi
        call decide_add
        add  esp, 12
        mov  g_amount, eax
        popad
        popfd
        mov  ecx, g_amount
        jmp  dword ptr [g_add]          // add [esi], ecx ; jmp to the VM's loop
    }
}

void __stdcall icon(int item, int flag, const char* mesh, const void* script) {
    const cleria::GrantDecision d = on_grant(query(cleria::GrantQuery::Kind::Icon, 0x116, item, script));
    if (d.action == cleria::GrantDecision::Action::Suppress) return;
    if (d.action == cleria::GrantDecision::Action::Replace) item = d.index;
    g_icon(item, flag, mesh);
}

// FUN_00573210 is entered from the VM's frame: [esp] = the return, then item, flag, mesh.
__declspec(naked) void hook_icon() {
    __asm {
        push dword ptr [ebp - 0x14]     // the script
        push dword ptr [esp + 0x10]     // mesh
        push dword ptr [esp + 0x10]     // flag
        push dword ptr [esp + 0x10]     // item
        call icon                       // pops its four
        ret  0xc
    }
}

void __fastcall hook_window(void* script, void*, int a, int item, const char* text) {
    cleria::GrantQuery q = query(cleria::GrantQuery::Kind::Window, 0xd5, item, script);
    q.text = text ? text : "";
    const cleria::GrantDecision d = on_grant(q);
    if (d.action == cleria::GrantDecision::Action::Suppress) return;
    static std::string replaced;   // the box copies its text before this returns
    if (d.action == cleria::GrantDecision::Action::Replace) {
        item = d.index;
        replaced = d.text;
        text = replaced.c_str();
    }
    g_window(script, a, item, text);
}

struct Site {
    std::uintptr_t at;
    const std::uint8_t* bytes;   // what the build has there
    std::size_t n;
    void* hook;
    void** original;
};
#define SITE(at, bytes, hook, original) {at, bytes, sizeof bytes, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(original)}
const Site kSites[] = {
    SITE(kLookup, kLookupBytes, &hook_lookup, &g_lookup), SITE(kRead, kReadBytes, &hook_read, &g_read),
    SITE(kStep, kStepBytes, &hook_step, &g_step),         SITE(kFrame, kFrameBytes, &hook_frame, &g_frame),
    SITE(kStore, kStoreBytes, &hook_store, &g_store),     SITE(kAdd, kAddBytes, &hook_add, &g_add),
    SITE(kIcon, kIconBytes, &hook_icon, &g_icon),         SITE(kWindow, kWindowBytes, &hook_window, &g_window),
};
#undef SITE

bool is_the_build() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (reinterpret_cast<std::uintptr_t>(base) != kImageBase) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kLinkTime) return false;
    for (const Site& site : kSites)
        if (std::memcmp(reinterpret_cast<const void*>(site.at), site.bytes, site.n) != 0) return false;
    return true;
}

}  // namespace

bool install_hooks() {
    if (!is_the_build()) return false;
    if (MH_Initialize() != MH_OK) return false;
    for (const Site& site : kSites)
        if (MH_CreateHook(reinterpret_cast<void*>(site.at), site.hook, site.original) != MH_OK) return false;
    g_hooked = MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    return g_hooked;
}

bool hooked() { return g_hooked; }

}  // namespace ore
