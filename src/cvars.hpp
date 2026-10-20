// Finds Unreal console variables in the running game and reads and sets them. Adapted from
// UntilDawnTweaks' cvars.hpp for Still Wakes the Deep (UE 5.4, no PDB): the layouts below come
// from UE 5.4's IConsoleManager.h (MSVC vtable order = declaration order, virtual destructor
// first), not from symbols.
//
// IConsoleManager::Singleton is found from the game's own code: cvar lookups load it with
// `mov rcx, [rip+Singleton]` shortly before `lea rdx, [rip+L"r.ScreenPercentage"]`. A few
// unrelated matches exist, so the address most matches agree on wins. The code is scanned in
// memory, after SteamStub has decrypted it.
//
// Vtable offsets (UE 5.4):
//   IConsoleManager  +0xb8  FindConsoleVariable(const TCHAR* Name, bool bTrackFrequentCalls)
//                           (5 RegisterConsoleVariable, 4 ...Ref, BitRef, 3 sink functions,
//                           8 RegisterConsoleCommand, 2 UnregisterConsoleObject before it)
//   IConsoleObject   +0x00 ~dtor  +0x08 GetHelp  +0x10 SetHelp  +0x18 GetFlags  +0x20 SetFlags
//                    +0x28 GetParentObject (new since UE 5.1; shifts the rest by 8)
//                    +0x30 AsVariable  +0x38 IsVariableBool  +0x40 IsVariableInt
//                    +0x48 IsVariableFloat  +0x50 IsVariableString  ...  +0x80 Release
//   IConsoleVariable +0x88 Set(const TCHAR* Value, EConsoleVariableFlags SetBy, FName Tag)
//                    +0x90 Unset  +0x98 GetBool  +0xa0 GetInt  +0xa8 GetFloat
//
// Setting from ReShade's thread: a plain Set off the game thread stores the value but never
// copies it to the render thread's shadow (OnCVarChange's check(0) is compiled out). With
// ECVF_Set_SetOnly_Unsafe, OnCVarChange copies it straight across and FConsoleVariableBase::
// OnChanged returns before touching flags or running change delegates - made for this. The
// cost: the SetBy priority isn't recorded, so a later scalability apply (a menu change) can
// overwrite the value; re-apply after that.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>

namespace cvars {

constexpr uint32_t kFindConsoleVariable = 0xb8;
constexpr uint32_t kIsVariableBool = 0x38;
constexpr uint32_t kIsVariableInt = 0x40;
constexpr uint32_t kIsVariableFloat = 0x48;
constexpr uint32_t kIsVariableString = 0x50;
constexpr uint32_t kSet = 0x88;
constexpr uint32_t kGetInt = 0xa0;
constexpr uint32_t kGetFloat = 0xa8;
constexpr uint32_t kSetByCode = 0x0d000000;
constexpr uint32_t kSetOnlyUnsafe = 0x00020000;

struct Section {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

inline bool FindSection(const uint8_t* base, const char* name, Section& out) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), name, 8) == 0) {
            out = {base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
            return true;
        }
    return false;
}

// A NUL-terminated UTF-16 string that starts right after another string's terminator.
inline const uint8_t* FindWideString(const Section& s, const wchar_t* text) {
    const size_t n = (wcslen(text) + 1) * 2;
    for (size_t i = 2; i + n <= s.size; i += 2)
        if (s.begin[i - 2] == 0 && s.begin[i - 1] == 0 && memcmp(s.begin + i, text, n) == 0) return s.begin + i;
    return nullptr;
}

// IConsoleManager::Singleton (the variable holding the pointer), or null. *votes gets how
// many of the matches agreed, *matches how many there were.
inline void** FindConsoleManagerSingleton(const uint8_t* base, int* votes = nullptr, int* matches = nullptr) {
    Section text, rdata;
    if (!FindSection(base, ".text", text) || !FindSection(base, ".rdata", rdata)) return nullptr;
    const uint8_t* name = FindWideString(rdata, L"r.ScreenPercentage");
    if (!name) return nullptr;
    std::map<void**, int> found;
    int total = 0;
    for (size_t i = 0x40; i + 7 <= text.size; ++i) {
        const uint8_t* p = text.begin + i;
        // lea r64, [rip+disp32] -> the name
        if ((p[0] != 0x48 && p[0] != 0x4c) || p[1] != 0x8d || (p[2] & 0xc7) != 0x05) continue;
        int32_t disp;
        memcpy(&disp, p + 3, 4);
        if (p + 7 + disp != name) continue;
        // mov rcx, [rip+disp32] within the 0x40 bytes before it
        for (const uint8_t* q = p - 7; q >= p - 0x40; --q) {
            if (q[0] != 0x48 || q[1] != 0x8b || q[2] != 0x0d) continue;
            memcpy(&disp, q + 3, 4);
            ++found[reinterpret_cast<void**>(const_cast<uint8_t*>(q + 7 + disp))];
            ++total;
            break;
        }
    }
    void** best = nullptr;
    int bestVotes = 0;
    for (auto& [addr, n] : found)
        if (n > bestVotes) best = addr, bestVotes = n;
    if (votes) *votes = bestVotes;
    if (matches) *matches = total;
    return best;
}

template <typename Fn>
inline Fn Virtual(void* object, uint32_t offset) {
    return *reinterpret_cast<Fn*>(*reinterpret_cast<uint8_t**>(object) + offset);
}

// memcpy that reports a fault instead of raising it.
inline bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// True if the object's vtable lies in the module's read-only data (a sanity check before any
// virtual call on a pointer we found ourselves).
inline bool HasModuleVtable(const uint8_t* base, const void* object) {
    Section rdata;
    const uint8_t* vtable = nullptr;
    if (!object || !FindSection(base, ".rdata", rdata) || !SafeCopy(&vtable, object, 8)) return false;
    return vtable >= rdata.begin && vtable < rdata.begin + rdata.size;
}

using FindFn = void*(__fastcall*)(void* self, const wchar_t* name, bool track);
using BoolFn = bool(__fastcall*)(void* self);
using IntFn = int32_t(__fastcall*)(void* self);
using FloatFn = float(__fastcall*)(void* self);
using SetFn = void(__fastcall*)(void* self, const wchar_t* value, uint32_t setBy, uint64_t tag);

// The IConsoleVariable object for <name> (null if absent or a command). Guarded: a wrong
// vtable layout faults here instead of taking the game down.
inline void* FindVariable(void* manager, const wchar_t* name) {
    __try {
        return Virtual<FindFn>(manager, kFindConsoleVariable)(manager, name, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

inline bool CallBool(void* var, uint32_t offset, bool& out) {
    __try {
        out = Virtual<BoolFn>(var, offset)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum class Type { Unknown, Bool, Int, Float, String };

inline const char* TypeName(Type t) {
    switch (t) {
        case Type::Bool: return "bool";
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::String: return "string";
        default: return "?";
    }
}

// The value type, from the IsVariable* slots.
inline Type GetType(void* var) {
    bool is = false;
    if (CallBool(var, kIsVariableFloat, is) && is) return Type::Float;
    if (CallBool(var, kIsVariableInt, is) && is) return Type::Int;
    if (CallBool(var, kIsVariableBool, is) && is) return Type::Bool;
    if (CallBool(var, kIsVariableString, is) && is) return Type::String;
    return Type::Unknown;
}

// The current value as text (as Set accepts it), or false. Off the game thread a render-thread
// -safe cvar reports its render-thread copy.
inline bool GetText(void* var, Type t, wchar_t* out, size_t n) {
    __try {
        if (t == Type::Float) {
            swprintf(out, n, L"%g", Virtual<FloatFn>(var, kGetFloat)(var));
            return true;
        }
        if (t == Type::Int || t == Type::Bool) {
            swprintf(out, n, L"%d", Virtual<IntFn>(var, kGetInt)(var));
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Set from any thread (see the header). False if it faulted.
inline bool Set(void* var, const wchar_t* value) {
    __try {
        Virtual<SetFn>(var, kSet)(var, value, kSetByCode | kSetOnlyUnsafe, 0);  // Tag = NAME_None
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace cvars
