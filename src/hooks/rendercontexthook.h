#pragma once

// =============================================================================
//  ip::RenderContextHook - hooks MinecraftUIRenderContext::drawText (vtable
//  slot 6) to capture the active render context + font, and triggers the
//  inventory overlay render once per frame.
//
//  The vtable is located at runtime by the sigscanner (find drawImage via the
//  "drawImage callback overload" string -> slot 8 -> vtable base -> hook slot 6).
// =============================================================================

#include <string>
#include "ui/minecraftuirendercontext.h"
#include "util/config.h"

class Font;
struct RectangleArea;
struct TextMeasureData;
struct CaretMeasureData;

namespace ip {

// Captured each time drawText runs.
inline MinecraftUIRenderContext* g_activeUIContext = nullptr;
inline Font*                     g_activeUIFont    = nullptr;

// Toggled true by the eglSwapBuffers hook at every frame boundary so the
// inventory overlay renders exactly once per frame (on the first drawText).
inline bool g_newFrame = true;

using MinecraftUIRenderContext_drawText_t = void (*)(
    MinecraftUIRenderContext*,
    Font&,
    const RectangleArea&,
    const std::string&,
    const mce::Color&,
    ui::TextAlignment,
    float,
    const TextMeasureData&,
    const CaretMeasureData&);

inline MinecraftUIRenderContext_drawText_t g_drawText_orig = nullptr;

// Defined in render/inventoryrenderer.h
inline void ip_renderInventory(MinecraftUIRenderContext* ctx);

inline void drawText_hook(
    MinecraftUIRenderContext* self,
    Font& font,
    const RectangleArea& rect,
    const std::string& text,
    const mce::Color& color,
    ui::TextAlignment align,
    float alpha,
    const TextMeasureData& tmd,
    const CaretMeasureData& cmd)
{
    g_activeUIContext = self;
    g_activeUIFont    = &font;

    if (g_drawText_orig)
        g_drawText_orig(self, font, rect, text, color, align, alpha, tmd, cmd);

    // Render the inventory overlay once per frame. g_newFrame is set true by
    // the eglSwapBuffers hook. Re-entry into drawText (from our renderer) is
    // safe because g_newFrame is cleared before rendering.
    if (g_showInventory && g_newFrame) {
        g_newFrame = false;
        ip_renderInventory(self);
    }
}

} // namespace ip
