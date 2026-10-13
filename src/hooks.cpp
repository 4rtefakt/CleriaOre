// The hooks into yso_win.exe. Every address is of ONE build (the Steam exe linked 2024-01-08, "v1.1.1.0"),
// so nothing is hooked unless the process is that exe: its link time and the first bytes of each hooked
// hooked site are checked first.
#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <set>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "files.hpp"
#include "cleria/vfs.hpp"
#include "ore.hpp"
#include "ui.hpp"

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
constexpr std::uintptr_t kArchive = 0x765554;   // the game's archive, the `this` of its lookups
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

// ---- combat: the damage of a hit, the EXP and SP of a kill (cleria.combat.add_filter, enemy_killed) ----
// FUN_004fbb40 applies a hit of actor ESI on actor EDI, FUN_004fd350 a hit on the player (EDI):
//   0x4fc802  mov ecx, [ebp-0x1c]       the final damage, about to leave the victim's HP (+0x98)
//   0x4fdc58  movss xmm0, [edi+0x98]    the same on the player, the damage in ESI (some paths jump here past
//                                       the load of ESI from [ebp-0x18] just before)
// and FUN_004fbb40's kill block (the victim's HP fell under 1):
//   0x4fcc07  addss xmm0, xmm1          the kill's EXP (xmm1) joins the total 0x76a748
//   0x4fcc18  mov eax, [edi+0x124]      past the level-ups; the kill's SP ([ebp-0x3c], a float) is credited later
// Two more places add EXP the same way, the award in xmm1: FUN_004fa4a0 (0x4fa525, a boss's) and FUN_005215e0
// (0x5226c7). cleria.game.exp_factor is the game's own EXP rate 0x76a5fc, which all three multiply by before
// they floor the award.
constexpr std::uintptr_t kExpRate = 0x76a5fc;
constexpr std::uintptr_t kDealt = 0x4fc802, kTaken = 0x4fdc58, kExpKill = 0x4fcc07, kKill = 0x4fcc18, kExpBoss = 0x4fa525, kExpOther = 0x5226c7;
// FUN_00430ef0(ecx scene id, edx force): a room change is asked for (a door's fade, a script's ChangeScene, a warp,
// the title's and the movies' own). The mods' "exit" filter may send it elsewhere or refuse it.
constexpr std::uintptr_t kScene = 0x430ef0;
constexpr std::uint8_t kSceneBytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, 0x80, 0x3d, 0xbe, 0x93};
using SceneFn = int(__fastcall*)(int scene, int force);
SceneFn g_scene = nullptr;
int __fastcall hook_scene(int scene, int force) {
    const int to = exit_filter(scene);
    return to ? g_scene(to, force) : int(0x80004005);   // refused, as while another change is pending
}
// The script VM's 0xAF, a blessing's effect (menu_re/BLESSING_SHOP.md): at 0x568d5d the blessing's index is in eax,
// before the jump table that gives it. Its SP was taken earlier, by the script's own flag operations.
//   0x568d5d  cmp eax, 0x21 ; ja <throw>
constexpr std::uintptr_t kBless = 0x568d5d, kVmNext = 0x5664c9;   // kVmNext: where every case of 0xAF ends
constexpr std::uint8_t kBlessBytes[] = {0x83, 0xf8, 0x21, 0x0f, 0x87, 0xb7, 0x4d, 0x00, 0x00};
void* g_bless = nullptr;
// FUN_004496f0(actor, args), stdcall: an animation script's anime_GenerateEffect (aso_re/ASOVM.md, funcid 0x25). The
// ten argument words: the effect id, then as floats the distance ahead, the angle, the height and the scale, the
// attach mode, and four more the factory takes as they are.
constexpr std::uintptr_t kEffect = 0x4496f0;
constexpr std::uint8_t kEffectBytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x40, 0x56, 0x8b, 0x75, 0x0c, 0x57, 0x8b, 0x7d};
using EffectFn = void(__stdcall*)(void* actor, std::int32_t* args);
EffectFn g_effect = nullptr;
bool g_effect_shown = false;            // an effect of puppet_effect is being made
void __stdcall hook_effect(void* actor, std::int32_t* args) {
    if (!g_effect_shown) on_effect(actor, args);
    g_effect(actor, args);
}
// FUN_00566190, the script VM's fetch of an integer operand: thiscall(script; context, the sub-op, a word, the
// operands, the operand's index), answering a pointer to the value. A blessing's script (@GrowNN, its name at
// script +0x124) tests the SP against its price (0x61: flag 216, 6, price), takes it (0x69: flag 216, price) and
// shows it in the menu (0xdd: text, label, price); the mods' shop filter may give another price to all three.
constexpr std::uintptr_t kFetch = 0x566190;
constexpr std::uint8_t kFetchBytes[] = {0x56, 0x8b, 0x74, 0x24, 0x18, 0x8b, 0xc6, 0x99, 0x83, 0xe2, 0x07, 0x03, 0xc2, 0x8b, 0x54, 0x24};
using FetchFn = int*(__thiscall*)(void* script, void* ctx, int sub, void* word, int* ops, int index);
FetchFn g_fetch = nullptr;
int* __fastcall hook_fetch(void* script, void*, void* ctx, int sub, void* word, int* ops, int index) {
    int* p = g_fetch(script, ctx, sub, word, ops, index);
    if (!shop_on() || !p || !script) return p;
    const char* name = static_cast<const char*>(script) + 0x124;
    // The armor's and the leggings' blessings price themselves (0xb0 puts the price in flag 218) and then compare
    // and take by that flag: the flag is given the filter's price when the script first names it (0x62: 216, 6, 218).
    if (sub == 0x62 && index == 2 && *p == 218 && ops[0] == 216) {
        static int left = -1;
        std::int32_t& cost = *reinterpret_cast<std::int32_t*>(0x76b91c + 4 * 218);
        if (cost != left) cost = left = on_shop_price(0xb0, name, name, cost);
        return p;
    }
    const bool price = (sub == 0x61 && index == 2 && ops[0] == 216) || (sub == 0x69 && index == 1 && ops[0] == 216) || (sub == 0xdd && index == 2);
    if (!price) return p;
    const char* of = name;
    if (sub == 0xdd)   // a row of the menu: its label operand names the blessing's script (FUN_00566100, the string fetch)
        of = reinterpret_cast<const char*(__thiscall*)(void*, void*, int, void*, int*, int)>(0x566100)(script, ctx, sub, word, ops, 1);
    static int answer = 0;
    answer = on_shop_price(sub, name, of, *p);
    return answer == *p ? p : &answer;
}
// FUN_0045a2f0, GoActor's update (its vtable's third entry): a jump to the actor's handler, by its index at +0x120.
// An actor a mod holds is not updated: it stays where and as it was put. It is still drawn: a handler hands its
// actor to the frame's draw list itself, so the hook does that part (FUN_00473540) in the handler's place.
constexpr std::uintptr_t kActorUpdate = 0x45a2f0;
constexpr std::uint8_t kActorUpdateBytes[] = {0x8b, 0x81, 0x20, 0x01, 0x00, 0x00, 0x8b, 0x04, 0x85, 0xc0, 0xb8, 0x69, 0x00, 0xff, 0xe0};
void* g_actor_update = nullptr;
bool __cdecl held(const void* actor) { return actor_held(actor); }
__declspec(naked) void hook_actor_update() {
    __asm {
        push ecx
        push ecx
        call held
        add  esp, 4
        pop  ecx
        test al, al
        jnz  skip
        jmp  dword ptr [g_actor_update]
    skip:
        mov  eax, 0x473540              // FUN_00473540(ecx actor): what a handler does on a step that is not a real one,
        jmp  eax                        // the actor's place and sprite handed to the frame's draw list
    }
}
// FUN_004faf20, the applier of a hit on anything that is not a monster (a map object: a boss's part, a breakable):
//   0x4fb0ae  call FUN_004fa550         the damage, in eax; esi the victim
//   0x4fb0b3  mov edx, [esi] ; add esp, 8
constexpr std::uintptr_t kPart = 0x4fb0b3;
constexpr std::uint8_t kPartBytes[] = {0x8b, 0x16, 0x83, 0xc4, 0x08};
void* g_part = nullptr;
constexpr std::uint8_t kDealtBytes[] = {0x8b, 0x4d, 0xe4, 0xf3, 0x0f, 0x10, 0x87, 0x98, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTakenBytes[] = {0xf3, 0x0f, 0x10, 0x87, 0x98, 0x00, 0x00, 0x00, 0x66, 0x0f, 0x6e, 0xce};
constexpr std::uint8_t kExpKillBytes[] = {0xf3, 0x0f, 0x58, 0xc1, 0xf3, 0x0f, 0x11, 0x05, 0x48, 0xa7, 0x76, 0x00};
constexpr std::uint8_t kKillBytes[] = {0x8b, 0x87, 0x24, 0x01, 0x00, 0x00, 0x81, 0x8f, 0x2c, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04};
constexpr std::uint8_t kExpAddBytes[] = {0xf3, 0x0f, 0x58, 0x0d, 0x48, 0xa7, 0x76, 0x00, 0xf3, 0x0f, 0x11, 0x0d, 0x48, 0xa7, 0x76, 0x00};
void* g_dealt = nullptr;
void* g_taken = nullptr;
void* g_exp_kill = nullptr;
void* g_kill = nullptr;
void* g_exp_boss = nullptr;
void* g_exp_other = nullptr;
float g_exp_value = 0;   // the award a hook hands to on_exp and takes back

int __cdecl dealt(int value, const void* victim, const void* attacker, const void* source) { return on_damage(true, value, victim, attacker, source); }
int __cdecl part(int value, const void* victim, const void* source) { return on_part_damage(value, victim, source); }
int __cdecl taken(int value, const void* victim, const void* attacker) { return on_damage(false, value, victim, attacker, attacker); }
void __cdecl exp_award(const void* victim, int source) { g_exp_value = on_exp(g_exp_value, source, victim); }
void __cdecl killed(const void* victim, float* sp) { on_kill(victim, sp); }

// Each hook keeps every register, the flags and the FPU / SSE state (fxsave) around its call: the game's code
// here holds values in xmm registers across what looks like plain instructions.
#define ORE_ENTER __asm pushfd __asm pushad __asm mov ebx, esp __asm sub esp, 512 __asm and esp, 0xFFFFFFF0 __asm fxsave [esp]
#define ORE_LEAVE __asm fxrstor [esp] __asm mov esp, ebx __asm popad __asm popfd

__declspec(naked) void hook_dealt() {
    __asm {
        ORE_ENTER
        push dword ptr [ebp - 0x20]     // what hit (the function's first argument)
        push esi                        // the attacker
        push edi                        // the victim
        push dword ptr [ebp - 0x1c]
        call dealt
        add  esp, 16
        mov  dword ptr [ebp - 0x1c], eax
        ORE_LEAVE
        jmp  dword ptr [g_dealt]        // mov ecx, [ebp-0x1c] ; ...
    }
}
__declspec(naked) void hook_part() {
    __asm {
        ORE_ENTER
        push edi                        // what hit
        push esi                        // the victim
        push eax
        call part
        add  esp, 12
        mov  dword ptr [ebx + 28], eax  // pushad's eax
        ORE_LEAVE
        jmp  dword ptr [g_part]
    }
}
// FUN_00593550, the bullet factory (ai_spec/BULLETS.md §1.1): fastcall(ecx owner, edx kind; 17 stack words: the
// effect id, &pos, &dir, f0..f12, out), the caller pops them.
constexpr std::uintptr_t kBullet = 0x593550;
constexpr std::uint8_t kBulletBytes[] = {0x53, 0x8b, 0xdc, 0x83, 0xec, 0x08, 0x83, 0xe4, 0xf8, 0x83, 0xc4, 0x04, 0x55, 0x8b, 0x6b, 0x04};
void* g_bullet = nullptr;
bool g_showing = false;                 // a bullet of show_bullet is being made: not one of the player's own
void __cdecl bullet_made(const void* owner, int kind, const std::uint32_t* args) {
    if (!g_showing) on_bullet(owner, kind, args);
}
__declspec(naked) void hook_bullet() {
    __asm {
        ORE_ENTER
        lea  eax, [ebx + 40]            // past pushfd, pushad and the return address: the stack words
        push eax
        push edx
        push ecx
        call bullet_made
        add  esp, 12
        ORE_LEAVE
        jmp  dword ptr [g_bullet]
    }
}
__declspec(naked) void* __cdecl fire_bullet(void* /*owner*/, int /*kind*/, const std::uint32_t* /*17 words*/) {
    __asm {
        push ebp
        mov  ebp, esp
        push esi
        mov  esi, dword ptr [ebp + 16]
        mov  ecx, 16
    next:
        push dword ptr [esi + ecx * 4]
        dec  ecx
        jns  next
        mov  ecx, dword ptr [ebp + 8]
        mov  edx, dword ptr [ebp + 12]
        mov  eax, kBullet
        call eax
        add  esp, 0x44
        pop  esi
        pop  ebp
        ret
    }
}
int __cdecl bless(int index) { return on_blessing(index); }
__declspec(naked) void hook_bless() {
    __asm {
        ORE_ENTER
        push eax
        call bless
        add  esp, 4
        mov  dword ptr [ebx + 28], eax  // pushad's eax
        ORE_LEAVE
        cmp  eax, -1
        je   no_effect
        jmp  dword ptr [g_bless]
    no_effect:
        mov  eax, kVmNext
        jmp  eax
    }
}
__declspec(naked) void hook_taken() {
    __asm {
        ORE_ENTER
        push dword ptr [ebx + 16]       // the game's EBX: what hit
        push edi                        // the victim: the player
        push esi
        call taken
        add  esp, 12
        mov  dword ptr [ebx + 4], eax   // the game's ESI, as popad gives it back
        mov  dword ptr [ebp - 0x18], eax
        ORE_LEAVE
        jmp  dword ptr [g_taken]        // movss xmm0, [edi+0x98] ; ...
    }
}
__declspec(naked) void hook_exp_kill() {
    __asm {
        movss dword ptr [g_exp_value], xmm1
        ORE_ENTER
        push 0                          // a kill
        push edi                        // the monster
        call exp_award
        add  esp, 8
        ORE_LEAVE
        movss xmm1, dword ptr [g_exp_value]
        jmp  dword ptr [g_exp_kill]     // addss xmm0, xmm1 ; the store
    }
}
__declspec(naked) void hook_exp_boss() {
    __asm {
        movss dword ptr [g_exp_value], xmm1
        ORE_ENTER
        push 1                          // a boss
        push 0
        call exp_award
        add  esp, 8
        ORE_LEAVE
        movss xmm1, dword ptr [g_exp_value]
        jmp  dword ptr [g_exp_boss]
    }
}
__declspec(naked) void hook_exp_other() {
    __asm {
        movss dword ptr [g_exp_value], xmm1
        ORE_ENTER
        push 0
        push 0
        call exp_award
        add  esp, 8
        ORE_LEAVE
        movss xmm1, dword ptr [g_exp_value]
        jmp  dword ptr [g_exp_other]
    }
}
__declspec(naked) void hook_kill() {
    __asm {
        ORE_ENTER
        lea  eax, [ebp - 0x3c]          // the SP the game is about to scale and credit
        push eax
        push edi                        // the monster
        call killed
        add  esp, 8
        ORE_LEAVE
        jmp  dword ptr [g_kill]
    }
}
// ---- pickups (CleriaCore's item_re/PICKUPS.md): the drops of a kill, a pickup taken ----------------------
//   FUN_00587010(ECX bag, EDX n; 16 stack words, the caller pops)   throws up `count` pickups of the bag's items;
//                             an enemy's drops are its calls from FUN_00478510 (0x478510..0x4793da), one item each
//   0x58613f  mov ecx, [ebx+0x134]    FUN_00585ff0 (EBX = the pickup): the player touched it, its grant follows
//   0x58671d                          ... and the tail every grant ends in: the effect, the sound, the pickup gone
constexpr std::uintptr_t kSpawn = 0x587010, kTouch = 0x58613f, kTouchTail = 0x58671d;
constexpr std::uintptr_t kDropsFirst = 0x478510, kDropsLast = 0x4793da;
constexpr std::uint8_t kSpawnBytes[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0x38, 0x78, 0x67, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00};
constexpr std::uint8_t kTouchBytes[] = {0x8b, 0x8b, 0x34, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x10, 0x05, 0x38, 0xd4, 0x6b, 0x00, 0xc7, 0x45};
void* g_spawn = nullptr;
void* g_touch = nullptr;
int g_answer = 0;   // a hook's C function's answer, read after the registers are back

// 1 = nothing drops. A filter's other item is written into the bag (the caller's one id).
int __cdecl drop(int* bag, int n, std::uintptr_t from, const float* pos) {
    if (n != 1 || !bag || !pos || from < kDropsFirst || from >= kDropsLast) return 0;
    return on_drop(bag, pos) ? 0 : 1;
}
int __cdecl touch(void* pickup) { return on_pickup(pickup) ? 0 : 1; }   // 1 = taken, the game's grant is skipped

__declspec(naked) void hook_spawn() {
    __asm {
        ORE_ENTER
        push dword ptr [ebx + 60]       // its position (the sixth stack word)
        push dword ptr [ebx + 36]       // who calls
        push dword ptr [ebx + 20]       // EDX: the bag's length
        push dword ptr [ebx + 24]       // ECX: the bag
        call drop
        add  esp, 16
        mov  g_answer, eax
        ORE_LEAVE
        cmp  g_answer, 0
        jne  no_drop
        jmp  dword ptr [g_spawn]
    no_drop:
        xor  eax, eax
        ret                             // the caller pops its words
    }
}
__declspec(naked) void hook_touch() {
    __asm {
        ORE_ENTER
        push dword ptr [ebx + 16]       // the game's EBX: the pickup
        call touch
        add  esp, 4
        mov  g_answer, eax
        ORE_LEAVE
        cmp  g_answer, 0
        jne  skip_grant
        jmp  dword ptr [g_touch]
    skip_grant:
        mov  dword ptr [ebp - 0x14], 0x58          // what the skipped grant sets first: the sound,
        mov  dword ptr [ebp - 0x18], 0x3f800000    // and 1.0
        mov  eax, kTouchTail
        jmp  eax
    }
}
// cleria.game.spawn_pickup: one pickup of `*id` thrown up from `pos` with an enemy drop's arguments.
__declspec(naked) void __cdecl spawn_one(int* /*id*/, const float* /*pos*/, int /*flags*/) {
    __asm {
        push ebp
        mov  ebp, esp
        push 0
        push 0
        push 0x40490fdb                 // the yaw: -pi .. pi
        push 0xc0490fdb
        push 0x3d851eb8                 // up: 0.04 .. 0.065
        push 0x3d23d70a
        push 0x3d3851ec                 // across: 0.01 .. 0.045
        push 0x3c23d70a
        push 0                          // no spin
        push 0x3f666666                 // size 0.9
        push dword ptr [ebp + 12]       // pos
        push dword ptr [ebp + 16]       // flags: 5 a drop; 0x1c a pickup that rests and stays
        push 1
        push 2
        push 1                          // one
        push 0x304                      // the item icons
        mov  ecx, dword ptr [ebp + 8]
        mov  edx, 1
        mov  eax, kSpawn
        call eax
        add  esp, 0x40
        pop  ebp
        ret
    }
}

#undef ORE_ENTER
#undef ORE_LEAVE

// ---- the stat recompute (cleria.player.set_modifier) -------------------------------------------------
// FUN_00420c40 rebuilds the derived stats into the record 0x76a72c (max HP +0, ATK +0x10, DEF +0x14), the parts
// its dirty bits 0x76b914 ask for, and copies the record into the player's actor (+0x94). The mods' modifiers
// go on top after each run; before a run the game's own values are put back, so a part it does not rebuild is
// not modified twice.
constexpr std::uintptr_t kRecompute = 0x420c40, kStats = 0x76a72c, kDirty = 0x76b914, kPlayerActor = 0x74c09c;
constexpr std::uint8_t kRecomputeBytes[] = {0x53, 0x8b, 0xdc, 0x83, 0xec, 0x08, 0x83, 0xe4, 0xf8, 0x83, 0xc4, 0x04, 0x55, 0x8b, 0x6b, 0x04};
constexpr std::size_t kStatAt[3] = {0x00, 0x10, 0x14};
using RecomputeFn = void(__cdecl*)();
RecomputeFn g_recompute = nullptr;
float g_stat_base[3] = {0, 0, 0}, g_stat_set[3] = {0, 0, 0};
bool g_stat_on = false;   // g_stat_set is what the record holds

void __cdecl hook_recompute() {
    float* rec = reinterpret_cast<float*>(kStats);
    auto stat = [&](int k) -> float& { return *reinterpret_cast<float*>(kStats + kStatAt[k]); };
    if (g_stat_on)
        for (int k = 0; k < 3; ++k)
            if (stat(k) == g_stat_set[k]) stat(k) = g_stat_base[k];
    g_recompute();
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        g_stat_base[k] = stat(k);
        g_stat_set[k] = modified(k, g_stat_base[k]);
        any = any || g_stat_set[k] != g_stat_base[k];
    }
    if (!any && !g_stat_on) return;
    g_stat_on = any;
    for (int k = 0; k < 3; ++k) stat(k) = g_stat_set[k];
    if (rec[1] > rec[0]) rec[1] = rec[0];   // a lowered max HP takes the HP above it
    if (char* actor = *reinterpret_cast<char**>(kPlayerActor)) {
        for (int k = 0; k < 3; ++k) std::memcpy(actor + 0x94 + kStatAt[k], &stat(k), 4);
        if (*reinterpret_cast<float*>(actor + 0x98) > rec[0]) std::memcpy(actor + 0x98, &rec[0], 4);
    }
}

// ---- the game loop (FUN_00410440 runs FUN_004106e0 once per 60 Hz step, then renders) --------------
using StepFn = void(__fastcall*)(char active, char arg);   // FUN_004106e0: `active` = a real step, 0 = an idle one
using FrameFn = int(__cdecl*)();                           // FUN_00410440
constexpr std::uintptr_t kStep = 0x4106e0, kFrame = 0x410440;
constexpr std::uintptr_t kDevice = 0x7651c0;   // the game's IDirect3DDevice9*
constexpr std::uint8_t kStepBytes[] = {0x83, 0xec, 0x30, 0x88, 0x0d, 0xf0, 0xc0, 0x74, 0x00, 0x88, 0x15, 0xf1, 0xc0, 0x74, 0x00, 0x0f};
constexpr std::uint8_t kFrameBytes[] = {0xa1, 0xc0, 0x51, 0x76, 0x00, 0x50, 0x8b, 0x08, 0xff, 0x51, 0x0c, 0x85, 0xc0, 0x79, 0x1e, 0x3d};

StepFn g_step = nullptr;
FrameFn g_frame = nullptr;
bool g_hooked = false;

// While the F1 panel is open the steps are idle ones, as the game runs a frame it has no step for.
// A mod's game.speed: the real steps are run more or less often than the game asks for them.
void __fastcall hook_step(char active, char arg) {
    if (ui_holds_game() || paused()) active = 0;
    if (!active) return g_step(0, arg);
    static double due = 0;
    due += speed();
    if (due < 1.0) return g_step(0, arg);   // a slowed game: an idle step
    for (; due >= 1.0; due -= 1.0) {
        g_step(1, arg);
        on_tick();
    }
}

int __cdecl hook_frame() {
    ui_attach(*reinterpret_cast<void**>(kDevice));
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

// ---- the sessions (menu_re/SAVELOAD.md of CleriaCore) --------------------------------------------------
//   FUN_0041fd10()           resets the flags for g_flags[150]'s character. The title's "start a game"
//                            helpers (0x575970..0x575bff) call it; so do the boot and both loaders.
//   FUN_0042f5e0(file)       reads a save into the game (the Load page); FUN_0042fb90(file) is the other reader
//   FUN_0042e9a0(file)       writes a save
// Both readers and the writer answer a negative code when they fail.
using InitFn = void(__cdecl*)();
using SaveFn = int(__fastcall*)(char* file);
constexpr std::uintptr_t kInit = 0x41fd10, kLoad = 0x42f5e0, kLoad2 = 0x42fb90, kSave = 0x42e9a0;
constexpr std::uintptr_t kStartFirst = 0x575970, kStartLast = 0x575c00;
constexpr std::uint8_t kInitBytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x81, 0xec, 0x04, 0x04, 0x00, 0x00, 0x53, 0x56, 0x8b, 0x35};
constexpr std::uint8_t kLoadBytes[] = {0x55, 0x8b, 0xec, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0xc0, 0x4b, 0x67, 0x00};
constexpr std::uint8_t kLoad2Bytes[] = {0x55, 0x8b, 0xec, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0xe0, 0x4b, 0x67, 0x00};
constexpr std::uint8_t kSaveBytes[] = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68, 0x78, 0x4b, 0x67, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00};

InitFn g_init = nullptr;
SaveFn g_load = nullptr, g_load2 = nullptr, g_save = nullptr;

void __cdecl hook_init() {
    const std::uintptr_t from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    g_init();
    if (from >= kStartFirst && from < kStartLast) on_new_game();
}

// The name is copied first: the game's buffer is not ours to trust after the call.
int loaded(SaveFn original, char* file) {
    const std::string name = file ? file : "";
    const int r = original(file);
    if (r >= 0) on_loaded(name.c_str());
    return r;
}
int __fastcall hook_load(char* file) { return loaded(g_load, file); }
int __fastcall hook_load2(char* file) { return loaded(g_load2, file); }

int __fastcall hook_save(char* file) {
    const std::string name = file ? file : "";
    const int r = g_save(file);
    if (r >= 0) on_saved(name.c_str());
    return r;
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
    SITE(kInit, kInitBytes, &hook_init, &g_init),         SITE(kSave, kSaveBytes, &hook_save, &g_save),
    SITE(kLoad, kLoadBytes, &hook_load, &g_load),         SITE(kLoad2, kLoad2Bytes, &hook_load2, &g_load2),
    SITE(kDealt, kDealtBytes, &hook_dealt, &g_dealt),     SITE(kTaken, kTakenBytes, &hook_taken, &g_taken),
    SITE(kExpKill, kExpKillBytes, &hook_exp_kill, &g_exp_kill), SITE(kKill, kKillBytes, &hook_kill, &g_kill),
    SITE(kExpBoss, kExpAddBytes, &hook_exp_boss, &g_exp_boss), SITE(kExpOther, kExpAddBytes, &hook_exp_other, &g_exp_other),
    SITE(kRecompute, kRecomputeBytes, &hook_recompute, &g_recompute), SITE(kPart, kPartBytes, &hook_part, &g_part),
    SITE(kActorUpdate, kActorUpdateBytes, &hook_actor_update, &g_actor_update), SITE(kScene, kSceneBytes, &hook_scene, &g_scene),
    SITE(kBless, kBlessBytes, &hook_bless, &g_bless),     SITE(kBullet, kBulletBytes, &hook_bullet, &g_bullet),
    SITE(kEffect, kEffectBytes, &hook_effect, &g_effect), SITE(kFetch, kFetchBytes, &hook_fetch, &g_fetch),
    SITE(kSpawn, kSpawnBytes, &hook_spawn, &g_spawn),       SITE(kTouch, kTouchBytes, &hook_touch, &g_touch),
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

// ---- the loose music folder ----------------------------------------------------------------------------------------
// The game streams its music from plain files, release\music\<name>.ogg, <name> from its table of 0x14-byte records
// (DAT_0074c118, DAT_0074c114 of them: 16 characters of name, then the id). They are opened through Windows, not
// through the archive: the game's imports of CreateFileA / W are pointed at ours, and a file of that folder a mod
// provides (files/music/<name>.ogg)
// is opened in its place. cleria.audio.music_file plays any Ogg file the same way: one record (id 30, which no room
// uses) is given a name of ours, and that name leads to the mod's file.
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileAFn g_create_a = nullptr;
CreateFileWFn g_create_w = nullptr;
constexpr wchar_t kOwnMusic[] = L"clmodbgm.ogg";
std::wstring g_music_file;              // what kOwnMusic leads to
std::mutex g_music_mtx;

bool music_redirect(const std::wstring& asked, std::wstring& to) {
    std::wstring p(asked);
    for (wchar_t& c : p) c = c == L'/' ? L'\\' : wchar_t(towlower(c));
    const std::size_t at = p.rfind(L"music\\");
    if (at == std::wstring::npos || (at != 0 && p[at - 1] != L'\\') || p.find(L'\\', at + 6) != std::wstring::npos) return false;
    const std::wstring file = p.substr(at + 6);
    if (file == kOwnMusic) {
        std::lock_guard<std::mutex> lk(g_music_mtx);
        if (g_music_file.empty()) return false;
        to = g_music_file;
        log("[files] music: %s", std::filesystem::path(to).u8string().c_str());
        return true;
    }
    std::string name = "music/";
    for (wchar_t c : file) {
        if (c > 127) return false;
        name += char(c);
    }
    const auto& t = cleria::vfs::table();
    const auto it = t.find(cleria::vfs::key(name));
    if (it == t.end()) return false;
    to = it->second.file.wstring();
    static std::set<std::string> told;
    if (told.insert(name).second) log("[files] %s <- %s", name.c_str(), cleria::vfs::layers()[std::size_t(it->second.layer)].id.c_str());
    return true;
}
HANDLE WINAPI hook_create_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD how, DWORD flags, HANDLE tmpl) {
    std::wstring to;
    if (name && music_redirect(name, to)) name = to.c_str();
    return g_create_w(name, access, share, sa, how, flags, tmpl);
}
HANDLE WINAPI hook_create_a(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD how, DWORD flags, HANDLE tmpl) {
    std::wstring to;
    if (name) {
        wchar_t wide[MAX_PATH * 2];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2) > 0 && music_redirect(wide, to))
            return g_create_w(to.c_str(), access, share, sa, how, flags, tmpl);
    }
    return g_create_a(name, access, share, sa, how, flags, tmpl);
}

// The game's own import of a kernel32 function pointed at `to`; -> what it pointed at, null when it has no such
// import. Only the game's calls change: kernel32 itself is left alone (no code of Windows is patched, no thread
// is stopped for it).
void* import_swap(const char* name, void* to) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "kernel32.dll") != 0 || !d->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + d->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const auto* by = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(by->Name, name) != 0) continue;
            DWORD old = 0;
            void* was = reinterpret_cast<void*>(slots->u1.Function);
            if (!VirtualProtect(&slots->u1.Function, 4, PAGE_READWRITE, &old)) return nullptr;
            slots->u1.Function = reinterpret_cast<DWORD>(to);
            VirtualProtect(&slots->u1.Function, 4, old, &old);
            return was;
        }
    }
    return nullptr;
}

bool install_hooks() {
    if (!is_the_build()) return false;
    if (MH_Initialize() != MH_OK) return false;
    g_create_w = static_cast<CreateFileWFn>(import_swap("CreateFileW", &hook_create_w));
    g_create_a = static_cast<CreateFileAFn>(import_swap("CreateFileA", &hook_create_a));
    if (!g_create_w) g_create_w = &CreateFileW;
    if (!g_create_a) g_create_a = &CreateFileA;
    for (const Site& site : kSites)
        if (MH_CreateHook(reinterpret_cast<void*>(site.at), site.hook, site.original) != MH_OK) return false;
    g_hooked = MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    return g_hooked;
}

bool hooked() { return g_hooked; }

bool spawn_pickup(int item, const float* pos, bool resting) {
    if (!g_hooked) return false;
    int id = item;
    spawn_one(&id, pos, resting ? 0x1c : 5);
    return true;
}

float base_stat(int stat) {
    if (!g_hooked || stat < 0 || stat > 2) return 0;
    return g_stat_on ? g_stat_base[stat] : *reinterpret_cast<float*>(kStats + kStatAt[stat]);
}
void recompute_stats() {
    if (!g_hooked || !*reinterpret_cast<void**>(kPlayerActor)) return;
    *reinterpret_cast<std::uint32_t*>(kDirty) |= 0x75;
    reinterpret_cast<RecomputeFn>(kRecompute)();   // through the hook
}

// (kind, effect, pos, dir, speed, accel, gravity, the three ints, the five floats): the player's bullet again, with
// no power and no knock. It is the player's, so it still meets the monsters, for a hit of 0.
bool show_bullet(const double* n) {
    void* player = g_hooked ? *reinterpret_cast<void**>(kPlayerActor) : nullptr;
    if (!player || int(n[0]) == 0xf) return false;                               // (0xf is a bomb, not a bullet)
    const float pos[3] = {float(n[2]), float(n[3]), float(n[4])}, dir[3] = {float(n[5]), float(n[6]), float(n[7])};
    auto bits = [](double v) { const float f = float(v); std::uint32_t u; std::memcpy(&u, &f, 4); return u; };
    const std::uint32_t args[17] = {std::uint32_t(int(n[1])), reinterpret_cast<std::uint32_t>(pos), reinterpret_cast<std::uint32_t>(dir),
                                    bits(n[8]), bits(n[9]), bits(n[10]), 0, 0, std::uint32_t(int(n[11])), std::uint32_t(int(n[12])),
                                    std::uint32_t(int(n[13])), bits(n[14]), bits(n[15]), bits(n[16]), bits(n[17]), bits(n[18]), 0};
    g_showing = true;
    fire_bullet(player, int(n[0]), args);
    g_showing = false;
    return true;
}

// As the Load page's "Yes": the slot byte, then the read, which stages the loaded game's scene itself.
bool load_slot(int slot) {
    if (!g_hooked || slot < 0 || slot > 63) return false;
    char file[32];
    std::snprintf(file, sizeof file, "yso_%02d.bin", slot);
    *reinterpret_cast<std::uint8_t*>(0x74ac3e) = std::uint8_t(slot);
    return hook_load(file) >= 0;
}

// The record of music id 30 named for our own file, then a script's Bgm_Play of it.
bool play_music_file(const std::string& file) {
    char* recs = g_hooked ? *reinterpret_cast<char**>(0x74c118) : nullptr;
    const int count = recs ? *reinterpret_cast<std::int32_t*>(0x74c114) : 0;
    char* rec = nullptr;
    for (int i = 0; i < count && i < 256; ++i)
        if (*reinterpret_cast<std::int32_t*>(recs + i * 0x14 + 0x10) == 30) rec = recs + i * 0x14;
    if (!rec || file.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(g_music_mtx);
        g_music_file = std::filesystem::u8path(file).wstring();
    }
    std::memset(rec, 0, 16);
    std::memcpy(rec, "CLMODBGM", 8);
    reinterpret_cast<void(__fastcall*)(int, int)>(0x432fc0)(0, 0);     // Bgm_Stop: the same id may be playing our last file
    reinterpret_cast<void(__fastcall*)(int, int)>(0x432ee0)(30, 0);    // Bgm_Play
    return true;
}

bool puppet_effect(void* actor, const double* n) {
    if (!g_hooked || !actor) return false;
    auto bits = [](double v) { const float f = float(v); std::int32_t u; std::memcpy(&u, &f, 4); return u; };
    std::int32_t args[10] = {std::int32_t(n[0]), bits(n[1]), bits(n[2]), bits(n[3]), bits(n[4]), std::int32_t(n[5]), 0, 0, 0, 0};
    g_effect_shown = true;
    g_effect(actor, args);
    g_effect_shown = false;
    return true;
}

void set_exp_factor(float f) {
    if (g_hooked) *reinterpret_cast<float*>(kExpRate) = f;
}

bool game_file_own(const char* path, std::vector<std::uint8_t>& out) {
    void* archive = g_hooked ? *reinterpret_cast<void**>(kArchive) : nullptr;
    files::TocEntry* e = archive ? g_lookup(archive, path) : nullptr;
    if (!e || e->size == 0) return false;
    std::vector<std::uint8_t> raw(e->size);
    Reader r{e, 0};
    std::uint32_t got = 0;
    g_read(&r, raw.data(), e->size, &got);
    if (got != e->size) return false;
    const std::size_t n = std::strlen(path);
    if (n > 2 && path[n - 2] == '.' && (path[n - 1] == 'z' || path[n - 1] == 'Z')) return files::unpack(raw, out);
    out = std::move(raw);
    return true;
}
bool game_file(const char* path, std::vector<std::uint8_t>& out) {
    void* archive = g_hooked ? *reinterpret_cast<void**>(kArchive) : nullptr;
    files::TocEntry* e = archive ? hook_lookup(archive, nullptr, path) : nullptr;
    if (!e || e->size == 0) return false;
    std::vector<std::uint8_t> raw(e->size);
    Reader r{e, 0};
    std::uint32_t got = 0;
    hook_read(&r, nullptr, raw.data(), e->size, &got);
    if (got != e->size) return false;
    const std::size_t n = std::strlen(path);
    if (n > 2 && std::strcmp(path + n - 2, ".z") == 0) return files::unpack(raw, out);
    out = std::move(raw);
    return true;
}

}  // namespace ore
