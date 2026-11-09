// Forge's ImGui overlay: its "forge" window (About, Loaded Plugins, Plugin UI: each plugin's settings) and whatever
// plugins draw every frame. On the console Forge draws it with its own NVN renderer onto the image about to be shown;
// here Dear ImGui's Vulkan backend draws it into the window image, over the game (runtime/nvn/vulkan/device.c).
//
// Threads:
//   - the game's thread builds the frame (runtime/forge.c's frame update, after the plugins' forge_onUpdate): the
//     plugins' ImGui code runs there (their calls: runtime/forge_imgui.c); texture uploads too, under the Vulkan lock
//   - the render thread draws a copy of the last frame's draw lists (taken under a lock of its own, so it never
//     waits for plugin code)
//   - the host's main thread passes input on: while the menu is open, keyboard and mouse go to ImGui, and the
//     controller moves through the menu (runtime/host_input.c then gives the game none)
// The menu opens and closes with the key of Forge's config (nativeNX/forge/config.ini [menu] key; Insert by default).
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>  // access

#include "imgui.h"
#ifndef MHGU_NULL_BACKEND
#include "backends/imgui_impl_vulkan.h"
#else  // (no renderer: no overlay)
struct ImGui_ImplVulkan_InitInfo { int unused; };
static void ImGui_ImplVulkan_NewFrame() {}
static void ImGui_ImplVulkan_UpdateTexture(ImTextureData *) {}
#endif

#include "config.h"
#include "touch.h"
#include "forge.h"
#include "hitchlog.h"
#include "logwriter.h"
#include "menu_confirm.h"
#include "overlay.h"
#include "perfstats.h"
#include "hle/chat.h"
#include "settings_locale.h"
#include "settings_panel.h"
#include "settings_locale.h"

#define LOG(...) rt_logf(__VA_ARGS__)  // (Windows: queued, the log's own thread writes it)

extern "C" {
void vk_lock(void);
void vk_unlock(void);
SDL_Window *ui_window(void);
void vi_pause_game(void);
}

static std::atomic<bool> g_enabled, g_ready, g_open, g_wants_text;
static std::atomic<bool> g_settings_open, g_forge_open;
static std::atomic<bool> g_modal_open;  // (the thank-you, settings_panel.cpp: a menu as far as input goes, until it is closed)
static int g_menu_key;
static float g_font_size;
static ImFont *g_main_font;  // (the menu's; the CJK font is merged into it)
static bool g_cjk_tried;

// Chinese in the menu: the system's CJK font merged into the menu's (Roboto has no Chinese). Loaded only once Chinese
// is shown, since it is big (about 20 MB read in); ImGui 1.92 makes its glyphs as they are first drawn.
static void cjk_font_if_needed() {
    if (g_cjk_tried || !settings_zh_used || !g_main_font) return;
    g_cjk_tried = true;
    static const struct { const char *path; int index; } fonts[] = {
#ifdef _WIN32
        {"C:/Windows/Fonts/msyh.ttc", 0}, {"C:/Windows/Fonts/msyh.ttf", 0}, {"C:/Windows/Fonts/simhei.ttf", 0},
        {"C:/Windows/Fonts/simsun.ttc", 0},
#else
        // (Android's Noto CJK collection: 0 Japanese, 1 Korean, 2 Simplified Chinese, 3 Traditional)
        {"/system/fonts/NotoSansCJK-Regular.ttc", 2}, {"/system/fonts/NotoSansSC-Regular.otf", 0},
        {"/system/fonts/NotoSansHans-Regular.otf", 0}, {"/system/fonts/MiSansVF.ttf", 0},
        {"/system/fonts/DroidSansFallbackFull.ttf", 0}, {"/system/fonts/DroidSansFallback.ttf", 0},
        {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 2},
        {"/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", 0},
#endif
    };
    for (const auto &f : fonts) {
        FILE *probe = fopen(f.path, "rb");
        if (!probe) continue;
        fclose(probe);
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.DstFont = g_main_font;
        cfg.FontNo = f.index;
        cfg.SizePixels = g_font_size;
        if (ImGui::GetIO().Fonts->AddFontFromFileTTF(f.path, g_font_size, &cfg)) {
            LOG("[forge] Chinese glyphs from %s\n", f.path);
            return;
        }
    }
    LOG("[forge] no Chinese font found on this device: Chinese text shows as boxes\n");
}
static ImGuiStyle g_base_style;  // (unscaled; scaled copies follow the window's height)
static float g_scale;

// ------------------------------------------------------------------ input (main thread -> game thread)
struct Ev {
    enum { POS, BUTTON, WHEEL, KEY, TEXT, FOCUS, CLEAR } kind;
    bool down;
    bool touch;  // (POS, BUTTON: a finger's - ImGui then knows there was no hover before it, and none after it lifts)
    int button;
    float x, y;
    ImGuiKey key;
    SDL_Keymod mods;
    char text[32];
};
static std::mutex g_in_lock;
static char g_composing[256];  // (the system keyboard's text not yet committed: overlay_composition)
static std::vector<Ev> g_in;
static OverlayPad g_pad;

static void queue(const Ev &e) {
    std::lock_guard<std::mutex> l(g_in_lock);
    if (g_in.size() < 4096) g_in.push_back(e);
}

static ImGuiKey key_of(SDL_Scancode sc, SDL_Keycode kc) {
    if (kc >= SDLK_A && kc <= SDLK_Z) return (ImGuiKey)(ImGuiKey_A + (int)(kc - SDLK_A));  // (letters as labelled)
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return (ImGuiKey)(ImGuiKey_A + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (ImGuiKey)(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return (ImGuiKey)(ImGuiKey_F1 + (sc - SDL_SCANCODE_F1));
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return (ImGuiKey)(ImGuiKey_Keypad1 + (sc - SDL_SCANCODE_KP_1));
    switch (sc) {
    case SDL_SCANCODE_0: return ImGuiKey_0;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_AC_BACK: return ImGuiKey_AppBack;  // (Android's Back: the settings menu's Back)
    case SDL_SCANCODE_APOSTROPHE: return ImGuiKey_Apostrophe;
    case SDL_SCANCODE_COMMA: return ImGuiKey_Comma;
    case SDL_SCANCODE_MINUS: return ImGuiKey_Minus;
    case SDL_SCANCODE_PERIOD: return ImGuiKey_Period;
    case SDL_SCANCODE_SLASH: return ImGuiKey_Slash;
    case SDL_SCANCODE_SEMICOLON: return ImGuiKey_Semicolon;
    case SDL_SCANCODE_EQUALS: return ImGuiKey_Equal;
    case SDL_SCANCODE_LEFTBRACKET: return ImGuiKey_LeftBracket;
    case SDL_SCANCODE_BACKSLASH: return ImGuiKey_Backslash;
    case SDL_SCANCODE_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDL_SCANCODE_GRAVE: return ImGuiKey_GraveAccent;
    case SDL_SCANCODE_CAPSLOCK: return ImGuiKey_CapsLock;
    case SDL_SCANCODE_SCROLLLOCK: return ImGuiKey_ScrollLock;
    case SDL_SCANCODE_NUMLOCKCLEAR: return ImGuiKey_NumLock;
    case SDL_SCANCODE_PRINTSCREEN: return ImGuiKey_PrintScreen;
    case SDL_SCANCODE_PAUSE: return ImGuiKey_Pause;
    case SDL_SCANCODE_KP_0: return ImGuiKey_Keypad0;
    case SDL_SCANCODE_KP_PERIOD: return ImGuiKey_KeypadDecimal;
    case SDL_SCANCODE_KP_DIVIDE: return ImGuiKey_KeypadDivide;
    case SDL_SCANCODE_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case SDL_SCANCODE_KP_MINUS: return ImGuiKey_KeypadSubtract;
    case SDL_SCANCODE_KP_PLUS: return ImGuiKey_KeypadAdd;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_KP_EQUALS: return ImGuiKey_KeypadEqual;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_APPLICATION: return ImGuiKey_Menu;
    default: return ImGuiKey_None;
    }
}

static void set_open(bool open) {
    g_forge_open = open;
    g_open = open || g_settings_open || g_modal_open;
    SDL_Window *w = ui_window();
    if (g_open) SDL_ShowCursor();  // (full screen hides it: runtime/ui.c)
    else if (w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) SDL_HideCursor();
    Ev e = {};
    e.kind = Ev::CLEAR;  // (keys and buttons held as it opens or closes: released)
    queue(e);
    LOG("[overlay] forge %s, settings %s%s\n", open ? "open" : "closed", g_settings_open ? "open" : "closed",
        g_modal_open ? ", thank-you open" : "");
}

static std::atomic<int> g_last_input{USED_KEYS};  // (a PC's: the keys until something else is used)
void overlay_input_seen(int kind) { g_last_input.store(kind, std::memory_order_relaxed); }
int overlay_last_input(void) { return g_last_input.load(std::memory_order_relaxed); }

static std::atomic<int> g_face_positions[4] = {1, 0, 3, 2};
void overlay_face_positions(const int positions[4]) {
    for (int i = 0; i < 4; ++i) g_face_positions[i] = positions[i];
}
int overlay_face_position(int semantic) { return g_face_positions[semantic & 3]; }
void overlay_request_close(void) {
    settings_panel_request_close();
    g_settings_open = true;
    set_open(g_forge_open);
}
bool overlay_settings_dirty(void) { return settings_panel_dirty(); }
#ifndef __ANDROID__
static std::atomic<bool> g_menu_opened;  // (the player opened the settings menu: menu_hint has nothing to say)
#endif
void overlay_toggle_settings(void) {
    if (!g_enabled || g_modal_open) return;  // (the thank-you first: closed, F1 and the chord work again)
    if (g_settings_open) { settings_panel_request_back(); return; }
#ifndef __ANDROID__
    g_menu_opened = true;
#endif
    g_settings_open = true;
    set_open(g_forge_open);
}

// A finger as the menus' mouse: SDL's own mouse events made from a touch, or runtime/touch.cpp's (to_menu), which it
// makes by hand and gives no time
static bool finger_mouse(SDL_MouseID which, Uint64 timestamp) { return which == SDL_TOUCH_MOUSEID || !timestamp; }
bool overlay_event(const SDL_Event *e) {
    if (!g_enabled) return false;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && settings_panel_key((int)e->key.scancode)) return true;
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && settings_panel_mouse(e->button.button)) return true;
    if (e->type == SDL_EVENT_MOUSE_WHEEL &&
        settings_panel_wheel(e->wheel.integer_y * (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1)))
        return true;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.scancode == SDL_SCANCODE_F1) {
        overlay_toggle_settings();
        return true;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && (int)e->key.scancode == g_menu_key) {
        if (!g_modal_open) set_open(!g_forge_open);
        return true;
    }
    if (!g_open) return false;
    SDL_Window *w = ui_window();
    float density = w ? SDL_GetWindowPixelDensity(w) : 1.0f;  // (ImGui works in the image's pixels)
    Ev v = {};
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        v.kind = Ev::POS, v.x = e->motion.x * density, v.y = e->motion.y * density;
        v.touch = finger_mouse(e->motion.which, e->motion.timestamp);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        static const int map[] = {-1, ImGuiMouseButton_Left, ImGuiMouseButton_Middle, ImGuiMouseButton_Right, 3, 4};
        if (e->button.button >= 6 || map[e->button.button] < 0) return true;
        v.kind = Ev::BUTTON, v.button = map[e->button.button], v.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        v.x = e->button.x * density, v.y = e->button.y * density;
        v.touch = finger_mouse(e->button.which, e->button.timestamp);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        v.kind = Ev::WHEEL, v.x = -e->wheel.x, v.y = e->wheel.y;
        break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        v.kind = Ev::KEY, v.down = e->type == SDL_EVENT_KEY_DOWN, v.mods = e->key.mod;
        v.key = key_of(e->key.scancode, e->key.key);
        queue(v);
        // the port's own keys still work (screenshots, Alt+Enter full screen)
        bool port_key = (e->key.scancode >= SDL_SCANCODE_F1 && e->key.scancode <= SDL_SCANCODE_F12) ||
                        ((e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) &&
                         (e->key.mod & SDL_KMOD_ALT));
        return !port_key;
    }
    case SDL_EVENT_TEXT_INPUT: {
        v.kind = Ev::TEXT;
        SDL_strlcpy(v.text, e->text.text, sizeof v.text);
        std::lock_guard<std::mutex> l(g_in_lock);
        g_composing[0] = 0;  // (committed: it comes as this text)
        break;
    }
    case SDL_EVENT_TEXT_EDITING: {
        std::lock_guard<std::mutex> l(g_in_lock);
        SDL_strlcpy(g_composing, e->edit.text ? e->edit.text : "", sizeof g_composing);
        return true;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST: case SDL_EVENT_WINDOW_FOCUS_GAINED:
        v.kind = Ev::FOCUS, v.down = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        queue(v);
        return false;  // (the game's input sees it too)
    default:
        return false;
    }
    queue(v);
    return true;
}

void overlay_pad(const OverlayPad *p) {
    std::lock_guard<std::mutex> l(g_in_lock);
    g_pad = *p;
}

bool overlay_enabled(void) { return g_enabled; }
bool overlay_menu_open(void) { return g_open; }

bool overlay_wants_text(void) { return g_wants_text; }

void overlay_composition(char *out, size_t size) {
    std::lock_guard<std::mutex> l(g_in_lock);
    SDL_strlcpy(out, g_open ? g_composing : "", size);
}

static void apply_input(ImGuiIO &io) {
    std::vector<Ev> evs;
    OverlayPad pad;
    {
        std::lock_guard<std::mutex> l(g_in_lock);
        evs.swap(g_in);
        pad = g_pad;
    }
    for (const Ev &e : evs) {
        switch (e.kind) {
        case Ev::POS:
            io.AddMouseSourceEvent(e.touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.x, e.y);
            break;
        case Ev::BUTTON:
            io.AddMouseSourceEvent(e.touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.x, e.y);
            io.AddMouseButtonEvent(e.button, e.down);
            if (e.touch && !e.down) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // (the finger is gone: nothing stays hovered)
            break;
        case Ev::WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case Ev::KEY:
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.mods & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.mods & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.mods & SDL_KMOD_ALT) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (e.mods & SDL_KMOD_GUI) != 0);
            if (e.key != ImGuiKey_None) io.AddKeyEvent(e.key, e.down);
            break;
        case Ev::TEXT: io.AddInputCharactersUTF8(e.text); break;
        case Ev::FOCUS: io.AddFocusEvent(e.down); break;
        case Ev::CLEAR:
            io.ClearInputKeys();
            io.ClearInputMouse();
            memset(&g_pad, 0, sizeof g_pad);
            pad = {};
            break;
        }
    }
    // the controller (as Forge maps it: A activates, B cancels, the left stick and the d-pad move)
    if (!g_open) pad = {};
    // Apply only to settings. Forge retains its existing semantic A/B mapping.
    if (g_settings_open) {
        int a = overlay_face_position(0), b = overlay_face_position(1);
        int confirm, cancel;
        menu_confirm_buttons(config_live_menu_confirm(), a, b, pad.a, pad.b, &confirm, &cancel);
        pad.a = confirm; pad.b = cancel;
    }
    static int last_policy = -1;
    static bool wait_release, was_settings;
    if (g_settings_open && !was_settings) wait_release = true;
    was_settings = g_settings_open;
    int policy = g_settings_open ? config_live_menu_confirm() : MENU_CONFIRM_ORIGINAL;
    if (last_policy >= 0 && policy != last_policy) wait_release = true;
    last_policy = policy;
    if (wait_release) {
        if (!pad.a && !pad.b) wait_release = false;
        pad.a = pad.b = false;
    }
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, pad.a);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, pad.b);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, pad.x);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, pad.y);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, pad.up);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, pad.down);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, pad.left);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, pad.right);
    io.AddKeyEvent(ImGuiKey_GamepadL1, pad.l);
    io.AddKeyEvent(ImGuiKey_GamepadR1, pad.r);
    io.AddKeyEvent(ImGuiKey_GamepadStart, pad.plus);
    io.AddKeyEvent(ImGuiKey_GamepadBack, pad.minus);
    auto axis = [&](ImGuiKey k, float v) { io.AddKeyAnalogEvent(k, v > 0.2f, std::min(v, 1.0f)); };
    axis(ImGuiKey_GamepadLStickLeft, -pad.lx);
    axis(ImGuiKey_GamepadLStickRight, pad.lx);
    axis(ImGuiKey_GamepadLStickUp, pad.ly);
    axis(ImGuiKey_GamepadLStickDown, -pad.ly);
}

// ------------------------------------------------------------------ setup
static void forge_style(ImGuiStyle &style) {  // Forge's own (source/forge/graphics.cpp, Graphics::setupStyle)
    style.WindowPadding = {12.0f, 12.0f};
    style.WindowRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMinSize = {20.0f, 20.0f};
    style.WindowTitleAlign = {0.5f, 0.5f};
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = {6.0f, 6.0f};
    style.FrameRounding = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = {12.0f, 6.0f};
    style.ItemInnerSpacing = {6.0f, 3.0f};
    style.CellPadding = {12.0f, 6.0f};
    style.IndentSpacing = 20.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabMinSize = 12.0f;
    style.GrabRounding = 1.0f;
    style.TabRounding = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = {0.5f, 0.5f};
    style.SelectableTextAlign = {0.0f, 0.0f};
    const ImVec4 bg = {0.078f, 0.086f, 0.102f, 1.0f}, dark = {0.047f, 0.055f, 0.071f, 1.0f},
                 frame = {0.118f, 0.133f, 0.149f, 1.0f}, line = {0.157f, 0.169f, 0.192f, 1.0f},
                 hover = {0.196f, 0.176f, 0.545f, 1.0f}, active = {0.235f, 0.216f, 0.596f, 1.0f},
                 accent = {0.498f, 0.514f, 1.0f, 1.0f}, bar = {0.098f, 0.106f, 0.122f, 1.0f},
                 dim = {0.196f, 0.176f, 0.545f, 0.502f}, sep = {0.157f, 0.184f, 0.251f, 1.0f};
    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = {1.0f, 1.0f, 1.0f, 1.0f};
    c[ImGuiCol_TextDisabled] = {0.275f, 0.318f, 0.451f, 1.0f};
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = c[ImGuiCol_PopupBg] = c[ImGuiCol_BorderShadow] = bg;
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = line;
    c[ImGuiCol_FrameBgActive] = active;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = dark;
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = bar;
    c[ImGuiCol_ScrollbarBg] = dark;
    c[ImGuiCol_ScrollbarGrab] = c[ImGuiCol_ScrollbarGrabActive] = frame;
    c[ImGuiCol_ScrollbarGrabHovered] = line;
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = {0.537f, 0.553f, 1.0f, 1.0f};
    c[ImGuiCol_Button] = {0.257f, 0.267f, 0.554f, 1.0f};
    c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ResizeGripHovered] = hover;
    c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = c[ImGuiCol_ResizeGripActive] = active;
    c[ImGuiCol_Header] = c[ImGuiCol_ResizeGrip] = frame;
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = sep;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = dark;
    c[ImGuiCol_TabHovered] = frame;
    c[ImGuiCol_TabSelected] = bar;
    c[ImGuiCol_TabDimmedSelected] = bg;
    c[ImGuiCol_PlotLines] = {0.522f, 0.600f, 0.702f, 1.0f};
    c[ImGuiCol_PlotLinesHovered] = {0.039f, 0.980f, 0.980f, 1.0f};
    c[ImGuiCol_PlotHistogram] = {1.0f, 0.290f, 0.596f, 1.0f};
    c[ImGuiCol_PlotHistogramHovered] = {0.996f, 0.475f, 0.698f, 1.0f};
    c[ImGuiCol_TableHeaderBg] = c[ImGuiCol_TableBorderStrong] = dark;
    c[ImGuiCol_TableBorderLight] = {0.0f, 0.0f, 0.0f, 1.0f};
    c[ImGuiCol_TableRowBg] = frame;
    c[ImGuiCol_TableRowBgAlt] = bar;
    c[ImGuiCol_TextSelectedBg] = active;
    c[ImGuiCol_DragDropTarget] = c[ImGuiCol_NavCursor] = c[ImGuiCol_NavWindowingHighlight] = accent;
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = dim;
}

void overlay_init(const char *font_path, float font_size, int menu_key) {
#ifdef MHGU_NULL_BACKEND
    return;
#endif
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = "forge_imgui.ini";  // (window positions; in the game folder, beside mhgurecomp.ini)
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "mhgurecomp";
    g_font_size = font_size > 0 ? font_size : 12.0f;
    ImFontConfig cfg;
    cfg.SizePixels = g_font_size;
#ifdef __ANDROID__
    static const char *const sys_fonts[] = {"/system/fonts/Roboto-Medium.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
                                            "/system/fonts/Roboto-Regular.ttf"};  // (the system's; Forge's is Roboto Medium)
    for (const char *p : sys_fonts)
        if (!font_path && access(p, R_OK) == 0) font_path = p;
#endif
    ImFont *font = font_path ? io.Fonts->AddFontFromFileTTF(font_path, g_font_size, &cfg) : nullptr;
    bool builtin = !font;  // (ImGui's own font: no file of the player's)
    g_main_font = font ? font : io.Fonts->AddFontDefaultVector(&cfg);
#ifdef _WIN32
    // (ImGui's own font has Latin-1 only: what it lacks - the thank-you's em dash, a licence's quotes, a chat line in another
    // script - comes from Windows' Segoe UI, merged after it; the font itself stays as it was)
    if (builtin) {
        const char *windir = getenv("WINDIR");
        std::string fallback = std::string(windir && *windir ? windir : "C:/Windows") + "/Fonts/segoeui.ttf";
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.SizePixels = g_font_size;
        merge.Flags |= ImFontFlags_NoLoadError;
        if (access(fallback.c_str(), R_OK) == 0) io.Fonts->AddFontFromFileTTF(fallback.c_str(), g_font_size, &merge);
    }
#endif
    ImGui::StyleColorsDark();
    forge_style(ImGui::GetStyle());
    ImGui::GetStyle().FontSizeBase = g_font_size;
    g_base_style = ImGui::GetStyle();
    g_menu_key = menu_key;
    g_enabled = true;
    LOG("[forge] ImGui %s overlay: %s opens the menu (font %s, %.0f px)\n", IMGUI_VERSION,
        SDL_GetScancodeName((SDL_Scancode)menu_key), font ? font_path : "ImGui's", g_font_size);
}

// ------------------------------------------------------------------ notices (any thread -> game thread)
// A few seconds at the top of the screen, menus open or not (runtime/net/ldn.c: the hub server's failures). Drawn on
// the foreground draw list: over every window, taking no input or focus.
static std::mutex g_notice_lock;
static std::string g_notice_title, g_notice_text;
static uint64_t g_notice_ns;  // when it came (SDL_GetTicksNS); 0: none
static const uint64_t NOTICE_NS = 7000000000ull;

void overlay_notice(const char *title, const char *text) {
    std::lock_guard<std::mutex> l(g_notice_lock);
    g_notice_title = title ? title : "";
    g_notice_text = text ? text : "";
    g_notice_ns = std::max<uint64_t>(SDL_GetTicksNS(), 1);
}

// (the thank-you open: a notice waits under it, its seconds not counting - shown whole once it is closed)
static void notice_hold(uint64_t now) {
    std::lock_guard<std::mutex> l(g_notice_lock);
    if (g_notice_ns) g_notice_ns = std::max<uint64_t>(now, 1);
}

// (now is read once a frame: a notice posted since, menu_hint's or another thread's, came after it and is still up)
static bool notice_due(uint64_t now) {
    std::lock_guard<std::mutex> l(g_notice_lock);
    if (g_notice_ns && now >= g_notice_ns + NOTICE_NS) g_notice_ns = 0;
    return g_notice_ns != 0;
}

#ifndef __ANDROID__
// (a desktop) Nothing on the screen says what opens the settings menu: a few seconds into each start a notice says so,
// until the player has opened it once ([Application] MenuHintSeen, which settings_panel.cpp turns On as it opens). A
// notice already up (a controller to set up, say) goes first; the menu opened before then, none this time.
static const uint64_t MENU_HINT_NS = 6000000000ull;  // (after the first frame the overlay drew)
static void menu_hint(uint64_t now) {
    static uint64_t first;
    static bool done;
    if (done) return;
    if (g_config.menu_hint_seen || g_menu_opened) {
        done = true;
        return;
    }
    if (!first) first = now;
    // (nor over the credits or the thank-you after them: once they are done)
    if (now - first < MENU_HINT_NS || notice_due(now) || settings_panel_credits_up()) return;
    done = true;
    int lang = config_language();
    overlay_notice(settings_text(lang, "F1: settings"), settings_text(lang, "+ and - together on a controller"));
    LOG("[overlay] notice: F1 opens the settings menu (MenuHintSeen = Off)\n");
}
#endif

static void draw_notice(uint64_t now, float scale) {
    std::string title, text;
    uint64_t at;
    {
        std::lock_guard<std::mutex> l(g_notice_lock);
        if (!g_notice_ns) return;
        title = g_notice_title, text = g_notice_text, at = g_notice_ns;
    }
    float age = now > at ? (now - at) / 1e9f : 0.f;  // (posted since now was read: just up)
    float alpha = std::clamp(std::min(age / .15f, (NOTICE_NS / 1e9f - age) / .5f), 0.f, 1.f);
    auto color = [alpha](float r, float g, float b, float a) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a * alpha));
    };
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImFont *font = ImGui::GetFont();
    float size = ImGui::GetFontSize() * 1.25f, pad = 10 * scale, bar = 4 * scale, gap = 3 * scale;
    float wrap = std::min(vp->WorkSize.x * .6f, 620 * scale);
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, wrap, title.c_str());
    ImVec2 xs = font->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
    float w = std::max(ts.x, xs.x) + bar + 2 * pad, h = ts.y + gap + xs.y + 2 * pad;
    float top = vp->WorkPos.y + 20 * scale, stats = perfstats_bottom();  // (under the FPS plate, both at the top)
    if (stats > 0) top = std::max(top, stats + 8 * scale);
    ImVec2 p0(vp->WorkPos.x + (vp->WorkSize.x - w) * .5f, top), p1(p0.x + w, p0.y + h);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    // In the settings menu's colours, as the chat line's (draw_chat): Hunter the game's window with a red and green rule
    // along its top and bottom, Blue and Gold a panel with a bar at its left
    int theme = settings_panel_theme();
    if (theme == THEME_HUNTER) {
        float line = 2 * scale;
        dl->AddRectFilled(p0, p1, color(50 / 255.f, 25 / 255.f, 24 / 255.f, .93f));
        dl->AddRect(p0, p1, color(0, 0, 0, 1), 0, 0, 2 * scale);
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + line), color(176 / 255.f, 34 / 255.f, 30 / 255.f, 1));
        dl->AddRectFilled(ImVec2(p0.x, p0.y + line), ImVec2(p1.x, p0.y + line * 1.7f), color(80 / 255.f, 142 / 255.f, 54 / 255.f, 1));
        dl->AddRectFilled(ImVec2(p0.x, p1.y - line * 1.7f), ImVec2(p1.x, p1.y - line), color(80 / 255.f, 142 / 255.f, 54 / 255.f, 1));
        dl->AddRectFilled(ImVec2(p0.x, p1.y - line), p1, color(176 / 255.f, 34 / 255.f, 30 / 255.f, 1));
    } else if (theme == THEME_GOLD) {
        dl->AddRectFilled(p0, p1, color(34 / 255.f, 25 / 255.f, 19 / 255.f, .94f), 6 * scale);
        dl->AddRectFilled(p0, ImVec2(p0.x + bar, p1.y), color(213 / 255.f, 162 / 255.f, 79 / 255.f, 1), 6 * scale,
                          ImDrawFlags_RoundCornersLeft);
    } else {
        dl->AddRectFilled(p0, p1, color(.025f, .04f, .065f, .92f), 6 * scale);
        dl->AddRectFilled(p0, ImVec2(p0.x + bar, p1.y), color(1, .72f, .3f, 1), 6 * scale, ImDrawFlags_RoundCornersLeft);
    }
    ImVec2 at_text(p0.x + bar + pad, p0.y + pad);
    dl->AddText(font, size, at_text, color(1, .8f, .45f, 1), title.c_str(), nullptr, wrap);
    ImVec4 ink = theme == THEME_BLUE ? ImVec4(.96f, .97f, 1, 1) : ImVec4(242 / 255.f, 216 / 255.f, 170 / 255.f, 1);
    dl->AddText(font, size, ImVec2(at_text.x, at_text.y + ts.y + gap), color(ink.x, ink.y, ink.z, 1), text.c_str(), nullptr, wrap);
}

// The line of chat being typed ("/", runtime/hle/chat.c): where the player dragged it (at first above the game's
// chat window), wrapped
static void draw_chat(const char *line, uint64_t now, float scale) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImFont *font = ImGui::GetFont();
    float size = ImGui::GetFontSize() * 1.25f, pad = 8 * scale, bar = 4 * scale;
    std::string text = std::string("Chat: ") + line + ((now / 500000000ull) & 1 ? "_" : " ");
    float wrap = std::min(vp->WorkSize.x * .45f, 560 * scale);  // (a long line wraps; the box grows upwards)
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
    float w = std::max(ts.x, 320 * scale) + bar + 2 * pad, h = ts.y + 2 * pad;
    float left, low;
    chat_position(&left, &low);
    float bottom = vp->WorkPos.y + vp->WorkSize.y * low;
    ImVec2 p0(vp->WorkPos.x + vp->WorkSize.x * left, bottom - h), p1(p0.x + w, bottom);
    chat_set_box((p0.x - vp->WorkPos.x) / vp->WorkSize.x, (p0.y - vp->WorkPos.y) / vp->WorkSize.y,
                 (p1.x - vp->WorkPos.x) / vp->WorkSize.x, (p1.y - vp->WorkPos.y) / vp->WorkSize.y);
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    // In the settings menu's colours ([Application] Theme, runtime/settings_panel.cpp theme()): Blue and Gold a panel
    // with a bar at its left, Hunter the game's window with a red and green rule along its top and bottom
    int theme = settings_panel_theme();
    if (theme == THEME_HUNTER) {
        float line = 2 * scale;
        dl->AddRectFilled(p0, p1, IM_COL32(50, 25, 24, 236));
        dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 255), 0, 0, 2 * scale);
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + line), IM_COL32(176, 34, 30, 255));
        dl->AddRectFilled(ImVec2(p0.x, p0.y + line), ImVec2(p1.x, p0.y + line * 1.7f), IM_COL32(80, 142, 54, 255));
        dl->AddRectFilled(ImVec2(p0.x, p1.y - line * 1.7f), ImVec2(p1.x, p1.y - line), IM_COL32(80, 142, 54, 255));
        dl->AddRectFilled(ImVec2(p0.x, p1.y - line), p1, IM_COL32(176, 34, 30, 255));
    } else {
        bool gold = theme == THEME_GOLD;
        dl->AddRectFilled(p0, p1, gold ? IM_COL32(34, 25, 19, 240) : IM_COL32(6, 10, 17, 235), 6 * scale);
        dl->AddRectFilled(p0, ImVec2(p0.x + bar, p1.y), gold ? IM_COL32(213, 162, 79, 255) : IM_COL32(111, 216, 255, 255),
                          6 * scale, ImDrawFlags_RoundCornersLeft);
    }
    ImU32 ink = theme == THEME_GOLD ? IM_COL32(242, 216, 170, 255) : IM_COL32(244, 247, 252, 255);
    dl->AddText(font, size, ImVec2(p0.x + bar + pad, p0.y + pad), ink, text.c_str(), nullptr, wrap);
}

// ------------------------------------------------------------------ frames (game thread)
static std::mutex g_snap_lock;  // the copy the render thread draws
static ImDrawData g_snap;
static std::atomic<uint32_t> g_disp_w, g_disp_h;
static bool g_plugins_init;
static uint64_t g_last_ns;

static void snap_clear(void) {
    for (ImDrawList *l : g_snap.CmdLists) IM_DELETE(l);
    g_snap.CmdLists.resize(0);
    g_snap.CmdListsCount = g_snap.TotalIdxCount = g_snap.TotalVtxCount = 0;
    g_snap.Valid = false;
}

// A texture ImGui wants made whose descriptor set could not be had (VK_ERROR_OUT_OF_POOL_MEMORY: the backend's pool
// full). The backend still made its image, gave it a null set and marked it ready, and the next draw naming it
// asserts ("ImDrawCmd is referring to ImTextureData that wasn't uploaded"). Undo it and ask again next frame, by
// when textures ImGui let go of (an outgrown font atlas: freed after ImageCount frames) have freed their sets.
// VkOverlayTex: the backend's ImGui_ImplVulkan_Texture (imgui_impl_vulkan.cpp, internal to it), field for field.
#ifndef MHGU_NULL_BACKEND
struct VkOverlayTex { VkDeviceMemory memory; VkImage image; VkImageView view; VkDescriptorSet set; };
static VkDevice g_dev;
static bool tex_create_failed(ImTextureData *tex) {
    VkOverlayTex *bt = (VkOverlayTex *)tex->BackendUserData;
    if (!bt || bt->set != VK_NULL_HANDLE) return false;
    static int logged;
    if (logged++ < 8) LOG("[forge] ImGui texture %dx%d: no descriptor set free, again next frame\n", tex->Width, tex->Height);
    vkDestroyImageView(g_dev, bt->view, nullptr);
    vkDestroyImage(g_dev, bt->image, nullptr);
    vkFreeMemory(g_dev, bt->memory, nullptr);
    IM_FREE(bt);
    tex->BackendUserData = nullptr;
    tex->SetStatus(ImTextureStatus_WantCreate);
    return true;
}
#else
static bool tex_create_failed(ImTextureData *) { return false; }
#endif

void overlay_frame(void *cpu) {
    // (once a frame on the game's update thread: with [Application] PauseInBackground the game stops here while the app
    // is in the background, its clock held: runtime/hle/vi.c)
    vi_pause_game();
    if (!g_ready) return;
    // (the menu closed: its state let go here, drawn - the credits - only inside the frame below: before the first frame
    // there is no font, the credits' text measured with none faulted at the first frame - "accessing 0" under
    // frame_update - and later ones were drawn into the last frame's list, cleared as the frame began)
    if (!g_settings_open) settings_panel_idle();
    bool credits = settings_panel_credits_up();
    bool open = g_open, free_render = forge_plugins_have(FORGE_IMGUI_FREE_RENDER);
    uint64_t now = SDL_GetTicksNS();
#ifndef __ANDROID__
    menu_hint(now);
#endif
    if (g_modal_open) notice_hold(now);
    bool notice = notice_due(now);
    char line[256];
    bool chat = chat_composing(line, sizeof line);
    ImGuiIO &io = ImGui::GetIO();
    g_wants_text = g_open && io.WantTextInput;  // (last frame's: a Forge plugin's text box has the focus; runtime/textinput.c)
    bool touch = touch_drawn();  // (hidden, the settings button still shows)
    bool stats = perfstats_visible();  // ([Performance] ShowStats: runtime/perfstats.cpp)
    bool fading = settings_panel_fading();  // (the settings menu closed a moment ago: fading out, taking no input)
    if (!open && !free_render && !notice && !chat && !touch && !stats && !credits && !fading) {  // (nothing to draw)
        std::lock_guard<std::mutex> l(g_snap_lock);
        if (g_snap.Valid) snap_clear();
        apply_input(io);  // (keep the queue short)
        g_last_ns = 0;
        return;
    }
    uint32_t w = g_disp_w, h = g_disp_h;
    if (!w || !h) return;
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    float scale = std::max(1.0f, h / 720.0f);  // (Forge's sizes are for the console's 720p screen)
    if (scale != g_scale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_base_style;
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
        g_scale = scale;
    }
    io.DeltaTime = g_last_ns ? std::clamp((now - g_last_ns) / 1e9f, 1e-4f, 0.1f) : 1.0f / 30;
    g_last_ns = now;
    apply_input(io);

    if (!g_plugins_init) {  // (Forge: right after its context is made)
        g_plugins_init = true;
        forge_imgui_init(cpu);
    }
    cjk_font_if_needed();  // (before the frame: the atlas can't change inside one)
    ImGui_ImplVulkan_NewFrame();
    settings_panel_before_frame(g_settings_open);
    ImGui::NewFrame();
    bool settings = g_settings_open;
    settings_panel(&settings);
    if (settings_panel_confirming()) settings = true;
    if (settings != g_settings_open) { g_settings_open = settings; set_open(g_forge_open); }
    // The thank-you opened or closed this frame: while it is open every input is the overlay's (overlay_event, the
    // controller in runtime/host_input.c, the fingers in runtime/touch.cpp), as with a menu
    bool modal = settings_panel_modal();
    if (modal != g_modal_open) { g_modal_open = modal; set_open(g_forge_open); }
    if (g_forge_open) {
        bool keep = true;
        ImGui::SetNextWindowPos(ImVec2(40 * scale, 40 * scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420 * scale, 360 * scale), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("forge", &keep)) {
            if (ImGui::CollapsingHeader("About Forge")) {
                ImGui::Text("Version: %s", "1.5.1");
                ImGui::SeparatorText("Developers");
                ImGui::BulletText("Fexty");
                ImGui::BulletText("jeffi2287");

                ImGui::SeparatorText("Libraries Used");
                ImGui::BulletText("devkitARM/libnx (32-bit)");
                ImGui::BulletText("NNSDK");
                ImGui::BulletText("switch-tools");
                ImGui::BulletText("iniparser");
                ImGui::BulletText("Dear ImGui");
            }
            if (ImGui::CollapsingHeader("Loaded Plugins")) {
                const char *names[16];
                int n = forge_plugin_titles(names, 16);
                for (int i = 0; i < n; i++) ImGui::BulletText("%s", names[i]);
            }
            if (ImGui::CollapsingHeader("Plugin UI")) forge_imgui_run(cpu, FORGE_IMGUI_RENDER);
        }
        ImGui::End();
        if (!keep) set_open(false);
    }
    forge_imgui_run(cpu, FORGE_IMGUI_FREE_RENDER);
    if (notice && !g_modal_open) draw_notice(now, scale);  // (not over the thank-you: it waits, notice_hold)
    if (chat && !g_modal_open) draw_chat(line, now, scale);
    if (stats) perfstats_draw((float)w, (float)h);  // (under the touch controls, on the same background list)
    if (touch) touch_draw((float)w, (float)h);  // (the touch controls: runtime/touch.cpp)
    ImGui::Render();
    settings_panel_after_render(ImGui::GetDrawData());

    ImDrawData *dd = ImGui::GetDrawData();
    bool tex_failed = false;
    if (dd->Textures)  // (texture uploads: on this thread, between the render thread's submissions)
        for (ImTextureData *tex : *dd->Textures)
            if (tex->Status != ImTextureStatus_OK) {
                bool create = tex->Status == ImTextureStatus_WantCreate;
                vk_lock();
                uint64_t t0 = HITCH_ON ? hitch_now() : 0;
                ImGui_ImplVulkan_UpdateTexture(tex);
                if (create && tex->TexID == ImTextureID_Invalid) tex_failed |= tex_create_failed(tex);
                if (HITCH_ON) hitch_add(HC_OVERLAY_TEX, hitch_now() - t0, (uint64_t)tex->Width * tex->Height * 4);
                vk_unlock();
            }
    std::lock_guard<std::mutex> l(g_snap_lock);
    snap_clear();
    if (tex_failed) return;  // (nothing drawn this frame: a draw naming a texture with no descriptor set is an assert)
    for (ImDrawList *list : dd->CmdLists) g_snap.CmdLists.push_back(list->CloneOutput());
    g_snap.CmdListsCount = dd->CmdListsCount;
    g_snap.TotalIdxCount = dd->TotalIdxCount;
    g_snap.TotalVtxCount = dd->TotalVtxCount;
    g_snap.DisplayPos = dd->DisplayPos;
    g_snap.DisplaySize = dd->DisplaySize;
    g_snap.FramebufferScale = dd->FramebufferScale;
    g_snap.Textures = nullptr;  // (done above)
    g_snap.Valid = true;
}

// ------------------------------------------------------------------ drawing (render thread)
static VkRenderPass g_pass;

static void vk_check(VkResult r) {
    if (r < 0) LOG("[forge] ImGui Vulkan error %d\n", (int)r);
}

void overlay_draw(VkCommandBuffer cb, const OverlayTarget *t) {
#ifndef MHGU_NULL_BACKEND
    if (!g_enabled) return;
    g_disp_w = t->width, g_disp_h = t->height;
    if (t->pass != g_pass) {  // (the window's render pass: made once, again only if the window's format changes)
        if (g_pass) {
            g_ready = false;
            ImGui_ImplVulkan_Shutdown();
        }
        ImGui_ImplVulkan_InitInfo info = {};
        info.ApiVersion = VK_API_VERSION_1_2;
        info.Instance = t->inst;
        info.PhysicalDevice = t->phys;
        info.Device = t->dev;
        info.QueueFamily = t->qfam;
        info.Queue = t->queue;
        info.PipelineCache = t->cache;
        // One set per ImGui texture: the font atlases (a new texture each time one grows or is repacked, the old one freed
        // ImageCount frames later; damage numbers at many sizes do that over and over) and the plugins' own. 16 ran out.
        info.DescriptorPoolSize = 1024;
        info.MinImageCount = 2;
        info.ImageCount = 8;  // (vertex buffers in turn: more than the frames ever in flight)
        info.PipelineInfoMain.RenderPass = t->pass;
        info.PipelineInfoMain.Subpass = 0;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn = vk_check;
        if (!ImGui_ImplVulkan_Init(&info)) {
            LOG("[forge] ImGui's Vulkan renderer did not start: no overlay\n");
            g_enabled = false;
            return;
        }
        g_pass = t->pass;
        g_dev = t->dev;
        g_ready = true;
    }
    std::lock_guard<std::mutex> l(g_snap_lock);
    if (g_snap.Valid && g_snap.CmdListsCount) ImGui_ImplVulkan_RenderDrawData(&g_snap, cb);
#endif
}
