#pragma once

// =============================================================================
//  ip::InventoryRenderer - draws the player inventory grid using
//  MinecraftUIRenderContext (panel/slots/counts) and ItemRenderer (item icons).
//
//  Layout: 9 columns x 4 rows (row 0 = hotbar, rows 1..3 = main inventory).
//  Rendering happens inside the drawText hook (game render pass), once per
//  frame, using the context captured from MinecraftUIRenderContext::mClient.
// =============================================================================

#include <cstdio>
#include <cstdint>

#include "ui/minecraftuirendercontext.h"
#include "ui/hashedstring.h"
#include "ui/resourcelocation.h"
#include "inventory/inventory.h"
#include "inventory/itemstack.h"
#include "render/helper.h"
#include "hooks/rendercontexthook.h"
#include "util/config.h"

namespace ip {

namespace invrender {
inline constexpr int   kColumns      = 9;
inline constexpr int   kRows         = 4; // hotbar + 3 main rows
inline constexpr float kSlotStride   = 18.0f;
inline constexpr float kSlotDrawSize = 17.5f;
inline constexpr float kPanelPadding = 6.0f;
inline constexpr float kItemDrawSize = 16.0f;
inline constexpr float kItemInset    = (kSlotStride - kItemDrawSize) * 0.5f;
inline constexpr float kCountTextH   = 6.0f;

inline const HashedString kFlushMaterial("ui_flush");
inline const mce::Color   kWhite{1.0f, 1.0f, 1.0f, 1.0f};
inline const mce::Color   kPanelBg{0.10f, 0.10f, 0.12f, 0.92f};

inline bool hasTex(const mce::TexturePtr& t) { return static_cast<bool>(t.mClientTexture); }

struct CachedTextures { bool loaded = false; mce::TexturePtr panel, slot; };

inline CachedTextures& getTextures(MinecraftUIRenderContext& ctx) {
    static CachedTextures t;
    if (!t.loaded) {
        t.panel = ctx.getTexture(ResourceLocation("textures/ui/dialog_background_opaque"), false);
        t.slot  = ctx.getTexture(ResourceLocation("textures/ui/item_cell"), false);
        t.loaded = true;
    }
    return t;
}

inline void* getMCGame(void* client) {
    if (!client) return nullptr;
    auto** vt = *reinterpret_cast<void***>(client);
    if (vt && vt[kClientGetMinecraftGameVfIndex]) {
        auto fn = reinterpret_cast<void* (*)(void*)>(vt[kClientGetMinecraftGameVfIndex]);
        if (void* r = fn(client)) return r;
    }
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(client) + kClientMinecraftGameOffset);
}

inline void destroyBarc(void* barc) {
    if (!barc) return;
    auto** vt = *reinterpret_cast<void***>(barc);
    if (vt && vt[0]) reinterpret_cast<void (*)(void*)>(vt[0])(barc);
}

template <typename Fn>
inline void forSlots(float ox, float oy, Fn&& fn) {
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < kColumns; ++c)
            fn(r * kColumns + c, ox + c * kSlotStride, oy + r * kSlotStride);
}

inline void drawPanel(MinecraftUIRenderContext& ctx, const RectangleArea& r) {
    ctx.fillRectangle(r, kPanelBg, 1.0f);
}

inline void drawSlot(MinecraftUIRenderContext& ctx, const CachedTextures& t, float sx, float sy) {
    if (hasTex(t.slot))
        ctx.drawImage(t.slot.getClientTexture(), {sx, sy}, {kSlotDrawSize, kSlotDrawSize}, {0, 0}, {1, 1}, false);
}

// Draw item icons via ItemRenderer::renderGuiItemNew, building a temporary
// BaseActorRenderContext from the captured screen context + client.
inline void drawIcons(MinecraftUIRenderContext& ctx, ClientInstance* client, float ox, float oy) {
    if (!BaseActorRenderContext_ctor || !ItemRenderer_renderGuiItemNew) return;
    if (!client || !ctx.mScreenContext) return;

    void* game = getMCGame(client);
    if (!game) return;

    LocalPlayer* player     = getLocalPlayer(client);
    PlayerInventory* pinv   = getPlayerInventory(player);
    if (!pinv) return;

    alignas(16) std::byte barcBuf[kBarcStorageSize]{};
    BaseActorRenderContext_ctor(barcBuf, ctx.mScreenContext, client, game);
    void* ir = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(barcBuf) + kBarcItemRendererOffset);
    if (!ir) { destroyBarc(barcBuf); return; }

    forSlots(ox, oy, [&](int slot, float x, float y) {
        ItemStackBase* s = getInventoryItem(pinv, slot);
        if (stackHasItem(s))
            ItemRenderer_renderGuiItemNew(ir, barcBuf, s, 0, 0, 0,
                                          x + kItemInset, y + kItemInset, 1, 1, 1);
    });

    destroyBarc(barcBuf);
    ctx.flushImages(kWhite, 1.0f, kFlushMaterial);
}

} // namespace invrender

// Render the full inventory overlay. Called from the drawText hook.
inline void ip_renderInventory(MinecraftUIRenderContext* ctx) {
    if (!ctx) return;
    ClientInstance* client = ctx->mClient;
    if (!client) return;

    const invrender::CachedTextures& tex = invrender::getTextures(*ctx);

    const float x = g_posX;
    const float y = g_posY;
    const float panelW = invrender::kColumns * invrender::kSlotStride + invrender::kPanelPadding * 2;
    const float panelH = invrender::kRows    * invrender::kSlotStride + invrender::kPanelPadding * 2;

    // 1. Panel background
    invrender::drawPanel(*ctx, {x, x + panelW, y, y + panelH});
    ctx->flushImages(invrender::kWhite, 1.0f, invrender::kFlushMaterial);

    const float ox = x + invrender::kPanelPadding;
    const float oy = y + invrender::kPanelPadding;

    // 2. Slot cells
    invrender::forSlots(ox, oy, [&](int, float sx, float sy) {
        invrender::drawSlot(*ctx, tex, sx, sy);
    });
    ctx->flushImages(invrender::kWhite, 1.0f, invrender::kFlushMaterial);

    // 3. Item icons (via ItemRenderer)
    invrender::drawIcons(*ctx, client, ox, oy);

    // 4. Stack counts (via MinecraftUIRenderContext::drawText)
    if (Font* fnt = g_activeUIFont) {
        LocalPlayer* player    = getLocalPlayer(client);
        PlayerInventory* pinv  = getPlayerInventory(player);
        if (pinv) {
            TextMeasureData measure{}; measure.fontSize = 1.0f;
            CaretMeasureData caret{};
            invrender::forSlots(ox, oy, [&](int slot, float sx, float sy) {
                ItemStackBase* s = getInventoryItem(pinv, slot);
                if (!stackHasItem(s)) return;
                uint8_t count = getStackCount(s);
                if (count <= 1) return;
                char txt[8];
                std::snprintf(txt, sizeof(txt), "%u", static_cast<unsigned>(count));
                float w = ctx->getLineLength(*fnt, txt, 1.0f, false);
                float ax = sx + invrender::kSlotDrawSize - 0.5f;
                float ay = sy + invrender::kSlotDrawSize - 1.5f;
                ctx->drawText(*fnt, {ax - w, ax, ay - invrender::kCountTextH, ay},
                              txt, invrender::kWhite, ui::TextAlignment::Right,
                              1.0f, measure, caret);
            });
        }
        ctx->flushText(0.0f, std::nullopt);
    }
}

} // namespace ip
