#pragma once

// =============================================================================
//  ip::Config - simple key=value text config (no external deps).
//  Stores: toggle key, inventory overlay position & scale, runtime offsets.
//
//  All vtable-indices / struct offsets are runtime-configurable so the mod
//  can be tuned for a new MCPE version without recompiling. Default values
//  are PLACEHOLDERS derived from prior analysis - confirm via the diagnostic
//  panel before trusting them.
// =============================================================================

#include <fstream>
#include <sstream>
#include <string>

namespace ip {

// ---- UI state -------------------------------------------------------------
inline int   g_toggleKey     = 'I';   // ImGui toggle key (placeholder)
inline bool  g_changingKey   = false;
inline bool  g_showInventory = false; // overlay on/off
inline float g_posX          = 20.0f; // top-left X of inventory panel (screen px)
inline float g_posY          = 100.0f;// top-left Y of inventory panel (screen px)
inline float g_scale         = 1.0f;  // reserved for future item scaling

// ---- Runtime offsets (placeholders, tune via diagnostic UI) ---------------
// All values loaded from config file at startup; defaults below are best
// guesses from static analysis of libminecraftpe 1.26.30.
inline int       g_clientInstanceGetLocalPlayerVfIndex = 0x14;  // ClientInstance::getLocalPlayer vtable slot
inline uintptr_t g_playerInventoryOffset               = 0x1B8; // Player -> PlayerInventory* (getInventoryMenu result)
inline int       g_playerInventoryGetItemVfIndex       = 0x06;  // PlayerInventory::getItem vtable slot
inline uintptr_t g_itemStackCountOffset                = 0x22;  // ItemStackBase -> mCount (uint8)
inline int       g_containerIdPlayer                   = 0;     // CONTAINER_ID_INVENTORY (confirmed via enum registration)

// ---- Diagnostic state -----------------------------------------------------
inline bool  g_showDiagnostics = false;
inline void* g_lastClientInstance = nullptr;     // captured for diagnostics
inline void* g_lastLocalPlayer    = nullptr;
inline void* g_lastPlayerInv      = nullptr;

inline std::string ip_getConfigPath() { return "../itempack.conf"; }

inline int ip_clampPercent(int value) {
    if (value < 0)   return 0;
    if (value > 200) return 200;
    return value;
}

inline void ip_loadConfig() {
    std::ifstream f(ip_getConfigPath());
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        std::istringstream ss(val);
        if      (key == "toggleKey")                              { int v; if (ss >> v) g_toggleKey = v; }
        else if (key == "posX")                                   { float v; if (ss >> v) g_posX = v; }
        else if (key == "posY")                                   { float v; if (ss >> v) g_posY = v; }
        else if (key == "scale")                                  { float v; if (ss >> v) g_scale = v; }
        else if (key == "clientGetLocalPlayerVf")                 { int v; if (ss >> v) g_clientInstanceGetLocalPlayerVfIndex = v; }
        else if (key == "playerInventoryOffset")                  { unsigned long long v; if (ss >> std::hex >> v) g_playerInventoryOffset = (uintptr_t)v; }
        else if (key == "playerInventoryGetItemVf")               { int v; if (ss >> v) g_playerInventoryGetItemVfIndex = v; }
        else if (key == "itemStackCountOffset")                   { unsigned long long v; if (ss >> std::hex >> v) g_itemStackCountOffset = (uintptr_t)v; }
        else if (key == "containerIdPlayer")                      { int v; if (ss >> v) g_containerIdPlayer = v; }
    }
}

inline void ip_saveConfig() {
    std::ofstream f(ip_getConfigPath());
    if (!f.is_open()) return;
    f << "toggleKey="                              << g_toggleKey                              << "\n";
    f << "posX="                                   << g_posX                                   << "\n";
    f << "posY="                                   << g_posY                                   << "\n";
    f << "scale="                                  << g_scale                                  << "\n";
    f << "clientGetLocalPlayerVf="                 << g_clientInstanceGetLocalPlayerVfIndex    << "\n";
    f << "playerInventoryOffset=0x"                << std::hex << g_playerInventoryOffset      << std::dec << "\n";
    f << "playerInventoryGetItemVf="               << g_playerInventoryGetItemVfIndex          << "\n";
    f << "itemStackCountOffset=0x"                 << std::hex << g_itemStackCountOffset       << std::dec << "\n";
    f << "containerIdPlayer="                      << g_containerIdPlayer                      << "\n";
}

} // namespace ip
