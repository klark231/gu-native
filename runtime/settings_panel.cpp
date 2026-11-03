#include "settings_panel.h"
#include "config.h"
#include "menu_confirm.h"
#include "settings_locale.h"
#include "settings_icons.h"
#include "app_actions.h"
#include "cheats.h"
#include "overlay.h"
#include "padmap.h"
#include "keymap.h"
#include "modorder.h"
#include "net/net.h"
#include "touch.h"
#include "imgui_internal.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

namespace {
enum class View { Main, Keyboard, CloseChanges, Exit, Restart, Reset, Move, Conflicts, KeyCapture, Keybinds, TouchButtons, ComboEdit,
                  PadList, PadMap };
enum class Command { None, Save };
struct Row {
    std::string id, title, value;
    std::string key, help;  // its ini key (config.c g_settings; its help: config_help), or a help text of its own
    bool enabled = true, cold = false, action = false, warn = false;
    bool header = false, current = false;  // a section title (not a row to select); the choice a dropdown has now
    bool menu = false;                     // Confirm opens a dropdown of its choices
    int toggle = -1;                       // On / Off (1 / 0; -1: not a switch): the phone draws a switch
    bool number = false;                   // a number: left / right step it (the phone: - and + buttons on the row)
    std::function<void(int)> adjust;
    std::function<void()> activate;
};
struct Target {
    std::string id;
    ImVec2 center;
    bool enabled;
};
struct Origin {
    View view;
    std::string focus;
    float scroll;
};
// A row's choices, listed in a window of their own under it (or above it, near the bottom) while it is open: the
// controller moves in the list, Confirm picks, Back or a click elsewhere closes it
struct Dropdown {
    bool open = false, hovered = false, anchored = false;
    std::string row;  // the row it belongs to
    std::vector<std::string> names;
    int selected = 0;
    std::function<void(int)> set;
    ImRect anchor;  // that row on the screen, this frame (draw_rows)
    std::string title;  // (the phone's picker: its row's title over the choices)
};
struct Keyboard {
    std::string title, original, buffer;
    size_t cursor = 0, capacity = 255;
    bool upper = false, select_all = false;
    std::function<bool(const std::string &, std::string &)> commit;
};
// The phone's sizes (touch_panel, each frame), from the screen's height in its pixels: a row, a tab and a button about
// 0.09 of it, the text 0.034
struct Metrics {
    float H = 720, f = 16, row = 40, pad = 8, line = 2;
};
// A finger dragging a list (a child window: the rows, the picker's choices) and the glide after it lets go
struct Drag {
    ImGuiID window = 0;
    float from = 0, scroll = 0, last = 0, velocity = 0;  // (the finger's height and the scroll as the drag began; px / s)
    bool pressed = false, moving = false;                // (moving: past the threshold, no longer a tap)
};
struct State {
    Config draft = {}, saved = {};
    bool editing = false, floating = false, advanced = false, focus_request = true, saved_notice = false;
    int page = 0;
    View view = View::Main;
    Command command = Command::None;
    std::string focus, error;
    std::string help_row, hovered;  // the row whose help shows (the one focused or pointed at last); the one pointed at
    bool pointing = false;          // the mouse pointer is on one of the view's rows (hovered: also the focused one)
    float help_height = 0;          // room for the longest help of the tab (last frame's)
    std::vector<Target> targets;
    std::vector<Origin> origins;
    Dropdown dropdown;
    Keyboard keyboard;
    uint64_t quest_keys[HP_QUESTS_MAX] = {}, next_quest_key = 1;
    bool restore_scroll = false;
    float scroll = 0, guide_height = 0;
    int focus_scroll_frames = 0;
    ImVec2 move_start = {}, move_position = {};
    // The mouse: dragging the top of the menu moves it, the grip at its bottom right corner sizes it (custom_size,
    // 0: as tall as its rows, as wide as it is by default); either one detaches it
    ImVec2 custom_size = {}, drag_mouse = {}, drag_from = {};
    int dragging = 0;  // 1 moving, 2 sizing
    bool dragged = false;  // (the press moved the menu: the tab under it is not chosen when it lets go)
    float header_bottom = 0;  // (the top part that moves the menu, from the window's top: last frame's)
    bool position_override = false;
    int capture_bind = -1;     // the keybind (BIND_*) a key is being chosen for; -1: the chat key
    bool capture_add = false;  // the key chosen is that keybind's second, else its only one
    // The phone's layout (touch_ui)
    bool touch = false;    // (not Android: the last press on the menu was a finger's - overlay.cpp marks them)
    bool touch_next = false;
    bool swallow = false;  // the press under way does nothing when it lets go (it closed the picker, began a drag)
    ImGuiID list = 0;      // the list of rows (a child window) this frame; 0: the menu itself scrolls (keyboard / mouse)
    Metrics m;
    Drag drag;
} s;
std::atomic<bool> request_exit, request_back, is_dirty, is_confirming, is_capturing, capture_mouse, capture_wheel;
// (the key pressed while a key is being chosen, or KEY_MOUSE + a mouse button; -1: none yet)
std::atomic<int> captured_key{-1};
const char *tr(const char *text) { return settings_text(s.draft.language, text); }
std::string fmt(const char *text, ...) {
    char buffer[512];
    va_list args;
    va_start(args, text);
    vsnprintf(buffer, sizeof buffer, tr(text), args);
    va_end(args);
    return buffer;
}
// The phone's layout (touch_panel): rows, tabs and buttons a finger hits, a header with Back / Save / Close, lists that
// a finger drags, a picker for a row's choices, - and + on a number's. On Android always (no hover, wheel or keyboard
// there; a controller still works it as before); elsewhere while fingers work the menu (a mouse click puts it back)
bool touch_ui() {
#ifdef __ANDROID__
    return true;
#else
    return s.touch;
#endif
}
// The menu's colours ([Application] Theme, applied as it is picked): Blue, the original look; Gold, a warm panel
// where rows light up with a gold bar at their left instead of a frame, and tabs that are only their names,
// underlined; and Hunter, after the game's own windows (the item pouch) - a maroon panel with a red and green rule at
// its top and bottom, gold corners, rows split by lines and the focused one filled gold.
struct Theme {
    ImU32 panel, edge, row, row_hover, row_focus, tab, tab_active, accent, accent_bright, text, text_dim, text_off,
        warn, divider, popup;
    bool bar;         // the focused row: lit with a bar at its left (else framed)
    float rounding;   // of the window, the rows and the tabs
    bool game;        // Hunter: the game's window frame (draw_frame), lines between the rows, a filled header
    ImU32 header;     // (game: the header band's colour)
};
const Theme &theme() {
    static const Theme blue = {IM_COL32(6, 10, 17, 250),     IM_COL32(110, 217, 255, 255), IM_COL32(23, 33, 48, 255),
                               IM_COL32(38, 71, 99, 255),    IM_COL32(41, 82, 117, 255),   IM_COL32(15, 23, 36, 255),
                               IM_COL32(46, 102, 143, 255),  IM_COL32(111, 216, 255, 255), IM_COL32(111, 216, 255, 255),
                               IM_COL32(244, 247, 252, 255), IM_COL32(178, 192, 208, 255), IM_COL32(132, 145, 160, 255),
                               IM_COL32(255, 184, 145, 255), IM_COL32(52, 72, 94, 255),    IM_COL32(13, 20, 31, 255),
                               false,                        3.f};
    static const Theme gold = {IM_COL32(34, 25, 19, 248),    IM_COL32(139, 97, 40, 255),   IM_COL32(251, 236, 206, 10),
                               IM_COL32(229, 195, 126, 28),  IM_COL32(213, 162, 79, 64),   IM_COL32(0, 0, 0, 0),
                               IM_COL32(0, 0, 0, 0),         IM_COL32(213, 162, 79, 255),  IM_COL32(247, 226, 162, 255),
                               IM_COL32(242, 216, 170, 255), IM_COL32(175, 150, 128, 255), IM_COL32(124, 107, 90, 255),
                               IM_COL32(210, 106, 80, 255),  IM_COL32(139, 97, 40, 90),    IM_COL32(42, 31, 23, 252),
                               true,                         6.f};
    static const Theme hunter = {IM_COL32(50, 25, 24, 236),    IM_COL32(0, 0, 0, 255),       IM_COL32(0, 0, 0, 0),
                                 IM_COL32(255, 255, 255, 20),  IM_COL32(176, 145, 24, 235),  IM_COL32(0, 0, 0, 0),
                                 IM_COL32(0, 0, 0, 0),         IM_COL32(236, 212, 44, 255),  IM_COL32(255, 232, 84, 255),
                                 IM_COL32(242, 238, 232, 255), IM_COL32(196, 176, 166, 255), IM_COL32(132, 112, 106, 255),
                                 IM_COL32(255, 96, 72, 255),   IM_COL32(96, 88, 88, 255),    IM_COL32(56, 28, 27, 250),
                                 false,                        0.f,                          true,
                                 IM_COL32(108, 46, 46, 255)};
    return s.draft.menu_theme == THEME_BLUE ? blue : s.draft.menu_theme == THEME_HUNTER ? hunter : gold;
}
ImVec4 vec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
ImU32 fade(ImU32 c, float a) {
    return (c & 0x00ffffffu) | ((ImU32)std::clamp(((c >> 24) & 0xff) * a, 0.f, 255.f) << 24);
}
// The phone's colours beside the theme's: Hunter's are the touch controls' (touch.cpp) - dark plates with a bronze edge
// and a gold rule, the picked row and tab lit ochre to gold as the game's cursor
struct Look {
    ImU32 plate, edge, rule, lit, ink, dim, glow, bar0, bar1, band;
};
Look touch_look() {
    const Theme &t = theme();
    if (t.game)
        return {IM_COL32(28, 20, 15, 235),   IM_COL32(112, 76, 34, 255),  IM_COL32(222, 186, 96, 255),
                IM_COL32(92, 62, 22, 255),   IM_COL32(244, 232, 200, 255), IM_COL32(204, 186, 160, 255),
                IM_COL32(255, 214, 92, 255), IM_COL32(206, 158, 42, 240),  IM_COL32(122, 82, 22, 225),
                IM_COL32(74, 34, 31, 255)};
    return {t.popup, t.divider | IM_COL32_A_MASK, t.accent, fade(t.accent, .35f), t.text, t.text_dim, t.accent_bright,
            fade(t.accent, .55f), fade(t.accent, .25f), t.popup};
}
void diamond(ImDrawList *dl, ImVec2 c, float d, ImU32 col) {
    ImVec2 q[4] = {ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y)};
    dl->AddConvexPolyFilled(q, 4, col);
}
// Text with a dark shadow under it (readable on the gold of the picked row, as the game's)
void shadow_text(ImDrawList *dl, float size, ImVec2 p, ImU32 col, const char *text, float wrap = 0) {
    float o = std::max(1.f, size * .06f);
    dl->AddText(ImGui::GetFont(), size, ImVec2(p.x + o, p.y + o), fade(IM_COL32(0, 0, 0, 170), ((col >> 24) & 0xff) / 255.f),
                text, nullptr, wrap);
    dl->AddText(ImGui::GetFont(), size, p, col, text, nullptr, wrap);
}
// A finger's button: a dark square plate with an edge and a rule inside (the touch controls' discs, made square), gold
// studs on its corners (Hunter); lit while held, faded while it does nothing
void plate(ImDrawList *dl, const ImRect &r, float line, bool held, bool enabled) {
    const Look k = touch_look();
    float a = enabled ? 1.f : .45f, in = line * 1.8f;
    dl->AddRectFilled(ImVec2(r.Min.x, r.Min.y + line), ImVec2(r.Max.x, r.Max.y + line), fade(IM_COL32(0, 0, 0, 110), a));
    dl->AddRectFilled(r.Min, r.Max, fade(held ? k.lit : k.plate, a));
    dl->AddRect(r.Min, r.Max, fade(k.edge, a), 0, 0, line);
    dl->AddRect(ImVec2(r.Min.x + in, r.Min.y + in), ImVec2(r.Max.x - in, r.Max.y - in), fade(k.rule, a * .7f), 0, 0,
                std::max(1.f, line * .45f));
    if (theme().game)
        for (ImVec2 c : {r.Min, ImVec2(r.Max.x, r.Min.y), ImVec2(r.Min.x, r.Max.y), r.Max})
            diamond(dl, c, line * 1.5f, fade(k.rule, a));
    if (held)
        dl->AddRect(ImVec2(r.Min.x - line, r.Min.y - line), ImVec2(r.Max.x + line, r.Max.y + line), fade(k.glow, .7f), 0,
                    0, line);
}
// The glyphs on the phone's buttons: an arrow (dir -1: back), a cross (close), - and +
void chevron(ImDrawList *dl, ImVec2 c, float r, int dir, ImU32 col, float thick) {
    dl->PathLineTo(ImVec2(c.x - dir * r * .5f, c.y - r));
    dl->PathLineTo(ImVec2(c.x + dir * r * .5f, c.y));
    dl->PathLineTo(ImVec2(c.x - dir * r * .5f, c.y + r));
    dl->PathStroke(col, 0, thick);
}
void cross(ImDrawList *dl, ImVec2 c, float r, ImU32 col, float thick) {
    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, thick);
    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, thick);
}
void sign(ImDrawList *dl, ImVec2 c, float r, bool plus, ImU32 col, float thick) {
    dl->AddRectFilled(ImVec2(c.x - r, c.y - thick * .5f), ImVec2(c.x + r, c.y + thick * .5f), col);
    if (plus)
        dl->AddRectFilled(ImVec2(c.x - thick * .5f, c.y - r), ImVec2(c.x + thick * .5f, c.y + r), col);
}
// On / Off: a slot whose stud slides to the right and lights it green when on
void switch_mark(ImDrawList *dl, const ImRect &r, bool on) {
    const Look k = touch_look();
    float line = s.m.line, in = line * 1.3f, side = r.GetHeight() - in * 2;
    dl->AddRectFilled(r.Min, r.Max, on ? IM_COL32(58, 112, 40, 255) : IM_COL32(18, 12, 10, 230));
    dl->AddRect(r.Min, r.Max, on ? k.rule : k.edge, 0, 0, line * .7f);
    float x = on ? r.Max.x - in - side : r.Min.x + in;
    ImRect knob(x, r.Min.y + in, x + side, r.Max.y - in);
    dl->AddRectFilled(knob.Min, knob.Max, on ? k.glow : IM_COL32(128, 108, 92, 255));
    dl->AddRect(knob.Min, knob.Max, IM_COL32(0, 0, 0, 170), 0, 0, std::max(1.f, line * .4f));
    diamond(dl, knob.GetCenter(), side * .2f, on ? IM_COL32(28, 20, 15, 255) : IM_COL32(60, 48, 40, 255));
}
// The Mods tab: the mods folder's state (modorder.h), which is saved with the rest of the settings
constexpr int PAGES = 8, MODS_PAGE = 5, CHEATS_PAGE = 6, APP_PAGE = 7;
const char *const page_names[PAGES] = {"Display", "Graphics & Audio", "Controls", "Network",
                                       "Misc",    "Mods",             "Cheats",   "Menu Options"};
struct ScanResult {
    std::vector<std::string> names;  // (the order ModConflict's indexes are in)
    ModScan scan = {};
    std::atomic<bool> done{false};
    ~ScanResult() { modscan_free(&scan); }
};
struct Mods {
    std::vector<ModEntry> draft, saved, boot;
    bool boot_set = false;
    std::shared_ptr<ScanResult> scan;
} mods;
bool same_mods(const std::vector<ModEntry> &a, const std::vector<ModEntry> &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].on != b[i].on || strcmp(a[i].name, b[i].name))
            return false;
    return true;
}
bool mods_dirty() { return !same_mods(mods.draft, mods.saved); }
// The Cheats tab: each cheat on or off (cheats.h), applied while the game runs when the settings are saved
struct Cheats {
    std::vector<char> draft, saved;
} cheat_tab;
bool cheats_dirty() { return cheat_tab.draft != cheat_tab.saved; }
void load_cheats() {
    cheat_tab.draft.assign(cheats_count(), 0);
    for (int i = 0; i < cheats_count(); ++i)
        cheat_tab.draft[i] = cheats_on(i);
    cheat_tab.saved = cheat_tab.draft;
}
bool mods_restart() { return !same_mods(mods.saved, mods.boot); }
void load_mods() {
    ModEntry all[MODS_MAX];
    int n = mods_installed(all, MODS_MAX);
    mods.draft.assign(all, all + n);
    mods.saved = mods.draft;
    if (!mods.boot_set) {
        mods.boot = mods.draft;
        mods.boot_set = true;
    }
    mods.scan.reset();
}
void start_scan() {
    auto result = std::make_shared<ScanResult>();
    for (const auto &m : mods.draft)
        result->names.push_back(m.name);
    std::sort(result->names.begin(), result->names.end(),
              [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    mods.scan = result;
    std::thread([result]() {
        std::vector<std::array<char, 128>> names;
        for (const auto &n : result->names) {
            names.emplace_back();
            snprintf(names.back().data(), 128, "%s", n.c_str());
        }
        modscan_run(reinterpret_cast<const char(*)[128]>(names.data()), (int)names.size(), &result->scan);
        result->done = true;
    }).detach();
}
int mod_index(const std::string &name) {
    for (int i = 0; i < (int)mods.draft.size(); ++i)
        if (name == mods.draft[i].name)
            return i;
    return -1;
}
bool dirty();
struct GuideLayout {
    ImVec2 offset[4];
    float height = 0;
};
GuideLayout guide_layout(float width) {
    GuideLayout layout;
    float f = ImGui::GetFontSize(), x = 0, y = 0;
    const char *labels[] = {"Navigation", "Confirm", "Back", "Save"};
    for (int i = 0; i < 4; i++) {
        float item_width = f * 1.4f + ImGui::CalcTextSize(tr(labels[i])).x;
        if (x && x + item_width > width) {
            x = 0;
            y += f + ImGui::GetStyle().ItemSpacing.y;
        }
        layout.offset[i] = ImVec2(x, y);
        x += item_width + f;
    }
    layout.height = y + f;
    return layout;
}
void draw_guides(ImGuiWindow *window, const GuideLayout &layout, float top) {
    auto *draw = window->DrawList;
    ImVec2 start(window->InnerRect.Min.x + ImGui::GetStyle().WindowPadding.x, top);
    ImU32 background = ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK;
    draw->AddRectFilled(ImVec2(window->InnerRect.Min.x, top - ImGui::GetStyle().ItemSpacing.y),
                        ImVec2(window->InnerRect.Max.x, top + layout.height), background);
    const int icons[] = {SettingsIcons::Dpad, SettingsIcons::Confirm, SettingsIcons::Back, SettingsIcons::Save};
    const char *labels[] = {"Navigation", "Confirm", "Back", "Save"};
    float f = ImGui::GetFontSize();
    for (int i = 0; i < 4; i++) {
        ImVec2 pos(start.x + layout.offset[i].x, start.y + layout.offset[i].y);
        // (Save: the top face button, Y / X / triangle - Ctrl+S on the keyboard - greyed while there is nothing to save)
        SettingsIcons::draw(draw, pos, f, s.draft.button_icons, icons[i],
                            i == 3 ? 3
                                   : menu_confirm_position(config_live_menu_confirm(), overlay_face_position(0),
                                                           overlay_face_position(1), i == 2));
        draw->AddText(ImVec2(pos.x + f * 1.4f, pos.y),
                      i == 3 && !dirty() ? theme().text_off : ImGui::GetColorU32(ImGuiCol_Text), tr(labels[i]));
    }
}
bool confirmation() {
    return s.view == View::CloseChanges || s.view == View::Exit || s.view == View::Restart ||
           s.view == View::Reset;
}
bool equal(const Config &a, const Config &b) {
#define SAME(f)                                                                                              \
    if (a.f != b.f)                                                                                          \
    return false
    SAME(display_mode);
    SAME(width);
    SAME(height);
    SAME(render_w);
    SAME(render_h);
    SAME(render_scale);
    SAME(low_end);
    SAME(fps);
    SAME(fsr);
    SAME(fsr_upscaler);
    SAME(fsr_quality);
    SAME(fsr_sharpness);
    SAME(fsr_objmotion);
    SAME(fsr_reactive);
    SAME(fsr_animsurf);
    SAME(vsync);
    SAME(shader_prewarm);
    SAME(draw_distance);
    SAME(smooth_gradients);
    SAME(msaa);
    SAME(fxaa);
    SAME(fxaa_mode);
    SAME(face_buttons);
    SAME(ldn_port);
    SAME(sync_small_monsters);
    SAME(bonus_content);
    SAME(village_in_hub);
    SAME(mouse_sensitivity);
    SAME(touch_controls);
    SAME(touch_hud);
    SAME(touch_camera);
    SAME(touch_opacity);
    SAME(aspect_ratio);
    SAME(fov);
    SAME(ui_anchor);
    SAME(fill_screen);
    SAME(background_input);
    SAME(combo_window);
    SAME(volume);
    SAME(mute_in_background);
    SAME(touch_vibration);
    SAME(phone_rumble);
    SAME(show_stats);
    SAME(pause_background);
    SAME(button_icons);
    SAME(menu_theme);
    SAME(discord);
    SAME(discord_hunter);
    SAME(language);
    SAME(menu_scale);
    SAME(menu_confirm);
    SAME(confirm_exit);
    SAME(updates);
    SAME(hp.village);
    SAME(hp.quests);
#undef SAME
    if (a.lock_on != b.lock_on)
        return false;
    if (memcmp(a.touch_show, b.touch_show, sizeof a.touch_show) || memcmp(a.touch_combo, b.touch_combo, sizeof a.touch_combo) ||
        memcmp(a.touch_combo_pad, b.touch_combo_pad, sizeof a.touch_combo_pad))
        return false;
    if (memcmp(a.binds, b.binds, sizeof a.binds))
        return false;
    if (strcmp(a.ldn_join, b.ldn_join) || strcmp(a.ldn_server, b.ldn_server) || strcmp(a.ldn_key, b.ldn_key) ||
        strcmp(a.chat_key, b.chat_key))
        return false;
    for (int n = 0; n < 4; ++n)
        if (a.hp.scale[n] != b.hp.scale[n])
            return false;
    for (int i = 0; i < a.hp.quests; ++i) {
        if (a.hp.quest[i].quest != b.hp.quest[i].quest)
            return false;
        for (int n = 0; n < 4; ++n)
            if (a.hp.quest[i].scale[n] != b.hp.quest[i].scale[n])
                return false;
    }
    return true;
}
bool dirty() { return !equal(s.draft, s.saved) || mods_dirty() || cheats_dirty(); }
bool pending_text() {
    bool editing = s.view == View::Keyboard;
    for (const auto &o : s.origins)
        editing |= o.view == View::Keyboard;
    return editing && s.keyboard.buffer != s.keyboard.original;
}
std::string quest_id(int index) { return "quest." + std::to_string(s.quest_keys[index]); }
void quest_keys() {
    for (int i = 0; i < s.draft.hp.quests; ++i)
        s.quest_keys[i] = s.next_quest_key++;
}
void focus(const std::string &id) {
    s.focus = id;
    s.focus_request = true;
}
void close_dropdown() {
    if (!s.dropdown.open)
        return;
    s.dropdown.open = false;
    focus(s.dropdown.row);
}
// The scroll of the rows: the phone's list, or the menu itself
float list_scroll() {
    auto *w = s.list ? ImGui::FindWindowByID(s.list) : ImGui::FindWindowByName("Settings##main");
    return w ? w->Scroll.y : 0;
}
void push(View view, const std::string &first) {
    s.dropdown.open = false;
    s.drag = {};
    s.origins.push_back({s.view, s.focus, list_scroll()});
    s.view = view;
    focus(first);
    s.error.clear();
    s.restore_scroll = true;
    s.scroll = 0;
    ImGui::ClearActiveID();
}
void pop() {
    s.dropdown.open = false;
    s.drag = {};
    if (s.origins.empty()) {
        s.view = View::Main;
        s.focus.clear();
    } else {
        auto o = s.origins.back();
        s.origins.pop_back();
        s.view = o.view;
        focus(o.focus);
        s.scroll = o.scroll;
    }
    s.restore_scroll = true;
    s.error.clear();
    ImGui::ClearActiveID();
}
void first_page(int page) {
    s.dropdown.open = false;
    s.drag = {};
    s.page = page;
    s.view = View::Main;
    s.origins.clear();
    s.focus.clear();
    s.focus_request = true;
    s.restore_scroll = true;
    s.scroll = 0;
    s.error.clear();
}
void discard() {
    s.draft = s.saved;
    mods.draft = mods.saved;
    cheat_tab.draft = cheat_tab.saved;
    quest_keys();
    s.error.clear();
    s.saved_notice = false;
}
void ask_back(bool *open) {
    if (s.dropdown.open)
        close_dropdown();
    else if (s.view != View::Main) {
        if (s.view == View::Move) {
            s.move_position = s.move_start;
            s.position_override = true;
        }
        if (s.view == View::CloseChanges) touch_edit_cancel();  // (B / Esc / the chord: the menu stays, no editor)
        pop();
    } else if (dirty())
        push(View::CloseChanges, "dialog.cancel");
    else
        *open = false;
}
// The phone's Close (X), from any view: asks first when something is unsaved, as Back does from the tabs
void ask_close(bool *open) {
    s.dropdown.open = false;
    if (dirty() || pending_text())
        push(View::CloseChanges, "dialog.cancel");
    else
        *open = false;
}
void keyboard(const std::string &title, const std::string &initial, size_t cap,
              std::function<bool(const std::string &, std::string &)> commit) {
    s.keyboard = {title, initial, initial, initial.size(), cap, false, false, std::move(commit)};
    push(View::Keyboard, "key.0");
}
void insert(const std::string &text) {
    if (s.keyboard.select_all) {
        s.keyboard.buffer.clear();
        s.keyboard.cursor = 0;
        s.keyboard.select_all = false;
    }
    if (s.keyboard.buffer.size() + text.size() > s.keyboard.capacity)
        return;
    s.keyboard.buffer.insert(s.keyboard.cursor, text);
    s.keyboard.cursor += text.size();
    s.error.clear();
}
void erase_previous() {
    if (s.keyboard.select_all) {
        s.keyboard.buffer.clear();
        s.keyboard.cursor = 0;
        s.keyboard.select_all = false;
        return;
    }
    if (!s.keyboard.cursor)
        return;
    size_t begin = s.keyboard.cursor - 1;
    while (begin && (static_cast<unsigned char>(s.keyboard.buffer[begin]) & 0xc0) == 0x80)
        --begin;
    s.keyboard.buffer.erase(begin, s.keyboard.cursor - begin);
    s.keyboard.cursor = begin;
    s.error.clear();
}
bool accept_keyboard() {
    if (!s.keyboard.commit(s.keyboard.buffer, s.error))
        return false;
    pop();
    return true;
}
// The row's choices in a dropdown under it; selected: the one it has now (-1: none of them)
void choices(const Row &row, std::vector<std::string> names, int selected, std::function<void(int)> set) {
    int first = std::clamp(selected, 0, (int)names.size() - 1);
    s.dropdown = {true, false, false, row.id, std::move(names), selected, std::move(set), {}, row.title};
    focus("choice." + std::to_string(first));
}
// What the settings table (config.c g_settings) says of a setting: does it need a restart, a number's range. ini():
// the key a row is for (its restart mark and its help)
const char *ini(const char *key) { return key; }
bool cold(const char *key) { return key && config_restart_key(key); }
float lo(const char *key) { float a = 0, b = 0; config_range(key, &a, &b); return a; }
float hi(const char *key) { float a = 0, b = 0; config_range(key, &a, &b); return b; }
void keyed(Row &r, const char *key) {
    r.key = key ? key : "";
    r.cold = cold(key);
}
void add_bool(std::vector<Row> &rows, const char *id, const char *name, int *value, const char *key = nullptr) {
    Row r;
    r.id = id;
    r.title = tr(name);
    keyed(r, key);
    r.value = tr(*value ? "On" : "Off");
    r.toggle = *value ? 1 : 0;
    r.adjust = [value](int d) { *value = d > 0 ? 1 : 0; };
    r.activate = [value]() { *value = !*value; };
    rows.push_back(std::move(r));
}
void add_choice(std::vector<Row> &rows, const char *id, const char *name, int selected,
                const std::vector<std::string> &names, std::function<void(int)> set, const char *key = nullptr) {
    Row r;
    r.id = id;
    r.title = tr(name);
    r.value = names[std::clamp(selected, 0, (int)names.size() - 1)];
    keyed(r, key);
    r.adjust = [selected, names, set](int d) { set((selected + d + (int)names.size()) % (int)names.size()); };
    Row copy = r;
    r.menu = true;
    r.activate = [copy, names, selected, set]() { choices(copy, names, selected, set); };
    rows.push_back(std::move(r));
}
void add_number(std::vector<Row> &rows, const std::string &id, const std::string &name,
                std::function<double()> get, std::function<void(double)> set, double low, double high,
                double step, bool integral, const char *key = nullptr,
                std::function<bool(double, std::string &)> check = {}) {
    char text[64];
    snprintf(text, sizeof text, integral ? "%.0f" : "%.2f", get());
    Row r;
    r.id = id;
    r.title = name;
    r.value = text;
    r.number = true;
    keyed(r, key);
    r.adjust = [get, set, low, high, step, check](int d) {
        double v = std::clamp(get() + d * step, low, high);
        std::string error;
        for (int attempts = 0; attempts <= HP_QUESTS_MAX; ++attempts) {
            if (!check || check(v, error)) {
                set(v);
                break;
            }
            double next = std::clamp(v + d * step, low, high);
            if (next == v)
                break;
            v = next;
        }
    };
    rows.push_back(std::move(r));
}
void number(std::vector<Row> &r, const char *id, const char *name, int *p, int low, int high, int step = 1,
            const char *key = nullptr) {
    add_number(
        r, id, tr(name), [p]() { return (double)*p; }, [p](double v) { *p = (int)v; }, low, high, step, true,
        key);
}
void number(std::vector<Row> &r, const std::string &id, const std::string &name, float *p, float low,
            float high, float step, const char *key = nullptr) {
    add_number(
        r, id, name, [p]() { return (double)*p; }, [p](double v) { *p = (float)v; }, low, high, step, false,
        key);
}
// A section's title between the rows (not selectable)
void header(std::vector<Row> &rows, const std::string &id, const std::string &title) {
    Row r;
    r.id = id;
    r.title = title;
    r.header = true;
    r.enabled = false;
    rows.push_back(std::move(r));
}
void action(std::vector<Row> &rows, const std::string &id, const std::string &name, std::function<void()> fn,
            bool enabled = true) {
    Row r;
    r.id = id;
    r.title = name;
    r.action = true;
    r.activate = std::move(fn);
    r.enabled = enabled;
    rows.push_back(std::move(r));
}
// A text setting (a char array of the draft, size bytes) typed on the on-screen keyboard; shown: its row's value
void text(std::vector<Row> &rows, const char *id, const char *name, char *field, size_t size, const std::string &shown,
          const char *key) {
    size_t at = (size_t)(field - (char *)&s.draft);
    Row r;
    r.id = id;
    r.title = tr(name);
    r.value = shown;
    keyed(r, key);
    r.activate = [name, at, size]() {
        keyboard(tr(name), (char *)&s.draft + at, size - 1, [at, size](const std::string &typed, std::string &error) {
            Config temp = s.draft;
            SDL_strlcpy((char *)&temp + at, typed.c_str(), size);
            char why[512];
            if (!config_validate(&temp, why, sizeof why)) {
                error = settings_text(s.draft.language, why);
                return false;
            }
            s.draft = temp;
            return true;
        });
    };
    rows.push_back(std::move(r));
}
// The hub server and key being edited, checked on request (runtime/net/ldn.c); the value: the server's last known
// state, from the check or from the game's own searches and hubs
void hub_check(std::vector<Row> &rows) {
    int ms = -1, hubs = -1;
    std::string state;
    switch (ldn_hub_status(s.draft.ldn_server, s.draft.ldn_key, &ms, &hubs)) {
    case HUB_CHECKING: state = tr("Checking..."); break;
    case HUB_OFF: state = tr("Off"); break;
    case HUB_ONLINE:
        state = hubs >= 0 ? fmt("Online, %d ms, %d hub(s)", ms, hubs) : ms >= 0 ? fmt("Online, %d ms", ms) : tr("Online");
        break;
    case HUB_NOT_FOUND: state = tr("Not found"); break;
    case HUB_UNREACHABLE: state = tr("Unreachable"); break;
    case HUB_LOST: state = tr("Connection lost"); break;
    case HUB_NO_ANSWER: state = tr("No answer"); break;
    case HUB_BUSY: state = tr("Busy"); break;
    case HUB_VERSION: state = tr("Other version"); break;
    case HUB_REFUSED: state = tr("Refused"); break;
    default: state = tr("Not checked"); break;
    }
    Row r;
    r.id = "server.check";
    r.title = tr("Check hub server");
    r.help = tr("Asks the hub server above whether it answers, with the matchmaking key above: how long it takes, and how many hubs it lists.");
    r.value = state;
    r.activate = []() { ldn_hub_check(s.draft.ldn_server, s.draft.ldn_key); };
    rows.push_back(std::move(r));
}
// The keybinds' rows (config.h BIND_*): what each is called
const char *const bind_titles[BINDS] = {"Move forward", "Move back", "Move left", "Move right", "Walk", "A", "B", "X",
                                        "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "D-pad up", "D-pad down",
                                        "D-pad left", "D-pad right", "Left stick click", "Right stick click",
                                        "Item left", "Item right"};
// What else a key (or KEY_MOUSE + a mouse button) does: one of the recomp's own keys (keymap.h), a keybind other than
// skip, or (for a keybind's) the chat key; nullptr: nothing
const char *key_use(int code, int skip) {
    if (const char *use = code < KEY_MOUSE ? key_reserved(code) : nullptr)
        return tr(use);
    for (int i = 0; i < BINDS; i++)
        for (int k = 0; k < BIND_KEYS; k++)
            if (i != skip && s.draft.binds[i][k] == code)
                return tr(bind_titles[i]);
    if (skip >= 0 && code == (int)SDL_GetScancodeFromName(s.draft.chat_key))
        return tr("Chat key");
    return nullptr;
}
// Choosing a key: for a keybind (BIND_*, add: its second key) or the chat key (-1)
void capture(int bind, bool add) {
    s.capture_bind = bind;
    s.capture_add = add;
    captured_key = -1;
    push(View::KeyCapture, "capture.cancel");
}
// The key that opens a line of chat; red when something else already uses it
void chat_key_row(std::vector<Row> &rows) {
    Row r;
    r.id = "chat.key";
    r.title = tr("Chat key");
    keyed(r, ini("ChatKey"));
    SDL_Scancode key = SDL_GetScancodeFromName(s.draft.chat_key);
    r.value = key != SDL_SCANCODE_UNKNOWN ? s.draft.chat_key : tr("None");
    if (const char *use = key != SDL_SCANCODE_UNKNOWN ? key_use((int)key, -1) : nullptr) {
        r.value += std::string(" (") + tr("in use:") + " " + use + ")";
        r.warn = true;
    }
    r.activate = []() { capture(-1, false); };
    rows.push_back(std::move(r));
}
// The Keybinds submenu: a row per keybind (its keys; red where one does something else too), the chat key
void keybind_rows(std::vector<Row> &r) {
    for (int i = 0; i < BINDS; i++) {
        if (i == BIND_MOVE_UP)
            header(r, "bind.movement", tr("Movement"));
        if (i == BIND_A)
            header(r, "bind.buttons", tr("Buttons"));
        if (i == BIND_ITEM_LEFT)
            header(r, "bind.items", tr("Item bar"));
        Row row;
        row.id = "bind." + std::to_string(i);
        row.title = tr(bind_titles[i]);
        for (int k = 0; k < BIND_KEYS; k++) {
            int code = s.draft.binds[i][k];
            if (!code)
                continue;
            row.value += (row.value.empty() ? "" : ", ") + std::string(config_key_name(code));
            if (const char *use = key_use(code, i)) {
                row.value += std::string(" (") + tr("in use:") + " " + use + ")";
                row.warn = true;
            }
        }
        if (row.value.empty())
            row.value = tr("None");
        Row copy = row;
        row.menu = true;
        row.activate = [copy, i]() {
            bool one = s.draft.binds[i][0] && !s.draft.binds[i][1];
            std::vector<std::string> names = {tr("Set key")};
            if (one)
                names.push_back(tr("Add a second key"));
            names.push_back(tr("None"));
            choices(copy, names, -1, [i, one](int v) {
                if (v == (one ? 2 : 1))
                    s.draft.binds[i][0] = s.draft.binds[i][1] = 0;
                else
                    capture(i, v == 1);
            });
        };
        r.push_back(std::move(row));
    }
    header(r, "bind.other", tr("Chat"));
    chat_key_row(r);
    action(r, "bind.reset", tr("Reset keybinds"), []() {
        Config f = config_defaults();
        memcpy(s.draft.binds, f.binds, sizeof f.binds);
        SDL_strlcpy(s.draft.chat_key, f.chat_key, sizeof s.draft.chat_key);
    });
    action(r, "bind.back", tr("Back"), []() { pop(); });
}
void reset_section() {
    Config f = config_defaults(), &c = s.draft;
    switch (s.page) {
    case 0:
        c.display_mode = f.display_mode;
        c.width = f.width;
        c.height = f.height;
        c.render_w = f.render_w;
        c.render_h = f.render_h;
        c.render_scale = f.render_scale;
        c.aspect_ratio = f.aspect_ratio;
        c.fov = f.fov;
        c.ui_anchor = f.ui_anchor;
        c.fill_screen = f.fill_screen;
        break;
    case 1:
        c.low_end = f.low_end;
        c.fsr = f.fsr;
        c.fsr_upscaler = f.fsr_upscaler;
        c.fsr_quality = f.fsr_quality;
        c.fsr_sharpness = f.fsr_sharpness;
        c.fsr_objmotion = f.fsr_objmotion;
        c.fsr_reactive = f.fsr_reactive;
        c.fsr_animsurf = f.fsr_animsurf;
        c.fps = f.fps;
        c.show_stats = f.show_stats;
        c.vsync = f.vsync;
        c.shader_prewarm = f.shader_prewarm;
        c.draw_distance = f.draw_distance;
        c.smooth_gradients = f.smooth_gradients;
        c.msaa = f.msaa;
        c.fxaa = f.fxaa;
        c.fxaa_mode = f.fxaa_mode;
        c.volume = f.volume;
        c.mute_in_background = f.mute_in_background;
        break;
    case 2:
        c.face_buttons = f.face_buttons;
        c.menu_confirm = f.menu_confirm;
        c.button_icons = f.button_icons;
        c.background_input = f.background_input;
        c.combo_window = f.combo_window;
        c.mouse_sensitivity = f.mouse_sensitivity;
        c.touch_controls = f.touch_controls;
        c.touch_hud = f.touch_hud;
        c.touch_camera = f.touch_camera;
        c.touch_opacity = f.touch_opacity;
        c.touch_vibration = f.touch_vibration;
        c.phone_rumble = f.phone_rumble;
        c.lock_on = f.lock_on;
        memcpy(c.touch_show, f.touch_show, sizeof c.touch_show);
        memcpy(c.touch_combo, f.touch_combo, sizeof c.touch_combo);
        memcpy(c.touch_combo_pad, f.touch_combo_pad, sizeof c.touch_combo_pad);
        SDL_strlcpy(c.chat_key, f.chat_key, sizeof c.chat_key);
        memcpy(c.binds, f.binds, sizeof c.binds);
        break;
    case 3:
        c.ldn_port = f.ldn_port;
        SDL_strlcpy(c.ldn_join, f.ldn_join, sizeof c.ldn_join);
        SDL_strlcpy(c.ldn_server, f.ldn_server, sizeof c.ldn_server);
        SDL_strlcpy(c.ldn_key, f.ldn_key, sizeof c.ldn_key);
        c.sync_small_monsters = f.sync_small_monsters;
        c.village_in_hub = f.village_in_hub;
        break;
    case 4:
        c.hp = f.hp;
        quest_keys();
        break;
    case MODS_PAGE:
        for (auto &m : mods.draft)
            m.on = m.name[0] != '_';
        std::sort(mods.draft.begin(), mods.draft.end(),
                  [](const ModEntry &a, const ModEntry &b) { return strcasecmp(a.name, b.name) < 0; });
        break;
    case CHEATS_PAGE:
        for (int i = 0; i < (int)cheat_tab.draft.size(); ++i)
            cheat_tab.draft[i] = 1;
        break;
    case APP_PAGE:
        c.language = f.language;
        c.menu_theme = f.menu_theme;
        c.menu_scale = f.menu_scale;
        c.confirm_exit = f.confirm_exit;
        c.pause_background = f.pause_background;
        c.updates = f.updates;
        c.bonus_content = f.bonus_content;
        break;
    }
    pop();
}
// Suggested order: the mods that replace each other's files keep their places among the mods that are on, with the
// ones that change the most files first and the small tweaks last, so a tweak wins over an overhaul.
std::vector<ModEntry> suggested_mods() {
    std::vector<ModEntry> order = mods.draft;
    auto result = mods.scan;
    if (!result || !result->done)
        return order;
    std::vector<int> slots;
    std::vector<std::string> involved;
    for (int i = 0; i < (int)order.size(); ++i) {
        if (!order[i].on)
            continue;
        for (int k = 0; k < result->scan.nconflicts; ++k) {
            const ModConflict &c = result->scan.conflicts[k];
            if (!c.replaced)
                continue;
            const std::string &a = result->names[c.a], &b = result->names[c.b];
            int ia = mod_index(a), ib = mod_index(b);
            if (ia >= 0 && ib >= 0 && mods.draft[ia].on && mods.draft[ib].on && (order[i].name == a || order[i].name == b)) {
                slots.push_back(i);
                involved.push_back(order[i].name);
                break;
            }
        }
    }
    auto files = [&](const std::string &name) {
        auto at = std::find(result->names.begin(), result->names.end(), name);
        return at == result->names.end() ? 0 : result->scan.files[at - result->names.begin()];
    };
    std::stable_sort(involved.begin(), involved.end(),
                     [&](const std::string &a, const std::string &b) { return files(a) > files(b); });
    for (size_t k = 0; k < slots.size(); ++k)
        snprintf(order[slots[k]].name, sizeof order[slots[k]].name, "%s", involved[k].c_str());
    return order;
}
// One row per pair of mods that are on and change the same file or archive entry; selecting it picks the one used
void conflict_rows(std::vector<Row> &r) {
    auto result = mods.scan;
    if (!result || !result->done)
        return;
    for (int k = 0; k < result->scan.nconflicts; ++k) {
        const ModConflict &c = result->scan.conflicts[k];
        if (!c.replaced)  // (archives whose changes are combined are not a problem)
            continue;
        int ia = mod_index(result->names[c.a]), ib = mod_index(result->names[c.b]);
        if (ia < 0 || ib < 0 || !mods.draft[ia].on || !mods.draft[ib].on)
            continue;
        if (ia > ib)
            std::swap(ia, ib);
        const char *first = mods.draft[ia].name, *second = mods.draft[ib].name;
        std::string text = fmt("%s and %s both change %d file(s); %s is used", first, second, c.replaced, second);
        std::string a = first, b = second;
        action(r, "mods.conflict." + std::to_string(k), text, []() {});
        {  // select it to choose which of the two is used: that one moves below the other
            Row &row = r.back();
            Row copy = row;
            copy.title = tr("Which mod is used?");
            row.menu = true;
            row.activate = [copy, a, b]() {
                choices(copy, {fmt("Use %s", a.c_str()), fmt("Use %s", b.c_str())}, 1, [a, b](int v) {
                    int ia = mod_index(a), ib = mod_index(b), win = v ? ib : ia, lose = v ? ia : ib;
                    if (ia < 0 || ib < 0 || win > lose)
                        return;
                    ModEntry moved = mods.draft[win];
                    mods.draft.erase(mods.draft.begin() + win);
                    mods.draft.insert(mods.draft.begin() + lose, moved);  // (the loser is now one place earlier)
                    s.saved_notice = false;
                });
            };
        }
    }
}
void cheat_rows(std::vector<Row> &r) {
    if (cheat_tab.draft.empty())
        action(r, "cheats.none", tr("No cheats found in the cheats folder"), []() {});
    for (int i = 0; i < (int)cheat_tab.draft.size(); ++i) {
        Row row;
        row.id = "cheat." + std::to_string(i);
        row.title = cheats_name(i);
        row.help = cheats_file(i);
        if (cheats_master(i)) {
            row.value = tr("Always on");
            row.enabled = false;
        } else {
            row.value = tr(cheat_tab.draft[i] ? "On" : "Off");
            row.toggle = cheat_tab.draft[i] ? 1 : 0;
            row.adjust = [i](int d) {
                cheat_tab.draft[i] = d > 0;
                s.saved_notice = false;
            };
            row.activate = [i]() {
                cheat_tab.draft[i] = !cheat_tab.draft[i];
                s.saved_notice = false;
            };
        }
        r.push_back(std::move(row));
    }
}
void mods_rows(std::vector<Row> &r) {
    if (!mods.scan)
        start_scan();
    auto result = mods.scan;  // (the scan thread fills it; `done` says when)
    if (mods.draft.empty())
        action(r, "mods.none", tr("No mods found in the mods folder"), []() {});
    for (int i = 0; i < (int)mods.draft.size(); ++i) {
        std::string name = mods.draft[i].name;
        bool on = mods.draft[i].on, clash = false;
        if (on && result->done)
            for (int k = 0; k < result->scan.nconflicts && !clash; ++k) {
                const ModConflict &c = result->scan.conflicts[k];
                if (!c.replaced || (result->names[c.a] != name && result->names[c.b] != name))
                    continue;
                int other = mod_index(result->names[result->names[c.a] == name ? c.b : c.a]);
                clash = other >= 0 && mods.draft[other].on;
            }
        Row row;
        row.id = "mod." + name;
        row.title = name;
        row.value = std::string(tr(on ? "On" : "Off")) + (clash ? " !" : "");
        row.cold = true;
        auto set_on = [name](bool v) {
            int at = mod_index(name);
            if (at >= 0 && mods.draft[at].on != v) {
                mods.draft[at].on = v;
                s.saved_notice = false;
            }
        };
        row.adjust = [set_on](int d) { set_on(d > 0); };
        Row copy = row;
        row.menu = true;
        row.activate = [copy, name, on, set_on]() {
            auto move = [name](int d) {
                int at = mod_index(name), to = at + d;
                if (at >= 0 && to >= 0 && to < (int)mods.draft.size()) {
                    std::swap(mods.draft[at], mods.draft[to]);
                    s.saved_notice = false;
                }
            };
            choices(copy, {tr(on ? "Turn off" : "Turn on"), tr("Move up"), tr("Move down")}, -1,
                    [set_on, move, on](int v) {
                        if (v == 0)
                            set_on(!on);
                        else
                            move(v == 1 ? -1 : 1);
                    });
        };
        r.push_back(std::move(row));
    }
    if (!result->done) {
        action(r, "mods.scanning", tr("Checking for conflicts..."), []() {});
    } else {
        std::vector<Row> found;
        conflict_rows(found);
        if (found.empty())
            action(r, "mods.clear", tr("No conflicts found"), []() {});
        else {
            action(r, "mods.conflicts", fmt("Mods change the same files (%d): select to review", (int)found.size()),
                   []() { push(View::Conflicts, ""); });
            r.back().value = "!";
        }
    }
    auto order = suggested_mods();
    action(
        r, "mods.suggest", tr("Apply suggested order"),
        [order]() {
            mods.draft = order;
            s.saved_notice = false;
        },
        !same_mods(order, mods.draft));
    action(r, "mods.rescan", tr("Rescan mods"), []() {
        std::vector<ModEntry> now(MODS_MAX);
        int n = mods_installed(now.data(), MODS_MAX);
        // new folders join the end (on), gone ones leave; the order and the switches of the rest stay
        std::vector<ModEntry> next;
        for (const auto &m : mods.draft)
            for (int i = 0; i < n; ++i)
                if (!strcmp(now[i].name, m.name)) {
                    next.push_back(m);
                    break;
                }
        mods.draft = next;
        for (int i = 0; i < n; ++i)
            if (mod_index(now[i].name) < 0) {
                now[i].on = true;  // (as at start: a mod added later loads)
                mods.draft.push_back(now[i]);
            }
        mods.scan.reset();
    });
}
std::vector<Row> main_rows() {
    std::vector<Row> r;
    Config &c = s.draft;
    auto low_end_off = [&r]() {  // (none of these with Low-end mode: config.c)
        if (!c.low_end) return;
        r.back().enabled = false;
        r.back().value = tr("Off (low-end)");
    };
    switch (s.page) {
    case 0:
#ifndef __ANDROID__  // (a phone: the window is the screen, at its own resolution; the 3D in percent of it below)
        add_choice(
            r, "display.mode", "Window mode", c.display_mode,
            {tr("Windowed"), tr("Fullscreen"), tr("Borderless")}, [](int v) { c.display_mode = v; }, ini("Mode"));
        add_choice(
            r, "display.auto", "Screen resolution", !c.width ? 0 : 1, {tr("Desktop"), tr("Custom")},
            [](int v) {
                s.draft.width = v ? 1920 : 0;
                s.draft.height = v ? 1080 : 0;
            },
            ini("Resolution"));
        if (c.width) {
            number(r, "display.width", "Width", &c.width, 320, 16384, 80, ini("Resolution"));
            number(r, "display.height", "Height", &c.height, 180, 16384, 45, ini("Resolution"));
        }
        add_choice(
            r, "render.auto", "Render resolution", !c.render_w ? 0 : 1, {tr("Auto"), tr("Custom")},
            [](int v) {
                s.draft.render_w = v ? 1920 : 0;
                s.draft.render_h = v ? 1080 : 0;
            },
            ini("RenderResolution"));
        if (c.render_w) {
            number(r, "render.width", "Width", &c.render_w, 320, 16384, 80, ini("RenderResolution"));
            number(r, "render.height", "Height", &c.render_h, 180, 16384, 45, ini("RenderResolution"));
        } else
#endif
        if ((!c.fsr || c.low_end) && !c.render_w) {  // (Auto: the 3D picture in percent of the screen's, RenderScale; the nearest of the presets;
                              // with FSR its quality sets the 3D size)
            static const int scales[] = {100, 90, 80, 75, 66, 50, 40, 33};
            int pick = 0;
            for (int i = 1; i < (int)std::size(scales); i++)
                if (std::abs(scales[i] - c.render_scale) < std::abs(scales[pick] - c.render_scale))
                    pick = i;
            add_choice(
                r, "render.scale", "3D resolution", pick, {"100%", "90%", "80%", "75%", "66%", "50%", "40%", "33%"},
                [](int v) { s.draft.render_scale = scales[v]; }, ini("RenderScale"));
            if (scales[pick] != c.render_scale)  // (another value set in the file: shown as it is)
                r.back().value = std::to_string(c.render_scale) + "%";
            r.back().help = tr("The size of the 3D picture, in percent of the screen's. Lower is faster and softer.");
        }
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_choice(
            r, "aspect.auto", "Automatic aspect ratio", c.aspect_ratio == 0 ? 0 : 1,
            {tr("Auto"), tr("Custom")}, [](int v) { s.draft.aspect_ratio = v ? 16.f / 9 : 0; }, ini("AspectRatio"));
        if (c.aspect_ratio)
            number(r, "aspect", tr("Aspect ratio"), &c.aspect_ratio, lo("AspectRatio"), hi("AspectRatio"), .05f, ini("AspectRatio"));
#endif
        number(r, "fov", tr("Field of view"), &c.fov, lo("FOV"), hi("FOV"), .05f, ini("FOV"));
        add_choice(
            r, "anchor", "HUD anchor", c.ui_anchor - 1, {"16:9", "21:9", "32:9", tr("Edges")},
            [](int v) { c.ui_anchor = v + 1; }, ini("UIAnchor"));
        add_bool(r, "fill", "Fill the screen", &c.fill_screen, ini("FillScreen"));
        break;
    case 1:
        add_choice(
            r, "lowend", "Low-end mode", c.low_end ? 1 : 0, {tr("Off"), tr("On")},
            [](int v) {  // (it brings the resolution and the frame rate along; both can then be changed)
                s.draft.low_end = v;
                if (v) {
                    s.draft.render_scale = std::min(s.draft.render_scale, LOW_END_SCALE);
                    s.draft.fps = std::min(s.draft.fps, LOW_END_FPS);
                } else {
                    if (s.draft.render_scale <= LOW_END_SCALE) s.draft.render_scale = 100;
                    if (s.draft.fps == LOW_END_FPS) s.draft.fps = 60;
                }
            },
            ini("LowEnd"));
        r.back().help = tr("For weak phones: turns off bloom and depth of field, uses lighter textures and keeps the HUD sharp over a smaller 3D picture. It also sets the 3D resolution and the frame rate, and you can change both.");
        number(r, "fps", "Frame rate limit", &c.fps, (int)lo("FPS"), (int)hi("FPS"), 1, ini("FPS"));
        {  // left / right: one frame per second more or less; Confirm: the common ones
            static const int presets[] = {30, 60, 90, 120, 150, 240};
            r.back().menu = true;
            Row copy = r.back();
            r.back().activate = [copy]() {
                std::vector<std::string> names;
                int now = -1;
                for (int i = 0; i < (int)std::size(presets); i++) {
                    names.push_back(std::to_string(presets[i]));
                    if (presets[i] <= s.draft.fps)
                        now = presets[i] == s.draft.fps ? i : -1 - i;  // (not one of them: start at the one below)
                }
                choices(copy, names, now, [](int v) { s.draft.fps = presets[v]; });
                if (now < -1)
                    focus("choice." + std::to_string(-1 - now));
            };
        }
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_bool(r, "vsync", "VSync", &c.vsync, ini("VSync"));
#endif
        add_bool(r, "stats", "Show FPS and temperature", &c.show_stats, ini("ShowStats"));
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_bool(r, "prewarm", "Shader prewarm", &c.shader_prewarm, ini("ShaderPrewarm"));
#endif
        add_choice(r, "msaa", "MSAA",
                   c.msaa == 8   ? 3
                   : c.msaa == 4 ? 2
                   : c.msaa == 2 ? 1
                                 : 0,
                   {tr("Off"), "2x", "4x", "8x"}, [](int v) { c.msaa = 1 << v; }, ini("MSAA"));
        low_end_off();
        if (g_config.fsr) {
            r.back().cold = true;
            if (c.fsr) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
        add_bool(r, "fxaa", "FXAA", &c.fxaa, ini("FXAA"));
        low_end_off();
        if (g_config.fsr) {
            r.back().cold = true;
            if (c.fsr) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_choice(r, "fxaamode", "FXAA mode", c.fxaa_mode, {tr("Original"), "3.11"}, [](int v) { c.fxaa_mode = v; },
                   ini("FXAAMode"));
        low_end_off();
        if (g_config.fsr) {
            r.back().cold = true;
            if (c.fsr) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
#endif
        number(r, "drawdist", tr("Draw distance"), &c.draw_distance, lo("DrawDistance"), hi("DrawDistance"), .25f,
               ini("DrawDistance"));
        if (c.draw_distance == 0)
            r.back().value = tr("No limit");
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_bool(r, "smooth", "Smooth gradients", &c.smooth_gradients, ini("SmoothGradients"));
#endif
        number(r, "volume", "Volume (%)", &c.volume, (int)lo("Volume"), (int)hi("Volume"), 1, ini("Volume"));
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_bool(r, "mutebg", "Mute in background", &c.mute_in_background, ini("MuteInBackground"));
#endif
        add_bool(r, "fsr", "FSR", &c.fsr, ini("FSR"));
        low_end_off();
        if (c.fsr && !c.low_end) {
#ifndef __ANDROID__  // (FSR 4: AMD's RX 9000 cards only)
            add_choice(
                r, "fsr.provider", "Upscaler", c.fsr_upscaler, {"FSR 3.1", "FSR 4"},
                [](int v) { s.draft.fsr_upscaler = v; }, ini("Upscaler"));
#endif
            if (g_fsr4_fallback && c.fsr_upscaler == FSR_UP_4 && g_config.fsr && g_config.fsr_upscaler == FSR_UP_4) {
                r.back().value += std::string(" (") + tr("FSR 3.1 in use") + ")";  // (fsr4.c: FSR 4 didn't start)
                r.back().warn = true;
                r.back().help = tr(g_fsr4_fallback);
            }
            add_choice(
                r, "fsr.quality", "FSR quality", c.fsr_quality,
                {tr("Native AA"), tr("Quality"), tr("Balanced"), tr("Performance"), tr("Ultra performance")},
                [](int v) { s.draft.fsr_quality = v; }, ini("FSRQuality"));
            number(r, "fsr.sharpness", tr("Sharpness"), &c.fsr_sharpness, lo("Sharpness"), hi("Sharpness"), .05f, ini("Sharpness"));
#ifndef __ANDROID__  // (a phone: not worth a row)
            add_bool(r, "fsr.motion", "Object motion", &c.fsr_objmotion, ini("ObjectMotion"));
#endif
#ifndef __ANDROID__  // (a phone: not worth a row)
            number(r, "fsr.reactive", tr("Reactive mask"), &c.fsr_reactive, lo("Reactive"), hi("Reactive"), .05f, ini("Reactive"));
#endif
#ifndef __ANDROID__  // (a phone: not worth a row)
            add_bool(r, "fsr.surfaces", "Animated surfaces", &c.fsr_animsurf, ini("AnimSurf"));
#endif
        }
#ifdef __ANDROID__
        if (vk_gpu_adreno()) {
            action(r, "gpu.driver", tr("GPU driver"), []() { ui_request_gpu_driver(); });
            r.back().value = *gpu_driver_name() ? gpu_driver_name() : tr("System");
            r.back().help = tr("Adreno only, at your own risk: load a Vulkan driver of your own. By default the system's is used.");
        }
#endif
        break;
    case 2:
        add_choice(r, "face", "Face buttons", c.face_buttons, {tr("Position"), tr("Label")},
                   [](int v) { c.face_buttons = v; }, ini("FaceButtons"));
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_choice(r, "menu.confirm", "Confirm button (recomp)", c.menu_confirm,
                   {tr("Original"), tr("Bottom button"), tr("Right button")},
                   [](int v) { s.draft.menu_confirm = v; }, ini("MenuConfirm"));
        add_choice(r, "icons", "Button icons", c.button_icons, {"Xbox", "Switch", "PlayStation"},
                   [](int v) { c.button_icons = v; }, ini("ButtonIcons"));
#endif
#ifndef __ANDROID__  // (a phone pauses in the background; no mouse, no keyboard)
        add_bool(r, "background", "Background input", &c.background_input, ini("BackgroundInput"));
#endif
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_choice(r, "combo", "Combo input window", c.combo_window, {tr("Lenient"), tr("Strict")},
                   [](int v) { c.combo_window = v; }, ini("ComboInputWindow"));
#endif
#ifndef __ANDROID__
        number(r, "mouse", tr("Mouse sensitivity"), &c.mouse_sensitivity, lo("MouseSensitivity"), hi("MouseSensitivity"), .05f, ini("MouseSensitivity"));
#endif
        add_choice(r, "touch", "Touch controls", c.touch_controls, {tr("Auto"), tr("On"), tr("Off")},
                   [](int v) { c.touch_controls = v; }, ini("Controls"));
        add_bool(r, "touch.hud", "Move the HUD for touch", &c.touch_hud, ini("MoveHUD"));
        add_bool(r, "lock.on", "Lock-on", &c.lock_on, ini("LockOn"));
        r.back().help = tr("The target icon, or a controller's right stick click, keeps the camera on the monster as in the newer games, and bows and bowguns fire at it. Off: the icon turns the camera to it once, as in the original game.");
        action(r, "touch.buttons", tr("Buttons on the screen"), []() { push(View::TouchButtons, "tb.0"); });
        r.back().help = tr("Choose which of the game's buttons show on the screen, and add buttons of your own that press several at once.");
        number(r, "touch.camera", tr("Touch camera speed"), &c.touch_camera, lo("CameraSpeed"), hi("CameraSpeed"), .05f, ini("CameraSpeed"));
        number(r, "touch.opacity", tr("Touch controls opacity"), &c.touch_opacity, lo("Opacity"), hi("Opacity"), .05f, ini("Opacity"));
        add_bool(r, "touch.vibration", "Vibrate on touch", &c.touch_vibration, ini("Vibration"));
        add_bool(r, "phone.rumble", "Game rumble on the phone", &c.phone_rumble, ini("PhoneRumble"));
        {  // (the layout editor is worked by fingers: offered where there is a touch screen)
            bool touch_edit = true;
#ifndef __ANDROID__
            int ntouch = 0;
            SDL_free(SDL_GetTouchDevices(&ntouch));
            touch_edit = ntouch > 0;
#endif
            if (touch_edit) {
                action(r, "touch.edit", tr("Customize touch controls"), []() {
                    touch_edit_begin();  // (the editor starts once the menu is closed: one back, or the unsaved-changes dialog)
                    request_back.store(true);
                });
                r.back().help = tr("Move and resize each button on the screen: drag a button, pinch or use - / + for its size, then Save.");
            }
        }
        action(r, "pad.map", tr("Map a controller"), []() { push(View::PadList, "pad.0"); });
        r.back().help = tr("For a controller the game does not read well: press each button once and it works from then on.");
#ifndef __ANDROID__
        action(r, "keybinds", tr("Keybinds"), []() { push(View::Keybinds, "bind.0"); });
        r.back().help = tr("The keys and mouse buttons for the Switch controller's buttons, for moving and for the item bar, and the chat key.");
#endif
        break;
    case 3:
        number(r, "port", "Host port (TCP / UDP)", &c.ldn_port, (int)lo("Port"), (int)hi("Port"), 1, ini("Port"));
        text(r, "hosts", "Search hosts", c.ldn_join, sizeof c.ldn_join, *c.ldn_join ? c.ldn_join : tr("LAN"), ini("Join"));
        text(r, "server", "Hub server", c.ldn_server, sizeof c.ldn_server,
             !*c.ldn_server ? std::string(tr("Default")) + (*LDN_DEFAULT_SERVER ? " (" LDN_DEFAULT_SERVER ")" : "")
                            : c.ldn_server,
             ini("Server"));
        text(r, "key", "Matchmaking key", c.ldn_key, sizeof c.ldn_key, *c.ldn_key ? c.ldn_key : tr("Public"), ini("Key"));
        hub_check(r);
        add_bool(r, "sync", "Sync small monsters", &c.sync_small_monsters, ini("SyncSmallMonsters"));
        add_bool(r, "village", "Village quests in Hub", &c.village_in_hub, ini("VillageQuestsInHub"));
#ifndef __ANDROID__  // (Discord's Rich Presence is its desktop app's)
        header(r, "discord.header", "Discord");
        add_bool(r, "discord", "Discord Rich Presence", &c.discord, ini("RichPresence"));
        add_bool(r, "discord.hunter", "Show hunter name and HR", &c.discord_hunter, ini("ShowHunter"));
#endif
        break;
    case 4: {
        const char *per_quest =
            tr("HP multipliers for single quests, by their quest ID: in those quests they are used instead of the ones above.");
        header(r, "hp.header", tr("Monster HP Scaling"));
        for (int i = 0; i < 4; i++)
            number(r, "hp." + std::to_string(i), std::to_string(i + 1) + " " + tr(i == 0 ? "Hunter" : "Hunters"), &c.hp.scale[i],
                   lo("1Player"), hi("1Player"), .05f, ini("1Player"));
        number(r, "hp.village", tr("Village quests in Hub"), &c.hp.village, lo("HubHP"), hi("HubHP"), .05f,
               ini("HubHP"));
#ifndef __ANDROID__  // (a phone: not worth a row)
        action(r, "hp.advanced", tr("Per-quest overrides"), []() { s.advanced = !s.advanced; });
        r.back().help = per_quest;
        if (s.advanced) {
            for (int i = 0; i < c.hp.quests; i++) {
                auto id = quest_id(i);
                add_number(
                    r, id, tr("Quest ID"), [i]() { return (double)s.draft.hp.quest[i].quest; },
                    [i](double v) { s.draft.hp.quest[i].quest = (uint32_t)v; }, 1, 4294967295., 1, true, ini("1Player"),
                    [i](double v, std::string &e) {
                        for (int j = 0; j < s.draft.hp.quests; j++)
                            if (j != i && s.draft.hp.quest[j].quest == (uint32_t)v) {
                                e = tr("Duplicate quest ID.");
                                return false;
                            }
                        return true;
                    });
                for (int n = 0; n < 4; n++)
                    number(r, id + "." + std::to_string(n), std::to_string(n + 1) + " " + tr(n == 0 ? "Hunter" : "Hunters"),
                           &c.hp.quest[i].scale[n], HP_SCALE_MIN, HP_SCALE_MAX, .05f, ini("1Player"));
                action(r, id + ".remove", tr("Remove quest"), [i]() {
                    for (int j = i; j < s.draft.hp.quests - 1; j++) {
                        s.draft.hp.quest[j] = s.draft.hp.quest[j + 1];
                        s.quest_keys[j] = s.quest_keys[j + 1];
                    }
                    --s.draft.hp.quests;
                    focus(i < s.draft.hp.quests ? quest_id(i) : "hp.add");
                });
            }
            action(
                r, "hp.add", tr("Add quest"),
                []() {
                    auto &hp = s.draft.hp;
                    uint32_t q = 1;
                    bool exists;
                    do {
                        exists = false;
                        for (int i = 0; i < hp.quests; i++)
                            if (hp.quest[i].quest == q) {
                                exists = true;
                                ++q;
                                break;
                            }
                    } while (exists);
                    int i = hp.quests++;
                    hp.quest[i].quest = q;
                    for (float &v : hp.quest[i].scale)
                        v = 1;
                    s.quest_keys[i] = s.next_quest_key++;
                    focus(quest_id(i));
                },
                c.hp.quests < HP_QUESTS_MAX);
        }
        for (auto &row : r)
            if (!row.id.compare(0, 6, "quest.") || row.id == "hp.add")
                row.help = per_quest;
#endif
        break;
    }
    case MODS_PAGE:
        mods_rows(r);
        break;
    case CHEATS_PAGE:
        cheat_rows(r);
        break;
    case APP_PAGE:
        add_choice(r, "language", "Language", c.language, {"English", "Español"},
                   [](int v) { c.language = v; }, ini("Language"));
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_choice(r, "theme", "Menu theme", c.menu_theme, {tr("Gold"), tr("Blue"), tr("Hunter")},
                   [](int v) { c.menu_theme = v; }, ini("Theme"));
#endif
        number(r, "scale", "Interface scale (%)", &c.menu_scale, (int)lo("MenuScale"), (int)hi("MenuScale"), 10, ini("MenuScale"));
        add_bool(r, "pause.bg", "Pause in the background", &c.pause_background, ini("PauseInBackground"));
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_bool(r, "confirm.exit", "Confirm exit", &c.confirm_exit, ini("ConfirmExit"));
#endif
#ifndef __ANDROID__  // (the PC build's updater)
        add_choice(r, "updates", "Updates", c.updates, {tr("Ask"), tr("Auto"), tr("Off")},
                   [](int v) { c.updates = v; }, ini("Updates"));
#endif
        add_bool(r, "bonus", "Bonus content", &c.bonus_content, ini("BonusContent"));
#ifdef __ANDROID__
        action(r, "game.folder", tr("Game folder"), []() { ui_request_game_folder(); });
        r.back().help = tr("Where the game's exefs and romfs are read from. A new folder is used the next time the game starts.");
        action(r, "save.import", tr("Import save"), []() { ui_request_save_import(); });
        r.back().help = tr("Brings your save from an emulator or another device. It replaces the characters on this device, and the current save is kept in a copy.");
        action(r, "save.export", tr("Export save"), []() { ui_request_save_export(); });
        r.back().help = tr("Copies your save to a folder you choose, to take it to an emulator or another device.");
        action(r, "log.share", tr("Share log"), []() { ui_request_share_log(); });
        r.back().help = tr("Sends the game's log to the app you choose, for whoever is helping you with a problem.");
        action(r, "licenses", tr("Licenses"), []() { ui_request_licenses(); });
#endif
        if (!touch_ui()) {  // (the phone's menu fills the screen: nothing to move)
            action(r, "detach", tr(s.floating ? "Attach menu" : "Detach menu"), []() {
                s.floating = !s.floating;
                if (!s.floating)
                    s.custom_size = ImVec2(0, 0);
            });
            r.back().help =
                tr(s.floating
                       ? "Puts the menu back at the top of the screen, at its usual size."
                       : "Lets the menu go anywhere on the screen: use Move menu, or drag its tabs with the mouse. Its bottom right corner sizes it.");
        }
        if (s.floating && !touch_ui())
            action(r, "move", tr("Move menu"), []() {
                auto *w = ImGui::GetCurrentWindow();
                s.move_start = s.move_position = w->Pos;
                push(View::Move, "move.accept");
            });
        action(r, "restart", tr("Restart"), []() { push(View::Restart, "dialog.cancel"); });
        action(r, "exit", tr("Exit game"), []() { push(View::Exit, "dialog.cancel"); });
        break;
    }
    if (!touch_ui() || SDL_HasGamepad())  // (a finger has the header's Save; a controller can't reach that one)
        action(r, "save", tr("Save"), []() { s.command = Command::Save; }, dirty());
    action(r, "reset", tr("Reset this tab"), []() { push(View::Reset, "dialog.cancel"); });
    return r;
}
bool save() {
    if (pending_text()) {
        std::string error;
        if (!s.keyboard.commit(s.keyboard.buffer, error)) {
            while (s.view != View::Keyboard && !s.origins.empty())
                pop();
            s.error = error;
            return false;
        }
        s.keyboard.original = s.keyboard.buffer;
    }
    char error[512];
    if (!config_save(&s.draft, error, sizeof error)) {
        s.error = settings_text(s.draft.language, error);
        return false;
    }
    if (mods_dirty()) {
        if (!mods_save_order(mods.draft.data(), (int)mods.draft.size(), error, sizeof error)) {
            s.error = settings_text(s.draft.language, error);
            return false;
        }
        mods.saved = mods.draft;
    }
    if (cheats_dirty()) {
        std::unique_ptr<bool[]> on(new bool[cheat_tab.draft.size() + 1]);
        for (size_t i = 0; i < cheat_tab.draft.size(); ++i)
            on[i] = cheat_tab.draft[i] != 0;
        if (!cheats_set(on.get(), (int)cheat_tab.draft.size(), error, sizeof error)) {
            s.error = settings_text(s.draft.language, error);
            return false;
        }
        cheat_tab.saved = cheat_tab.draft;
    }
    s.saved = s.draft;
    s.saved_notice = true;
    s.error.clear();
    return true;
}
// Which of the game's buttons are on the screen, and the player's own
const char *const TB_TITLES[TB_COUNT] = {"Attacks", "Dodge (B)", "Guard (R)", "Sheathe (Y)", "Game menu (+)", "Minus (-)", "D-pad",
                                         "L", "ZL", "ZR", "Left stick click (L3)", "Right stick click (R3)", "Movement stick",
                                         "Layout for bows and bowguns", "Fire / shoot", "Reload / melee", "Aim",
                                         "Crouching fire / coating", "Ammo arrows", "Item arrows"};
const char *const TB_KEYS[TB_COUNT] = {"ShowAttacks", "ShowDodge", "ShowGuard", "ShowSheathe", "ShowPlus", "ShowMinus", "ShowDpad",
                                       "ShowL", "ShowZL", "ShowZR", "ShowL3", "ShowR3", "ShowStick", "ShowRanged", "ShowFire",
                                       "ShowSecond", "ShowAim", "ShowRangedSpecial", "ShowAmmo", "ShowItemArrows"};
int s_combo;  // the combined button being edited
void touch_button_rows(std::vector<Row> &r) {
    Config &c = s.draft;
    header(r, "tb.head", tr("Buttons on the screen"));
    for (int i = 0; i < TB_COUNT; i++) {
        add_bool(r, ("tb." + std::to_string(i)).c_str(), TB_TITLES[i], &c.touch_show[i], nullptr);
        r.back().key = TB_KEYS[i];
    }
    header(r, "tc.head", tr("Combined buttons"));
    for (int k = 0; k < TOUCH_COMBOS; k++) {
        action(r, "tc." + std::to_string(k), fmt("Button %d", k + 1), [k]() {
            s_combo = k;
            push(View::ComboEdit, "ce.0");
        });
        r.back().value = c.touch_combo[k][0] ? c.touch_combo[k] : tr("Empty");
    }
    r.back().help = tr("A button of your own presses several of the game's together. Place it with Customize touch controls.");
    action(r, "tb.back", tr("Back"), []() { pop(); });
}
void combo_rows(std::vector<Row> &r) {
    static const char *const NAMES[16] = {"A", "B", "X", "Y", "L3", "R3", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"};
    static const int ORDER[16] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 13, 15, 12, 14, 4, 5};  // (the face buttons first)
    Config &c = s.draft;
    int k = s_combo;
    uint32_t mask = config_combo_mask(c.touch_combo[k]);
    for (int n = 0; n < 16; n++) {
        int b = ORDER[n];
        Row row;
        row.id = "ce." + std::to_string(n);
        row.title = tr(NAMES[b]);
        row.toggle = mask >> b & 1;
        row.activate = [k, b, mask]() { config_combo_text(mask ^ 1u << b, s.draft.touch_combo[k], sizeof s.draft.touch_combo[k]); };
        row.adjust = [k, b, mask](int d) {
            config_combo_text(d > 0 ? mask | 1u << b : mask & ~(1u << b), s.draft.touch_combo[k], sizeof s.draft.touch_combo[k]);
        };
        r.push_back(std::move(row));
    }
    add_choice(r, "ce.pad", "Controller button", c.touch_combo_pad[k],
               {tr("None"), "L3", "R3", tr("Minus"), "Guide", "Misc", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"},
               [k](int v) { s.draft.touch_combo_pad[k] = v; });
    r.back().help = tr("That button of a controller does the same, and no longer does what it did.");
    action(r, "ce.clear", tr("Clear"), [k]() {
        s.draft.touch_combo[k][0] = 0;
        s.draft.touch_combo_pad[k] = 0;
    });
    action(r, "ce.back", tr("Back"), []() { pop(); });
}
// Mapping a controller (runtime/padmap.c): the list of those connected, then its steps
void pad_list_rows(std::vector<Row> &r) {
    padmap_watch();
    char names[PADMAP_MAX][64];
    bool custom[PADMAP_MAX];
    int n = padmap_list(names, custom, PADMAP_MAX);
    if (!n) header(r, "pad.none", tr("No controller connected"));
    for (int i = 0; i < n; i++) {
        action(r, "pad." + std::to_string(i), names[i], [i]() {
            padmap_begin(i);
            push(View::PadMap, "pm.go");
        });
        if (custom[i]) {
            r.back().value = tr("Mapped");
            action(r, "padf." + std::to_string(i), fmt("Forget the map of %s", names[i]), [i]() { padmap_forget(i); });
            r.back().help = tr("The next time the game starts it reads this controller as before.");
        }
    }
    action(r, "pad.back", tr("Back"), []() { pop(); });
}
void pad_map_rows(std::vector<Row> &r) {
    padmap_watch();
    int step = padmap_step(), steps = padmap_steps();
    if (!padmap_active()) {  // (saved, or it was unplugged)
        pop();
        return;
    }
    if (step == PADMAP_REST) {
        header(r, "pm.rest", tr("Move both sticks and press the triggers once, then let everything go"));
        action(r, "pm.go", tr("Continue"), []() { padmap_continue(); });
    } else if (step < steps) {
        header(r, "pm.step", fmt("%d of %d: press", step + 1, steps) + std::string(" ") + tr(padmap_step_title(step)));
        action(r, "pm.go", tr("Skip"), []() { padmap_skip(); });
        r.back().help = tr("Leave this one out: the buttons your controller does not have.");
        if (step > 0) action(r, "pm.back", tr("Previous"), []() { padmap_back(); });
    } else {
        header(r, "pm.done", tr("That's all of them"));
        action(r, "pm.go", tr("Save"), []() {
            padmap_save();
            pop();
        });
        action(r, "pm.back", tr("Previous"), []() { padmap_back(); });
    }
    action(r, "pm.cancel", tr("Cancel"), []() {
        padmap_cancel();
        pop();
    });
}

std::vector<Row> rows_for_view(bool *open) {
    if (s.view == View::Main)
        return main_rows();
    std::vector<Row> r;
    if (s.view == View::TouchButtons) { touch_button_rows(r); return r; }
    if (s.view == View::ComboEdit) { combo_rows(r); return r; }
    if (s.view == View::PadList) { pad_list_rows(r); return r; }
    if (s.view == View::PadMap) { pad_map_rows(r); return r; }
    if (s.view == View::Keyboard) {
        std::string chars = "1234567890abcdefghijklmnopqrstuvwxyz.:,-_/@";
        if (s.keyboard.upper)
            for (char &c : chars)
                c = (char)SDL_toupper(c);
        for (int i = 0; i < (int)chars.size(); i++) {
            std::string ch(1, chars[i]);
            action(r, "key." + std::to_string(i), ch, [ch]() { insert(ch); });
        }
        action(r, "key.delete", tr("Delete"), []() { erase_previous(); });
        action(r, "key.clear", tr("Clear"), []() {
            s.keyboard.buffer.clear();
            s.keyboard.cursor = 0;
        });
        action(r, "key.space", tr("Space"), []() { insert(" "); });
        action(r, "key.case", tr("Case"), []() { s.keyboard.upper = !s.keyboard.upper; });
        action(r, "key.accept", tr("Accept"), []() { accept_keyboard(); });
        action(r, "key.cancel", tr("Cancel"), []() { pop(); });
    } else if (s.view == View::Conflicts) {
        conflict_rows(r);
        action(r, "conflicts.back", tr("Back"), []() { pop(); });
    } else if (s.view == View::Keybinds) {
        keybind_rows(r);
    } else if (s.view == View::KeyCapture) {
        int key = captured_key.exchange(-1);
        const char *name = key > 0 && key < KEY_MOUSE ? SDL_GetScancodeName((SDL_Scancode)key) : "";
        const char *reserved = key > 0 && key < KEY_MOUSE ? key_reserved(key) : nullptr;
        if (key == SDL_SCANCODE_ESCAPE || (touch_ui() && key == SDL_SCANCODE_AC_BACK))  // (the phone's Back key: cancel)
            pop();
        else if (s.capture_bind >= 0 && key > 0) {
            int *keys = s.draft.binds[s.capture_bind];
            if (reserved)
                s.error = fmt("That key is the recomp's own (%s).", tr(reserved));
            else if (!*config_key_name(key))
                s.error = tr("That key can't be used.");
            else {
                if (!s.capture_add)
                    keys[0] = key, keys[1] = 0;
                else if (keys[0] != key)
                    keys[1] = key;
                pop();
            }
        } else if (key > 0 && *name && strlen(name) < sizeof s.draft.chat_key && !strpbrk(name, ";#")) {
            SDL_strlcpy(s.draft.chat_key, name, sizeof s.draft.chat_key);
            pop();
        } else if (key > 0)
            s.error = tr("That key can't be used for chat.");
        if (s.view == View::KeyCapture) {
            action(r, "capture.none", tr("None"), []() {
                if (s.capture_bind >= 0)
                    s.draft.binds[s.capture_bind][0] = s.draft.binds[s.capture_bind][1] = 0;
                else
                    s.draft.chat_key[0] = 0;
                pop();
            });
            action(r, "capture.cancel", tr("Cancel"), []() { pop(); });
        }
    } else if (s.view == View::Move) {
        action(r, "move.accept", tr("Accept"), []() { pop(); });
        action(r, "move.cancel", tr("Cancel"), []() {
            s.move_position = s.move_start;
            s.position_override = true;
            pop();
        });
    } else {
        action(r, "dialog.cancel", tr("Cancel"), []() {
            if (s.view == View::CloseChanges) touch_edit_cancel();  // (the menu stays: no touch layout editor after all)
            pop();
        });
        if (s.view == View::Reset)
            action(r, "dialog.reset", tr("Reset"), []() { reset_section(); });
        if (s.view == View::CloseChanges) {
            action(r, "dialog.save", tr("Save and close"), [open]() {
                if (save())
                    *open = false;
            });
            action(r, "dialog.discard", tr("Discard and close"), [open]() {
                discard();
                *open = false;
            });
        }
        if (s.view == View::Exit) {
            action(r, "dialog.exit", tr((dirty() || pending_text()) ? "Save and exit" : "Exit game"), []() {
                if ((!dirty() && !pending_text()) || save())
                    ui_request_action(false);
            });
            if (dirty() || pending_text())
                action(r, "dialog.discard", tr("Exit without saving"), []() { ui_request_action(false); });
        }
        if (s.view == View::Restart)
            action(r, "dialog.restart", tr("Save and restart"), []() {
                if (save())
                    ui_request_action(true);
            });
    }
    return r;
}
// The open dropdown's choices
std::vector<Row> dropdown_rows() {
    std::vector<Row> r;
    for (int i = 0; i < (int)s.dropdown.names.size(); i++) {
        action(r, "choice." + std::to_string(i), s.dropdown.names[i], [i]() {
            auto set = s.dropdown.set;
            close_dropdown();
            set(i);
        });
        r.back().current = i == s.dropdown.selected;
    }
    return r;
}
// A row's help: its own, or what the default ini says of its setting (each line looked up in settings_locale.h)
#ifdef __ANDROID__
// A phone's own: the default ini's speaks of keys and of a PC, and stays in English
struct PhoneHelp {
    const char *key, *en, *es;
};
const PhoneHelp PHONE_HELP[] = {
    {"RenderScale", "The size of the 3D picture against the screen's. Lower runs faster and looks softer.",
     "El tamaño de la imagen 3D respecto de la pantalla. Más bajo corre más rápido y se ve más suave."},
    {"LowEnd", "For a weak phone: no bloom or depth of field, cheaper and smaller textures, and the HUD stays sharp over a smaller 3D picture.",
     "Para un teléfono débil: sin resplandor ni profundidad de campo, texturas más livianas y más chicas, y el HUD sigue nítido sobre una imagen 3D más chica."},
    {"LockOn", "Keeps the camera on the monster, as in the newer games, and a bow's or a bowgun's shots go to it. Off: the icon turns the camera to it once.",
     "Mantiene la cámara sobre el monstruo, como en los juegos nuevos, y los disparos de arcos y ballestas van hacia él. Apagado: el ícono gira la cámara una sola vez."},
    {"Join", "A host's IP address, to play over a VPN. Empty: searches your network.", "La dirección IP de un anfitrión, para jugar por VPN. Vacío: busca en tu red."},
    {"1Player", "The health of large monsters in Hub quests, by number of hunters. When you join a hub, the host's values are used.",
     "La vida de los monstruos grandes en las misiones del Hub, según la cantidad de cazadores. Al unirte a un Hub se usan los valores del anfitrión."},
    {"FOV", "How much of the scene you see: higher shows more.", "Cuánto se ve de la escena: un valor mayor muestra más."},
    {"ShowRanged", "With a bow or a bowgun in hand, in a hunt, the touch controls change to a layout of their own. Its buttons are moved in the editor.",
     "Con un arco o una ballesta en la mano, en una cacería, los controles táctiles pasan a un layout propio. Sus botones se mueven en el editor."},
    {"ShowFire", "A bowgun fires. A bow draws while you hold it and shoots when you let go. Drag on it to aim while you hold it.",
     "La ballesta dispara. El arco se tensa mientras lo mantienes y dispara al soltar. Arrastra sobre él para apuntar mientras lo mantienes."},
    {"ShowSecond", "A bowgun reloads. A bow does its melee attack.", "La ballesta recarga. El arco da su golpe cuerpo a cuerpo."},
    {"ShowAim", "A bowgun: a tap, the scope; held, the quick aim. A bow: the aiming mode.", "Ballesta: un toque, la mira; mantenido, el apuntado rápido. Arco: el modo de apuntado."},
    {"ShowRangedSpecial", "A bowgun's crouching fire. A bow applies its coating.", "El fuego agachado de la ballesta. El arco aplica su capa."},
    {"ShowAmmo", "Change the ammo or the coating. A swipe up or down on the item bar does it too.", "Cambian la munición o la capa. También se puede deslizar hacia arriba o abajo sobre la barra de ítems."},
    {"ShowItemArrows", "Arrows beside the item bar. A swipe on the bar turns it too.", "Flechas a los lados de la barra de ítems. Deslizar sobre la barra también la gira."},
    {"UIAnchor", "On a screen wider than 16:9, where the HUD goes.", "En una pantalla más ancha que 16:9, dónde va el HUD."},
    {"FillScreen", "Movies and menu backdrops fill the screen, with a little cut above and below, instead of bars at the sides.",
     "Los videos y los fondos de los menús llenan la pantalla, con un pequeño recorte arriba y abajo, en lugar de barras a los costados."},
    {"FPS", "The most frames a second. A lower limit heats the phone less and saves battery.",
     "El máximo de cuadros por segundo. Un límite más bajo calienta menos el teléfono y gasta menos batería."},
    {"ShowStats", "Shows the frame rate and the phone's temperature on the screen.", "Muestra los FPS y la temperatura del teléfono en la pantalla."},
    {"MSAA", "Smooths the edges of 3D models. It asks more of the GPU.", "Suaviza los bordes de los modelos 3D. Exige más a la GPU."},
    {"FXAA", "Smooths the edges of the whole picture. On Mali GPUs it can leave a green line at the edge of the screen.",
     "Suaviza los bordes de toda la imagen. En GPU Mali puede dejar una línea verde al borde de la pantalla."},
    {"DrawDistance", "How far away people in towns are drawn: 1 is the game's distance, 2 twice as far, 0 no limit.",
     "Hasta qué distancia se dibuja a la gente en los pueblos: 1 es la distancia del juego, 2 el doble, 0 sin límite."},
    {"Volume", "The game's volume.", "El volumen del juego."},
    {"FSR", "Rebuilds the picture from a lower resolution. It needs a Vulkan 1.2 GPU.", "Reconstruye la imagen desde una resolución menor. Necesita una GPU con Vulkan 1.2."},
    {"FSRQuality", "How low the resolution goes before the picture is rebuilt.", "Cuánto baja la resolución antes de reconstruir la imagen."},
    {"Sharpness", "How sharp FSR leaves the picture.", "Qué tan nítida deja FSR la imagen."},
    {"FaceButtons", "How A, B, X and Y are read on a controller. Position: as on the Switch. Label: by the letter printed on the button.",
     "Cómo se leen A, B, X e Y en un mando. Posición: como en la Switch. Etiqueta: según la letra impresa en el botón."},
    {"Controls", "Auto: they show while no controller is connected.", "Automático: se ven mientras no haya un mando conectado."},
    {"MoveHUD", "While the touch controls show, the HUD moves out of your thumbs' way.", "Mientras se ven los controles táctiles, el HUD se aparta de los pulgares."},
    {"CameraSpeed", "How far the camera turns for a drag.", "Cuánto gira la cámara al arrastrar."},
    {"Opacity", "How solid the touch controls look.", "Qué tan sólidos se ven los controles táctiles."},
    {"Vibration", "A short vibration when you press a touch control.", "Una vibración corta al tocar un control."},
    {"PhoneRumble", "The game's rumble on the phone too, with or without a controller.", "La vibración del juego también en el teléfono, con o sin mando."},
    {"Port", "The port for local play hubs. Everyone must use the same one.", "El puerto de las salas de juego local. Todos deben usar el mismo."},
    {"SyncSmallMonsters", "Small monsters are the same for every player. Experimental: they may stop attacking.",
     "Los monstruos pequeños son los mismos para todos los jugadores. Experimental: pueden dejar de atacar."},
    {"VillageQuestsInHub", "In the Hub, L or R while choosing a quest shows the Village quests, to play them with others.",
     "En el Hub, L o R al elegir una misión muestra las de la aldea, para jugarlas con otros."},
    {"HubHP", "A Village quest played in the Hub first multiplies the monster's health by this.",
     "Una misión de la aldea jugada en el Hub primero multiplica la vida del monstruo por este valor."},
    {"Language", "The language of this menu.", "El idioma de este menú."},
    {"MenuScale", "The size of this menu.", "El tamaño de este menú."},
    {"PauseInBackground", "The game stops while you are out of the app.", "El juego se detiene mientras estás fuera de la app."},
    {"BonusContent", "Copies the free downloadable content (event quests, item packs, Palicoes) to a save that has not received it yet.",
     "Copia el contenido descargable gratuito (misiones de evento, packs de objetos, Palicoes) a una partida que todavía no lo recibió."},
};
#endif
std::string help_text(const Row &r) {
    if (!r.help.empty() || r.key.empty())
        return r.help;
#ifdef __ANDROID__
    for (const PhoneHelp &h : PHONE_HELP)
        if (r.key == h.key) return s.draft.language == 1 ? h.es : h.en;
#endif
    static std::vector<std::pair<std::string, std::string>> cache;  // (key, help in English)
    auto at = std::find_if(cache.begin(), cache.end(), [&](const auto &e) { return e.first == r.key; });
    if (at == cache.end()) {
        char buf[2048];
        cache.emplace_back(r.key, config_help(r.key.c_str(), buf, sizeof buf) ? buf : "");
        at = cache.end() - 1;
    }
    std::string out, line;
    for (size_t i = 0, j; i < at->second.size(); i = j + 1) {
        j = at->second.find('\n', i);
        if (j == std::string::npos)
            j = at->second.size();
        line = at->second.substr(i, j - i);
        out += (out.empty() ? "" : "\n") + std::string(tr(line.c_str()));
    }
    return out;
}
bool pressed(ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); }
bool direction(ImGuiKey a, ImGuiKey b, ImGuiKey c) {
    return pressed(a, true) || pressed(b, true) || pressed(c, true);
}
// list: a dropdown's choices (left / right do nothing there)
void navigate(std::vector<Row> &rows, bool list = false) {
    if (rows.empty())
        return;
    auto it =
        std::find_if(rows.begin(), rows.end(), [](const Row &r) { return r.id == s.focus && r.enabled; });
    if (it == rows.end()) {
        it = std::find_if(rows.begin(), rows.end(), [](const Row &r) { return r.enabled; });
        if (it == rows.end())
            return;
        focus(it->id);
    }
    int index = (int)(it - rows.begin());
    int dy = direction(ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown) ? 1
             : direction(ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp)     ? -1
                                                                                                 : 0;
    int dx = direction(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight) ? 1
             : direction(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft)  ? -1
                                                                                                    : 0;
    if (pressed(ImGuiKey_Tab, true))
        dy = ImGui::GetIO().KeyShift ? -1 : 1;
    if (list)
        dx = 0;
    if (dx && it->adjust) {
        it->adjust(dx);
        return;
    }
    int delta = dy;
    if (s.view == View::Keyboard && (dx || dy) && !pressed(ImGuiKey_Tab, true)) {
        auto from =
            std::find_if(s.targets.begin(), s.targets.end(), [](const Target &t) { return t.id == s.focus; });
        if (from != s.targets.end()) {
            float best = 1e30f;
            std::string next;
            for (const auto &t : s.targets) {
                float x = t.center.x - from->center.x, y = t.center.y - from->center.y;
                if (!t.enabled || (dy && y * dy <= 1) || (dx && (x * dx <= 1 || std::abs(y) > 1)))
                    continue;
                float score = dy ? std::abs(y) * 1000 + std::abs(x) : std::abs(x);
                if (score < best) {
                    best = score;
                    next = t.id;
                }
            }
            if (!next.empty())
                focus(next);
            return;
        }
    }
    if (dx)
        delta = dx;
    if (delta) {
        int next = std::clamp(index + delta, 0, (int)rows.size() - 1);
        int step = delta > 0 ? 1 : -1;
        while (next >= 0 && next < (int)rows.size() && !rows[next].enabled)
            next += step;
        if (next >= 0 && next < (int)rows.size())
            focus(rows[next].id);
    }
    auto first = std::find_if(rows.begin(), rows.end(), [](const Row &r) { return r.enabled; });
    auto last = std::find_if(rows.rbegin(), rows.rend(), [](const Row &r) { return r.enabled; });
    if (pressed(ImGuiKey_Home))
        focus(first->id);
    if (pressed(ImGuiKey_End))
        focus(last->id);
}
// Keep ImGui internals at this boundary: rows retain native activation and rendering.
void own_focus(ImGuiWindow *w, ImGuiID id, const ImRect &rect) {
    ImGuiContext &g = *ImGui::GetCurrentContext();
    // Focus first: a click outside the menu leaves no focused window, and SetNavID asserts one
    ImGui::FocusWindow(w);
    ImGui::SetNavID(id, ImGuiNavLayer_Main, 0,
                    ImRect(ImVec2(rect.Min.x - w->Pos.x, rect.Min.y - w->Pos.y),
                           ImVec2(rect.Max.x - w->Pos.x, rect.Max.y - w->Pos.y)));
    g.NavCursorVisible = true;
    g.NavInitRequest = false;
    g.NavMoveSubmitted = false;
    // Let auto-sizing settle after a view change; do not fight later mouse-wheel scrolling.
    if (s.focus_request) {
        s.focus_scroll_frames = 3;
        s.focus_request = false;
    }
    if (s.focus_scroll_frames > 0) {
        --s.focus_scroll_frames;
        if (s.guide_height) {
            float top = w->ClipRect.Min.y, bottom = w->ClipRect.Max.y;
            float delta = rect.Min.y < top ? rect.Min.y - top : rect.Max.y > bottom ? rect.Max.y - bottom : 0;
            if (delta)
                ImGui::SetScrollY(w, w->Scroll.y + delta);
        } else  // (the phone's lists scroll, the menu around them stays)
            ImGui::ScrollToRect(w, rect, ImGuiScrollFlags_KeepVisibleEdgeY | (s.list ? ImGuiScrollFlags_NoScrollParent : 0));
    }
}
// A value cut to fit (available: its width), "..." at its end
void fit(std::string &value, float available) {
    if (ImGui::CalcTextSize(value.c_str()).x <= available)
        return;
    do {
        size_t n = value.size() - 1;
        while (n && ((unsigned char)value[n] & 0xc0) == 0x80)
            --n;
        value.resize(n);
    } while (!value.empty() && ImGui::CalcTextSize((value + "...").c_str()).x > available);
    value += "...";
}
// An invisible button over r for a finger (not one of the rows: the controller never lands on it); true when tapped
bool touch_button(const char *id, const ImRect &r, bool enabled, bool *held) {
    ImGui::SetCursorScreenPos(r.Min);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::BeginDisabled(!enabled);
    bool tapped = ImGui::InvisibleButton(id, ImVec2(std::max(1.f, r.GetWidth()), std::max(1.f, r.GetHeight())));
    *held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImGui::PopItemFlag();
    return tapped && !s.swallow;
}
// A row on the phone: lit gold when it is the one picked (a diamond at its left, as the game's cursor), lighter while a
// finger holds it; its title at the left, at the right an On / Off switch, the value between - and + (stepper: those
// are draw_rows' buttons, room their width), or the value, marked when a tap lists its choices. plated: a key or a
// dialog's button, a plate with its title in the middle
void touch_row(ImDrawList *dl, const Row &r, const ImRect &rect, bool lit, bool held, bool plated, ImVec2 label,
               float wrap, float room, bool toggle, bool stepper) {
    const Theme &t = theme();
    const Look k = touch_look();
    float font = ImGui::GetFontSize(), line = s.m.line, cy = rect.GetCenter().y;
    if (plated && !lit)
        plate(dl, rect, line * .8f, held, r.enabled);
    if (lit) {
        dl->AddRectFilledMultiColor(rect.Min, rect.Max, k.bar0, k.bar1, k.bar1, k.bar0);
        dl->AddRect(rect.Min, rect.Max, fade(k.glow, held ? 1.f : .8f), 0, 0, line * .6f);
        if (held)
            dl->AddRectFilled(rect.Min, rect.Max, IM_COL32(255, 240, 200, 40));
    } else if (held && !plated)
        dl->AddRectFilled(rect.Min, rect.Max, fade(k.glow, .14f));
    if (!plated && t.game) {  // (lines between the rows, as in the game's lists)
        float y = rect.Max.y + ImGui::GetStyle().ItemSpacing.y * .5f;
        dl->AddLine(ImVec2(rect.Min.x, y), ImVec2(rect.Max.x, y), fade(t.divider, .8f), std::max(1.f, line * .5f));
    }
    dl->PushClipRect(rect.Min, rect.Max, true);
    if (lit && !plated)
        diamond(dl, ImVec2(rect.Min.x + font * .55f, cy), font * .2f, k.glow);
    ImU32 ink = !r.enabled ? t.text_off : lit ? IM_COL32(255, 248, 232, 255) : r.current ? k.glow : k.ink;
    float x = plated ? rect.GetCenter().x - label.x * .5f : rect.Min.x + font * 1.1f;
    shadow_text(dl, font, ImVec2(x, cy - label.y * .5f), ink, r.title.c_str(), wrap);
    float right = rect.Max.x - font * .6f;
    if (r.current)  // (the picker: the choice the row has now)
        diamond(dl, ImVec2(right - font * .3f, cy), font * .24f, k.glow);
    if (toggle) {
        ImRect slot(right - font * 2.6f, cy - font * .6f, right, cy + font * .6f);
        switch_mark(dl, slot, r.toggle == 1);
        if (r.cold)
            shadow_text(dl, font, ImVec2(slot.Min.x - font * .8f, cy - font * .5f), lit ? ink : t.warn, "*");
    } else if (!r.value.empty() && !plated) {
        std::string value = r.value + (r.cold ? " *" : "");
        float bs = stepper ? std::min(s.m.row * .84f, font * 2.3f) : 0;
        fit(value, stepper ? room - bs * 2 - font * .4f : room - font * (r.menu ? 1.4f : .3f));
        ImVec2 size = ImGui::CalcTextSize(value.c_str());
        float vx = stepper ? right - room * .5f - size.x * .5f : right - size.x - (r.menu && r.enabled ? font * 1.1f : 0);
        ImU32 value_ink = r.warn ? IM_COL32(255, 110, 96, 255) : !r.enabled ? t.text_off : lit ? ink : k.rule;
        shadow_text(dl, font, ImVec2(vx, cy - size.y * .5f), value_ink, value.c_str());
        if (r.menu && r.enabled) {  // (a tap lists the choices)
            float m = font * .26f, mx = right - font * .4f;
            dl->AddTriangleFilled(ImVec2(mx - m, cy - m * .55f), ImVec2(mx + m, cy - m * .55f), ImVec2(mx, cy + m * .65f),
                                  lit ? ink : k.rule);
        }
    }
    dl->PopClipRect();
}
// list: a dropdown's choices; else the view's rows (while a dropdown is open they keep its row marked, the focus being
// in the dropdown). The phone (touch_ui): taller rows, drawn by touch_row, with - and + on a number's; the keyboard's
// Delete ... Cancel six to a line, a dialog's choices as plates
void draw_rows(std::vector<Row> &rows, bool list = false) {
    auto *w = ImGui::GetCurrentWindow();
    bool touch = touch_ui();
    float font = ImGui::GetFontSize(), width = ImGui::GetContentRegionAvail().x;
    if (touch)
        width -= s.m.line * 3;  // (room for the scroll mark at the list's right: scroll_mark)
    bool keyboard_view = s.view == View::Keyboard && !list;
    bool passive = s.dropdown.open && !list;
    bool plated = touch && !list && (confirmation() || s.view == View::KeyCapture || s.view == View::Move);
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float cell = (width - 9 * gap) / 10;
    float bs = std::min(s.m.row * .84f, font * 2.3f);  // (the phone: a - or + button's side)
    int keys = 0;  // (the keyboard's characters, first)
    for (const Row &r : rows)
        keys += keyboard_view && r.id.size() > 4 && r.id[4] >= '0' && r.id[4] <= '9';
    if (!list) {
        s.targets.clear();
        s.hovered.clear();
        s.pointing = false;
        s.dropdown.anchored = false;
    }
    std::function<void()> activate;
    std::function<void(int)> adjust;  // (a - or + tapped: its row's, by step)
    int step = 0;
    for (int i = 0; i < (int)rows.size(); i++) {
        Row &r = rows[i];
        if (r.header) {  // (a section's title: accent-coloured, a little apart from the rows above it)
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = font * (i ? 2.2f : 1.6f);
            ImGui::Dummy(ImVec2(width, h));
            w->DrawList->AddText(ImGui::GetFont(), font, ImVec2(p.x + font * .25f, p.y + h - font * 1.35f),
                                 theme().accent, r.title.c_str());
            float end = p.x + font * .75f + ImGui::CalcTextSize(r.title.c_str()).x, y = p.y + h - font * .85f;
            if (touch && end < p.x + width)  // (the phone: a rule after it, as the game's section titles)
                w->DrawList->AddLine(ImVec2(end, y), ImVec2(p.x + width, y), fade(theme().accent, .45f),
                                     std::max(1.f, s.m.line * .5f));
            continue;
        }
        bool key = keyboard_view && i < keys;
        bool grid = key || (touch && keyboard_view);
        int per_line = key ? 10 : 6, column = key ? i : i - keys;
        float rw = !grid ? width : !touch ? cell : (width - (per_line - 1) * gap) / per_line;
        if (grid && column % per_line)
            ImGui::SameLine();
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool stepper = touch && r.number && r.adjust && !grid;
        bool toggle = touch && r.toggle >= 0 && r.enabled;
        // (the phone: the title wraps before the value's room - a switch, - value +, or the value)
        float room = toggle ? font * 3.6f : stepper ? bs * 2 + font * 5.8f : r.value.empty() ? 0 : rw * .42f;
        float wrap = touch ? std::max(font, rw - font * (grid || plated ? 1.2f : 1.9f) - room)
                           : std::max(font, rw * (r.action ? 1.f : .58f) - font);
        ImVec2 label = ImGui::CalcTextSize(r.title.c_str(), nullptr, false, wrap);
        float h = touch ? grid ? s.m.row : std::max(s.m.row, label.y + font * .9f)
                  : grid ? font * 2
                         : std::max(font * 2.1f, label.y + font * .8f);
        ImGui::PushID(r.id.c_str());
        ImGuiID id = w->GetID("row");
        ImRect rect(p, ImVec2(p.x + rw, p.y + h));
        bool selected = passive ? r.id == s.dropdown.row : s.focus == r.id;
        if (passive && selected)
            s.dropdown.anchor = rect, s.dropdown.anchored = true;
        const Theme &t = theme();
        ImVec4 clear(0, 0, 0, 0);  // (the phone: touch_row draws it)
        ImGui::PushStyleColor(ImGuiCol_Button, touch ? clear : vec(selected && r.enabled ? t.row_focus : t.row));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              touch ? clear : vec(selected && r.enabled ? t.row_focus : t.row_hover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, touch ? clear : vec(t.row_focus));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, t.rounding);
        ImGui::BeginDisabled(!r.enabled);
        if (stepper)
            ImGui::SetNextItemAllowOverlap();  // (its - and + are over it)
        bool clicked = ImGui::Button("###row", ImVec2(rw, h));
        bool held = ImGui::IsItemActive();
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        if (!list && ImGui::IsItemHovered())
            s.hovered = r.id;
        if (!list && ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride))
            s.pointing = true;
        if (touch) {
            if (selected && r.enabled && !passive)
                own_focus(w, id, rect);
            touch_row(w->DrawList, r, rect, selected && r.enabled, held, grid || plated, label, wrap, room, toggle,
                      stepper);
            if (stepper) {  // - and + (held: they repeat)
                ImVec2 next = ImGui::GetCursorScreenPos();
                float right = rect.Max.x - font * .6f, cy = rect.GetCenter().y;
                for (int d = -1; d <= 1; d += 2) {
                    float x = d < 0 ? right - room : right - bs;
                    ImRect b(x, cy - bs * .5f, x + bs, cy + bs * .5f);
                    ImGui::SetCursorScreenPos(b.Min);
                    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
                    ImGui::BeginDisabled(!r.enabled);
                    // (on release, as a tap; held, it repeats: a drag that starts on it changes nothing)
                    bool hit = ImGui::InvisibleButton(d < 0 ? "###less" : "###more", ImVec2(bs, bs),
                                                      ImGuiButtonFlags_PressedOnClickRelease);
                    bool down = ImGui::IsItemActive();
                    ImGui::EndDisabled();
                    ImGui::PopItemFlag();
                    ImGui::PopItemFlag();
                    if (hit && r.enabled && !s.swallow)
                        step = d, adjust = r.adjust, focus(r.id);
                    plate(w->DrawList, b, s.m.line * .8f, down, r.enabled);
                    sign(w->DrawList, b.GetCenter(), bs * .24f, d > 0, down ? touch_look().glow : touch_look().ink,
                         std::max(2.f, s.m.line * 1.4f));
                }
                ImGui::SetCursorScreenPos(next);
            }
            s.targets.push_back({r.id, rect.GetCenter(), r.enabled});
            ImGui::PopID();
            if (clicked && r.enabled && !s.swallow) {
                focus(r.id);
                activate = r.activate;
            }
            continue;
        }
        if (selected && r.enabled) {
            if (!passive)
                own_focus(w, id, rect);
            if (t.bar)
                w->DrawList->AddRectFilled(rect.Min, ImVec2(rect.Min.x + font * .22f, rect.Max.y), t.accent,
                                           t.rounding, ImDrawFlags_RoundCornersLeft);
            else if (!t.game)
                w->DrawList->AddRect(rect.Min, rect.Max, t.accent, t.rounding, 0, 2.f);
        }
        if (t.game && !grid)  // (Hunter: a line under each row)
            w->DrawList->AddLine(ImVec2(rect.Min.x, rect.Max.y + ImGui::GetStyle().ItemSpacing.y * .5f),
                                 ImVec2(rect.Max.x, rect.Max.y + ImGui::GetStyle().ItemSpacing.y * .5f), t.divider,
                                 2.f);
        w->DrawList->PushClipRect(rect.Min, rect.Max, true);
        ImU32 ink = !r.enabled ? t.text_off : selected && t.bar ? t.accent_bright : t.text;
        ImU32 value_ink = r.warn ? IM_COL32(255, 110, 96, 255) : ink;
        if (r.current)  // (the dropdown's choice now: a bar on its left)
            w->DrawList->AddRectFilled(rect.Min, ImVec2(rect.Min.x + font * .22f, rect.Max.y), t.accent,
                                       t.rounding, ImDrawFlags_RoundCornersLeft);
        w->DrawList->AddText(ImGui::GetFont(), font, ImVec2(p.x + font * .5f, p.y + (h - label.y) * .5f), ink,
                             r.title.c_str(), nullptr, wrap);
        float arrow = 0;
        if (r.menu && r.enabled) {  // (Confirm opens a dropdown: an arrow after the value)
            arrow = font * 1.1f;
            ImGui::RenderArrow(w->DrawList, ImVec2(p.x + rw - font * .5f - font * .7f, p.y + (h - font) * .5f), ink,
                               s.dropdown.open && s.dropdown.row == r.id ? ImGuiDir_Up : ImGuiDir_Down, .7f);
        }
        if (!r.value.empty()) {
            std::string value = r.value + (r.cold ? " *" : "");
            fit(value, rw * .38f - font - arrow);
            auto sz = ImGui::CalcTextSize(value.c_str());
            w->DrawList->AddText(ImVec2(p.x + rw - font * .5f - arrow - sz.x, p.y + (h - font) * .5f), value_ink,
                                 value.c_str());
        }
        w->DrawList->PopClipRect();
        s.targets.push_back({r.id, rect.GetCenter(), r.enabled});
        ImGui::PopID();
        if (clicked && r.enabled) {
            focus(r.id);
            activate = r.activate;
        }
    }
    if (adjust)
        adjust(step);
    if (activate)
        activate();
}
// The open dropdown, a window of its own over the menu: under its row, at the right, or above the row when there is
// more room there; it scrolls when it doesn't fit. scale, base: the menu's (settings_panel)
void draw_dropdown(std::vector<Row> &options, float scale, float base) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImGuiStyle &style = ImGui::GetStyle();
    float font = base * scale, longest = 0;
    for (const auto &o : options)
        longest = std::max(longest, ImGui::CalcTextSize(o.title.c_str()).x * scale);
    const ImRect &a = s.dropdown.anchor;
    float w = std::clamp(longest + font * 2 + style.WindowPadding.x * 2, a.GetWidth() * .4f, a.GetWidth());
    float h = options.size() * (font * 2.1f + style.ItemSpacing.y) - style.ItemSpacing.y + style.WindowPadding.y * 2;
    float below = vp->WorkPos.y + vp->WorkSize.y - a.Max.y, above = a.Min.y - vp->WorkPos.y;
    bool up = h > below && above > below;
    h = std::min(h, up ? above : below);
    ImGui::SetNextWindowPos(ImVec2(a.Max.x - w, up ? a.Min.y - h : a.Max.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(theme().popup));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(theme().edge));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme().rounding);
    ImGui::Begin("Settings##dropdown", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(scale);
    draw_rows(options, true);
    s.dropdown.hovered = ImGui::IsWindowHovered();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}
// Hunter's window frame, over the rest: a red rule with a green one inside it along the top and the bottom, and a gold
// square on each corner
void draw_frame(ImGuiWindow *w, const Theme &look) {
    if (!look.game)
        return;
    const ImU32 red = IM_COL32(176, 34, 30, 255), green = IM_COL32(80, 142, 54, 255), gold = IM_COL32(222, 190, 40, 255);
    float b = w->WindowBorderSize, x0 = w->Pos.x + b, x1 = w->Pos.x + w->Size.x - b, y0 = w->Pos.y + b,
          y1 = w->Pos.y + w->Size.y - b, line = std::max(2.f, ImGui::GetFontSize() * .14f);
    w->DrawList->PushClipRectFullScreen();
    w->DrawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + line), red);
    w->DrawList->AddRectFilled(ImVec2(x0, y0 + line), ImVec2(x1, y0 + line * 1.7f), green);
    w->DrawList->AddRectFilled(ImVec2(x0, y1 - line * 1.7f), ImVec2(x1, y1 - line), green);
    w->DrawList->AddRectFilled(ImVec2(x0, y1 - line), ImVec2(x1, y1), red);
    float c = ImGui::GetFontSize() * .5f;
    for (ImVec2 corner : {w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y), ImVec2(w->Pos.x, w->Pos.y + w->Size.y),
                          ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y)}) {
        ImVec2 a(corner.x - c * .5f, corner.y - c * .5f), z(corner.x + c * .5f, corner.y + c * .5f);
        w->DrawList->AddRectFilled(a, z, IM_COL32(0, 0, 0, 255));
        w->DrawList->AddRect(ImVec2(a.x + 1, a.y + 1), ImVec2(z.x - 1, z.y - 1), gold, 0, 0, std::max(1.f, c * .16f));
        w->DrawList->AddRectFilled(ImVec2(corner.x - c * .14f, corner.y - c * .14f),
                                   ImVec2(corner.x + c * .14f, corner.y + c * .14f), gold);
    }
    w->DrawList->PopClipRect();
}
// The corner grip that sizes the menu (mouse_drag): three short lines across the bottom right corner
float grip_size() { return ImGui::GetFontSize() * 1.3f; }
void draw_grip(ImGuiWindow *w, const Theme &look) {
    ImVec2 corner(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
    float g = grip_size(), inset = look.bar ? g * .35f : g * .2f;
    bool lit =
        s.dragging == 2 || ImRect(ImVec2(corner.x - g, corner.y - g), corner).Contains(ImGui::GetIO().MousePos);
    w->DrawList->PushClipRectFullScreen();
    for (int i = 1; i <= 3; i++) {
        float d = g * .22f * i;
        w->DrawList->AddLine(ImVec2(corner.x - inset - d, corner.y - inset),
                             ImVec2(corner.x - inset, corner.y - inset - d), lit ? look.accent : look.text_off, 1.5f);
    }
    w->DrawList->PopClipRect();
}
// The mouse on the menu, before it is drawn: a press on its top part (the tabs, or a dialog's title) and a drag moves
// it, one on the grip at its bottom right corner sizes it. Either one detaches it (Menu Options: Attach menu puts it back).
void mouse_drag(ImGuiViewport *vp, float width) {
    ImGuiIO &io = ImGui::GetIO();
    auto *w = ImGui::FindWindowByName("Settings##main");
    if (!w || s.view == View::Move)
        return;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !s.dropdown.open) {
        float g = grip_size();
        ImVec2 corner(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
        s.dragged = false;
        bool grip = ImRect(ImVec2(corner.x - g, corner.y - g), corner).Contains(io.MousePos);
        bool top = ImRect(w->Pos, ImVec2(corner.x, w->Pos.y + s.header_bottom)).Contains(io.MousePos);
        s.dragging = grip ? 2 : top ? 1 : 0;
        s.drag_mouse = io.MousePos;
        s.drag_from = s.dragging == 2 ? w->Size : w->Pos;
    }
    if (!s.dragging)
        return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s.dragging = 0;
        return;
    }
    ImVec2 d(io.MousePos.x - s.drag_mouse.x, io.MousePos.y - s.drag_mouse.y);
    if (!s.dragged && d.x * d.x + d.y * d.y < io.MouseDragThreshold * io.MouseDragThreshold)
        return;
    s.dragged = true;
    if (!s.floating) {
        s.floating = true;
        s.move_position = w->Pos;
    }
    if (s.dragging == 2) {
        s.custom_size = ImVec2(s.drag_from.x + d.x, s.drag_from.y + d.y);
        s.move_position = w->Pos;
    } else {
        float h = w->Size.y;
        s.move_position.x = std::clamp(s.drag_from.x + d.x, vp->WorkPos.x - width * .5f,
                                       vp->WorkPos.x + vp->WorkSize.x - width * .5f);
        s.move_position.y = std::clamp(s.drag_from.y + d.y, vp->WorkPos.y,
                                       vp->WorkPos.y + std::max(0.f, vp->WorkSize.y - h * .25f));
    }
    s.position_override = true;
}
// Back (B, Esc, the Android Back key, F1 / the chord: request_back), and a click away from an open dropdown
void back_keys(bool *open) {
    if (request_back.exchange(false) || pressed(ImGuiKey_GamepadFaceRight) || pressed(ImGuiKey_Escape) ||
        pressed(ImGuiKey_AppBack))
        ask_back(open);
    if (s.dropdown.open && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !s.dropdown.hovered) {
        close_dropdown();  // (a click elsewhere: on a row, it acts as it does with no dropdown open - but the phone's
        s.swallow = touch_ui();  // picker covers the menu: the tap only closes it)
    }
}
// The tabs' keys: Save from anywhere on a tab (the top face button, or Ctrl+S), L / R the tab before / after
void page_keys() {
    if ((pressed(ImGuiKey_GamepadFaceUp) || (ImGui::GetIO().KeyCtrl && pressed(ImGuiKey_S))) && dirty() &&
        !s.dropdown.open)
        s.command = Command::Save;
    if (pressed(ImGuiKey_GamepadR1))
        first_page((s.page + 1) % PAGES);
    if (pressed(ImGuiKey_GamepadL1))
        first_page((s.page + PAGES - 1) % PAGES);
}
// The Keyboard view's keys from a real keyboard: typed text, Backspace / Delete, Ctrl+V / Ctrl+A, Enter; L / R move
// the cursor
void keyboard_keys() {
    ImGuiIO &io = ImGui::GetIO();
    for (ImWchar ch : io.InputQueueCharacters) {
        char utf8[5];
        int n = ImTextCharToUtf8(utf8, ch);
        utf8[n] = 0;
        insert(utf8);
    }
    if (pressed(ImGuiKey_Backspace, true))
        erase_previous();
    if (io.KeyCtrl && pressed(ImGuiKey_V)) {
        char *clip = SDL_GetClipboardText();
        if (clip) {
            insert(clip);
            SDL_free(clip);
        }
    }
    if (io.KeyCtrl && pressed(ImGuiKey_A))
        s.keyboard.select_all = true;
    if (pressed(ImGuiKey_Delete, true) && s.keyboard.select_all)
        erase_previous();
    else if (pressed(ImGuiKey_Delete, true) && s.keyboard.cursor < s.keyboard.buffer.size()) {
        size_t n = s.keyboard.cursor + 1;
        while (n < s.keyboard.buffer.size() && ((unsigned char)s.keyboard.buffer[n] & 0xc0) == 0x80)
            ++n;
        s.keyboard.buffer.erase(s.keyboard.cursor, n - s.keyboard.cursor);
    }
    if (pressed(ImGuiKey_GamepadL1, true) && s.keyboard.cursor) {
        --s.keyboard.cursor;
        while (s.keyboard.cursor && ((unsigned char)s.keyboard.buffer[s.keyboard.cursor] & 0xc0) == 0x80)
            --s.keyboard.cursor;
    }
    if (pressed(ImGuiKey_GamepadR1, true) && s.keyboard.cursor < s.keyboard.buffer.size()) {
        ++s.keyboard.cursor;
        while (s.keyboard.cursor < s.keyboard.buffer.size() &&
               ((unsigned char)s.keyboard.buffer[s.keyboard.cursor] & 0xc0) == 0x80)
            ++s.keyboard.cursor;
    }
    if (pressed(ImGuiKey_Enter))
        accept_keyboard();
}
// A view's title (not the tabs')
std::string view_title() {
    return s.view == View::Keyboard ? s.keyboard.title
           : s.view == View::KeyCapture && s.capture_bind >= 0
               ? fmt("Press a key or mouse button for %s (Esc: cancel)", tr(bind_titles[s.capture_bind]))
               : tr(s.view == View::Exit         ? "Exit game?"
                    : s.view == View::Restart    ? "Restart game?"
                    : s.view == View::Reset      ? "Reset this tab?"
                    : s.view == View::Move       ? "Move menu"
                    : s.view == View::Conflicts  ? "Mod conflicts"
                    : s.view == View::Keybinds   ? "Keybinds"
                    : s.view == View::TouchButtons ? "Buttons on the screen"
                    : s.view == View::ComboEdit  ? "Combined button"
                    : s.view == View::PadList    ? "Map a controller"
                    : s.view == View::PadMap     ? "Map a controller"
                    : s.view == View::KeyCapture ? "Press the key for chat (Esc: cancel)"
                                                 : "Unsaved changes");
}
// The tab's notes under its rows (text, a warning)
std::vector<std::pair<const char *, bool>> page_notes() {
    std::vector<std::pair<const char *, bool>> notes;
    if (s.page == 1 && (s.draft.fsr || g_config.fsr))
        notes.push_back({tr("FSR overrides MSAA and FXAA while active."), false});
    if (s.page == MODS_PAGE)
        notes.push_back({tr("Mods load from top to bottom: a later mod wins. Select a conflict to choose which mod is used. Press Save, then restart."), false});
    if (s.page == CHEATS_PAGE) {
        notes.push_back({tr("Online: only use cheats when everyone in the lobby agrees to it."), true});
        notes.push_back({tr("Cheats turn on and off when you press Save, no restart needed. Turning one off undoes its changes to the game's code; values it already changed (money, items) stay. New cheat files show up after a restart."), false});
    }
    return notes;
}
const char *saved_text() {
    return tr((config_restart_required(&s.draft) || mods_restart()) ? "Saved - restart pending" : "Saved");
}
// What a row or a key asked for this frame (Save), and what the draft's changes undo (the "Saved" notice)
void commands(const Config &before) {
    if (s.command != Command::None) {
        auto cmd = s.command;
        s.command = Command::None;
        // (the Save row is greyed once saved: Reset is picked; the phone's header Save leaves the row picked be)
        if (cmd == Command::Save && save() && (!touch_ui() || s.focus == "save"))
            focus("reset");
    }
    if (!equal(before, s.draft) && dirty())
        s.saved_notice = false;
    if (ui_action_failed())
        s.error = tr("Restart failed. The game is still running.");
}
// ---------------------------------------------------------------- the phone's menu (touch_ui)
const ImGuiWindowFlags fixed_window = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse;
// A finger dragged on a list scrolls it: a press that moves less than ~2% of the screen's height is still a tap, past
// that the row or button under it lets go. Let go, the list glides on and slows to a stop; a tap on a gliding list
// only stops it. w: the list (a child window), after its rows
void finger_scroll(ImGuiWindow *w) {
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiIO &io = g.IO;
    Drag &d = s.drag;
    float dt = std::max(io.DeltaTime, 1e-4f);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && g.HoveredWindow == w) {
        if (d.window == w->ID && std::fabs(d.velocity) > s.m.H * .3f)
            s.swallow = true;
        d = {w->ID, io.MousePos.y, w->Scroll.y, w->Scroll.y, 0, true, false};
    }
    if (d.window != w->ID)
        return;
    if (d.pressed && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!d.moving && std::fabs(io.MousePos.y - d.from) > s.m.H * .022f) {
            d.moving = true;
            d.from = io.MousePos.y, d.scroll = d.last = w->Scroll.y;  // (from here: no jump by the threshold)
            ImGui::ClearActiveID();  // (the row or the button under the finger: not tapped after all)
            s.swallow = true;
        }
        if (d.moving) {
            float to = std::clamp(d.scroll - (io.MousePos.y - d.from), 0.f, w->ScrollMax.y);
            d.velocity = d.velocity * .5f + (to - d.last) / dt * .5f;
            d.last = to;
            ImGui::SetScrollY(w, to);
        }
        return;
    }
    if (d.pressed) {  // (let go: a drag glides on, a tap doesn't)
        d.pressed = false;
        if (!d.moving)
            d.velocity = 0;
        d.moving = false;
    }
    if (d.velocity) {
        float to = std::clamp(w->Scroll.y + d.velocity * dt, 0.f, w->ScrollMax.y);
        ImGui::SetScrollY(w, to);
        d.velocity *= std::exp(-dt * 4.f);
        if (std::fabs(d.velocity) < s.m.H * .05f || to <= 0 || to >= w->ScrollMax.y)
            d.velocity = 0;
    }
}
// A thin gold mark at a list's right: how much of it shows, how far down it is
void scroll_mark(ImGuiWindow *w) {
    if (w->ScrollMax.y < 1)
        return;
    const Look k = touch_look();
    float view = w->Size.y, line = s.m.line, bar = std::max(view * view / (view + w->ScrollMax.y), s.m.row * .5f);
    float y = w->Pos.y + (view - bar) * std::clamp(w->Scroll.y / w->ScrollMax.y, 0.f, 1.f);
    float x = w->Pos.x + w->Size.x - line * 1.5f;
    w->DrawList->AddRectFilled(ImVec2(x, w->Pos.y), ImVec2(x + line, w->Pos.y + view), fade(k.rule, .18f));
    w->DrawList->AddRectFilled(ImVec2(x - line * .25f, y), ImVec2(x + line * 1.25f, y + bar), fade(k.rule, .9f));
}
// The picked row's help on the phone: its name and what the settings file says of it; the tab's notes, the last save's
// outcome and what the restart mark means at the bottom. side: a column at the right of the rows, else a box under
// them (the help at its left, the outcome and the mark at its right; the notes go under the rows)
void touch_help(ImDrawList *dl, const ImRect &r, const std::vector<Row> &rows, bool side) {
    const Theme &t = theme();
    const Look k = touch_look();
    ImFont *font = ImGui::GetFont();
    float f = s.m.f, pad = f * .55f, small = std::floor(f * .8f), line = s.m.line;
    dl->AddRectFilled(r.Min, r.Max, fade(k.plate, .6f));
    dl->AddRect(r.Min, r.Max, k.edge, 0, 0, std::max(1.f, line * .5f));
    std::vector<std::pair<std::string, ImU32>> foot;
    if (side)
        for (const auto &note : page_notes())
            foot.push_back({note.first, note.second ? t.warn : k.dim});
    if (!s.error.empty())
        foot.push_back({s.error, t.warn});
    else if (s.saved_notice)
        foot.push_back({saved_text(), k.glow});
    foot.push_back({tr("* Restart required"), k.dim});
    float x = r.Min.x + pad, wrap = r.GetWidth() - pad * 2, bottom = r.Max.y - pad * .7f;
    if (side) {
        for (auto it = foot.rbegin(); it != foot.rend(); ++it) {
            bottom -= font->CalcTextSizeA(small, FLT_MAX, wrap, it->first.c_str()).y;
            dl->AddText(font, small, ImVec2(x, bottom), it->second, it->first.c_str(), nullptr, wrap);
            bottom -= pad * .45f;
        }
        dl->AddLine(ImVec2(x, bottom), ImVec2(r.Max.x - pad, bottom), fade(k.rule, .4f), std::max(1.f, line * .4f));
    } else {
        float most = wrap * .42f, widest = 0, y = bottom;
        for (auto it = foot.rbegin(); it != foot.rend(); ++it) {
            ImVec2 size = font->CalcTextSizeA(small, FLT_MAX, most, it->first.c_str());
            y -= size.y;
            dl->AddText(font, small, ImVec2(r.Max.x - pad - size.x, y), it->second, it->first.c_str(), nullptr, most);
            widest = std::max(widest, size.x);
        }
        wrap = std::max(f * 4, wrap - widest - pad);
    }
    const std::string &id = s.dropdown.open ? s.dropdown.row : s.focus;
    const Row *row = nullptr;
    for (const Row &candidate : rows)
        if (candidate.id == id && !candidate.header)
            row = &candidate;
    if (!row)
        return;
    dl->PushClipRect(r.Min, ImVec2(r.Max.x, std::max(r.Min.y, bottom - pad * .3f)), true);
    float y = r.Min.y + pad * .7f;
    if (side) {
        shadow_text(dl, f, ImVec2(x, y), k.glow, row->title.c_str(), wrap);
        y += font->CalcTextSizeA(f, FLT_MAX, wrap, row->title.c_str()).y + pad * .5f;
        dl->AddLine(ImVec2(x, y), ImVec2(x + wrap, y), fade(k.rule, .5f), std::max(1.f, line * .4f));
        y += pad * .6f;
    }
    std::string help = help_text(*row);
    dl->AddText(font, small, ImVec2(x, y), k.ink, help.c_str(), nullptr, wrap);
    dl->PopClipRect();
}
// The phone's picker: a row's choices in a plate of their own in the middle of the screen, over the menu dimmed; a
// tap picks one, a tap away from it, its X or Back closes it. scale: the menu's (touch_panel)
void touch_picker(std::vector<Row> &options, float scale) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const Theme &look = theme();
    const Look k = touch_look();
    ImFont *font = ImGui::GetFont();
    float f = s.m.f, pad = s.m.pad, line = s.m.line, spacing = ImGui::GetStyle().ItemSpacing.y;
    float band = std::max(f * 2.4f, s.m.row * .95f), side = band * .74f, title = std::floor(f * 1.05f);
    float frame = std::max(2.f, f * .14f) * 1.7f;  // (draw_frame's rules at the bottom)
    float longest = font->CalcTextSizeA(title, FLT_MAX, 0, s.dropdown.title.c_str()).x + side + pad * 3;
    for (const auto &o : options)
        longest = std::max(longest, font->CalcTextSizeA(f, FLT_MAX, 0, o.title.c_str()).x + f * 3.4f);
    float w = std::clamp(longest + pad * 2, vp->WorkSize.x * .32f, vp->WorkSize.x * .62f);
    float h = std::min(band + line * 2 + pad * 2 + options.size() * (s.m.row + spacing) + pad + frame,
                       vp->WorkSize.y * .86f);
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + (vp->WorkSize.x - w) * .5f, vp->WorkPos.y + (vp->WorkSize.y - h) * .5f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(look.popup));
    ImGui::Begin("Settings##dropdown", nullptr, fixed_window);
    ImGui::SetWindowFontScale(scale);
    auto *win = ImGui::GetCurrentWindow();
    auto *dl = win->DrawList;
    float x0 = win->Pos.x, y0 = win->Pos.y, x1 = x0 + win->Size.x, by = y0 + (band - side) * .5f + line * .5f;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + band), k.band);
    if (look.game) {
        dl->AddRectFilled(ImVec2(x0, y0 + band), ImVec2(x1, y0 + band + line), IM_COL32(176, 34, 30, 255));
        dl->AddRectFilled(ImVec2(x0, y0 + band + line), ImVec2(x1, y0 + band + line * 1.7f),
                          IM_COL32(80, 142, 54, 255));
    }
    ImRect shut(x1 - pad - side, by, x1 - pad, by + side);
    bool held = false;
    if (touch_button("###shut", shut, true, &held))
        close_dropdown();
    plate(dl, shut, line, held, true);
    cross(dl, shut.GetCenter(), side * .2f, held ? k.glow : k.ink, std::max(2.f, line * 1.5f));
    ImVec2 ts = font->CalcTextSizeA(title, FLT_MAX, 0, s.dropdown.title.c_str());
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(shut.Min.x - pad * .5f, y0 + band), true);
    shadow_text(dl, title, ImVec2(x0 + pad * 1.5f, by + side * .5f - ts.y * .5f), k.glow, s.dropdown.title.c_str());
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(x0 + pad, y0 + band + line * 2 + pad));
    ImGui::BeginChild("##choices",
                      ImVec2(win->Size.x - pad * 2, std::max(1.f, h - band - line * 2 - pad * 2 - frame)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetWindowFontScale(scale);
    auto *choices = ImGui::GetCurrentWindow();
    draw_rows(options, true);
    ImGui::Dummy(ImVec2(1, pad));
    finger_scroll(choices);
    scroll_mark(choices);
    ImGui::EndChild();
    s.dropdown.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    draw_frame(win, look);
    ImGui::End();
    ImGui::PopStyleColor();
}
// The phone's menu: nearly the whole screen - a header with the title, Back (in a submenu or a dialog), Save and Close
// (X); the tabs in a column at the left, the rows in a list a finger drags, the picked row's help at the right (under
// the rows where the screen is narrower). The rows, what they do and the dialogs are the controller's (rows_for_view),
// which still works it as before. before: the draft as the frame began
void touch_panel(bool *open, const Config &before) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    const Theme &look = theme();
    const Look k = touch_look();
    float H = vp->WorkSize.y, base = ImGui::GetFontSize();
    float scale = std::clamp(.034f * H * s.draft.menu_scale / 100.f, .022f * H, .05f * H) / base;
    ImVec2 panel(std::min(vp->WorkSize.x * .92f, H * 2.6f), H * .92f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - panel.x) * .5f,
                                   vp->WorkPos.y + (vp->WorkSize.y - panel.y) * .5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(panel, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(look.panel));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(look.text));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(look.edge));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, look.game ? 0.f : look.rounding * 2);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Settings##main", nullptr, fixed_window);
    ImGui::SetWindowFontScale(scale);
    auto *window = ImGui::GetCurrentWindow();
    auto *dl = window->DrawList;
    ImFont *font = ImGui::GetFont();
    float f = ImGui::GetFontSize();
    s.m.H = H, s.m.f = f, s.m.row = std::max(.09f * H, f * 2.4f), s.m.pad = .014f * H, s.m.line = std::max(2.f, .0035f * H);
    float pad = s.m.pad, line = s.m.line;
    s.guide_height = 0;  // (own_focus: the list scrolls to the row picked)
    back_keys(open);
    if (s.view == View::Main)
        page_keys();
    if (s.view == View::Keyboard)
        keyboard_keys();
    auto rows = rows_for_view(open);
    std::vector<Row> options;  // (a dropdown open: the picker's, the controller moves in it)
    if (s.dropdown.open)
        options = dropdown_rows();
    if (s.view == View::Move)
        focus("move.accept");
    else
        navigate(s.dropdown.open ? options : rows, s.dropdown.open);
    bool dialog = confirmation() || s.view == View::KeyCapture || s.view == View::Move;
    bool picking = s.dropdown.open && !options.empty();  // (the picker over the menu this frame: the menu dimmed)
    // The header: Back at its left, Save and Close at its right, the title between
    float x0 = window->Pos.x, y0 = window->Pos.y, x1 = x0 + window->Size.x, y1 = y0 + window->Size.y;
    float band = std::max(.11f * H, f * 2.9f), side = band * .8f, by = y0 + (band - side) * .5f + line * .5f;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + band), k.band);
    if (look.game) {  // (the game's red and green rules under it)
        dl->AddRectFilled(ImVec2(x0, y0 + band), ImVec2(x1, y0 + band + line), IM_COL32(176, 34, 30, 255));
        dl->AddRectFilled(ImVec2(x0, y0 + band + line), ImVec2(x1, y0 + band + line * 1.7f),
                          IM_COL32(80, 142, 54, 255));
    } else
        dl->AddRectFilled(ImVec2(x0, y0 + band), ImVec2(x1, y0 + band + line * .7f), look.accent);
    float title_x = x0 + pad * 1.5f, right = x1 - pad;
    bool held = false;
    std::function<void()> later;  // (what the header or a tab asked for: once the frame is drawn as it began)
    if (s.view != View::Main) {  // (a submenu or a dialog: a step back)
        ImRect r(x0 + pad, by, x0 + pad + side, by + side);
        if (touch_button("###back", r, true, &held))
            later = [open]() { ask_back(open); };
        plate(dl, r, line, held, true);
        chevron(dl, r.GetCenter(), side * .2f, -1, held ? k.glow : k.ink, std::max(2.f, line * 1.5f));
        title_x = r.Max.x + pad * 1.2f;
    }
    if (!dialog) {  // (a dialog asks for those itself)
        ImRect r(right - side, by, right, by + side);
        if (touch_button("###close", r, true, &held))
            later = [open]() { ask_close(open); };
        plate(dl, r, line, held, true);
        cross(dl, r.GetCenter(), side * .2f, held ? k.glow : k.ink, std::max(2.f, line * 1.5f));
        bool can = dirty() || pending_text();
        const char *label = tr("Save");
        ImVec2 ts = ImGui::CalcTextSize(label);
        float sw = std::max(side * 1.8f, ts.x + f * 1.8f);
        ImRect save_rect(r.Min.x - pad - sw, by, r.Min.x - pad, by + side);
        if (touch_button("###save", save_rect, can, &held))
            s.command = Command::Save;
        plate(dl, save_rect, line, held, can);
        shadow_text(dl, f,
                    ImVec2(save_rect.GetCenter().x - ts.x * .5f, save_rect.GetCenter().y - ts.y * .5f),
                    can ? k.glow : look.text_off, label);
        right = save_rect.Min.x - pad;
    }
    {  // (the title; the tab's name or the submenu's after it)
        const char *title = tr("SETTINGS");
        float size = std::floor(f * 1.1f), cy = by + side * .5f;
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, title);
        std::string sub = s.view == View::Main ? tr(page_names[s.page]) : dialog ? "" : view_title();
        dl->PushClipRect(ImVec2(x0, y0), ImVec2(std::max(title_x, right), y0 + band), true);
        shadow_text(dl, size, ImVec2(title_x, cy - ts.y * .5f), k.glow, title);
        if (!sub.empty()) {
            float dx = title_x + ts.x + f * .7f;
            diamond(dl, ImVec2(dx, cy), f * .18f, k.rule);
            ImVec2 ss = ImGui::CalcTextSize(sub.c_str());
            shadow_text(dl, f, ImVec2(dx + f * .7f, cy - ss.y * .5f), k.ink, sub.c_str());
        }
        dl->PopClipRect();
    }
    // The body: the tabs and the help around the list on the tabs; a submenu's or a dialog's rows alone
    float frame = std::max(2.f, f * .14f) * 1.7f;  // (draw_frame's rules at the bottom)
    float top = y0 + band + line * 2 + pad, bottom = y1 - frame - pad, left = x0 + pad, end = x1 - pad;
    auto error_text = [&](float y, float x, float w) {  // (s.error, wrapped at w; the height it takes)
        if (s.error.empty())
            return 0.f;
        dl->AddText(font, f, ImVec2(x, y), look.warn, s.error.c_str(), nullptr, w);
        return font->CalcTextSizeA(f, FLT_MAX, w, s.error.c_str()).y + pad * .6f;
    };
    ImRect list_rect, help_rect;
    bool side_help = false;
    if (s.view == View::Main) {
        float tw = 0;
        for (const char *name : page_names)
            tw = std::max(tw, ImGui::CalcTextSize(tr(name)).x);
        tw = std::clamp(tw + f * 1.8f, (end - left) * .14f, (end - left) * .22f);
        float gap = pad * .5f, th = (bottom - top - gap * (PAGES - 1)) / PAGES;
        for (int i = 0; i < PAGES; i++) {
            ImRect r(left, top + i * (th + gap), left + tw, top + i * (th + gap) + th);
            ImGui::PushID(i);
            if (touch_button("###tab", r, true, &held) && s.page != i)
                later = [i]() { first_page(i); };
            ImGui::PopID();
            bool active = s.page == i;
            float cy = r.GetCenter().y;
            ImU32 bright = IM_COL32(255, 248, 232, 255);
            if (active) {  // (lit as the picked row, a notch at its right pointing at the rows)
                float n = std::min(th * .18f, pad);
                dl->AddRectFilledMultiColor(r.Min, r.Max, k.bar0, k.bar1, k.bar1, k.bar0);
                dl->AddRect(r.Min, r.Max, fade(k.glow, .9f), 0, 0, line * .7f);
                dl->AddTriangleFilled(ImVec2(r.Max.x, cy - n), ImVec2(r.Max.x + pad * .75f, cy),
                                      ImVec2(r.Max.x, cy + n), k.glow);
                diamond(dl, ImVec2(r.Min.x + f * .5f, cy), f * .17f, bright);
            } else
                plate(dl, r, line * .8f, held, true);
            const char *name = tr(page_names[i]);
            float room = tw - f * 1.4f, wide = ImGui::CalcTextSize(name).x;
            float text = wide > room ? std::floor(f * room / wide) : f;  // (a long name: smaller, on one line)
            ImVec2 ts = font->CalcTextSizeA(text, FLT_MAX, 0, name);
            shadow_text(dl, text, ImVec2(r.Min.x + f * .95f, cy - ts.y * .5f), active ? bright : k.dim, name);
        }
        float lx = left + tw + pad * 1.4f, help_w = f * 14;
        side_help = end - lx - help_w - pad >= f * 25;
        if (side_help) {
            list_rect = ImRect(lx, top, end - help_w - pad, bottom);
            help_rect = ImRect(end - help_w, top, end, bottom);
        } else {
            float small = std::floor(f * .8f), hw = end - lx - pad * 2, tall = small * 3.4f;
            for (const Row &r : rows)  // (as tall as the page's longest help: nothing shows it otherwise on a phone)
                if (!r.header)
                    tall = std::max(tall, ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, std::max(f * 4, hw * .7f), help_text(r).c_str()).y);
            float box = std::min(tall + f * .55f * 1.7f, (bottom - top) * .45f);  // (touch_help's own padding)
            list_rect = ImRect(lx, top, end, bottom - box - pad);
            help_rect = ImRect(lx, bottom - box, end, bottom);
        }
    } else if (s.view == View::Keyboard) {  // (the text typed so far, the keys under it)
        ImRect field(left, top, end, top + f * 2.3f);
        dl->AddRectFilled(field.Min, field.Max, IM_COL32(14, 10, 8, 235));
        dl->AddRect(field.Min, field.Max, k.rule, 0, 0, line * .7f);
        std::string text = s.keyboard.buffer;
        text.insert(s.keyboard.cursor, "|");
        float room = field.GetWidth() - f * 1.2f;
        float upto = ImGui::CalcTextSize(text.c_str(), text.c_str() + s.keyboard.cursor + 1).x;
        float tx = field.Min.x + f * .6f - std::max(0.f, upto - room);  // (a long text: the part at the cursor shows)
        ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        float ty = field.GetCenter().y - ts.y * .5f;
        dl->PushClipRect(ImVec2(field.Min.x + line, field.Min.y), ImVec2(field.Max.x - line, field.Max.y), true);
        if (s.keyboard.select_all)
            dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + ts.x, ty + ts.y), fade(k.glow, .3f));
        dl->AddText(ImVec2(tx, ty), k.ink, text.c_str());
        dl->PopClipRect();
        float y = field.Max.y + pad;
        y += error_text(y, left, end - left);
        list_rect = ImRect(left, y, end, bottom);
    } else if (dialog) {  // (the question, its choices in a column under it)
        float cw = std::min((end - left) * .55f, f * 26), cx = (left + end) * .5f;
        std::string question = view_title();
        float size = std::floor(f * 1.1f), y = top + pad;
        ImVec2 qs = font->CalcTextSizeA(size, FLT_MAX, cw, question.c_str());
        shadow_text(dl, size, ImVec2(cx - qs.x * .5f, y), k.glow, question.c_str(), cw);
        y += qs.y + pad * 1.5f;
        y += error_text(y, cx - cw * .5f, cw);
        list_rect = ImRect(cx - cw * .5f, y, cx + cw * .5f, bottom);
    } else {  // (Keybinds, Conflicts: the whole width)
        float y = top + error_text(top, left, end - left);
        list_rect = ImRect(left, y, end, bottom);
    }
    // The rows' list
    ImGui::SetCursorScreenPos(list_rect.Min);
    ImGui::BeginChild("##list", ImVec2(list_rect.GetWidth(), std::max(1.f, list_rect.GetHeight())),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetWindowFontScale(scale);
    auto *list = ImGui::GetCurrentWindow();
    s.list = list->ID;
    if (s.restore_scroll) {
        ImGui::SetScrollY(s.scroll);
        s.restore_scroll = false;
    }
    draw_rows(rows);
    if (s.view == View::Main && !side_help)
        for (const auto &note : page_notes()) {
            ImGui::PushStyleColor(ImGuiCol_Text, vec(note.second ? look.warn : k.dim));
            ImGui::TextWrapped("%s", note.first);
            ImGui::PopStyleColor();
        }
    ImGui::Dummy(ImVec2(1, pad));
    finger_scroll(list);
    scroll_mark(list);
    if (picking)  // (a window of its own, drawn over the menu's: dimmed on its own)
        list->DrawList->AddRectFilled(list->Pos, ImVec2(list->Pos.x + list->Size.x, list->Pos.y + list->Size.y),
                                      IM_COL32(0, 0, 0, 150));
    ImGui::EndChild();
    if (s.view == View::Main)
        touch_help(dl, help_rect, rows, side_help);
    if (later)
        later();
    commands(before);
    draw_frame(window, look);
    if (picking) {
        dl->PushClipRectFullScreen();
        dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0, 0, 0, 150));
        dl->PopClipRect();
    }
    ImGui::End();
    if (s.dropdown.open && !options.empty())
        touch_picker(options, scale);
    else
        s.dropdown.hovered = false;
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        s.swallow = false;  // (the press it was for has let go)
}
// What the other threads read of the menu, as its frame ends
void report(bool *open) {
    is_dirty = *open && (dirty() || pending_text());
    is_confirming = *open && confirmation();
    is_capturing = *open && s.view == View::KeyCapture;
    // (a click on the capture view's buttons is theirs; anywhere else it is the mouse button chosen - a finger's tap
    // is no mouse button)
    capture_mouse = is_capturing && s.capture_bind >= 0 && !s.pointing && !touch_ui();
    capture_wheel = is_capturing && s.capture_bind >= 0;  // (the capture view doesn't scroll)
    if (!*open)
        s.editing = false;
}
} // namespace
void settings_panel_request_close() { request_exit.store(true); }
void settings_panel_request_back() { request_back.store(true); }
bool settings_panel_dirty() { return is_dirty.load(); }
int settings_panel_theme() { return s.editing ? s.draft.menu_theme : config_editable().menu_theme; }
bool settings_panel_confirming() { return is_confirming.load() || request_exit.load(); }
bool settings_panel_key(int scancode) {
    if (!is_capturing.load())
        return false;
    captured_key = scancode;
    return true;
}
bool settings_panel_mouse(int button) {
    if (!capture_mouse.load() || button < 1 || button > SDL_BUTTON_X2)
        return false;
    captured_key = KEY_MOUSE + button;
    return true;
}
bool settings_panel_wheel(int notches) {
    if (!capture_wheel.load() || !notches)
        return false;
    captured_key = notches > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
    return true;
}
// Ownership has to precede NewFrame: native cancel/windowing runs there.
void settings_panel_before_frame(bool open) {
    if (!open && !request_exit.load())
        return;
    const ImGuiKey keys[] = {ImGuiKey_GamepadFaceRight,
                             ImGuiKey_GamepadFaceLeft,
                             ImGuiKey_GamepadFaceUp,
                             ImGuiKey_GamepadL1,
                             ImGuiKey_GamepadR1,
                             ImGuiKey_GamepadDpadUp,
                             ImGuiKey_GamepadDpadDown,
                             ImGuiKey_GamepadDpadLeft,
                             ImGuiKey_GamepadDpadRight,
                             ImGuiKey_GamepadLStickUp,
                             ImGuiKey_GamepadLStickDown,
                             ImGuiKey_GamepadLStickLeft,
                             ImGuiKey_GamepadLStickRight,
                             ImGuiKey_Escape,
                             ImGuiKey_AppBack,
                             ImGuiKey_UpArrow,
                             ImGuiKey_DownArrow,
                             ImGuiKey_LeftArrow,
                             ImGuiKey_RightArrow,
                             ImGuiKey_Tab,
                             ImGuiKey_Home,
                             ImGuiKey_End};
    for (ImGuiKey key : keys)
        ImGui::SetKeyOwner(key, 0x6d686775, ImGuiInputFlags_LockThisFrame);
    if (s.view == View::Keyboard) {
        ImGui::SetKeyOwner(ImGuiKey_Enter, 0x6d686775, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_Space, 0x6d686775, ImGuiInputFlags_LockThisFrame);
    }
}
void settings_panel(bool *open) {
    if (!*open) {
        s.editing = false;
        is_dirty = false;
        is_confirming = false;
        is_capturing = false;
        capture_mouse = false;
        return;
    }
    if (!s.editing) {
        s.draft = s.saved = config_editable();
        s.editing = true;
        s.view = View::Main;
        s.origins.clear();
        s.focus.clear();
        s.focus_request = true;
        s.error.clear();
        s.saved_notice = false;
        s.floating = false;
        s.advanced = false;
        s.dropdown.open = false;
        s.help_row.clear();
        s.drag = {};
        s.swallow = false;
        quest_keys();
        load_mods();
        load_cheats();
    }
    if (request_exit.exchange(false) && s.view != View::Exit)
        push(View::Exit, "dialog.cancel");
    Config before = s.draft;
#ifndef __ANDROID__
    s.touch = s.touch_next;  // (the layout follows what the last press came from, a frame later: the release that
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))  // tells went to what it pressed, in the layout it was made on)
        s.touch_next = ImGui::GetIO().MouseSource == ImGuiMouseSource_TouchScreen;
#endif
    if (touch_ui()) {
        touch_panel(open, before);
        report(open);
        return;
    }
    s.list = 0;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float scale = s.draft.menu_scale / 100.f;
    float base = ImGui::GetFontSize();
    float margin = std::min(16.f, vp->WorkSize.x * .025f);
    float width = std::min(vp->WorkSize.x - 2 * margin, base * scale * 56);
    bool sized = s.custom_size.x > 0;
    if (sized) {
        ImVec2 least(std::min(base * scale * 26, vp->WorkSize.x), std::min(base * scale * 14, vp->WorkSize.y));
        s.custom_size.x = std::clamp(s.custom_size.x, least.x, vp->WorkSize.x);
        s.custom_size.y = std::clamp(s.custom_size.y, least.y, vp->WorkSize.y);
        width = s.custom_size.x;
        ImGui::SetNextWindowSize(s.custom_size, ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, vp->WorkSize.y - 2 * margin));
    }
    mouse_drag(vp, width);
    if (s.view == View::Move) {
        float x =
            (direction(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight) ? 1 : 0) -
            (direction(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft) ? 1 : 0);
        float y =
            (direction(ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown) ? 1 : 0) -
            (direction(ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp) ? 1 : 0);
        auto *w = ImGui::FindWindowByName("Settings##main");
        float h = w ? w->Size.y : 200;
        s.move_position.x = std::clamp(s.move_position.x + x * 16, vp->WorkPos.x,
                                       vp->WorkPos.x + std::max(0.f, vp->WorkSize.x - width));
        s.move_position.y = std::clamp(s.move_position.y + y * 16, vp->WorkPos.y,
                                       vp->WorkPos.y + std::max(0.f, vp->WorkSize.y - h));
        s.position_override = true;
    }
    if (s.position_override && s.floating) {
        ImGui::SetNextWindowPos(s.move_position, ImGuiCond_Always);
        s.position_override = false;
    }
    if (!s.floating)
        ImGui::SetNextWindowPos(
            ImVec2(vp->WorkPos.x + (vp->WorkSize.x - width) * .5f, vp->WorkPos.y + margin), ImGuiCond_Always);
    const Theme &look = theme();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(look.panel));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(look.text));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(look.edge));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        look.bar ? 12.f : look.game ? 0.f : ImGui::GetStyle().WindowRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                        look.bar ? 1.5f : look.game ? 2.f : ImGui::GetStyle().WindowBorderSize);
    ImGui::Begin("Settings##main", nullptr,
                 (sized ? 0 : ImGuiWindowFlags_AlwaysAutoResize) | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize);
    ImGui::SetWindowFontScale(scale);
    if (s.restore_scroll) {
        ImGui::SetScrollY(s.scroll);
        s.restore_scroll = false;
    }
    back_keys(open);
    bool pinned_guides = s.view == View::Main;
    bool clipped_body = false;
    auto *window = ImGui::GetCurrentWindow();
    float spacing = ImGui::GetStyle().ItemSpacing.y;
    GuideLayout guides = guide_layout(ImGui::GetContentRegionAvail().x);
    // pinned above the guides: the help of the row focused or pointed at, with room for the longest of the tab
    float help_area = pinned_guides && s.help_height > 0 ? s.help_height + spacing * 2 : 0;
    s.guide_height = pinned_guides ? guides.height + help_area : 0;
    float guide_top =
        std::max(window->Pos.y + window->Size.y - ImGui::GetStyle().WindowPadding.y - guides.height,
                 window->InnerClipRect.Min.y);
    float help_top = std::max(guide_top - help_area, window->InnerClipRect.Min.y);
    if (pinned_guides)
        ImGui::PushClipRect(
            window->InnerClipRect.Min,
            ImVec2(window->InnerClipRect.Max.x, std::max(window->InnerClipRect.Min.y, help_top - spacing)), true);
    if (s.view == View::Main) {
        page_keys();
        float avail = ImGui::GetContentRegionAvail().x;
        float fw = ImGui::GetFontSize();
        ImVec2 scrolling_start = ImGui::GetCursorScreenPos();
        ImVec2 tabs_start(scrolling_start.x, window->Pos.y + ImGui::GetStyle().WindowPadding.y);
        ImGui::SetCursorScreenPos(tabs_start);
        float gutter = fw * 2.1f;
        float tabs_width = avail - gutter * 2;
        int columns = tabs_width / fw < 50 ? 4 : PAGES;
        float tabw = (tabs_width - ImGui::GetStyle().ItemSpacing.x * (columns - 1)) / columns;
        float tabh = fw * (columns == PAGES ? 2.7f : 2.1f);
        int lines = (PAGES + columns - 1) / columns;
        float header_height = tabh * lines + ImGui::GetStyle().ItemSpacing.y * (lines - 1);
        auto *header = ImGui::GetWindowDrawList();
        {  // (over the rows scrolled under the tabs; inside the border, with the window's rounded top corners - the
           // right one left to the window where the scrollbar is)
            float border = window->WindowBorderSize;
            bool scrollbar = window->ScrollbarY;
            header->AddRectFilled(
                ImVec2(window->Pos.x + border, window->Pos.y + border),
                ImVec2(scrollbar ? window->InnerRect.Max.x : window->Pos.x + window->Size.x - border,
                       tabs_start.y + header_height + ImGui::GetStyle().ItemSpacing.y),
                look.game ? look.header : ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK, std::max(0.f, window->WindowRounding - border),
                scrollbar ? ImDrawFlags_RoundCornersTopLeft : ImDrawFlags_RoundCornersTop);
        }
        SettingsIcons::draw(header, ImVec2(tabs_start.x, tabs_start.y + (header_height - fw) * .5f), fw,
                            s.draft.button_icons, SettingsIcons::Left, 0);
        SettingsIcons::draw(
            header, ImVec2(tabs_start.x + avail - fw * 1.5f, tabs_start.y + (header_height - fw) * .5f), fw,
            s.draft.button_icons, SettingsIcons::Right, 0);
        for (int i = 0; i < PAGES; i++) {
            if (i % columns)
                ImGui::SameLine();
            else
                ImGui::SetCursorPosX(tabs_start.x - ImGui::GetWindowPos().x + gutter);
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool active = s.page == i;
            ImGui::PushStyleColor(ImGuiCol_Button, vec(active ? look.tab_active : look.tab));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(look.row_hover));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, look.rounding);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::Button("###tab", ImVec2(tabw, tabh)) && !s.dragged)
                first_page(i);
            ImGui::PopItemFlag();
            ImGui::PopStyleVar();
            // A label wraps at its spaces; one word wider than the tab is drawn smaller instead of
            // being cut in two
            const char *name = tr(page_names[i]);
            float room = tabw - fw * .5f, size = fw;
            float word = ImGui::CalcTextSize(name).x;
            if (!strchr(name, ' ') && word > room) size = fw * room / word;
            auto label = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, room, name);
            // (Gold: the names centred, the other tabs dimmed, the active one underlined in its middle)
            bool named = look.bar || look.game;
            float indent = named ? (tabw - label.x) * .5f : fw * .25f;
            ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size,
                                                ImVec2(p.x + indent, p.y + (tabh - label.y) * .5f),
                                                named ? (active ? look.accent_bright : look.text_dim) : look.text,
                                                name, nullptr, room);
            float inset = named ? tabw * .18f : 0;
            if (active)
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + inset, p.y + tabh - 3),
                                                          ImVec2(p.x + tabw - inset, p.y + tabh), look.accent,
                                                          named && !look.game ? 2.f : 0.f);
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
        if (look.bar) {  // (Gold: a rule under the tabs, gold at the sides and fading out in the middle)
            float y = tabs_start.y + header_height + ImGui::GetStyle().ItemSpacing.y * .5f;
            float mid = (window->InnerRect.Min.x + window->InnerRect.Max.x) * .5f;
            ImU32 clear = look.accent & ~IM_COL32_A_MASK;
            header->AddRectFilledMultiColor(ImVec2(tabs_start.x, y), ImVec2(mid, y + 2), look.accent, clear, clear,
                                            look.accent);
            header->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(tabs_start.x + avail, y + 2), clear, look.accent,
                                            look.accent, clear);
        }
        ImGui::SetCursorScreenPos(
            ImVec2(scrolling_start.x, scrolling_start.y + header_height + ImGui::GetStyle().ItemSpacing.y));
        float body_top = tabs_start.y + header_height + ImGui::GetStyle().ItemSpacing.y;
        s.header_bottom = body_top - window->Pos.y;
        ImGui::PushClipRect(ImVec2(window->ClipRect.Min.x, std::min(body_top, window->ClipRect.Max.y)),
                            window->ClipRect.Max, true);
        clipped_body = true;
    } else
        ImGui::TextWrapped("%s", view_title().c_str());
    if (s.view == View::Keyboard) {
        keyboard_keys();
        if (s.view == View::Keyboard) {
            auto text = s.keyboard.buffer;
            text.insert(s.keyboard.cursor, "|");
            ImGui::TextWrapped("%s", text.c_str());
        }
    }
    auto rows = rows_for_view(open);
    std::vector<Row> options;  // (a dropdown open: the controller moves in it)
    if (s.dropdown.open)
        options = dropdown_rows();
    if (s.view == View::Move)
        focus("move.accept");
    else
        navigate(s.dropdown.open ? options : rows, s.dropdown.open);
    draw_rows(rows);
    float help_wrap = window->InnerRect.GetWidth() - 2 * ImGui::GetStyle().WindowPadding.x;
    std::string help;
    if (s.view == View::Main) {  // (the row moved to, or the one the mouse moved onto)
        static std::string last_focus;
        if (s.focus != last_focus)
            last_focus = s.help_row = s.focus;
        ImVec2 moved = ImGui::GetIO().MouseDelta;
        if ((moved.x || moved.y) && !s.hovered.empty())
            s.help_row = s.hovered;
        const std::string &id = s.dropdown.open ? s.dropdown.row : s.help_row;
        float tallest = 0;
        for (const auto &r : rows) {
            std::string text = help_text(r);
            if (!text.empty())
                tallest = std::max(tallest, ImGui::CalcTextSize(text.c_str(), nullptr, false, help_wrap).y);
            if (r.id == id)
                help = text;
        }
        s.help_height = tallest;
    } else
        s.help_height = 0;
    commands(before);
    if (!s.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, vec(look.warn));
        ImGui::TextWrapped("%s", s.error.c_str());
        ImGui::PopStyleColor();
    } else if (s.saved_notice)
        ImGui::TextUnformatted(saved_text());
    if (s.view == View::Main) {
        for (const auto &note : page_notes()) {
            if (note.second)
                ImGui::PushStyleColor(ImGuiCol_Text, vec(look.warn));
            ImGui::TextWrapped("%s", note.first);
            if (note.second)
                ImGui::PopStyleColor();
        }
        ImGui::TextUnformatted(tr("* Restart required"));
        // Keep scroll space for the pinned hints so the last action can scroll fully above them.
        ImGui::Dummy(ImVec2(1, guides.height + help_area + ImGui::GetStyle().ItemSpacing.y));
    }
    if (clipped_body)
        ImGui::PopClipRect();
    if (pinned_guides)
        ImGui::PopClipRect();
    if (pinned_guides && help_area > 0) {
        auto *draw = window->DrawList;
        float x0 = window->InnerRect.Min.x, x1 = window->InnerRect.Max.x, pad = ImGui::GetStyle().WindowPadding.x;
        draw->AddRectFilled(ImVec2(x0, help_top - spacing), ImVec2(x1, guide_top),
                            ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK);
        draw->AddLine(ImVec2(x0 + pad, help_top), ImVec2(x1 - pad, help_top), look.divider);
        if (!help.empty())
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x0 + pad, help_top + spacing),
                          look.text_dim, help.c_str(), nullptr, help_wrap);
    }
    if (pinned_guides && s.view == View::Main)
        draw_guides(window, guides, guide_top);
    if (s.view != View::Main)
        s.header_bottom = ImGui::GetStyle().WindowPadding.y + ImGui::GetFontSize() * 1.5f;
    draw_frame(window, look);
    draw_grip(window, look);
    ImGui::End();
    if (s.dropdown.open && s.dropdown.anchored && !options.empty())
        draw_dropdown(options, scale, base);
    else
        s.dropdown.hovered = false;
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    report(open);
}
