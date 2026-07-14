#pragma once

// =============================================================================
//  ip::Keybinds - ImGui-driven keybind/toggle handling.
//  On Android there is no physical keyboard, so the "toggle key" is exposed
//  through the ImGui overlay (a tap button). This header keeps state shared
//  between the overlay and the render hook.
// =============================================================================

#include "config.h"

namespace ip {

// Set to true while the overlay's "change key" capture is active.
inline bool g_keyDown = false;

inline void ip_register_keybinds() {}

// Starts the ImGui overlay thread (defined in main.cpp).
void ip_initModMenu();

} // namespace ip
