#pragma once

// =============================================================================
//  ip::Inventory - player inventory access chain for stripped libminecraftpe.so.
//
//  Chain:  ClientInstance -> LocalPlayer -> PlayerInventory -> ItemStack*
//
//  ClientInstance::getLocalPlayer()  : vtable call (slot from config).
//  LocalPlayer -> PlayerInventory    : inventory lives at a fixed offset
//                                      inside the Player (offset from config).
//  PlayerInventory::getItem(slot, containerId) : vtable call (slot from config),
//                                      returns ItemStackBase*.
//
//  Slot layout:
//    0..8   hotbar
//    9..35  main inventory (3 rows x 9)
//    36..39 armor
//    40     offhand
//
//  All offsets/slots are runtime-configurable via ip::g_* variables in
//  config.h. Defaults are PLACEHOLDERS - tune via the diagnostic UI.
//
//  Confirmed from static analysis of libminecraftpe 1.26.30:
//    - containerIdPlayer = 0 (CONTAINER_ID_INVENTORY, via enum registration
//      function at 0xDB2DB04).
//  Placeholders (must verify at runtime):
//    - clientGetLocalPlayerVfIndex = 0x14
//    - playerInventoryOffset       = 0x1B8
//    - playerInventoryGetItemVf    = 0x06
//    - itemStackCountOffset        = 0x22
// =============================================================================

#include <cstdint>
#include "itemstack.h"
#include "util/config.h"

class ClientInstance;
class LocalPlayer;
class PlayerInventory;

namespace ip {

// Slot layout (fixed by Minecraft data format).
inline constexpr int kHotbarFirst          = 0;
inline constexpr int kHotbarLast           = 8;
inline constexpr int kMainFirst            = 9;
inline constexpr int kMainLast             = 35;
inline constexpr int kArmorFirst           = 36;
inline constexpr int kArmorLast            = 39;
inline constexpr int kOffhandSlot          = 40;
inline constexpr int kTotalDisplaySlots    = 36; // hotbar(9) + main(27) = 9x4 grid

// ---- Safe vtable call helpers --------------------------------------------

// Validate a vtable index is in a plausible range before calling.
inline bool isValidVfIndex(int idx) { return idx >= 0 && idx < 0x400; }

// Validate a pointer looks plausible (non-null, in user address space).
// Templated so it accepts both object pointers and function pointers
// (C++ forbids implicit function-pointer-to-void* conversion).
template <typename T>
inline bool plausiblePtr(T p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    return v > 0x1000 && v < 0x7FFFFFFFFFFF;
}

// ClientInstance::getLocalPlayer() via vtable.
// Returns nullptr on any sanity-check failure (no crash).
inline LocalPlayer* getLocalPlayer(ClientInstance* client) {
    g_lastClientInstance = client;
    if (!plausiblePtr(client)) return nullptr;
    void** vt = *reinterpret_cast<void***>(client);
    if (!plausiblePtr(vt)) return nullptr;
    if (!isValidVfIndex(g_clientInstanceGetLocalPlayerVfIndex)) return nullptr;
    using Fn = LocalPlayer* (*)(void*);
    auto fn = reinterpret_cast<Fn>(vt[g_clientInstanceGetLocalPlayerVfIndex]);
    if (!plausiblePtr(fn)) return nullptr;
    LocalPlayer* p = fn(client);
    g_lastLocalPlayer = p;
    return plausiblePtr(p) ? p : nullptr;
}

// LocalPlayer -> PlayerInventory. Corresponds to getInventoryMenu() returning
// (this + g_playerInventoryOffset). Treated as an embedded subobject, not a
// pointer - if offset is wrong we just return a bad pointer that callers
// must validate before dereferencing.
inline PlayerInventory* getPlayerInventory(LocalPlayer* player) {
    if (!plausiblePtr(player)) return nullptr;
    if (g_playerInventoryOffset == 0 || g_playerInventoryOffset > 0x10000) return nullptr;
    auto* pinv = reinterpret_cast<PlayerInventory*>(
        reinterpret_cast<char*>(player) + g_playerInventoryOffset);
    g_lastPlayerInv = pinv;
    return pinv;
}

// PlayerInventory::getItem(slot, containerId) -> ItemStackBase*, via vtable.
// Returns nullptr on any sanity-check failure (no crash).
inline ItemStackBase* getInventoryItem(PlayerInventory* inv, int slot,
                                       int containerId = g_containerIdPlayer) {
    if (!plausiblePtr(inv)) return nullptr;
    void** vt = *reinterpret_cast<void***>(inv);
    if (!plausiblePtr(vt)) return nullptr;
    if (!isValidVfIndex(g_playerInventoryGetItemVfIndex)) return nullptr;
    using Fn = ItemStackBase* (*)(void*, int, int);
    auto fn = reinterpret_cast<Fn>(vt[g_playerInventoryGetItemVfIndex]);
    if (!plausiblePtr(fn)) return nullptr;
    ItemStackBase* s = fn(inv, slot, containerId);
    return plausiblePtr(s) ? s : nullptr;
}

// Best-effort stack count read (configurable offset). Returns 0 if unknown.
// Validates the offset is within the ItemStackBase (sizeof==0x88).
inline uint8_t getStackCount(ItemStackBase* s) {
    if (!plausiblePtr(s)) return 0;
    if (g_itemStackCountOffset >= 0x88) return 0;
    return *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(s) + g_itemStackCountOffset);
}

inline bool stackHasItem(ItemStackBase* s) {
    if (!plausiblePtr(s)) return false;
    // mItem is a WeakPtr<Item> at offset 0x08 (after vtable ptr at 0x00).
    // WeakPtr is 8 bytes (just a SharedCounter*). We treat "has item" as
    // "mItem.get() != nullptr" but guard against the WeakPtr layout being
    // wrong by validating the pointer.
    auto* item = s->mItem.get();
    return plausiblePtr(item);
}

// ---- Diagnostic: try to read an item name from ItemStackBase --------------
// ItemStackBase has a WeakPtr<Item> at +0x08. Item has a HashedString name
// at some offset (placeholder). Returns "<unknown>" if anything fails.
inline const char* getStackItemName(ItemStackBase* s) {
    if (!plausiblePtr(s)) return "<null>";
    auto* item = s->mItem.get();
    if (!plausiblePtr(item)) return "<empty>";
    // TODO: read Item::mName (HashedString) - offset is version-specific.
    // For now, just report that an item is present.
    return "<item>";
}

} // namespace ip
