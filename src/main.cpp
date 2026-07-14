// =============================================================================
//  itempack - main entry point.
//   - mod_init(): signature resolution, vtable hooking, ImGui init.
//   - Vtable for MinecraftUIRenderContext is recovered without RTTI by finding
//     drawImage (via the "drawImage callback overload" string) at slot 8, then
//     hooking drawText at slot 6.
// =============================================================================

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>

#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>

// Use preloader-android's PUBLIC API only (pl/memory/Hook.hpp is exported via
// target_include_directories(preloader PUBLIC include)). pl/Gloss.h is PRIVATE
// and must not be included directly.
#include "pl/memory/Hook.hpp"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#include "main.h"

#define TAG "[itempack]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Resolved function pointers (declared extern in render/helper.h).
BaseActorRenderContext_ctor_t   BaseActorRenderContext_ctor   = nullptr;
ItemRenderer_renderGuiItemNew_t ItemRenderer_renderGuiItemNew = nullptr;

// ---------------------------------------------------------------------------
//  Signature resolution with wildcard "??" support.
//  Tries pl::Signature first, then falls back to ip::scanPatternInSection.
//  Patterns use "??" for wildcard bytes (register-specific fields that may
//  change between compiler versions).
// ---------------------------------------------------------------------------
static void* resolve(const char* sig, const char* name) {
    // Try pl::Signature first (supports "? ?" wildcard format)
    void* addr = ip::resolveSignature(sig, name);
    if (addr) return addr;
    // Fallback: our own wildcard scanner ("??" format)
    uintptr_t a = ip::scanPatternInSection(sig, ".text");
    if (a) {
        LOGI("found %s @ 0x%lx (via scanPattern)", name, (unsigned long)a);
        return (void*)a;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
//  mod_init
// ---------------------------------------------------------------------------
__attribute__((constructor))
static void mod_init() {
    // GlossInit() was removed from preloader-android's public API; the hook
    // engine self-initialises on first pl::memory::hook() call. itempack uses
    // its own ip:: hook helpers, so no explicit init is needed here.
    LOGI("mod_init starting (itempack for libminecraftpe 1.26.30)");

    // ---- Signature resolution (wildcard patterns for 1.26.30) ----
    // Patterns use "??" for wildcard bytes. Key idea: keep opcode + register
    // encodings that are version-stable, wildcard stack offsets/immediates
    // that change with compiler optimizations.
    //
    // BaseActorRenderContext_ctor:
    //   3x STP (reg saves) + ADD X29,SP,#0 + 2x STP + MOV X19,X0 + MOV X20,X1
    //   The "FD 03 00 91" (ADD X29, SP, #0) and MOV pair are distinctive.
    BaseActorRenderContext_ctor = (BaseActorRenderContext_ctor_t)resolve(
        "?? ?? ?? A9 ?? ?? ?? A9 ?? ?? ?? A9 FD 03 00 91 ?? ?? ?? ?? ?? ?? ?? 91 ?? ?? ?? A9 ?? ?? ?? A9 F3 03 00 AA F4 03 02 AA",
        "BaseActorRenderContext_ctor");

    // ItemRenderer_renderGuiItemNew:
    //   SUB SP + 4x STP(float regs) + 6x STP(int regs) + ADD X29 + MRS TPIDR_EL0
    //   The MRS TPIDR_EL0 (5B D0 3B D5) at the end is highly distinctive.
    //   Wildcard the stack frame size and STP offsets.
    ItemRenderer_renderGuiItemNew = (ItemRenderer_renderGuiItemNew_t)resolve(
        "FF ?? ?? D1 EC 73 ?? FD EB 2B ?? 6D E9 23 ?? 6D FD 7B ?? A9 FC 6F ?? A9 FA 67 ?? A9 F8 5F ?? A9 F6 57 ?? A9 F4 4F ?? A9 FD 43 ?? 91 5B D0 3B D5",
        "ItemRenderer_renderGuiItemNew");

    LOGI("BaseActorRenderContext_ctor: %p", (void*)BaseActorRenderContext_ctor);
    LOGI("ItemRenderer_renderGuiItemNew: %p", (void*)ItemRenderer_renderGuiItemNew);

    // ---- Hook MinecraftUIRenderContext::drawText (vtable slot 6) ----
    // Strategy: find drawImage via its assertion string, recover the vtable
    // (drawImage is slot 8), then patch slot 6 (drawText).
    //
    // NOTE: On 1.26.30 the string "drawImage callback overload" actually
    // belongs to Coherent UI, not MinecraftUIRenderContext. This hook will
    // likely fail until a correct identification strategy is found. The mod
    // still works without it - only the in-world MC-context inventory
    // rendering is unavailable; the ImGui "Inv Preview" tab still works
    // once ClientInstance is captured (via g_lastClientInstance).
    uintptr_t drawImageFn = ip::findFunctionByString(
        "drawImage callback overload", "MinecraftUIRenderContext::drawImage");
    if (drawImageFn) {
        ip::findAndHookVtable(
            drawImageFn, 8 /* drawImage slot */,
            6 /* drawText slot */,
            (void*)ip::drawText_hook,
            (void**)&ip::g_drawText_orig,
            "MinecraftUIRenderContext");
    } else {
        LOGE("drawImage not found; drawText hook skipped (expected on 1.26.30)");
        LOGI("Use the Diagnostics tab to manually locate the vtable.");
    }

    ip::ip_loadConfig();
    ip::ip_initModMenu();
    LOGI("initialized - open menu via the ItemPack button on the left edge");
}

// ===========================================================================
//  ImGui overlay (eglSwapBuffers hook + touch input)
// ===========================================================================
static bool        g_initialized    = false;
static int         g_width = 0, g_height = 0;
static EGLContext  g_targetcontext  = EGL_NO_CONTEXT;
static EGLSurface  g_targetsurface  = EGL_NO_SURFACE;
static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

struct WindowBounds { float x, y, w, h; bool visible; };
static WindowBounds g_menuBounds = {0, 0, 0, 0, false};
static std::mutex   g_boundsMutex;

static void drawmenu() {
    static bool show_settings = false;
    ImGuiIO& io = ImGui::GetIO();

    // Trigger button (always visible).
    {
        const float btn_w = 180.0f, btn_h = 60.0f;
        const float btn_y = io.DisplaySize.y * 0.35f;
        ImGui::SetNextWindowPos(ImVec2(0.0f, btn_y), ImGuiCond_Always, ImVec2(0.0f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(btn_w, btn_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("IPTrigger", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(2);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 bmin = ImGui::GetWindowPos();
        ImVec2 bmax = ImVec2(bmin.x + btn_w, bmin.y + btn_h);
        ImGui::InvisibleButton("##ip_trig", ImVec2(btn_w, btn_h));
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked();

        ImU32 bg = hovered ? IM_COL32(40, 40, 42, 235) : IM_COL32(18, 18, 20, 220);
        dl->AddRectFilled(bmin, bmax, bg, 7.0f);
        dl->AddRect(bmin, bmax, IM_COL32(100, 100, 108, 220), 7.0f, 0, 1.0f);

        const char* lbl = "ItemPack";
        ImFont* fnt = ImGui::GetFont();
        float fsz = 24.0f;
        ImVec2 lsz = fnt->CalcTextSizeA(fsz, FLT_MAX, -1.0f, lbl);
        dl->AddText(fnt, fsz,
            ImVec2(bmin.x + (btn_w - lsz.x) * 0.5f, bmin.y + (btn_h - lsz.y) * 0.5f),
            IM_COL32(235, 235, 235, 255), lbl);

        if (clicked) show_settings = !show_settings;
        ImGui::End();

        std::lock_guard<std::mutex> lk(g_boundsMutex);
        if (!show_settings)
            g_menuBounds = {0.0f, btn_y - btn_h * 0.5f, btn_w, btn_h, true};
    }

    if (!show_settings) return;

    const float WIN_W = 480.0f, WIN_H = 420.0f;
    ImGui::SetNextWindowSize(ImVec2(WIN_W, WIN_H), ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.28f, 0.28f, 0.35f, 1.0f));

    ImGui::Begin("IP_Main", &show_settings,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoScrollbar);

    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.0f), "ItemPack");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();

    const ImVec4 accent    = ImVec4(0.30f, 0.67f, 1.00f, 1.0f);
    const ImVec4 text_dim  = ImVec4(0.60f, 0.60f, 0.65f, 1.0f);
    const ImVec4 ok_color  = ImVec4(0.40f, 0.85f, 0.40f, 1.0f);
    const ImVec4 err_color = ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_CheckMark,  accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, accent);
    ImGui::PushStyleColor(ImGuiCol_Text,       ImVec4(0.92f, 0.92f, 0.92f, 1.0f));

    if (ImGui::BeginTabBar("##tabs")) {
        // ---- Overlay tab ----
        if (ImGui::BeginTabItem("Overlay")) {
            ImGui::TextColored(text_dim, "Inventory Overlay");
            if (ImGui::Checkbox("Show Inventory (MC render)", &ip::g_showInventory))
                ip::ip_saveConfig();

            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextColored(text_dim, "Position X");
            ImGui::PushItemWidth(WIN_W - 80);
            if (ImGui::SliderFloat("##posX", &ip::g_posX, 0.0f, io.DisplaySize.x, "%.0f"))
                ip::ip_saveConfig();
            ImGui::TextColored(text_dim, "Position Y");
            if (ImGui::SliderFloat("##posY", &ip::g_posY, 0.0f, io.DisplaySize.y, "%.0f"))
                ip::ip_saveConfig();
            ImGui::TextColored(text_dim, "Scale");
            if (ImGui::SliderFloat("##scale", &ip::g_scale, 0.5f, 2.0f, "%.2f"))
                ip::ip_saveConfig();
            ImGui::PopItemWidth();

            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextColored(text_dim, "Hook status");
            ImGui::TextColored(ip::g_drawText_orig ? ok_color : err_color,
                "  drawText hook:  %s",
                ip::g_drawText_orig ? "installed" : "NOT installed");
            ImGui::TextColored(BaseActorRenderContext_ctor ? ok_color : err_color,
                "  BaseActorRenderContext_ctor: %s",
                BaseActorRenderContext_ctor ? "found" : "missing");
            ImGui::TextColored(ItemRenderer_renderGuiItemNew ? ok_color : err_color,
                "  ItemRenderer_renderGuiItemNew: %s",
                ItemRenderer_renderGuiItemNew ? "found" : "missing");
            ImGui::Dummy(ImVec2(0, 4));
            if (ImGui::Button("Save Config", ImVec2(WIN_W - 80, 0)))
                ip::ip_saveConfig();
            ImGui::EndTabItem();
        }

        // ---- Diagnostics tab (offset editor + vtable scanner) ----
        if (ImGui::BeginTabItem("Diagnostics")) {
            ImGui::TextColored(text_dim, "Runtime Offsets (edit then Save)");
            ImGui::PushItemWidth(WIN_W - 140);
            int vf1 = ip::g_clientInstanceGetLocalPlayerVfIndex;
            if (ImGui::InputInt("ClientInstance::getLocalPlayer vf", &vf1, 1, 10)) {
                ip::g_clientInstanceGetLocalPlayerVfIndex = vf1 < 0 ? 0 : vf1;
                ip::ip_saveConfig();
            }
            int vf2 = ip::g_playerInventoryGetItemVfIndex;
            if (ImGui::InputInt("PlayerInventory::getItem vf", &vf2, 1, 10)) {
                ip::g_playerInventoryGetItemVfIndex = vf2 < 0 ? 0 : vf2;
                ip::ip_saveConfig();
            }
            int off1 = (int)ip::g_playerInventoryOffset;
            if (ImGui::InputInt("PlayerInventory offset (dec)", &off1, 16, 256)) {
                ip::g_playerInventoryOffset = (uintptr_t)(off1 < 0 ? 0 : off1);
                ip::ip_saveConfig();
            }
            int off2 = (int)ip::g_itemStackCountOffset;
            if (ImGui::InputInt("ItemStackBase count offset (dec)", &off2, 1, 16)) {
                ip::g_itemStackCountOffset = (uintptr_t)(off2 < 0 ? 0 : off2);
                ip::ip_saveConfig();
            }
            int cid = ip::g_containerIdPlayer;
            if (ImGui::InputInt("ContainerID (player)", &cid, 1, 10)) {
                ip::g_containerIdPlayer = cid;
                ip::ip_saveConfig();
            }
            ImGui::PopItemWidth();

            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextColored(text_dim, "Captured pointers");
            ImGui::Text("  ClientInstance: %p", ip::g_lastClientInstance);
            ImGui::Text("  LocalPlayer:    %p", ip::g_lastLocalPlayer);
            ImGui::Text("  PlayerInventory:%p", ip::g_lastPlayerInv);
            ImGui::Text("  UIContext:      %p", ip::g_activeUIContext);

            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextColored(text_dim, "Vtable scanner");
            ImGui::TextColored(err_color,
                "WARNING: entering an invalid pointer will crash the game.");
            ImGui::TextWrapped(
                "Enter an object pointer (hex, e.g. 0xabc12340) and the scanner\n"
                "will list the first 32 vtable entries. Useful for finding the\n"
                "correct vf index by recognising function addresses.");
            static char ptrBuf[32] = "0x";
            ImGui::PushItemWidth(WIN_W - 200);
            ImGui::InputText("##ptr", ptrBuf, sizeof(ptrBuf));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("Scan")) {
                // parsed below
            }
            // Parse and display
            if (ptrBuf[0] && ptrBuf[1] == 'x') {
                uintptr_t p = strtoull(ptrBuf, nullptr, 16);
                if (p > 0x1000) {
                    void** vt = *reinterpret_cast<void***>(p);
                    if (vt) {
                        ImGui::BeginChild("##vts", ImVec2(WIN_W - 80, 120), true);
                        for (int i = 0; i < 32; ++i) {
                            void* fn = vt[i];
                            ImGui::Text("[%2d] %p", i, fn);
                        }
                        ImGui::EndChild();
                    }
                }
            }
            ImGui::EndTabItem();
        }

        // ---- Inventory preview (text mode) ----
        if (ImGui::BeginTabItem("Inv Preview")) {
            ImGui::TextColored(text_dim, "Live inventory preview (text mode)");
            ImGui::Separator();
            ClientInstance* cli = (ClientInstance*)ip::g_lastClientInstance;
            LocalPlayer* pl = ip::getLocalPlayer(cli);
            PlayerInventory* pinv = ip::getPlayerInventory(pl);
            if (!pinv) {
                ImGui::TextColored(err_color, "No PlayerInventory - check offsets / hook status");
            } else {
                // 9 columns x 4 rows
                ImGui::BeginChild("##inv", ImVec2(WIN_W - 80, 200), true);
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 9; ++c) {
                        int slot = r * 9 + c;
                        ItemStackBase* s = ip::getInventoryItem(pinv, slot);
                        uint8_t cnt = s ? ip::getStackCount(s) : 0;
                        bool has = s && ip::stackHasItem(s);
                        ImGui::Text("%2d:%s%2d", slot, has ? "*" : " ", cnt);
                        if (c < 8) ImGui::SameLine();
                    }
                }
                ImGui::EndChild();
                ImGui::TextWrapped("* = item present (mItem != null). Count uses configured offset.");
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::PopStyleColor(3);

    {
        std::lock_guard<std::mutex> lk(g_boundsMutex);
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 wsz  = ImGui::GetWindowSize();
        g_menuBounds = {wpos.x, wpos.y, wsz.x, wsz.y, true};
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void ip_setup() {
    if (g_initialized || g_width <= 0) return;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImFontConfig fcfg;
    fcfg.OversampleH = 3; fcfg.OversampleV = 2; fcfg.PixelSnapH = true;
    const char* font_paths[] = {
        "/system/fonts/Roboto-Medium.ttf",
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/NotoSans-Regular.ttf",
        "/system/fonts/DroidSans.ttf"
    };
    bool ok = false;
    for (auto* p : font_paths)
        if (io.Fonts->AddFontFromFileTTF(p, 26.0f, &fcfg)) { ok = true; break; }
    if (!ok) { io.Fonts->AddFontDefault(); io.FontGlobalScale = 1.3f; }

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_initialized = true;
}

static void ip_render() {
    if (!g_initialized) return;

    GLint last_prog, last_ab, last_eab, last_fbo, last_vp[4];
    GLint last_tex0, last_tex1, last_at;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &last_at);
    glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex0);
    glActiveTexture(GL_TEXTURE1); glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex1);
    glActiveTexture(last_at);
    glGetIntegerv(GL_CURRENT_PROGRAM,              &last_prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,         &last_ab);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_eab);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,          &last_fbo);
    glGetIntegerv(GL_VIEWPORT,                     last_vp);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean last_depth   = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_blend   = glIsEnabled(GL_BLEND);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();
    drawmenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glUseProgram(last_prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, last_tex0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, last_tex1);
    glActiveTexture(last_at);
    glBindBuffer(GL_ARRAY_BUFFER, last_ab);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_eab);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_vp[0], last_vp[1], last_vp[2], last_vp[3]);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (last_depth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (last_blend)   glEnable(GL_BLEND);         else glDisable(GL_BLEND);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT ||
        (g_targetcontext != EGL_NO_CONTEXT && (ctx != g_targetcontext || surf != g_targetsurface)))
        return orig_eglswapbuffers(dpy, surf);

    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_eglswapbuffers(dpy, surf);

    if (g_targetcontext == EGL_NO_CONTEXT) { g_targetcontext = ctx; g_targetsurface = surf; }
    g_width = w; g_height = h;

    // Mark a new frame so the inventory overlay renders once on the next
    // drawText call.
    ip::g_newFrame = true;

    ip_setup();
    ip_render();

    return orig_eglswapbuffers(dpy, surf);
}

typedef bool (*PreloaderInput_OnTouch_Fn)(int action, int pointerId, float x, float y);
struct PreloaderInput_Interface {
    void (*RegisterTouchCallback)(PreloaderInput_OnTouch_Fn);
};
typedef PreloaderInput_Interface* (*GetPreloaderInput_Fn)();

static bool OnTouchCallback(int action, int pointerId, float x, float y) {
    (void)pointerId;
    if (!g_initialized) return false;

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    if      (action == AMOTION_EVENT_ACTION_DOWN) io.AddMouseButtonEvent(0, true);
    else if (action == AMOTION_EVENT_ACTION_UP)   io.AddMouseButtonEvent(0, false);

    bool hit = false;
    {
        std::lock_guard<std::mutex> lk(g_boundsMutex);
        if (g_menuBounds.visible &&
            x >= g_menuBounds.x && x <= g_menuBounds.x + g_menuBounds.w &&
            y >= g_menuBounds.y && y <= g_menuBounds.y + g_menuBounds.h)
            hit = true;
    }
    return hit || io.WantCaptureMouse;
}

static void* imgui_thread(void*) {
    sleep(3); // wait for game libs to load
    // eglSwapBuffers hook via standard dlsym + preloader's PUBLIC hook API.
    // (Previously used GlossOpen/GlossSymbol/GlossHook from pl/Gloss.h, but
    //  pl/Gloss.h is a PRIVATE header of preloader-android and not exported.)
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (egl) {
        void* swap = dlsym(egl, "eglSwapBuffers");
        if (swap) {
            // pl::memory::hook self-initialises GlossHook on first call.
            pl::memory::hook(swap,
                             (pl::memory::FuncPtr)hook_eglswapbuffers,
                             (pl::memory::FuncPtr*)&orig_eglswapbuffers);
        }
    }

    void* preloaderLib = dlopen("libpreloader.so", RTLD_NOW);
    if (preloaderLib) {
        auto GetInput = (GetPreloaderInput_Fn)dlsym(preloaderLib, "GetPreloaderInput");
        if (GetInput) {
            PreloaderInput_Interface* inp = GetInput();
            if (inp && inp->RegisterTouchCallback)
                inp->RegisterTouchCallback(OnTouchCallback);
        }
    }
    return nullptr;
}

void ip::ip_initModMenu() {
    pthread_t t;
    pthread_create(&t, nullptr, imgui_thread, nullptr);
    pthread_detach(t);
    LOGI("ImGui menu thread started");
}
