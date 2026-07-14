#pragma once

// =============================================================================
//  ip::SigScanner
//  Signature + string + vtable scanning for stripped libminecraftpe.so
//  (no symbols, no RTTI).
//
//  Three capabilities:
//   1. resolveSignature()  - pl::Signature pattern based (placeholder patterns).
//   2. findFunctionByString() - locate a NUL string in .rodata, then scan .text
//      for an ADRP+ADD pair that materialises that address; walk back to the
//      function prologue. Works on fully stripped binaries.
//   3. findVtableByFunction() / findAndHookVtable() - scan .data.rel.ro for a
//      pointer to a known function to recover a class vtable (no RTTI needed),
//      then patch a chosen slot.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <android/log.h>

#include "pl/Gloss.h"
#include "pl/Signature.h"

namespace ip {

inline constexpr const char* MCPE_LIB = "libminecraftpe.so";

#define IP_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "[itempack]", __VA_ARGS__)
#define IP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "[itempack]", __VA_ARGS__)

// ---------------------------------------------------------------------------
//  Section helpers
// ---------------------------------------------------------------------------
inline uintptr_t getSection(const char* lib, const char* section, size_t* outSize) {
    return GlossGetLibSection(lib, section, outSize);
}

inline void unprotect(uintptr_t addr, size_t len) {
    Unprotect(addr, len);
}

// ---------------------------------------------------------------------------
//  1. pl::Signature resolver (placeholder patterns - update per version)
// ---------------------------------------------------------------------------
inline void* resolveSignature(const char* sig, const char* name, const char* lib = MCPE_LIB) {
    uintptr_t addr = pl::signature::pl_resolve_signature(sig, lib);
    if (!addr) { IP_LOGE("sig not found: %s", name); return nullptr; }
    IP_LOGI("found %s @ 0x%lx", name, (unsigned long)addr);
    return reinterpret_cast<void*>(addr);
}

// ---------------------------------------------------------------------------
//  Raw byte scanner
// ---------------------------------------------------------------------------
inline uintptr_t scanBytes(uintptr_t base, size_t size, const void* pat, size_t patLen) {
    auto* m = (const uint8_t*)base;
    auto* p = (const uint8_t*)pat;
    if (patLen == 0 || size < patLen) return 0;
    for (size_t i = 0; i + patLen <= size; ++i)
        if (std::memcmp(m + i, p, patLen) == 0) return base + i;
    return 0;
}

// ---------------------------------------------------------------------------
//  Wildcard pattern scanner
//  Pattern format: "AA BB ?? CC" where "??" matches any byte.
//  Spaces are optional separators. Returns first match address or 0.
// ---------------------------------------------------------------------------
inline uintptr_t scanPattern(uintptr_t base, size_t size, const char* pattern) {
    if (!pattern || !base || size == 0) return 0;

    // Parse pattern into bytes + mask
    uint8_t pat[256];
    bool    mask[256]; // true = must match, false = wildcard
    int     patLen = 0;

    const char* s = pattern;
    while (*s && patLen < 256) {
        // skip spaces
        if (*s == ' ') { ++s; continue; }
        // wildcard byte
        if (s[0] == '?' && (s[1] == '?' || s[1] == ' ' || s[1] == '\0')) {
            pat[patLen]  = 0;
            mask[patLen] = false;
            ++patLen;
            s += (s[1] == '?') ? 2 : 1;
            continue;
        }
        // hex byte
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int hi = hexVal(s[0]);
        int lo = hexVal(s[1]);
        if (hi < 0 || lo < 0) { ++s; continue; } // skip invalid char
        pat[patLen]  = (uint8_t)((hi << 4) | lo);
        mask[patLen] = true;
        ++patLen;
        s += 2;
    }

    if (patLen == 0) return 0;
    auto* m = (const uint8_t*)base;
    for (size_t i = 0; i + patLen <= size; ++i) {
        bool match = true;
        for (int j = 0; j < patLen; ++j) {
            if (mask[j] && m[i + j] != pat[j]) { match = false; break; }
        }
        if (match) return base + i;
    }
    return 0;
}

// Scan a section for a wildcard pattern.
inline uintptr_t scanPatternInSection(const char* pattern, const char* section = ".text",
                                      const char* lib = MCPE_LIB) {
    size_t sz = 0;
    uintptr_t base = getSection(lib, section, &sz);
    if (!base || !sz) return 0;
    return scanPattern(base, sz, pattern);
}

// ---------------------------------------------------------------------------
//  2. String-based scanner (stripped binary, no RTTI)
// ---------------------------------------------------------------------------

// Find a NUL-terminated string in .rodata, return its runtime address.
inline uintptr_t findStringInRodata(const char* str, const char* lib = MCPE_LIB) {
    size_t sz = 0;
    uintptr_t rodata = getSection(lib, ".rodata", &sz);
    if (!rodata || !sz || !str) return 0;
    return scanBytes(rodata, sz, str, std::strlen(str) + 1);
}

namespace arm64 {

inline bool isAdrp(uint32_t insn)    { return (insn & 0x9F000000u) == 0x90000000u; } // ADRP Xd, page
inline bool isAddImm64(uint32_t insn){ return (insn & 0xFF000000u) == 0x91000000u; } // ADD (imm) 64-bit
inline bool isRet(uint32_t insn)     { return (insn & 0xFFFFFC1Fu) == 0xD65F0000u; } // RET
inline bool isSubSpSpImm(uint32_t insn){ return (insn & 0xFF0003FFu) == 0xD10003FFu; } // SUB SP,SP,#imm

struct AdrpAdd { uintptr_t target; int reg; bool valid; };

// Decode an adjacent (or near) ADRP + ADD(immediate) pair into the absolute
// address they build. adrpPc is the runtime address of the ADRP instruction.
inline AdrpAdd decodeAdrpAdd(uint32_t adrp, uint32_t add, uintptr_t adrpPc) {
    AdrpAdd r{}; r.valid = false;
    int rd = adrp & 0x1F;          // ADRP destination
    int rn = (add >> 5) & 0x1F;    // ADD source register
    if (rn != rd) return r;        // ADD must consume the ADRP result

    uint64_t immlo = (adrp >> 29) & 0x3;
    uint64_t immhi = (adrp >> 5)  & 0x7FFFF;
    uint64_t imm   = (immhi << 2) | immlo;        // 21-bit signed immediate
    if (imm & (1ULL << 20)) imm |= ~0ULL << 21;   // sign extend
    uintptr_t page = (adrpPc & ~0xFFFULL) + (imm << 12);

    uint64_t imm12 = (add >> 10) & 0xFFF;
    int      shift = (add >> 22) & 0x3;
    uint64_t addImm = (shift == 1) ? (imm12 << 12) : imm12;

    r.target = page + addImm;
    r.reg    = rd;
    r.valid  = true;
    return r;
}

// Walk backward from adrpAddr to a likely function start.
inline uintptr_t findFunctionStart(uintptr_t adrpAddr, uintptr_t textBase, size_t textSize) {
    const size_t kMaxBack = 0x2000;
    uintptr_t lower = adrpAddr;
    if (adrpAddr > textBase) {
        size_t back = adrpAddr - textBase;
        if (back > kMaxBack) back = kMaxBack;
        lower = adrpAddr - back;
    }
    for (uintptr_t p = adrpAddr; p >= lower; p -= 4) {
        uint32_t insn = *reinterpret_cast<uint32_t*>(p);
        if (isSubSpSpImm(insn)) return p;          // typical prologue
        if (isRet(insn))        return p + 4;      // function begins after a RET
    }
    return adrpAddr; // fallback
}

} // namespace arm64

// Scan .text for an ADRP+ADD pair that references targetAddr; return the
// containing function's start address.
inline uintptr_t findFunctionReferencingAddr(uintptr_t targetAddr, const char* lib = MCPE_LIB) {
    size_t tsz = 0;
    uintptr_t text = getSection(lib, ".text", &tsz);
    if (!text || !tsz) return 0;

    for (size_t i = 0; i + 8 <= tsz; i += 4) {
        uint32_t a = *reinterpret_cast<uint32_t*>(text + i);
        if (!arm64::isAdrp(a)) continue;
        int rd = a & 0x1F;

        // The ADD that consumes the ADRP result is usually the next insn,
        // but may be up to a few instructions later.
        for (size_t j = 1; j <= 4 && i + j * 4 + 4 <= tsz; ++j) {
            uint32_t b = *reinterpret_cast<uint32_t*>(text + i + j * 4);
            if (!arm64::isAddImm64(b)) continue;
            int rn = (b >> 5) & 0x1F;
            if (rn != rd) continue;
            auto r = arm64::decodeAdrpAdd(a, b, text + i);
            if (r.valid && r.target == targetAddr)
                return arm64::findFunctionStart(text + i, text, tsz);
            break; // matched ADD for this reg but wrong target
        }
    }
    return 0;
}

// Find a function that references the literal string `str` (e.g. a log/error
// string unique to the target method). Returns 0 on failure.
inline uintptr_t findFunctionByString(const char* str, const char* name, const char* lib = MCPE_LIB) {
    uintptr_t s = findStringInRodata(str, lib);
    if (!s) { IP_LOGE("string not found: '%s' (%s)", str, name); return 0; }
    uintptr_t fn = findFunctionReferencingAddr(s, lib);
    if (!fn) { IP_LOGE("no ADRP+ADD ref to '%s' for %s", str, name); return 0; }
    IP_LOGI("found %s @ 0x%lx (via string '%s')", name, (unsigned long)fn, str);
    return fn;
}

// ---------------------------------------------------------------------------
//  3. Vtable scanner (no RTTI)
// ---------------------------------------------------------------------------

// Scan .data.rel.ro for a slot holding funcAddr; return the slot address.
inline uintptr_t findVtableSlotAddress(uintptr_t funcAddr, const char* lib = MCPE_LIB) {
    size_t dsz = 0;
    uintptr_t drr = getSection(lib, ".data.rel.ro", &dsz);
    if (!drr || !dsz) return 0;
    for (size_t i = 0; i + sizeof(uintptr_t) <= dsz; i += sizeof(uintptr_t))
        if (*reinterpret_cast<uintptr_t*>(drr + i) == funcAddr) return drr + i;
    return 0;
}

// Given a known function that lives at vtable slot `knownSlot`, recover the
// vtable base address.
inline uintptr_t findVtableByFunction(uintptr_t funcAddr, int knownSlot, const char* lib = MCPE_LIB) {
    uintptr_t slotAddr = findVtableSlotAddress(funcAddr, lib);
    if (!slotAddr) return 0;
    return slotAddr - (uintptr_t)knownSlot * sizeof(void*);
}

// Patch a vtable slot (vtable base already known).
inline bool hookVtableSlot(uintptr_t vtblBase, int slot, void* hookFn, void** outOrig) {
    if (!vtblBase) return false;
    void** vt = reinterpret_cast<void**>(vtblBase);
    uintptr_t slotAddr = vtblBase + (uintptr_t)slot * sizeof(void*);
    unprotect(slotAddr, sizeof(void*));
    if (outOrig) *outOrig = vt[slot];
    vt[slot] = hookFn;
    __builtin___clear_cache((char*)slotAddr, (char*)(slotAddr + sizeof(void*)));
    return true;
}

// True if addr points into the lib's .text section.
inline bool isInText(uintptr_t addr, const char* lib = MCPE_LIB) {
    size_t tsz = 0;
    uintptr_t text = getSection(lib, ".text", &tsz);
    return text && tsz && addr >= text && addr < text + tsz;
}

// Find a vtable via a known function+slot, sanity check that the target slot
// currently holds a .text pointer, then patch targetSlot with hookFn.
inline bool findAndHookVtable(uintptr_t knownFunc, int knownSlot,
                              int targetSlot, void* hookFn, void** outOrig,
                              const char* clsName, const char* lib = MCPE_LIB) {
    if (!knownFunc) { IP_LOGE("knownFunc null for %s", clsName); return false; }
    uintptr_t vtbl = findVtableByFunction(knownFunc, knownSlot, lib);
    if (!vtbl) { IP_LOGE("vtable not found for %s", clsName); return false; }

    void** vt = reinterpret_cast<void**>(vtbl);
    uintptr_t cur = reinterpret_cast<uintptr_t>(vt[targetSlot]);
    if (!isInText(cur, lib)) {
        IP_LOGE("vtable sanity check failed for %s (slot %d = 0x%lx not in .text)",
                clsName, targetSlot, (unsigned long)cur);
        return false;
    }

    IP_LOGI("vtable for %s @ 0x%lx, hooking slot %d (was 0x%lx)",
            clsName, (unsigned long)vtbl, targetSlot, (unsigned long)cur);
    return hookVtableSlot(vtbl, targetSlot, hookFn, outOrig);
}

} // namespace ip
