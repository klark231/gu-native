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
#include "platform.h"
#include "textinput.h"
#include "touch.h"
#include "imgui_internal.h"
#ifndef MHGU_VERSION
#define MHGU_VERSION ""  // (the build gives it: native/CMakeLists.txt)
#endif
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

#ifndef __ANDROID__
// (nvn/vulkan/device.c) the Vulkan devices that can run the game and the one in use, for the GPU row
extern "C" int vk_gpu_list(const char **names, int max, int *in_use);
#endif

namespace {
enum class View { Main, Keyboard, CloseChanges, Exit, Restart, Reset, Move, Conflicts, KeyCapture, Keybinds, TouchButtons, ComboEdit,
                  PadList, PadMap, ImportSave, Licenses, Remove, RoomBrowser, CitraBrowser, HubsMenu, RyuMenu, YuzuMenu,
                  CitraMenu, HubServers };
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
    bool secret = false;  // (a password: Android's keyboard types it without suggestions or learning)
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
    // A desktop's: the window's close button opened the menu for Exit game alone (leaving that dialog closes the menu);
    // what the player used last, read once a frame (overlay_last_input: the hints, the rows lit under the mouse); when
    // the menu, the view and the dropdown opened (their animation: anim_in)
    bool exit_only = false;
    int input = USED_KEYS;
    double opened_at = -1, view_at = -1, dropdown_at = -1;
    bool dropdown_drawn = false;  // (the dropdown's window was drawn last frame: closed since, it fades out)
    bool dropdown_up = false;     // (it opened above its row)
    // The phone's layout (touch_ui)
    bool touch = false;    // (not Android: the last press on the menu was a finger's - overlay.cpp marks them)
    bool touch_next = false;
    bool swallow = false;  // the press under way does nothing when it lets go (it closed the picker, began a drag)
    ImGuiID list = 0;      // the list of rows (a child window) this frame; 0: the menu itself scrolls (keyboard / mouse)
    Metrics m;
    Drag drag;
} s;
std::atomic<bool> request_exit, request_back, is_dirty, is_confirming, is_capturing, capture_mouse, capture_wheel;
std::atomic<int> request_view;  // (SETTINGS_VIEW_*: a desktop's dialog answered, settings_panel_request_view)
// (the key pressed while a key is being chosen, or KEY_MOUSE + a mouse button; -1: none yet)
std::atomic<int> captured_key{-1};
// (the draft's language while the menu edits it; before the menu first opens - the credits at the start - the saved one:
// the draft is still empty then, and the credits came out in English on a Spanish system)
const char *tr(const char *text) { return settings_text(s.editing ? s.draft.language : config_language(), text); }
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
// A touch screen for the touch controls: a phone's, or one SDL lists on a PC (most have none: the menu leaves out the
// rows that only act on one)
bool touch_screen() {
#ifdef __ANDROID__
    return true;
#else
    int n = 0;
    SDL_free(SDL_GetTouchDevices(&n));
    return n > 0;
#endif
}
// The touch controls' rows: with a touch screen, or with the controls shown anyway ([Touch] Controls On, as saved:
// their rows stay, to turn them off)
bool touch_rows() { return touch_screen() || s.saved.touch_controls == TOUCH_ON; }
// The menu's colours ([Application] Theme, applied as it is picked): Blue, the original look; Gold, a warm panel
// where rows light up with a gold bar at their left instead of a frame, and tabs that are only their names,
// underlined; and Hunter, after the game's own windows (the item pouch) - a maroon panel with a red and green rule at
// its top and bottom, gold corners, rows split by lines and the focused one filled gold (the one under the mouse a
// lighter gold).
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
                                 IM_COL32(176, 145, 24, 105),  IM_COL32(176, 145, 24, 235),  IM_COL32(0, 0, 0, 0),
                                 IM_COL32(0, 0, 0, 0),         IM_COL32(236, 212, 44, 255),  IM_COL32(255, 232, 84, 255),
                                 IM_COL32(242, 238, 232, 255), IM_COL32(196, 176, 166, 255), IM_COL32(132, 112, 106, 255),
                                 IM_COL32(255, 96, 72, 255),   IM_COL32(96, 88, 88, 255),    IM_COL32(56, 28, 27, 250),
                                 false,                        0.f,                          true,
                                 IM_COL32(108, 46, 46, 255)};
    return s.draft.menu_theme == THEME_BLUE ? blue : s.draft.menu_theme == THEME_HUNTER ? hunter : gold;
}
ImVec4 vec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
bool pressed(ImGuiKey key, bool repeat);
// A text's paragraphs joined into one line each, for the page to wrap at its own width: files broken at a fixed width (the
// legal notice, the project's license, the GNU licenses) came out with every other line cut short. Joined: a paragraph
// broken at a width (a line before its last one 60 characters or more) whose lines all start at the same indent, kept
// once; and a list whose items start at one indent with "- ", "* ", "1." or "a)" and go on further in, each item with its
// own lines. Anything else stays as it is: centred headings, addresses and signatures (short lines), columns (runs of
// spaces inside a line).
std::string reflow_prose(const std::string &in) {
    std::string out;
    std::vector<std::string> para;
    auto indent_of = [](const std::string &l) { return l.find_first_not_of(" \t"); };
    auto item_start = [](const std::string &l, size_t at) {
        if (at + 1 < l.size() && (l[at] == '-' || l[at] == '*') && l[at + 1] == ' ') return true;
        size_t k = at;
        while (k < l.size() && k - at < 3 && isalnum((unsigned char)l[k])) k++;
        return k > at && k + 1 < l.size() && (l[k] == '.' || l[k] == ')') && l[k + 1] == ' ';
    };
    auto columns = [](const std::string &l, size_t at) { return l.find("   ", at) != std::string::npos; };
    auto join = [](std::string &acc, const std::string &l, size_t from) {
        std::string t = l.substr(from);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!acc.empty() && acc.back() != ' ') acc += ' ';
        acc += t;
    };
    auto flush = [&]() {
        if (para.empty()) return;
        const size_t i0 = indent_of(para[0]), i1 = para.size() > 1 ? indent_of(para[1]) : i0;
        bool same = true, first_in = i1 < i0, wrapped = false, cols = false;
        for (size_t k = 0; k < para.size(); k++) {
            same &= indent_of(para[k]) == i0;
            if (k) first_in &= indent_of(para[k]) == i1;  // (only the first line further in: the GNU texts' paragraphs)
            wrapped |= k + 1 < para.size() && para[k].size() >= 60;
            cols |= columns(para[k], indent_of(para[k]));
        }
        std::vector<std::string> lines;
        if (para.size() > 1 && wrapped && !cols && (same || first_in)) {  // (prose, at the left edge or indented alike)
            lines.push_back(para[0].substr(0, i0));
            for (const auto &l : para) join(lines.back(), l, indent_of(l));
        } else if (para.size() > 1 && !cols && item_start(para[0], i0)) {  // (a list: items at i0, their lines further in)
            for (const auto &l : para) {
                const size_t at = indent_of(l);
                if (at == i0 && item_start(l, at)) lines.push_back(l.substr(0, at)), join(lines.back(), l, at);
                else if (at > i0) join(lines.back(), l, at);
                else { lines = para; break; }
            }
        } else {
            lines = para;
        }
        for (const auto &l : lines) out += l + '\n';
        para.clear();
    };
    size_t i = 0;
    while (i <= in.size()) {
        size_t e = in.find('\n', i);
        if (e == std::string::npos) e = in.size();
        std::string line = in.substr(i, e - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) {
            flush();
            if (e < in.size()) out += '\n';
        } else {
            if (line.size() > 64 && line.find_first_not_of('-') == std::string::npos)
                line.resize(64);  // (a rule between the notice's languages: one line on the page, not one and a bit)
            para.push_back(line);
        }
        if (e >= in.size()) break;
        i = e + 1;
    }
    flush();
    return out;
}
// The licenses' page (a desktop; Android: LicensesActivity): every text of the licenses folder beside the executable,
// one after the other, read once. The window scrolls with the wheel; arrows, page keys and the controller scroll it too.
// reserve: room left under it (the menu's guides)
void licenses_text(const Theme &look, float reserve = 0) {
    static std::vector<std::pair<std::string, std::string>> texts;  // (file, text)
    static bool loaded;
    if (!loaded) {
        loaded = true;
        char exe[1100] = "";
        if (plat_exe_path(exe, sizeof exe - 16)) {
            char *b = strrchr(exe, '\\'), *b2 = strrchr(exe, '/');
            if (b2 > b) b = b2;
            if (b) strcpy(b + 1, "licenses");
        }
        char dir[1200];
        if (!plat_path_to_utf8(exe, dir, sizeof dir)) snprintf(dir, sizeof dir, "%s", exe);
        int n = 0;
        char **names = SDL_GlobDirectory(dir, "*.txt", SDL_GLOB_CASEINSENSITIVE, &n);
        std::vector<std::string> sorted(names ? names : nullptr, names ? names + n : nullptr);
        SDL_free(names);
        // (the legal notice first, the project's license after it, then the rest by name)
        auto rank = [](const std::string &n) { return !SDL_strcasecmp(n.c_str(), "LEGAL.txt") ? 0 : !SDL_strcasecmp(n.c_str(), "LICENSE.txt") ? 1 : 2; };
        std::sort(sorted.begin(), sorted.end(), [&](const std::string &a, const std::string &b) {
            return rank(a) != rank(b) ? rank(a) < rank(b) : a < b;
        });
        for (const std::string &name : sorted) {
            size_t len = 0;
            char *text = (char *)SDL_LoadFile((std::string(dir) + "/" + name).c_str(), &len);
            if (text) texts.push_back({name.substr(0, name.size() - 4), reflow_prose(std::string(text, len))}), SDL_free(text);
        }
        if (texts.empty()) texts.push_back({"", tr("The licenses are in the licenses folder beside MHGU.exe.")});
    }
    // (a box of its own: the menu keeps its Close row in sight, the box scrolls on its own)
    float line = ImGui::GetTextLineHeightWithSpacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));  // (the menu's own colour behind the text)
    ImGui::BeginChild("##licenses", ImVec2(0, std::max(ImGui::GetContentRegionAvail().y - reserve, line * 20)));
    ImGui::PopStyleColor();
    if (pressed(ImGuiKey_DownArrow, true) || pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_GamepadLStickDown, true))
        ImGui::SetScrollY(ImGui::GetScrollY() + line * 3);
    if (pressed(ImGuiKey_UpArrow, true) || pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_GamepadLStickUp, true))
        ImGui::SetScrollY(ImGui::GetScrollY() - line * 3);
    if (pressed(ImGuiKey_PageDown, true) || pressed(ImGuiKey_GamepadR1, true))
        ImGui::SetScrollY(ImGui::GetScrollY() + ImGui::GetWindowHeight() * .8f);
    if (pressed(ImGuiKey_PageUp, true) || pressed(ImGuiKey_GamepadL1, true))
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetWindowHeight() * .8f);
    for (const auto &t : texts) {
        ImGui::Spacing();
        if (!t.first.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, vec(look.accent));
            ImGui::TextUnformatted(t.first.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, vec(look.text_dim));
        ImGui::TextWrapped("%s", t.second.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}
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
constexpr int PAGES = 9, MODS_PAGE = 5, CHEATS_PAGE = 6, APP_PAGE = 7, ABOUT_PAGE = 8;
const char *const page_names[PAGES] = {"Display", "Graphics & Audio", "Controls", "Network",
                                       "Misc",    "Mods",             "Cheats",   "Menu Options",
                                       "Credits & Licenses"};
// The thank-you shown once, after the credits of the first start (draw_thanks, further down), and on Credits & Licenses
constexpr const char *THANKS_TEXT =
    "To everyone who created this game, and to everyone who kept it alive over the years: thank you. This work is a love "
    "letter to the Monster Hunter series, with my deepest respect to the people who develop it.";
void thanks_show();
const char *thanks_signature();
bool thanks_on(double now);
void credits_show();  // (the credits shown when the app opens: draw_credits, further down)
// The program's name in the credits and on their page: "MHGU for Android", or on a desktop "MHGU for PC" in the menu's
// language
const char *app_title() {
#ifdef __ANDROID__
    return "MHGU for Android";
#else
    return tr("MHGU for PC");
#endif
}
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
// The guides at the menu's bottom: Navigation, Confirm, Back and Save (items: which of them, bit i). A controller's
// buttons only after a controller's press ([Application] ButtonIcons); else the keys - with the mouse used last, its
// wheel for Navigation and its click for Confirm
enum { GUIDE_NAV = 1, GUIDE_CONFIRM = 2, GUIDE_BACK = 4, GUIDE_SAVE = 8, GUIDE_ALL = 15 };
const char *const guide_labels[4] = {"Navigation", "Confirm", "Back", "Save"};
struct GuideLayout {
    ImVec2 offset[4];
    float height = 0;
    int items = GUIDE_ALL;
};
// Guide i's glyph at p (d: nullptr, only measured); its width
float guide_glyph(ImDrawList *d, ImVec2 p, float f, int i) {
    if (s.input == USED_PAD) {
        // (Save: the top face button, Y / X / triangle - greyed while there is nothing to save)
        const int icons[] = {SettingsIcons::Dpad, SettingsIcons::Confirm, SettingsIcons::Back, SettingsIcons::Save};
        if (d)
            SettingsIcons::draw(d, p, f, s.draft.button_icons, icons[i],
                                i == 3 ? 3
                                       : menu_confirm_position(config_live_menu_confirm(), overlay_face_position(0),
                                                               overlay_face_position(1), i == 2));
        return f;
    }
    if (i < 2 && s.input == USED_MOUSE)
        return d ? SettingsIcons::mouse(d, p, f, i == 0) : f;
    const char *key = i == 0 ? "" : i == 1 ? "Enter" : i == 2 ? "Esc" : "Ctrl+S";  // ("": the arrow keys)
    return d ? SettingsIcons::key(d, p, f, key) : SettingsIcons::key_width(f, key);
}
GuideLayout guide_layout(float width, int items = GUIDE_ALL) {
    GuideLayout layout;
    layout.items = items;
    float f = ImGui::GetFontSize(), x = 0, y = 0;
    for (int i = 0; i < 4; i++) {
        if (!(items >> i & 1))
            continue;
        float item_width = guide_glyph(nullptr, ImVec2(0, 0), f, i) + f * .4f + ImGui::CalcTextSize(tr(guide_labels[i])).x;
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
    float f = ImGui::GetFontSize();
    for (int i = 0; i < 4; i++) {
        if (!(layout.items >> i & 1))
            continue;
        ImVec2 pos(start.x + layout.offset[i].x, start.y + layout.offset[i].y);
        float glyph = guide_glyph(draw, pos, f, i);
        draw->AddText(ImVec2(pos.x + glyph + f * .4f, pos.y),
                      i == 3 && !dirty() ? theme().text_off : ImGui::GetColorU32(ImGuiCol_Text), tr(guide_labels[i]));
    }
}
bool confirmation() {
    return s.view == View::CloseChanges || s.view == View::Exit || s.view == View::Restart ||
           s.view == View::Reset || s.view == View::ImportSave || s.view == View::Remove;
}
// The guides a desktop's view shows at its bottom (0: none): all four on the tabs, Confirm and Back on a dialog, and
// Navigation too on a page of rows (the licenses' scroll: no Confirm there). None while a key is being chosen, text
// typed, the menu moved or a controller mapped, where those keys and buttons do something else
int view_guides() {
    switch (s.view) {
    case View::Main: return GUIDE_ALL;
    case View::KeyCapture: case View::Keyboard: case View::Move: case View::PadMap: return 0;
    case View::Licenses: return GUIDE_NAV | GUIDE_BACK;
    default: return confirmation() ? GUIDE_CONFIRM | GUIDE_BACK : GUIDE_NAV | GUIDE_CONFIRM | GUIDE_BACK;
    }
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
    SAME(shadows);
    SAME(bloom);
    SAME(reverb);
    SAME(adreno_fix);
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
    SAME(crossplay);
    SAME(ryu_host);
    SAME(ryu_port);
    SAME(room_host);
    SAME(room_port);
    SAME(sync_small_monsters);
    SAME(bonus_content);
    SAME(village_in_hub);
    SAME(hub_full_list);
    SAME(ldn_direct);
    SAME(mouse_sensitivity);
    SAME(touch_controls);
    SAME(touch_scheme);
    SAME(touch_hud);
    SAME(touch_auto_run);
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
    SAME(mouse_menus);
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
    if (memcmp(a.touch_show, b.touch_show, sizeof a.touch_show) || memcmp(a.touch_show_trad, b.touch_show_trad, sizeof a.touch_show_trad) || memcmp(a.touch_combo, b.touch_combo, sizeof a.touch_combo) ||
        memcmp(a.touch_combo_pad, b.touch_combo_pad, sizeof a.touch_combo_pad))
        return false;
    if (memcmp(a.binds, b.binds, sizeof a.binds))
        return false;
    if (strcmp(a.ldn_join, b.ldn_join) || strcmp(a.ldn_server, b.ldn_server) || strcmp(a.ldn_key, b.ldn_key) ||
        strcmp(a.chat_key, b.chat_key) || strcmp(a.gpu, b.gpu) || strcmp(a.ryu_servers, b.ryu_servers) || strcmp(a.ryu_password, b.ryu_password) ||
        strcmp(a.room_server, b.room_server) || strcmp(a.room_name, b.room_name) ||
        strcmp(a.room_password, b.room_password) || strcmp(a.web_api_url, b.web_api_url) ||
        strcmp(a.last_room, b.last_room) || strcmp(a.last_room_name, b.last_room_name) ||
        strcmp(a.citra_room_server, b.citra_room_server) || strcmp(a.citra_room_password, b.citra_room_password) ||
        strcmp(a.citra_web_api_url, b.citra_web_api_url) || strcmp(a.citra_web_api_url_2, b.citra_web_api_url_2) ||
        strcmp(a.citra_token, b.citra_token) || strcmp(a.last_citra_room, b.last_citra_room) ||
        strcmp(a.last_citra_room_name, b.last_citra_room_name))
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
// (the Network tab's submenus, further down)
bool xp_uses(int xp, int family);
std::string hubs_summary();
std::string ryu_summary();
std::string yuzu_summary();
std::string citra_summary();
double anim_now() { return SDL_GetTicksNS() / 1e9; }
void push(View view, const std::string &first) {
    s.dropdown.open = false;
    s.drag = {};
    s.origins.push_back({s.view, s.focus, list_scroll()});
    s.view = view;
    focus(first);
    s.error.clear();
    s.restore_scroll = true;
    s.scroll = 0;
    s.view_at = anim_now();
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
    s.view_at = anim_now();
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
// A view left without doing what it asks (Back: B, Esc, a click outside the menu; a dialog's Cancel or Close): each
// leaves things as they were, then the view before it shows. A desktop's Exit game that the window's close button asked
// for, the menu closed before: back to the game
void leave_view(bool *open) {
    if (s.view == View::Move) {
        s.move_position = s.move_start;
        s.position_override = true;
    }
    if (s.view == View::CloseChanges) touch_edit_cancel();  // (B / Esc / the chord: the menu stays, no editor)
#ifndef __ANDROID__
    if (s.view == View::ImportSave) pc_import_save_cancel();
    if (s.view == View::PadMap) padmap_cancel();  // (as its Cancel: or the controller stayed in the mapping, unread)
    bool to_game = s.view == View::Exit && s.exit_only;
#endif
    pop();
#ifndef __ANDROID__
    if (to_game)
        *open = false;
#endif
}
void ask_back(bool *open) {
    if (s.dropdown.open)
        close_dropdown();
    else if (s.view != View::Main)
        leave_view(open);
    else if (dirty())
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
              std::function<bool(const std::string &, std::string &)> commit, bool secret = false) {
    s.keyboard = {title, initial, initial, initial.size(), cap, false, false, std::move(commit), secret};
#ifdef __ANDROID__
    push(View::Keyboard, "key.accept");  // (the phone's own keyboard types: the page's first row is Accept)
#else
    push(View::Keyboard, "key.0");
#endif
}
// The clipboard: the text there, one line (a list copied from a chat comes with line breaks), or "" for none
std::string clipboard_text() {
    std::string t;
    if (char *clip = SDL_GetClipboardText()) {
        t = clip;
        SDL_free(clip);
    }
    for (char &c : t)
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
    while (!t.empty() && t.back() == ' ')
        t.pop_back();
    size_t lead = t.find_first_not_of(' ');
    return lead == std::string::npos ? std::string() : t.substr(lead);
}
void copy_text(const std::string &text) {
    if (SDL_SetClipboardText(text.c_str()))
        overlay_notice(tr("Copied to the clipboard"), "");
}
size_t utf8_count(const std::string &t) {
    size_t n = 0;
    for (unsigned char c : t)
        n += (c & 0xc0) != 0x80;
    return n;
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
    s.dropdown_at = anim_now();
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
    bool secret = key && (strstr(key, "Password") || strstr(key, "Token"));
    r.activate = [name, at, size, secret]() {
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
        }, secret);
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
// The hub servers (runtime/net/ldn.c lobby_servers_of): ini Server holds them in one text, separated by spaces or
// commas (empty: the defaults, "Off": none). The Hub servers view lists them one to a row, so one can be changed,
// removed or added without writing over the others.
constexpr size_t HUB_SERVERS_MAX = 8;
std::vector<std::string> hub_servers() {
    std::string t = *s.draft.ldn_server ? s.draft.ldn_server : LDN_DEFAULT_SERVER;
    std::vector<std::string> out;
    if (t.size() == 3 && (t[0] | 0x20) == 'o' && (t[1] | 0x20) == 'f' && (t[2] | 0x20) == 'f') return out;
    std::string cur;
    for (char ch : t + " ") {
        if (ch == ' ' || ch == ',' || ch == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else cur += ch;
    }
    return out;
}
// Stores the list as the ini text (the defaults as empty, none as "Off"); false (error set): not valid or too long
bool hub_servers_set(const std::vector<std::string> &list, std::string &error) {
    std::string t;
    for (const std::string &h : list) t += (t.empty() ? "" : " ") + h;
    if (t.empty()) t = "Off";
    else if (t == LDN_DEFAULT_SERVER) t.clear();
    if (t.size() >= sizeof s.draft.ldn_server) {
        error = tr("Too many hub servers: the list is too long.");
        return false;
    }
    Config temp = s.draft;
    SDL_strlcpy(temp.ldn_server, t.c_str(), sizeof temp.ldn_server);
    char why[512];
    if (!config_validate(&temp, why, sizeof why)) {
        error = settings_text(s.draft.language, why);
        return false;
    }
    s.draft = temp;
    return true;
}
// One typed server name: no blanks inside, not empty
bool hub_server_name(const std::string &typed, std::string &name, std::string &error) {
    size_t a = typed.find_first_not_of(" \t"), b = typed.find_last_not_of(" \t");
    name = a == std::string::npos ? "" : typed.substr(a, b - a + 1);
    if (name.empty() || name.find_first_of(" ,\t") != std::string::npos) {
        error = tr("Type one server name, such as host or host:port.");
        return false;
    }
    return true;
}
std::string hub_servers_summary() {
    std::vector<std::string> l = hub_servers();
    return l.empty() ? tr("Off") : l.size() == 1 ? l[0] : fmt("%d servers", (int)l.size());
}
void hub_server_rows(std::vector<Row> &r) {
    std::vector<std::string> list = hub_servers();
    if (list.empty()) header(r, "hs.none", tr("No hub server: only your local network is searched"));
    for (size_t i = 0; i < list.size(); i++) {
        std::string id = std::to_string(i);
        Row row;
        row.id = "hs." + id;
        row.title = fmt("Server %d", (int)i + 1);
        row.value = list[i];
        row.help = tr("Select to change this server's address. Leave a name starting with - to keep it in the list switched off.");
        row.activate = [i]() {
            keyboard(fmt("Server %d", (int)i + 1), hub_servers()[i], 120, [i](const std::string &typed, std::string &error) {
                std::vector<std::string> l = hub_servers();
                std::string name;
                if (i >= l.size() || !hub_server_name(typed, name, error)) return false;
                l[i] = name;
                return hub_servers_set(l, error);
            });
        };
        r.push_back(std::move(row));
        Row del;
        del.id = "hsd." + id;
        del.title = fmt("Remove server %d", (int)i + 1);
        del.action = true;
        del.help = tr("Takes this server out of the list. Hubs listed only there are no longer searched.");
        del.activate = [i]() {
            std::vector<std::string> l = hub_servers();
            std::string error;
            if (i < l.size()) l.erase(l.begin() + (long)i);
            hub_servers_set(l, error);
        };
        r.push_back(std::move(del));
    }
    action(r, "hs.add", tr("Add a server"), []() {
        keyboard(tr("New hub server"), "", 120, [](const std::string &typed, std::string &error) {
            std::vector<std::string> l = hub_servers();
            std::string name;
            if (l.size() >= HUB_SERVERS_MAX) {
                error = tr("At most 8 hub servers.");
                return false;
            }
            if (!hub_server_name(typed, name, error)) return false;
            l.push_back(name);
            return hub_servers_set(l, error);
        });
    }, hub_servers().size() < HUB_SERVERS_MAX);
    r.back().help = tr("Another hub server: hubs are listed on all of them and a search asks all of them.");
    action(r, "hs.paste", tr("Paste servers"), []() {
        std::string clip = clipboard_text(), error;
        std::vector<std::string> l = hub_servers();
        int added = 0;
        size_t i = 0;
        while (i < clip.size()) {  // (one or several, as the Server setting takes them: spaces or commas between)
            size_t j = clip.find_first_of(" ,;", i);
            std::string one = clip.substr(i, j == std::string::npos ? std::string::npos : j - i), name;
            i = j == std::string::npos ? clip.size() : j + 1;
            if (one.empty()) continue;
            if (l.size() >= HUB_SERVERS_MAX) { error = tr("At most 8 hub servers."); break; }
            if (!hub_server_name(one, name, error)) break;
            if (std::find(l.begin(), l.end(), name) == l.end()) l.push_back(name), added++;
        }
        if (clip.empty()) error = tr("The clipboard is empty.");
        if (added) {
            std::string why;
            if (!hub_servers_set(l, why)) error = why;
        }
        s.error = error;
    });
    r.back().help = tr("Adds the servers in the clipboard: one, or several separated by spaces or commas, as a friend sends them.");
    action(r, "hs.copy", tr("Copy servers"), []() {
        std::string all;
        for (const std::string &one : hub_servers())
            all += (all.empty() ? "" : " ") + one;
        copy_text(all);
    }, !hub_servers().empty());
    r.back().help = tr("Puts the list in the clipboard, to send it to a friend: Paste servers adds them on their phone.");
    action(r, "hs.reset", tr("Restore the default servers"), []() { s.draft.ldn_server[0] = 0; });
    r.back().help = tr("The two servers the game comes with.");
    action(r, "hs.back", tr("Back"), []() { pop(); });
}
// The keybinds' rows (config.h BIND_*): what each is called
const char *const bind_titles[] = {"Move forward", "Move back", "Move left", "Move right", "Walk", "Camera up",
                                   "Camera down", "Camera left", "Camera right", "A", "B", "X", "Y", "L", "R", "ZL",
                                   "ZR", "Plus", "Minus", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
                                   "Left stick click", "Right stick click", "Item left", "Item right",
                                   "Combined button 1", "Combined button 2", "Combined button 3", "Combined button 4",
                                   "Combined button 5", "Combined button 6"};
static_assert(std::size(bind_titles) == BINDS, "a title for each keybind");
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
// A keybind's row: its keys (red where one does something else too); Confirm sets a key, adds a second one or clears them.
// what: its help, instead of what the Keybinds view says of it
Row bind_row(int i, const char *what = nullptr) {
    Row row;
    row.id = "bind." + std::to_string(i);
    row.title = tr(bind_titles[i]);
    bool modifier = false;
    for (int k = 0; k < BIND_KEYS; k++) {
        int code = s.draft.binds[i][k];
        if (!code)
            continue;
        modifier |= code >= SDL_SCANCODE_LCTRL && code <= SDL_SCANCODE_RGUI && code != SDL_SCANCODE_LSHIFT &&
                    code != SDL_SCANCODE_RSHIFT;  // (host_input.c is_tap_mod: Shift is held, as for running)
        row.value += (row.value.empty() ? "" : ", ") + std::string(config_key_name(code));
        if (const char *use = key_use(code, i)) {
            row.value += std::string(" (") + tr("in use:") + " " + use + ")";
            row.warn = true;
        }
    }
    if (row.value.empty())
        row.value = tr("None");
    bool stick = i <= BIND_WALK || (i >= BIND_CAM_UP && i <= BIND_CAM_RIGHT);  // (held; the rest press something)
    if (i >= BIND_CAM_UP && i <= BIND_CAM_RIGHT)
        row.help = tr("Turns the camera as the right stick does, all the way. In the game's menus it is the right stick.");
#ifndef __ANDROID__  // (host_input.c mouse_a_ok; a phone's keyboard and mouse as before)
    if (i == BIND_A)
        row.help = tr("A mouse button here presses A outside the game's menus and talks only with the weapon out: it attacks, and never talks, opens, gathers or picks up - a key does that (F by default).");
#endif
    if (i >= BIND_COMBO1) {  // (host_input.c: the buttons of [Touch] Combo<n>)
        const char *combo = s.draft.touch_combo[i - BIND_COMBO1];
        row.help = *combo ? fmt("Presses %s together while held: the buttons of combined button %d, chosen in Controls.",
                                combo, i - BIND_COMBO1 + 1)
                          : fmt("Combined button %d has no buttons yet: choose them in Controls.", i - BIND_COMBO1 + 1);
    }
    if (what)
        row.help = what;
    if (modifier && !stick)  // (host_input.c mod_spent)
        row.help += (row.help.empty() ? "" : "\n") +
                    std::string(tr("Ctrl, Alt and the Windows key press it when tapped on their own: with another key, as in Alt+Tab or Alt+Enter, they press nothing."));
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
    return row;
}
// The Keybinds submenu: a row per keybind, the chat key
void keybind_rows(std::vector<Row> &r) {
    for (int i = 0; i < BINDS; i++) {
        if (i == BIND_MOVE_UP)
            header(r, "bind.movement", tr("Movement"));
        if (i == BIND_CAM_UP)
            header(r, "bind.camera", tr("Camera"));
        if (i == BIND_A)
            header(r, "bind.buttons", tr("Buttons"));
        if (i == BIND_ITEM_LEFT)
            header(r, "bind.items", tr("Item bar"));
        if (i == BIND_COMBO1)
            header(r, "bind.combos", tr("Combined buttons"));
        r.push_back(bind_row(i));
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
        SDL_strlcpy(c.gpu, f.gpu, sizeof c.gpu);
        c.low_end = f.low_end;
        c.shadows = f.shadows;
        c.bloom = f.bloom;
        c.reverb = f.reverb;
        c.adreno_fix = f.adreno_fix;
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
        c.touch_scheme = f.touch_scheme;
        c.touch_hud = f.touch_hud;
        c.touch_auto_run = f.touch_auto_run;
        c.touch_camera = f.touch_camera;
        c.touch_opacity = f.touch_opacity;
        c.touch_vibration = f.touch_vibration;
        c.phone_rumble = f.phone_rumble;
        c.mouse_menus = f.mouse_menus;
        c.lock_on = f.lock_on;
        memcpy(c.touch_show, f.touch_show, sizeof c.touch_show);
        memcpy(c.touch_show_trad, f.touch_show_trad, sizeof c.touch_show_trad);
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
        c.crossplay = f.crossplay;
        SDL_strlcpy(c.ryu_servers, f.ryu_servers, sizeof c.ryu_servers);
        c.ryu_host = f.ryu_host;
        c.ryu_port = f.ryu_port;
        SDL_strlcpy(c.ryu_password, f.ryu_password, sizeof c.ryu_password);
        SDL_strlcpy(c.room_server, f.room_server, sizeof c.room_server);
        c.room_host = f.room_host;
        c.room_port = f.room_port;
        SDL_strlcpy(c.room_name, f.room_name, sizeof c.room_name);
        SDL_strlcpy(c.room_password, f.room_password, sizeof c.room_password);
        SDL_strlcpy(c.web_api_url, f.web_api_url, sizeof c.web_api_url);
        SDL_strlcpy(c.citra_room_server, f.citra_room_server, sizeof c.citra_room_server);
        SDL_strlcpy(c.citra_room_password, f.citra_room_password, sizeof c.citra_room_password);
        SDL_strlcpy(c.citra_web_api_url, f.citra_web_api_url, sizeof c.citra_web_api_url);
        SDL_strlcpy(c.citra_web_api_url_2, f.citra_web_api_url_2, sizeof c.citra_web_api_url_2);
        SDL_strlcpy(c.citra_token, f.citra_token, sizeof c.citra_token);
        c.sync_small_monsters = f.sync_small_monsters;
        c.village_in_hub = f.village_in_hub;
        c.hub_full_list = f.hub_full_list;
        c.ldn_direct = f.ldn_direct;
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
            cheat_tab.draft[i] = !cheats_dropped(i);  // (a removed file's stay off)
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
#ifndef __ANDROID__
// A desktop's Cheats tab: the cheat files - what the cheats folders hold, and what the game loaded from them - each one
// with its cheats under its name and a Remove, as Android's import screen lists them; one added since the start has its
// cheats read at the next one
struct CheatFile {
    std::string name;    // on disk (UTF-8; "" once it is gone)
    std::string loaded;  // the game's name for it (cheats_file; "": not loaded)
};
std::vector<CheatFile> cheat_files;
std::string utf8(const char *native) {
    char out[600];
    return plat_path_to_utf8(native, out, sizeof out) ? out : native;
}
void load_cheat_files() {
    cheat_files.clear();
    std::vector<std::array<char, 256>> disk(256);
    int n = pc_cheat_files(reinterpret_cast<char(*)[256]>(disk.data()), (int)disk.size());
    std::vector<bool> used(n, false);
    for (int i = 0; i < cheats_count(); ++i) {  // (the loaded ones first, in the order the game read them)
        std::string loaded = cheats_file(i);
        bool known = cheats_dropped(i);
        for (const auto &f : cheat_files)
            known |= f.loaded == loaded;
        if (known)
            continue;
        CheatFile file{"", loaded};
        std::string name = utf8(loaded.c_str());
        for (int k = 0; k < n && file.name.empty(); ++k)  // (the game keeps 63 bytes of a longer name)
            if (!used[k] && (name == disk[k].data() || (loaded.size() == 63 && !strncmp(disk[k].data(), name.c_str(), name.size()))))
                file.name = disk[k].data(), used[k] = true;
        cheat_files.push_back(file);
    }
    for (int k = 0; k < n; ++k)
        if (!used[k])
            cheat_files.push_back({disk[k].data(), ""});
}
// The mods folder read again (a mod added): the list as it is now, the tab's unsaved order and switches kept for the
// mods still there, a new one joining at the end as it loads
void refresh_mods() {
    std::vector<ModEntry> now(MODS_MAX);
    now.resize(mods_installed(now.data(), MODS_MAX));
    if (mods_dirty()) {
        std::vector<ModEntry> next;
        for (const auto &m : mods.draft)
            for (const auto &k : now)
                if (!strcmp(k.name, m.name)) {
                    next.push_back(m);
                    break;
                }
        for (const auto &k : now) {
            bool known = false;
            for (const auto &m : next)
                known |= !strcmp(m.name, k.name);
            if (!known)
                next.push_back(k);
        }
        mods.draft = next;
    } else
        mods.draft = now;
    mods.saved = now;
    mods.scan.reset();
}
// What View::Remove asks about: a mod (mods_installed's name), or a cheat file (its name on disk and the game's)
struct Removal {
    bool mod = false;
    std::string name, loaded, shown;
    bool link = false;  // (a mod linked in from elsewhere: the link alone is deleted)
} removal;
void ask_remove(bool mod, const std::string &name, const std::string &loaded, const std::string &shown) {
    removal = {mod, name, loaded, shown, mod && pc_mod_is_link(name.c_str())};
    push(View::Remove, "dialog.cancel");
}
std::string removal_question() {
    const char *shown = removal.shown.c_str();
    bool used = removal.mod && mods_loaded(removal.name.c_str());
    if (removal.link)
        return fmt(used ? "Remove the mod %s? It is a link, and the game is using it: the link is deleted when the game "
                          "restarts, and the folder it points to is kept."
                        : "Remove the mod %s? It is a link: only the link is deleted from the mods folder, and the folder it "
                          "points to is kept.",
                   shown);
    if (removal.mod)
        return fmt(used ? "Remove the mod %s? The game is using it: its folder is deleted when the game restarts."
                        : "Remove the mod %s? Its folder is deleted from the mods folder.",
                   shown);
    return fmt(removal.loaded.empty() ? "Remove the cheat file %s? It is deleted from the cheats folder."
                                      : "Remove the cheat file %s? It is deleted from the cheats folder, and its cheats stop now.",
               shown);
}
// Remove, in View::Remove (runtime/pc_actions.c): the tabs drop it; a mod the game is using asks to restart
void remove_confirmed() {
    char error[700];
    if (removal.mod) {
        std::string name = removal.name;
        int done = pc_remove_mod(name.c_str(), error, sizeof error);
        if (!done) {
            s.error = error;
            return;
        }
        auto drop = [&name](std::vector<ModEntry> &list) {
            list.erase(std::remove_if(list.begin(), list.end(), [&name](const ModEntry &m) { return name == m.name; }),
                       list.end());
        };
        drop(mods.draft);
        drop(mods.saved);
        if (done == 1)
            drop(mods.boot);  // (not loaded: no restart waits for it)
        mods.scan.reset();
        pop();
        focus("mods.add");
        if (done == 2)  // (the game still has it: the restart takes it out)
            push(View::Restart, "dialog.cancel");
        return;
    }
    if (!pc_remove_cheat_file(removal.name.c_str(), removal.loaded.c_str(), error, sizeof error)) {
        s.error = error;
        return;
    }
    for (int i = 0; i < (int)cheat_tab.draft.size(); ++i)
        if (cheats_dropped(i))
            cheat_tab.draft[i] = cheat_tab.saved[i] = 0;
    load_cheat_files();
    pop();
    focus("cheats.add");
}
#endif
// A cheat's row: On / Off, or Always on (a master code)
Row cheat_row(int i) {
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
    return row;
}
void cheat_rows(std::vector<Row> &r) {
#ifdef __ANDROID__  // (the app's cheats folder is in Android/data, out of a file manager's reach: copied in by ImportActivity)
    action(r, "cheats.add", tr("Add cheats"), []() { ui_request_import(0); });
    r.back().help = tr("Copies cheat .txt files (Atmosphere's format, for 1.4.0) from your phone's storage. You can also put them yourself in the cheats folder inside your game folder. The game restarts to read them.");
    for (int i = 0; i < (int)cheat_tab.draft.size(); ++i)
        r.push_back(cheat_row(i));
#else  // (a desktop: the system's file dialog, runtime/pc_actions.c; each file under its name, with its Remove)
    action(r, "cheats.add", tr("Add cheats"), []() { ui_request_import(0); });
    r.back().help = tr("Copies cheat .txt files (Atmosphere's format, for 1.4.0) into the cheats folder inside your game folder, where you can also put them yourself. They start off: turn them on here. The game restarts to read them.");
    if (cheat_files.empty())
        action(r, "cheats.none", tr("No cheats found in the cheats folder"), []() {});
    for (const auto &f : cheat_files) {
        std::string shown = f.name.empty() ? utf8(f.loaded.c_str()) : f.name;
        header(r, "cheatfile." + shown, shown);
        for (int i = 0; i < (int)cheat_tab.draft.size(); ++i)
            if (!f.loaded.empty() && f.loaded == cheats_file(i) && !cheats_dropped(i))
                r.push_back(cheat_row(i));
        if (f.loaded.empty())
            action(r, "cheatnew." + shown, tr("Its cheats show up after a restart"), []() {}, false);
        action(r, "cheatrm." + shown, fmt("Remove %s", shown.c_str()), [f, shown]() { ask_remove(false, f.name, f.loaded, shown); });
        r.back().help = tr(f.loaded.empty() ? "Deletes this file from the cheats folder."
                                            : "Deletes this file from the cheats folder. Its cheats stop at once, no restart needed.");
    }
#endif
}
void mods_rows(std::vector<Row> &r) {
    if (!mods.scan)
        start_scan();
    auto result = mods.scan;  // (the scan thread fills it; `done` says when)
#ifdef __ANDROID__  // (as the cheats: copied into the app's folders by ImportActivity)
    action(r, "mods.add", tr("Add a mod"), []() { ui_request_import(1); });
    r.back().help = tr("Copies a mod's folder (with romfs or exefs in it) or its .zip from your phone's storage. You can also put it yourself in the mods folder inside your game folder. The game restarts to load it.");
    // (DLC: the game takes them only in its Download menu, which needs its online services, and stops with an error
    // without them, even with a connection said to be there. The rest is in place - services/dlc.c, ImportActivity -
    // and the row shows in a test run with MHGU_DLC=1)
    if (getenv("MHGU_DLC"))
        action(r, "dlc.add", tr("Add DLC"), []() { ui_request_import(2); });
    if (getenv("MHGU_DLC"))
        r.back().help = tr("Your own DLC of the game: each one's folder holds a DLC.dlcp. You can also put them yourself in the dlc folder inside your game folder. Then, in the game's main menu: Download. Experimental.");
#else  // (a desktop: the system's dialog for the mod's folder or its .zip, runtime/pc_actions.c)
    action(r, "mods.add", tr("Add a mod"), []() {});
    {
        Row &row = r.back();
        row.help = tr("Copies a mod's folder (with romfs or exefs in it), or unpacks its .zip, into the mods folder inside your game folder, where you can also put it yourself. The game restarts to load it.");
        Row copy = row;
        row.menu = true;
        row.activate = [copy]() {
            choices(copy, {tr("The mod's folder"), tr("The mod's .zip")}, -1, [](int v) {
                pc_mod_dialog(v == 1);
                ui_request_import(1);
            });
        };
    }
    if (mods.draft.empty())
        action(r, "mods.none", tr("No mods found in the mods folder"), []() {});
#endif
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
            std::vector<std::string> names = {tr(on ? "Turn off" : "Turn on"), tr("Move up"), tr("Move down")};
#ifndef __ANDROID__  // (a desktop removes it here, as Android's import screen does)
            names.push_back(tr("Remove"));
#endif
            choices(copy, names, -1, [set_on, move, on, name](int v) {
                if (v == 0)
                    set_on(!on);
                else if (v < 3)
                    move(v == 1 ? -1 : 1);
#ifndef __ANDROID__
                else
                    ask_remove(true, name, "", utf8(name.c_str()));
#endif
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
    // (none of these with Low-end mode: config.c. fxaa, FXAA's rows: a desktop's Low-end mode keeps them, only Toaster's
    // turns FXAA off)
    auto low_end_off = [&r](bool fxaa = false) {
#ifdef __ANDROID__
        (void)fxaa;
        if (!c.low_end) return;
#else
        if (!c.low_end || (fxaa && c.low_end != LOW_END_TOASTER)) return;
#endif
        r.back().enabled = false;
        r.back().value = tr("Off (low-end)");
    };
    // (the anti-aliasing rows while FSR runs: FSR at the next start turns them off. A desktop's Low-end mode keeps FXAA
    // and turns FSR off there, config.c, so with it they say what that start will have)
#ifdef __ANDROID__
    const bool fsr_next = c.fsr != 0;
#else
    const bool fsr_next = c.fsr && !c.low_end;
#endif
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
            static const int scales[] = {100, 90, 80, 75, 66, 50, 40, 33, 25};
            int pick = 0;
            for (int i = 1; i < (int)std::size(scales); i++)
                if (std::abs(scales[i] - c.render_scale) < std::abs(scales[pick] - c.render_scale))
                    pick = i;
            add_choice(
                r, "render.scale", "3D resolution", pick, {"100%", "90%", "80%", "75%", "66%", "50%", "40%", "33%", "25%"},
                [](int v) { s.draft.render_scale = scales[v]; }, ini("RenderScale"));
            if (scales[pick] != c.render_scale)  // (another value set in the file: shown as it is)
                r.back().value = std::to_string(c.render_scale) + "%";
#ifdef __ANDROID__
            r.back().help = tr("The size of the 3D picture, in percent of the screen's. Lower is faster and softer.");
#else  // (config.c: below 100, the HUD at the screen's size over the smaller picture, fsr.c's plain scaling; the menus'
       // models drawn after its start get none of the anti-aliasing, as with FSR: nvn/vulkan/exec.c models_aa)
            r.back().help = tr("The size of the 3D picture, in percent of the screen's. Lower is faster and softer; the HUD and the text stay sharp at full resolution. On a weak GPU this is what speeds the game up the most. Below 100%, the 3D models in the menus get no anti-aliasing.");
#endif
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
#ifndef __ANDROID__  // (the values by the menu's names: the ini calls 16:9 Center; a phone's: PHONE_HELP)
        r.back().help = tr("On a picture wider than 16:9, how wide the HUD is laid out: 16:9 in the middle, 21:9, 32:9, or Edges for the whole width.");
#endif
#ifndef __ANDROID__  // (a desktop's, with the mouse: on a phone the touch layout editor moves them)
        action(r, "hud.edit", tr("Move the HUD"), []() {
            touch_hud_edit_begin();  // (it starts once the menu is closed, as the touch layout editor: runtime/touch.cpp)
            request_back.store(true);
        });
        r.back().help = tr("Drag any part of the HUD with the mouse: the clock, health and stamina, the sharpness and gauges, the party, the map, the target camera, the item bar and the Hunter Arts. The wheel over one changes its size; Hide takes it off the screen. Esc cancels; on a controller, the button shown on Cancel.");
#endif
        add_bool(r, "fill", "Fill the screen", &c.fill_screen, ini("FillScreen"));
        break;
    case 1:
#ifndef __ANDROID__  // (a phone has one GPU)
        {  // [Display] GPU: Automatic, or a device by its name (nvn/vulkan/device.c pick_device), at the next start
            const char *names[16];
            int used = -1, n = vk_gpu_list(names, 16, &used), pick = 0;
            bool automatic = !*c.gpu || !SDL_strcasecmp(c.gpu, "Auto");
            std::vector<std::string> list = {tr("Automatic")};
            for (int i = 0; i < n; i++) {
                list.push_back(names[i]);
                if (!automatic && !pick && !SDL_strcasecmp(names[i], c.gpu))
                    pick = i + 1;
            }
            if (!automatic && !pick) {  // (one not here now, as the file says it: kept until another is picked)
                list.push_back(fmt("%s (not found)", c.gpu));
                pick = n + 1;
            }
            add_choice(
                r, "gpu", "GPU", pick, list,
                [list, n](int v) {
                    if (v <= n)
                        SDL_strlcpy(s.draft.gpu, v ? list[v].c_str() : "Auto", sizeof s.draft.gpu);
                },
                ini("GPU"));
            if (!pick && used >= 0 && used < n)  // (Automatic: the one it took)
                r.back().value = std::string(tr("Automatic")) + " (" + names[used] + ")";
            r.back().help = tr("The graphics card the game runs on. Automatic: a dedicated card if there is one.");
        }
#endif
        add_choice(
            r, "lowend", "Low-end mode", c.low_end < 0 || c.low_end > 2 ? 0 : c.low_end, {tr("Off"), tr("On"), tr("Toaster")},
            [](int v) {  // (it brings the resolution, the frame rate and the switches along; all can then be changed)
                Config &d = s.draft;
                int was = d.low_end;
                d.low_end = v;
#ifdef __ANDROID__
                if (v) {
                    int scale = v == LOW_END_TOASTER ? TOASTER_SCALE : LOW_END_SCALE;
                    if (was == LOW_END_TOASTER && d.render_scale == TOASTER_SCALE) d.render_scale = scale;  // (back to On)
                    d.render_scale = std::min(d.render_scale, scale);
                    d.fps = std::min(d.fps, LOW_END_FPS);
                    d.shadows = 0;
                    d.bloom = 0;
                    if (v == LOW_END_TOASTER) d.reverb = 0;
                    else if (was == LOW_END_TOASTER) d.reverb = 1;
                } else {
                    if (d.render_scale <= LOW_END_SCALE) d.render_scale = 100;
                    if (d.fps == LOW_END_FPS) d.fps = 60;
                    d.shadows = 1;
                    d.bloom = 1;
                    d.reverb = 1;
                }
#else  // (a desktop: the 3D picture about 720 lines tall, 540 with Toaster, whatever the screen (config_lowend_scale), and
       // Bloom off; Low-end keeps the frame rate and the shadows, Toaster sets 30 FPS and no shadows; the reverb stays.
       // Turned off, or from Toaster to On, what it lowered comes back where it still has the value it set)
                if (v == was)
                    return;
                int on = config_lowend_scale(&d, 1), toaster = config_lowend_scale(&d, LOW_END_TOASTER);
                // (an older PC build's switch was a phone's: RenderScale 40, Toaster 25, 30 FPS and no shadows, Toaster no
                // reverb either. A file it wrote keeps those, and leaving the mode takes them back too; 40 under Toaster is
                // On's, which Toaster's min kept. Under On, 40 is that only with its 30 FPS or no shadows still there: this
                // On sets neither, and 40% picked by hand stays)
                bool old = was == LOW_END_TOASTER ? (d.render_scale == TOASTER_SCALE || d.render_scale == LOW_END_SCALE)
                                                  : (was && d.render_scale == LOW_END_SCALE && (d.fps == LOW_END_FPS || !d.shadows));
                if (v) {
                    int scale = v == LOW_END_TOASTER ? toaster : on;
                    if (was == LOW_END_TOASTER && (d.render_scale == toaster || old)) d.render_scale = scale;  // (back to On)
                    d.render_scale = std::min(d.render_scale, scale);
                    d.bloom = 0;
                    if (v == LOW_END_TOASTER) {
                        d.fps = std::min(d.fps, LOW_END_FPS);
                        d.shadows = 0;
                    }
                } else {
                    if (old || d.render_scale == (was == LOW_END_TOASTER ? toaster : on)) d.render_scale = 100;
                    d.bloom = 1;
                }
                if (was == LOW_END_TOASTER || (old && !v)) {
                    if (d.fps == LOW_END_FPS) d.fps = 60;
                    d.shadows = 1;
                    if (old && was == LOW_END_TOASTER) d.reverb = 1;
                }
#endif
            },
            ini("LowEnd"));
#ifdef __ANDROID__
        r.back().help = tr("For weak phones: turns off bloom, depth of field and shadows, uses lighter textures and keeps the HUD sharp over a smaller 3D picture. It also sets the 3D resolution and the frame rate, and you can change all of them. Toaster: everything for speed and the battery before looks: 25%, 30 FPS, no reverb, and a lighter HUD and textures still.");
#else  // (a PC's GPU takes the game's textures as they are: none smaller, nvn/vulkan/textures.c; config.c turns MSAA and FSR off,
       // and with Toaster FXAA. The switch sets RenderScale for the Resolution the draft has, once: config.c ignores
       // RenderScale with a RenderResolution of its own)
        r.back().help = tr("For a weak GPU, such as integrated graphics: it sets the 3D resolution to about 720 lines for the screen resolution you have now (66% at 1080p, 50% at 1440p, 33% at 4K; with Render resolution on Auto), keeps the HUD and the text sharp, and turns off bloom, depth of field, MSAA and FSR. The frame rate, shadows, FXAA, texture filtering and reverb stay as they are. Toaster, for the weakest: about 540 lines, 30 FPS, no shadows or FXAA, and cheaper texture filtering. You can change the 3D resolution, the frame rate, shadows and bloom afterwards.");
#endif
        number(r, "fps", "Frame rate limit", &c.fps, (int)lo("FPS"), (int)hi("FPS"), 1, ini("FPS"));
#ifndef __ANDROID__  // (a desktop: past 240 comes Unlimited; a number in between, as the file may have it, stays a number)
        if (c.fps >= FPS_UNLIMITED)
            r.back().value = tr("Unlimited");
        r.back().adjust = [](int d) {
            int &v = s.draft.fps;
            if (d > 0)
                v = v >= FPS_PHONE_MAX ? FPS_UNLIMITED : std::min(FPS_PHONE_MAX, v + d);
            else if (d < 0)
                v = v > FPS_PHONE_MAX ? FPS_PHONE_MAX : std::max(30, v + d);
        };
        r.back().help = tr("The most frames a second. Unlimited: as many as the PC can draw, with VSync off; with VSync on, the monitor's refresh rate is the most. On a PC that can't hold 60, 30 is steadier: Toaster sets it, and Low-end mode leaves the frame rate as it is.");
#endif
        {  // left / right: one frame per second more or less; Confirm: the common ones
            static const int presets[] = {30, 60, 90, 120, 150, 240,
#ifndef __ANDROID__
                                          FPS_UNLIMITED,
#endif
            };
            r.back().menu = true;
            Row copy = r.back();
            r.back().activate = [copy]() {
                std::vector<std::string> names;
                int now = -1;
                for (int i = 0; i < (int)std::size(presets); i++) {
                    names.push_back(presets[i] >= FPS_UNLIMITED ? std::string(tr("Unlimited")) : std::to_string(presets[i]));
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
#ifdef __ANDROID__
        add_bool(r, "stats", "Show FPS and temperature", &c.show_stats, ini("ShowStats"));
#else  // (a desktop: Off, Basic (the file's On) or Full: runtime/perfstats.cpp, platform/win_stats.c)
        add_choice(r, "stats", "Show FPS and PC stats", c.show_stats, {tr("Off"), tr("Basic"), tr("Full")},
                   [](int v) { s.draft.show_stats = v; }, ini("ShowStats"));
        r.back().help = tr("Basic: frames per second, frame time, the longest frame and the GPU's temperature. Full adds a line on the GPU (use, temperature, clock, video memory) and one on the CPU (the game's use and memory, the CPU's clock, the time the game's and the render threads take per frame). The CPU's temperature is not shown: Windows lets no program read it without installing a driver.");
#endif
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
            if (fsr_next) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
        add_bool(r, "fxaa", "FXAA", &c.fxaa, ini("FXAA"));
        low_end_off(true);
        if (g_config.fsr) {
            r.back().cold = true;
            if (fsr_next) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_choice(r, "fxaamode", "FXAA mode", c.fxaa_mode, {tr("Original"), "3.11"}, [](int v) { c.fxaa_mode = v; },
                   ini("FXAAMode"));
        low_end_off(true);
        if (g_config.fsr) {
            r.back().cold = true;
            if (fsr_next) {
                r.back().enabled = false;
                r.back().value = tr("Off (FSR)");
            }
        }
#endif
        number(r, "drawdist", tr("Draw distance"), &c.draw_distance, lo("DrawDistance"), hi("DrawDistance"), .25f,
               ini("DrawDistance"));
        if (c.draw_distance == 0)
            r.back().value = tr("No limit");
        add_bool(r, "shadows", "Shadows", &c.shadows, ini("Shadows"));
        add_bool(r, "grass", "Grass", &c.grass, ini("Grass"));
        add_bool(r, "bloom", "Bloom and depth of field", &c.bloom, ini("Bloom"));
        if (c.fsr && !c.low_end) {  // (FSR finds the start of the HUD from these passes: lowend_skipped)
            r.back().enabled = false;
            r.back().value = tr("On (FSR)");
        }
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_bool(r, "smooth", "Smooth gradients", &c.smooth_gradients, ini("SmoothGradients"));
#endif
        number(r, "volume", "Volume (%)", &c.volume, (int)lo("Volume"), (int)hi("Volume"), 1, ini("Volume"));
        add_bool(r, "reverb", "Reverb", &c.reverb, ini("Reverb"));
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_bool(r, "mutebg", "Mute in background", &c.mute_in_background, ini("MuteInBackground"));
#endif
        add_bool(r, "fsr", "FSR", &c.fsr, ini("FSR"));
#ifndef __ANDROID__  // (measured on an RX 6800 XT: FSR 3.1 Quality's frame 3.7 ms, 1.8 to 2.3 without FSR and with FXAA)
        r.back().help = tr("Rebuilds the 3D picture with FSR's temporal upscaling and anti-aliasing. FSR costs GPU time of its own: about 2 ms a frame on a fast graphics card, more on integrated graphics. For speed on a weak GPU, lower the 3D resolution or use Low-end mode instead.");
#endif
        low_end_off();
        if (c.fsr && !c.low_end) {
#ifndef __ANDROID__  // (FSR 4: AMD's RX 9000 cards only; XeSS-SR: Windows, libxess.dll beside MHGU.exe)
            add_choice(
                r, "fsr.provider", "Upscaler", c.fsr_upscaler,
#ifdef _WIN32
                {"FSR 3.1", "FSR 4", "XeSS-SR"},
#else
                {"FSR 3.1", "FSR 4"},
#endif
                [](int v) { s.draft.fsr_upscaler = v; }, ini("Upscaler"));
#ifdef _WIN32
            r.back().help = tr("Which upscaler rebuilds the 3D picture. FSR 3.1 runs on most graphics cards. XeSS-SR, Intel's machine-learning upscaler, needs Intel Iris Xe or Arc graphics, or another card with DP4a (AMD Radeon RX 6000, NVIDIA GeForce RTX 3000 or newer), where it costs more GPU time than FSR 3.1. It also needs the Microsoft Visual C++ Redistributable (x64). When the one chosen can't run, the game uses FSR 3.1.");
#endif
#endif
            if (g_fsr4_fallback && c.fsr_upscaler == FSR_UP_4 && g_config.fsr && g_config.fsr_upscaler == FSR_UP_4) {
                r.back().value += std::string(" (") + tr("FSR 3.1 in use") + ")";  // (fsr4.c: FSR 4 didn't start)
                r.back().warn = true;
                r.back().help = tr(g_fsr4_fallback);
            }
#ifdef _WIN32
            // (xess.c: XeSS-SR didn't start, or stopped; it would not after a restart either)
            bool xess_out = g_xess_fallback && g_config.fsr && g_config.fsr_upscaler == FSR_UP_XESS;
            if (xess_out && c.fsr_upscaler == FSR_UP_XESS) {
                r.back().value += std::string(" (") + tr("FSR 3.1 in use") + ")";
                r.back().warn = true;
                r.back().help = tr(g_xess_fallback);
            }
#endif
            add_choice(
                r, "fsr.quality", "FSR quality", c.fsr_quality,
                {tr("Native AA"), tr("Quality"), tr("Balanced"), tr("Performance"), tr("Ultra performance")},
                [](int v) { s.draft.fsr_quality = v; }, ini("FSRQuality"));
#ifndef __ANDROID__  // (config.c: FSRQuality's ratios 1, 1.5, 1.7, 2 and 3)
            r.back().help = tr("How small FSR draws the 3D picture before rebuilding it. Native AA draws it at full size: it only improves the image, and costs more than FSR off. Quality, Balanced, Performance and Ultra performance draw it at 67%, 59%, 50% and 33%; FSR's own cost stays, so the 3D resolution alone at the same size is faster, though softer.");
#endif
            number(r, "fsr.sharpness", tr("Sharpness"), &c.fsr_sharpness, lo("Sharpness"), hi("Sharpness"), .05f, ini("Sharpness"));
#ifdef _WIN32
            if (c.fsr_upscaler == FSR_UP_XESS && !xess_out) {  // (XeSS-SR has no sharpening; FSR 3.1 in its place has)
                r.back().enabled = false;
                r.back().value = tr("Off (XeSS-SR)");
                r.back().help = tr("XeSS-SR has no sharpening filter: Sharpness is for FSR 3.1 and FSR 4.");
            }
#endif
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
        if (vk_gpu_adreno_fix_applies()) {  // (an Adreno on a Vulkan 1.1 driver: device.c msaa_floor)
            add_choice(r, "adrenofix", "Adreno fix", c.adreno_fix ? 1 : 0, {tr("Auto"), tr("Off")},
                       [](int v) { s.draft.adreno_fix = v; }, ini("AdrenoFix"));
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
        add_bool(r, "mouse.menus", "Mouse in the game's menus", &c.mouse_menus, ini("MouseMenus"));  // (hle/mousemenu.c)
#endif
        {  // (the touch controls' rows where there is a touch screen to work them; Lock-on is a controller's too)
            bool fingers = touch_rows();
            if (fingers) {
                add_choice(r, "touch", "Touch controls", c.touch_controls, {tr("Auto"), tr("On"), tr("Off")},
                           [](int v) { c.touch_controls = v; }, ini("Controls"));
                add_choice(r, "touch.scheme", "Touch layout", c.touch_scheme, {tr("Adaptive"), tr("Traditional")},
                           [](int v) { c.touch_scheme = v; }, ini("Layout"));
                r.back().help = tr("Adaptive: the buttons change with what you do: the menus' and the bows' and bowguns' own. Traditional: as an emulator's, the same buttons always: A B X Y, L R ZL ZR, + -, the d-pad and the stick. Each has a layout of its own in the editor.");
                add_bool(r, "touch.hud", "Move the HUD for touch", &c.touch_hud, ini("MoveHUD"));
                if (c.touch_scheme == TS_TRADITIONAL) {  // (the Adaptive layout has no R button: it always runs with the stick)
                    add_bool(r, "touch.autorun", "Run with the stick", &c.touch_auto_run, ini("AutoRun"));
                    r.back().help = tr("On: the thumb past the stick's rim runs. Off: the stick only walks, and the R button runs.");
                }
            }
            add_bool(r, "lock.on", "Lock-on", &c.lock_on, ini("LockOn"));
            r.back().help = tr("The target icon, or a controller's right stick click, keeps the camera on the monster as in the newer games, and bows and bowguns fire at it. Off: the icon turns the camera to it once, as in the original game.");
            if (fingers) {
                action(r, "touch.buttons", tr("Buttons on the screen"), []() { push(View::TouchButtons, s.draft.touch_scheme == TS_TRADITIONAL ? "tbt.0" : "tb.0"); });
                r.back().help = tr("Choose which of the game's buttons show on the screen, and add buttons of your own that press several at once.");
                number(r, "touch.camera", tr("Touch camera speed"), &c.touch_camera, lo("CameraSpeed"), hi("CameraSpeed"), .05f, ini("CameraSpeed"));
                number(r, "touch.opacity", tr("Touch controls opacity"), &c.touch_opacity, lo("Opacity"), hi("Opacity"), .05f, ini("Opacity"));
            } else {  // (a controller's button or a key presses them: that view's combined buttons alone)
                action(r, "touch.combos", tr("Combined buttons"), []() { push(View::TouchButtons, "tc.0"); });
                r.back().help = tr("Buttons of your own that press several of the game's at once: on a controller's button, or on a key in Keybinds.");
            }
#ifdef __ANDROID__  // (the phone's vibrator: elsewhere runtime/haptics.c does nothing)
            add_bool(r, "touch.vibration", "Vibrate on touch", &c.touch_vibration, ini("Vibration"));
            add_bool(r, "phone.rumble", "Game rumble on the phone", &c.phone_rumble, ini("PhoneRumble"));
#endif
            if (touch_screen()) {  // (the layout editor is worked by fingers)
                action(r, "touch.edit", tr("Customize controls and HUD"), []() {
                    touch_edit_begin();  // (the editor starts once the menu is closed: one back, or the unsaved-changes dialog)
                    request_back.store(true);
                });
                r.back().help = tr("Move and resize each button on the screen and each part of the game's HUD: drag it, pinch or use - / + for its size, hide a part, then Save. HUD: controller edits the HUD shown without the touch controls.");
            }
        }
        action(r, "pad.map", tr("Map a controller"), []() { push(View::PadList, "pad.0"); });
        r.back().help = tr("For a controller the game does not read well: press each button once and it works from then on.");
#ifndef __ANDROID__
        action(r, "keybinds", tr("Keybinds"), []() { push(View::Keybinds, "bind.0"); });
        r.back().help = tr("The keys and mouse buttons for the Switch controller's buttons, for moving, the camera, the item bar and the combined buttons, and the chat key.");
#endif
        break;
    case 3:
        header(r, "net.header", tr("Local play and hubs"));
        number(r, "port", "Host port (TCP / UDP)", &c.ldn_port, (int)lo("Port"), (int)hi("Port"), 1, ini("Port"));
        text(r, "hosts", "Search hosts", c.ldn_join, sizeof c.ldn_join, *c.ldn_join ? c.ldn_join : tr("LAN"), ini("Join"));
        action(r, "hubs", tr("Internet hubs"), []() { push(View::HubsMenu, "server"); });
        r.back().value = hubs_summary();
        r.back().help = tr("Hubs over the internet through a hub server: the server, the matchmaking key, and a check that it answers.");
        header(r, "xp.header", tr("Crossplay"));
        add_choice(r, "crossplay", "Crossplay", c.crossplay, {tr("Off"), tr("Ryujinx"), tr("Yuzu Room"), tr("Mixed"), tr("Citra Room"), tr("All Rooms")},
                   [](int v) { s.draft.crossplay = v; }, ini("Crossplay"));
        r.back().help = tr("Play with console emulators: Ryujinx (RyuLdn servers), Yuzu-family rooms (Eden, Suyu, Yuzu, Citron), or both at once (Mixed); or Citra Room: Azahar / Citra rooms, to play with Monster Hunter XX on the 3DS. All Rooms: a hub you host is on the LAN / hub server, in the Yuzu room and in the Citra room at once, and this device passes everything between them. Small monster sync and HP scaling are off in those hubs.");
        // (one row for each family of rooms the mode uses, its settings behind it)
        if (xp_uses(c.crossplay, XP_ROOM) || xp_uses(c.crossplay, XP_CITRA)) {
            text(r, "room.name", "Room nickname", c.room_name, sizeof c.room_name, std::string(c.room_name), ini("RoomName"));
            r.back().help = tr("Your name in the room's member list, in Yuzu Rooms and in Citra Rooms (your hunter's name in the game doesn't change). 4 to 20 letters, digits, spaces, dots, dashes or underscores; if someone in the room has it already, a number is added for that room.");
        }
        if (xp_uses(c.crossplay, XP_RYU)) {
            action(r, "ryu", tr("Ryujinx (RyuLdn)"), []() { push(View::RyuMenu, "ryu.servers"); });
            r.back().value = ryu_summary();
            r.back().help = tr("The RyuLdn servers Ryujinx players use, or one run on this device.");
        }
        if (xp_uses(c.crossplay, XP_ROOM)) {
            action(r, "yuzu", tr("Yuzu Room"), []() { push(View::YuzuMenu, "room.host"); });
            r.back().value = yuzu_summary();
            r.back().help = tr("Eden, Suyu, Yuzu and Citron players' rooms: run one on this device, or join one.");
        }
        if (xp_uses(c.crossplay, XP_CITRA)) {
            action(r, "citra", tr("Citra Room"), []() { push(View::CitraMenu, "citra.rooms"); });
            r.back().value = citra_summary();
            r.back().help = tr("Azahar / Citra rooms, where Monster Hunter XX players are: the room browser, the room and its password.");
        }
        header(r, "hub.header", tr("In the hub"));
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
        add_choice(r, "language", "Language", c.language, {"English", "Español", "中文", tr("Auto (phone language)")},
                   [](int v) { c.language = v; }, ini("Language"));
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_choice(r, "theme", "Menu theme", c.menu_theme, {tr("Gold"), tr("Blue"), tr("Hunter")},
                   [](int v) { c.menu_theme = v; }, ini("Theme"));
#endif
        number(r, "scale", "Interface scale (%)", &c.menu_scale, (int)lo("MenuScale"), (int)hi("MenuScale"), 10, ini("MenuScale"));
        add_bool(r, "pause.bg", "Pause in the background", &c.pause_background, ini("PauseInBackground"));
#ifndef __ANDROID__  // (a desktop's background: the window minimized, host_input.c)
        r.back().help = tr("The game stops while its window is minimized, and goes on where it was when you bring it back.");
#endif
#ifndef __ANDROID__  // (a phone: not worth a row)
        add_bool(r, "confirm.exit", "Confirm exit", &c.confirm_exit, ini("ConfirmExit"));
#endif
#ifdef MHGU_UPDATER  // (MHGURecomp's updater of its PC releases: not part of this project, so nothing would read it)
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
#else  // (a desktop: the system's dialogs, runtime/pc_actions.c; the licenses are on Credits & Licenses)
        action(r, "game.folder", tr("Game folder"), []() { ui_request_game_folder(); });
        r.back().help = tr("Where the game's exefs, romfs and mhgugame.dll are read from. A new folder is used the next time the game starts.");
        action(r, "save.import", tr("Import save"), []() { ui_request_save_import(); });
        r.back().help = tr("Brings your save from an emulator or another device: choose its file named system. It replaces the characters here, and the current save is kept in a copy.");
        action(r, "save.export", tr("Export save"), []() { ui_request_save_export(); });
        r.back().help = tr("Copies your save to a folder you choose, to take it to an emulator or another device.");
        action(r, "log.open", tr("Log folder"), []() { ui_request_share_log(); });
        r.back().help = tr("Opens the folder with log.txt, the game's log, to send it to whoever is helping you with a problem.");
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
    case ABOUT_PAGE: // (who made it, where the official one is, the licences and the legal notice)
        header(r, "about.head", std::string(app_title()) + " " + MHGU_VERSION);
        action(r, "credits", tr("Show the credits"), []() { credits_show(); });
        r.back().help = tr("Made by khorne-hub. Free. Only the links khorne-hub gives are official: if you got it anywhere else, paid for it, or it came with ads, it is not the official one. Official page: github.com/khorne-hub/gu-native. Based on MHGURecomp. Also by khorne-hub: MH4U for Android, github.com/khorne-hub/4u-native.");
#ifdef __ANDROID__
        action(r, "licenses", tr("Licenses"), []() { ui_request_licenses(); });
        r.back().help = tr("The licenses of the software this app is made with, and the legal notice: this app includes no game code, files, ROMs or keys, you need your own copy of the game, and it is not affiliated with the Great C or the Great N.");
#else  // (a desktop: the menu's own page, the legal notice first: licenses_text)
        action(r, "licenses", tr("Licenses"), []() { push(View::Licenses, "dialog.cancel"); });
        r.back().help = tr("The legal notice and the licenses of the software this program is made with: it includes no game code, files, ROMs or keys, you need your own copy of the game, and it is not affiliated with the Great C or the Great N.");
#endif
        header(r, "about.thanks", tr("Thank you"));  // (its text: page_notes)
        action(r, "thanks", tr("Show the thanks"), []() { thanks_show(); });
        r.back().help = tr("Shows the thank-you again, as it showed the first time the game opened.");
        break;
    }
    if (!touch_ui() || SDL_HasGamepad())  // (a finger has the header's Save; a controller can't reach that one)
        action(r, "save", tr("Save"), []() { s.command = Command::Save; }, dirty());
    if (s.page != ABOUT_PAGE) // (Credits & Licenses: nothing to reset)
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
// ---- the Network tab's submenus: a family of rooms each (and the internet hubs)
// Whether crossplay mode xp uses the rooms of family XP_RYU, XP_ROOM (Yuzu) or XP_CITRA.
bool xp_uses(int xp, int family) {
    switch (family) {
    case XP_RYU: return xp == XP_RYU || xp == XP_MIXED;
    case XP_ROOM: return xp == XP_ROOM || xp == XP_MIXED || xp == XP_ALL;
    case XP_CITRA: return xp == XP_CITRA || xp == XP_ALL;
    default: return false;
    }
}
std::string room_state_text(int kind) {  // (what the saved settings are doing now, ldn_room_state)
    switch (ldn_room_state(kind)) {
    case ROOM_IN: return std::string(" · ") + tr("In the room");
    case ROOM_JOINING: return std::string(" · ") + tr("Joining...");
    default: return "";
    }
}
std::string hubs_summary() {
    const Config &c = s.draft;
    return hub_servers_summary() + " · " + (*c.ldn_key ? std::string(c.ldn_key) : std::string(tr("Public")));
}
std::string ryu_summary() {
    const Config &c = s.draft;
    if (*c.ryu_servers) return c.ryu_servers;
    return c.ryu_host ? fmt("This device, port %d", c.ryu_port) : std::string(tr("None"));
}
std::string yuzu_summary() {
    const Config &c = s.draft;
    std::string where = c.room_host ? fmt("This device, port %d", c.room_port) : *c.room_server ? std::string(c.room_server) : std::string(tr("None"));
    return where + (c.room_host || *c.room_server ? room_state_text(ROOMS_YUZU) : "");
}
std::string citra_summary() {
    const Config &c = s.draft;
    if (!*c.citra_room_server) return tr("None");
    return std::string(*c.last_citra_room_name && !strcmp(c.last_citra_room, c.citra_room_server) ? c.last_citra_room_name : c.citra_room_server) +
           room_state_text(ROOMS_CITRA);
}
void hubs_rows(std::vector<Row> &r) {
    Config &c = s.draft;
    // (each hub server on a row of its own, in the Hub servers view: ini Server keeps them as one list)
    action(r, "server", tr("Hub servers"), []() { push(View::HubServers, "hs.0"); });
    r.back().value = hub_servers_summary();
    r.back().help = tr("The servers that list internet hubs. Select to change, remove or add servers.");
    keyed(r.back(), ini("Server"));
    text(r, "key", "Matchmaking key", c.ldn_key, sizeof c.ldn_key, *c.ldn_key ? c.ldn_key : tr("Public"), ini("Key"));
    hub_check(r);
    add_bool(r, "fullhubs", "Show all hubs", &c.hub_full_list, ini("FullHubList"));
    r.back().help = tr("Join Hub: each search asks every hub server several times and merges the answers, so all the hubs are in the lists (the numbered buttons) from the first search, without pressing Retry. A little more network use.");
    add_bool(r, "direct", "Direct connections", &c.ldn_direct, ini("Direct"));
    r.back().help = tr("In an internet hub, your game and the host's send their packets straight to each other when both routers let them, for a lower ping; else through the hub server. The other player's game then sees your IP address. Off: always through the hub server.");
}
void ryu_rows(std::vector<Row> &r) {
    Config &c = s.draft;
    text(r, "ryu.servers", "RyuLdn servers", c.ryu_servers, sizeof c.ryu_servers,
         *c.ryu_servers ? std::string(c.ryu_servers) : std::string(tr("None")), ini("RyuServers"));
    text(r, "ryu.password", "RyuLdn passphrase", c.ryu_password, sizeof c.ryu_password,
         *c.ryu_password ? std::string("********") : std::string(tr("None")), ini("RyuPassword"));
    header(r, "ryu.here", tr("Run a server on this device"));
    add_bool(r, "ryu.host", "Run a RyuLdn server", &c.ryu_host, ini("RyuHost"));
    number(r, "ryu.port", "RyuLdn port", &c.ryu_port, (int)lo("RyuPort"), (int)hi("RyuPort"), 1, ini("RyuPort"));
}
void yuzu_rows(std::vector<Row> &r) {
    Config &c = s.draft;
    header(r, "room.here", tr("Run a room on this device"));
    add_bool(r, "room.host", "Run a Yuzu Room", &c.room_host, ini("RoomHost"));
    r.back().help = tr("Eden / Suyu players join this device's address and port. With All Rooms, this is the way that lets them meet your LAN, hub and MHXX players too.");
    number(r, "room.port", "Yuzu Room port", &c.room_port, (int)lo("RoomPort"), (int)hi("RoomPort"), 1, ini("RoomPort"));
    text(r, "room.password", "Yuzu Room password", c.room_password, sizeof c.room_password,
         *c.room_password ? std::string("********") : std::string(tr("None")), ini("RoomPassword"));
    header(r, "room.join", tr("Or join one"));
    const bool here = c.room_host != 0;  // (a room runs here: joining another one does nothing, so those rows are dim)
    action(r, "rooms", tr("Yuzu Room Browser"), []() {
        webrooms_refresh(s.draft.web_api_url);
        push(View::RoomBrowser, "rb.refresh");
    }, !here);
    r.back().help = tr("Lists the public Yuzu-family rooms (Eden, Suyu) from the Yuzu Web API. Pick one to join it.");
    text(r, "room.server", "Yuzu Room server", c.room_server, sizeof c.room_server,
         here ? std::string(tr("This device")) : *c.room_server ? std::string(c.room_server) : std::string(tr("None")), ini("RoomServer"));
    if (here) r.back().enabled = false;
    header(r, "room.adv", tr("Advanced"));
    text(r, "webapi", "Yuzu Web API URL", c.web_api_url, sizeof c.web_api_url,
         *c.web_api_url ? std::string(c.web_api_url) : std::string(tr("None")), ini("WebApiUrl"));
}
void citra_rows(std::vector<Row> &r) {
    Config &c = s.draft;
    action(r, "citra.rooms", tr("Citra Room Browser"), []() {
        webrooms_refresh_kind(WEBROOMS_CITRA, s.draft.citra_web_api_url, s.draft.citra_web_api_url_2);
        push(View::CitraBrowser, "rb.refresh");
    });
    r.back().help = tr("Lists the public Azahar / Citra rooms, where Monster Hunter XX players are. Pick one to join it; most players never need the rows below.");
    text(r, "citra.server", "Citra Room server", c.citra_room_server, sizeof c.citra_room_server,
         *c.citra_room_server ? std::string(c.citra_room_server) + room_state_text(ROOMS_CITRA) : std::string(tr("None")), ini("CitraRoomServer"));
    text(r, "citra.password", "Citra Room password", c.citra_room_password, sizeof c.citra_room_password,
         *c.citra_room_password ? std::string("********") : std::string(tr("None")), ini("CitraRoomPassword"));
    if (*c.citra_room_server) {
        action(r, "citra.leave", tr("Leave the room"), []() {
            Config &d = s.draft;
            d.citra_room_server[0] = 0;  // (saved at once, as joining one from the browser is)
            s.command = Command::Save;
        });
        r.back().help = tr("Leaves the Citra room and forgets it. The room browser finds it again.");
    }
    header(r, "citra.adv", tr("Advanced"));
    text(r, "citra.webapi", "Citra Web API URL", c.citra_web_api_url, sizeof c.citra_web_api_url,
         *c.citra_web_api_url ? std::string(c.citra_web_api_url) : std::string(tr("None")), ini("CitraWebApiUrl"));
    text(r, "citra.webapi2", "Citra Web API URL 2", c.citra_web_api_url_2, sizeof c.citra_web_api_url_2,
         *c.citra_web_api_url_2 ? std::string(c.citra_web_api_url_2) : std::string(tr("None")), ini("CitraWebApiUrl2"));
    text(r, "citra.token", "Citra Token", c.citra_token, sizeof c.citra_token,
         *c.citra_token ? std::string("********") : std::string(tr("None")), ini("CitraToken"));
}
#ifndef __ANDROID__
// The player opened the menu: the notice at the start that says how (overlay.cpp menu_hint) is done with.
// [Application] MenuHintSeen goes into the file now, with the settings as saved: the ones about to be edited are saved
// or not later, as the player chooses.
void menu_hint_seen() {
    Config c = s.saved;
    c.menu_hint_seen = 1;
    char error[512];
    if (!config_save(&c, error, sizeof error)) {
        fprintf(stderr, "[settings] MenuHintSeen could not be saved: %s\n", error);
        return;
    }
    s.draft.menu_hint_seen = s.saved.menu_hint_seen = 1;
    fprintf(stderr, "[settings] the menu was opened: MenuHintSeen = On, no notice at the next start\n");
}
#endif
// Which of the game's buttons are on the screen, and the player's own
const char *const TB_TITLES[TB_COUNT] = {"Attacks", "Dodge (B)", "Guard (R)", "Sheathe (Y)", "Game menu (+)", "Minus (-)", "D-pad",
                                         "L", "ZL", "ZR", "Left stick click (L3)", "Right stick click (R3)", "Movement stick",
                                         "Layout for bows and bowguns", "Fire / shoot", "Reload / melee", "Aim",
                                         "Crouching fire / coating", "Ammo arrows", "Item arrows", "ZL in menus", "ZR in menus",
                                         "Menu pad"};
const char *const TB_KEYS[TB_COUNT] = {"ShowAttacks", "ShowDodge", "ShowGuard", "ShowSheathe", "ShowPlus", "ShowMinus", "ShowDpad",
                                       "ShowL", "ShowZL", "ShowZR", "ShowL3", "ShowR3", "ShowStick", "ShowRanged", "ShowFire",
                                       "ShowSecond", "ShowAim", "ShowRangedSpecial", "ShowAmmo", "ShowItemArrows", "ShowMenuZL",
                                       "ShowMenuZR", "ShowMenuPad"};
int s_combo;  // the combined button being edited
void touch_button_rows(std::vector<Row> &r) {
    Config &c = s.draft;
    bool fingers = touch_rows();  // (none: the combined buttons alone, for a controller's button or a key)
    if (fingers) {
        header(r, "tb.head", tr("Buttons on the screen"));
        if (c.touch_scheme == TS_TRADITIONAL)  // (the layout chosen in Controls; each keeps its own set, every Traditional button on by default)
            for (int i = 0; i <= TB_STICK; i++) {
                add_bool(r, ("tbt." + std::to_string(i)).c_str(), TB_TITLES[i], &c.touch_show_trad[i], nullptr);
                r.back().key = std::string("Trad") + TB_KEYS[i];
            }
        else
        for (int n = 0; n < TB_COUNT; n++) {  // (the menu pad before the tabs that go with it)
            int i = n < TB_MENU_ZL ? n : n == TB_MENU_ZL ? TB_MENU_PAD : n - 1;
            add_bool(r, ("tb." + std::to_string(i)).c_str(), TB_TITLES[i], &c.touch_show[i], nullptr);
            r.back().key = TB_KEYS[i];
            if (i == TB_MINUS)  // (both platforms: the default ini's says only what the buttons are)
                r.back().help = tr("The game's Minus, in a hunt. While it is off, the menu pad has a small Minus below it: the item box opens its other sections with it.");
#ifndef __ANDROID__  // (the default ini says nothing of these two; a phone's: PHONE_HELP)
            if (i == TB_MENU_ZL)
                r.back().help = tr("With the menu pad, a ZL tab above its left one (L when the game turns pages with ZL and ZR). Some menus use it to change sections or show more information: the item box, the smithy.");
            if (i == TB_MENU_ZR)
                r.back().help = tr("With the menu pad, a ZR tab above its right one (R when the game turns pages with ZL and ZR). Some menus use it to change sections or show more information: the item box, the smithy.");
#endif
        }
    }
    header(r, "tc.head", tr("Combined buttons"));
    for (int k = 0; k < TOUCH_COMBOS; k++) {
        action(r, "tc." + std::to_string(k), fmt("Button %d", k + 1), [k]() {
            s_combo = k;
            push(View::ComboEdit, "ce.0");
        });
        r.back().value = c.touch_combo[k][0] ? c.touch_combo[k] : tr("Empty");
        if (!fingers)
            r.back().help = tr("A button of your own presses several of the game's together. Give it a controller's button here, or a key in Keybinds.");
    }
    if (fingers)
        r.back().help = tr("A button of your own presses several of the game's together. Place it with Customize controls and HUD.");
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
#ifndef __ANDROID__  // (a key does the same while held: [Keybinds] Combo<n>Key, also in Keybinds)
    r.push_back(bind_row(BIND_COMBO1 + k, tr("A key or mouse button that presses these buttons while held. Keybinds has it too.")));
    r.back().id = "ce.key";
    r.back().title = tr("Key");
#endif
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

// The Room Browsers (runtime/net/webapi.c): the public rooms a Web API lists. Yuzu: the Eden Web API's Yuzu-family rooms.
// Citra: the Citra Web APIs' Azahar / Citra rooms (Monster Hunter XX). Joining one points that family's room server at it,
// turns the matching Crossplay mode on and saves, so the game's next search or hub joins it.
struct RoomFilters { int hide_locked = 0, hide_full = 0, hide_empty = 0; } room_filters;
std::string room_search;  // the Room Browsers' search text (name, host or game)
constexpr uint32_t CITRA_NET_VERSION = 4;  // the room protocol version this build speaks (Azahar / Citra: 4)
std::string lower_text(std::string t) {
    for (char &c : t) c = (char)SDL_tolower((unsigned char)c);
    return t;
}
// The room joined last, as the settings keep it ("address:port", and its name unless that is empty)
bool is_last_room(const WebRoom &w, int kind) {
    const char *last = kind == WEBROOMS_CITRA ? s.draft.last_citra_room : s.draft.last_room;
    const char *last_name = kind == WEBROOMS_CITRA ? s.draft.last_citra_room_name : s.draft.last_room_name;
    char at[96];
    snprintf(at, sizeof at, "%s:%u", w.address, (unsigned)w.port);
    return last[0] && !strcmp(last, at) && (!last_name[0] || !strcmp(last_name, w.name));
}
bool join_web_room(const WebRoom &w, const std::string &password, int kind) {
    Config temp = s.draft;
    char server[256];
    snprintf(server, sizeof server, "%s:%u", w.address, (unsigned)w.port);
    char *last = kind == WEBROOMS_CITRA ? temp.last_citra_room : temp.last_room;
    size_t last_size = kind == WEBROOMS_CITRA ? sizeof temp.last_citra_room : sizeof temp.last_room;
    char *last_name = kind == WEBROOMS_CITRA ? temp.last_citra_room_name : temp.last_room_name;
    size_t last_name_size = kind == WEBROOMS_CITRA ? sizeof temp.last_citra_room_name : sizeof temp.last_room_name;
    snprintf(last, last_size, "%s", server);
    SDL_strlcpy(last_name, w.name, last_name_size);
    for (char *c = last_name; *c; c++)  // (the settings file can't hold these: a name with them matches any)
        if (*c == ';' || *c == '#' || *c == '\n' || *c == '\r') { last_name[0] = 0; break; }
    if (kind == WEBROOMS_CITRA) {
        SDL_strlcpy(temp.citra_room_server, server, sizeof temp.citra_room_server);
        SDL_strlcpy(temp.citra_room_password, password.c_str(), sizeof temp.citra_room_password);
        if (temp.crossplay != XP_ALL) temp.crossplay = XP_CITRA;  // (All Rooms keeps the Yuzu room too; else one family at a time)
    } else {
        SDL_strlcpy(temp.room_server, server, sizeof temp.room_server);
        SDL_strlcpy(temp.room_password, password.c_str(), sizeof temp.room_password);
        if (temp.crossplay == XP_OFF || temp.crossplay == XP_CITRA) temp.crossplay = XP_ROOM;
        else if (temp.crossplay == XP_RYU) temp.crossplay = XP_MIXED;  // (the RyuLdn servers stay in use)
    }
    char why[512];
    if (!config_validate(&temp, why, sizeof why)) {
        s.error = settings_text(s.draft.language, why);
        return false;
    }
    s.draft = temp;
    s.command = Command::Save;
    return true;
}
void refresh_rooms(int kind) {
    if (kind == WEBROOMS_CITRA) webrooms_refresh_kind(WEBROOMS_CITRA, s.draft.citra_web_api_url, s.draft.citra_web_api_url_2);
    else webrooms_refresh(s.draft.web_api_url);
}
void room_browser_rows(std::vector<Row> &r, int kind) {
    const bool citra = kind == WEBROOMS_CITRA;
    char err[96];
    int state = webrooms_state_kind(kind, err, sizeof err);
    action(r, "rb.refresh", tr("Refresh"), [kind]() { refresh_rooms(kind); }, state != WEBROOMS_LOADING);
    r.back().value = state == WEBROOMS_LOADING ? tr("Loading...") : "";
    r.back().help = tr(citra ? "Asks the Citra Web APIs for the public rooms again." : "Asks the Yuzu Web API for the public rooms again.");
    add_bool(r, "rb.locked", "Hide locked", &room_filters.hide_locked);
    r.back().help = tr("Hides the rooms that need a password.");
    add_bool(r, "rb.full", "Hide full", &room_filters.hide_full);
    r.back().help = tr("Hides the rooms with no free place.");
    add_bool(r, "rb.empty", "Hide empty", &room_filters.hide_empty);
    r.back().help = tr("Hides the rooms nobody is in.");
    action(r, "rb.search", tr("Search"), []() {
        keyboard(tr("Search rooms"), room_search, 40, [](const std::string &typed, std::string &) {
            room_search = typed;
            return true;
        });
    });
    r.back().value = room_search.empty() ? std::string(tr("None")) : room_search;
    r.back().help = tr("Shows only the rooms whose name, host or game has this text.");
    if (state == WEBROOMS_ERROR) header(r, "rb.error", fmt("Can't get the room list: %s", err));
    WebRoom rooms[WEBROOMS_MAX];
    int n = webrooms_get_kind(kind, rooms, WEBROOMS_MAX), shown = 0;
    // The rooms that pass the filters: the one joined last pinned first, then the fullest first, the empty ones at the end
    std::vector<int> order;
    const std::string query = lower_text(room_search);
    for (int i = 0; i < n; i++) {
        const WebRoom &w = rooms[i];
        if (room_filters.hide_locked && w.locked) continue;
        if (room_filters.hide_full && w.max_players > 0 && w.players >= w.max_players) continue;
        if (room_filters.hide_empty && w.players == 0) continue;
        if (!query.empty() && lower_text(w.name).find(query) == std::string::npos &&
            lower_text(w.owner).find(query) == std::string::npos && lower_text(w.game).find(query) == std::string::npos)
            continue;
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        bool la = is_last_room(rooms[a], kind), lb = is_last_room(rooms[b], kind);
        if (la != lb) return la;
        return rooms[a].players > rooms[b].players;
    });
    for (int i : order) {
        const WebRoom &w = rooms[i];
        shown++;
        std::string title = w.name[0] ? w.name : w.address;
        std::string value = std::to_string(w.players) + "/" + std::to_string(w.max_players);
        bool last = is_last_room(w, kind);
        const bool wrong_version = citra && w.net_version && w.net_version != CITRA_NET_VERSION;  // (another room protocol: can't join)
        if (w.locked) value += std::string("  ") + tr("Locked");
        if (wrong_version) value += std::string("  ") + tr("Incompatible");
        if (last) value += std::string("  ") + tr("Last joined");
        WebRoom room = w;
        action(r, "rb.room." + std::to_string(i), title, [room, kind, wrong_version]() {
            if (wrong_version) {
                s.error = tr("This room speaks another room protocol version than this app (Citra rooms: 4).");
                return;
            }
            if (!room.locked) {
                if (join_web_room(room, "", kind)) pop();
                return;
            }
            // (the password typed for this room last time is already in the box)
            const char *saved = kind == WEBROOMS_CITRA ? s.draft.citra_room_password : s.draft.room_password;
            size_t cap = (kind == WEBROOMS_CITRA ? sizeof s.draft.citra_room_password : sizeof s.draft.room_password) - 1;
            keyboard(tr("Room password"), is_last_room(room, kind) ? std::string(saved) : std::string(), cap,
                     [room, kind](const std::string &typed, std::string &error) {
                if (typed.empty()) {
                    error = tr("Type the room's password.");
                    return false;
                }
                return join_web_room(room, typed, kind);  // (the keyboard closes itself: back in the list)
            }, true);
        });
        r.back().value = value;
        std::string help = std::string(room.address) + ":" + std::to_string(room.port);
        if (room.owner[0]) help += std::string("  ") + tr("Host") + ": " + room.owner;
        if (room.game[0]) help += std::string("\n") + room.game;
        r.back().help = help;
    }
    if (state == WEBROOMS_OK && !shown)
        header(r, "rb.none", tr(n ? "No room matches the filters" : "No public rooms right now"));
    action(r, "rb.back", tr("Back"), []() { pop(); });
}

std::vector<Row> rows_for_view(bool *open) {
    if (s.view == View::Main)
        return main_rows();
    std::vector<Row> r;
    if (s.view == View::TouchButtons) { touch_button_rows(r); return r; }
    if (s.view == View::ComboEdit) { combo_rows(r); return r; }
    if (s.view == View::PadList) { pad_list_rows(r); return r; }
    if (s.view == View::PadMap) { pad_map_rows(r); return r; }
    if (s.view == View::RoomBrowser) { room_browser_rows(r, WEBROOMS_YUZU); return r; }
    if (s.view == View::CitraBrowser) { room_browser_rows(r, WEBROOMS_CITRA); return r; }
    if (s.view == View::HubsMenu) { hubs_rows(r); return r; }
    if (s.view == View::RyuMenu) { ryu_rows(r); return r; }
    if (s.view == View::YuzuMenu) { yuzu_rows(r); return r; }
    if (s.view == View::CitraMenu) { citra_rows(r); return r; }
    if (s.view == View::HubServers) { hub_server_rows(r); return r; }
    if (s.view == View::Keyboard) {
#ifdef __ANDROID__
        // (the phone's own keyboard types, with its clipboard; these rows are what it has no key for)
        action(r, "key.accept", tr("Accept"), []() { accept_keyboard(); });
        action(r, "key.paste", tr("Paste"), []() {
            std::string clip = clipboard_text();
            if (clip.empty())
                s.error = tr("The clipboard is empty.");
            else
                insert(clip);
        });
        action(r, "key.copy", tr("Copy"), []() { copy_text(s.keyboard.buffer); }, !s.keyboard.buffer.empty());
        action(r, "key.clear", tr("Clear"), []() {
            s.keyboard.buffer.clear();
            s.keyboard.cursor = 0;
        });
        action(r, "key.show", tr("Show the keyboard"), []() { textinput_tap(); });
        r.back().help = tr("Brings the phone's keyboard back after Back hid it. A tap anywhere does too.");
        action(r, "key.cancel", tr("Cancel"), []() { pop(); });
#else
        std::string chars = "1234567890abcdefghijklmnopqrstuvwxyz.:,-_/@!$%&*+=?~()";
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
#endif
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
        // (as Back: the unsaved-changes dialog's leaves the menu open with no touch layout editor after all)
        action(r, "dialog.cancel", tr(s.view == View::Licenses ? "Close" : "Cancel"), [open]() { leave_view(open); });
#ifndef __ANDROID__
        if (s.view == View::ImportSave)  // (runtime/pc_actions.c: kept for the next start, which puts it in place)
            action(r, "dialog.import", tr("Import and restart"), []() {
                if (pc_import_save_confirm()) ui_request_action(true);
                else pop();
            });
#endif
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
#ifndef __ANDROID__
        if (s.view == View::Remove)  // (a mod or a cheat file: runtime/pc_actions.c)
            action(r, "dialog.remove", tr("Remove"), []() { remove_confirmed(); });
#endif
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
    {"ShowMenuPad", "While a menu of the game's is open, a small pad shows that moves its cursor, with the page buttons above it (L and R, or ZL and ZR if the game's options turn pages with them). The movement stick keeps walking and the other buttons stay where they are; where the hunter can't walk (a town's item box, a shop) it moves the cursor too. If the pad covers a combined button, move it with Customize controls and HUD.",
     "Mientras hay un menú del juego abierto aparece un pad chico que mueve su cursor, con los botones de página encima (L y R, o ZL y ZR si las opciones del juego pasan páginas con ellos). La palanca de movimiento sigue caminando y los demás botones quedan donde están; donde el cazador no puede caminar (la caja de objetos de un pueblo, una tienda) también mueve el cursor. Si el pad tapa un botón combinado, muévelo con Personalizar controles y HUD."},
    {"ShowMenuZL", "With the menu pad, a ZL tab above its left one (L when the game turns pages with ZL and ZR). Some menus use it to change sections or show more information: the item box, the smithy.",
     "Con el pad de menú, una pestaña ZL sobre la de la izquierda (L si el juego pasa páginas con ZL y ZR). Algunos menús la usan para cambiar de sección o ver más información: la caja de objetos, la herrería."},
    {"ShowMenuZR", "With the menu pad, a ZR tab above its right one (R when the game turns pages with ZL and ZR). Some menus use it to change sections or show more information: the item box, the smithy.",
     "Con el pad de menú, una pestaña ZR sobre la de la derecha (R si el juego pasa páginas con ZL y ZR). Algunos menús la usan para cambiar de sección o ver más información: la caja de objetos, la herrería."},
    {"Bloom", "The glow around lights and the blur of what is far away. Off: faster on a weak phone. Low-end mode turns it off.",
     "El resplandor alrededor de las luces y el desenfoque de lo lejano. Apagado: más rápido en un teléfono débil. El modo gama baja lo apaga."},
    {"Reverb", "The echo of caves and rooms. Off: the sound is dry, and a weak phone's processor does less.",
     "El eco de cuevas y salas. Apagada: el sonido queda seco y el procesador de un teléfono débil trabaja menos."},
    {"LowEnd", "For a weak phone: no bloom or depth of field, cheaper and smaller textures, and the HUD stays sharp over a smaller 3D picture. Toaster: everything for speed and the battery before looks.",
     "Para un teléfono débil: sin resplandor ni profundidad de campo, texturas más livianas y más chicas, y el HUD sigue nítido sobre una imagen 3D más chica. Tostadora: todo para la velocidad y la batería antes que el aspecto."},
    {"Shadows", "The shadows of hunters, monsters and scenery. Off: a little faster on a weak phone.",
     "Las sombras de cazadores, monstruos y escenario. Apagadas: un poco más rápido en un teléfono débil."},
    {"Grass", "The tall grass and the foliage of the maps. Off: faster where there is a lot of it (the Marshland's area 8) on a weak phone.",
     "El pasto alto y el follaje de los mapas. Apagado: más rápido donde hay mucho (la zona 8 del Pantano) en un teléfono débil."},
    {"AdrenoFix", "Some Adreno drivers repeat the 3D picture in a grid at a low 3D resolution: Auto keeps 2x MSAA there. If the picture looks dark or flickers, or for more speed, choose Off.",
     "Algunos drivers de Adreno repiten la imagen 3D en grilla con una resolución 3D baja: Auto deja MSAA 2x en ese caso. Si la imagen se ve oscura o parpadea, o para ganar velocidad, elige Off."},
    {"LockOn", "Keeps the camera on the monster, as in the newer games, and a bow's or a bowgun's shots go to it. Off: the icon turns the camera to it once.",
     "Mantiene la cámara sobre el monstruo, como en los juegos nuevos, y los disparos de arcos y ballestas van hacia él. Apagado: el ícono gira la cámara una sola vez."},
    {"Join", "A host's IP address, to play over a VPN. Empty: searches your network.", "La dirección IP de un anfitrión, para jugar por VPN. Vacío: busca en tu red."},
    {"Server", "A hub server's address (name or IP, and :port if it is not 39510), to play over the internet with everyone who uses the same server: their hubs show when you search for hubs in the game. Empty: none, only your network.",
     "La dirección de un servidor de hubs (nombre o IP, y :puerto si no es el 39510), para jugar por internet con todos los que usen el mismo servidor: sus hubs aparecen al buscar hubs en el juego. Vacío: ninguno, solo tu red."},
    {"Key", "On the hub server, only players with the same key see each other's hubs: agree on one with your friends. Empty: public hubs.",
     "En el servidor de hubs, solo los jugadores con la misma clave ven los hubs de los demás: acuerden una con tus amigos. Vacío: hubs públicos."},
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
    {"MoveHUD", "On: with the touch controls the HUD has an arrangement of its own, out of your thumbs' way. Off: the same HUD as without them.", "Activado: con los controles táctiles el HUD tiene un acomodo propio, lejos de los pulgares. Desactivado: el mismo HUD que sin ellos."},
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
    {"Language", "The language of this menu. Auto: the phone's language (English, Spanish or Chinese; English for any other).",
     "El idioma de este menú. Automático: el del teléfono (inglés, español o chino; inglés para cualquier otro)."},
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
        if (r.key == h.key) {
            const int lang = settings_language(s.draft.language);
            return lang == MENU_LANG_ES ? h.es : settings_text(lang, h.en);
        }
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
// Tab moves to the next row (Shift+Tab the one before); a desktop's Ctrl+Tab changes the tab instead (page_keys)
bool tab_key() {
#ifndef __ANDROID__
    if (ImGui::GetIO().KeyCtrl)
        return false;
#endif
    return pressed(ImGuiKey_Tab, true);
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
    if (tab_key())
        dy = ImGui::GetIO().KeyShift ? -1 : 1;
    if (list)
        dx = 0;
    if (dx && it->adjust) {
        it->adjust(dx);
        return;
    }
    int delta = dy;
    if (s.view == View::Keyboard && (dx || dy) && !tab_key()) {
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
        // (lit as the selection while pressed, a lighter version of it under the mouse - only while the mouse is what
        // moves, not under a pointer left there while the keys or a controller move, nor under a dropdown open)
        ImU32 base = selected && r.enabled ? t.row_focus : t.row;
        ImU32 hover = s.input == USED_MOUSE && !passive && !(selected && r.enabled) ? t.row_hover : base;
        ImGui::PushStyleColor(ImGuiCol_Button, touch ? clear : vec(base));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, touch ? clear : vec(hover));
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
        if (held && r.enabled)  // (pressed: brighter still, the selected row too)
            w->DrawList->AddRectFilled(rect.Min, rect.Max, IM_COL32(255, 240, 200, 38), t.rounding);
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
        if (clicked && r.enabled && !s.swallow) {  // (not the click that closed the dropdown: back_keys)
            focus(r.id);
            activate = r.activate;
        }
    }
    if (adjust)
        adjust(step);
    if (activate)
        activate();
}
#ifndef __ANDROID__
// ---------------------------------------------------------------- coming in and going out (a desktop's menu)
// The menu, its dropdown and its views come in with a short fade and a small slide; the menu and the dropdown go out
// with a fade. Only what is drawn moves, never where input lands: the menu takes input from its first frame, and once
// closed it takes none - its fade out is a copy of its last frame, drawn with no window (settings_panel_after_render)
constexpr double ANIM_IN = .14, ANIM_OUT = .12;  // (seconds)
// How far an animation that began at `since` has come: 0 .. 1, easing out (1: over, or none)
float anim_in(double since) {
    if (since < 0)
        return 1;
    float t = (float)std::clamp((anim_now() - since) / ANIM_IN, 0.0, 1.0);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
// What a draw list holds from vertex `from` to `to`, faded by alpha and moved by off; clips: its commands' clip
// rectangles moved too (a whole window's)
void tween_list(ImDrawList *dl, int from, int to, float alpha, ImVec2 off, bool clips) {
    for (int i = from; i < to; i++) {
        ImDrawVert &v = dl->VtxBuffer[i];
        v.pos.x += off.x, v.pos.y += off.y;
        v.col = fade(v.col, alpha);
    }
    if (clips && (off.x || off.y))
        for (ImDrawCmd &c : dl->CmdBuffer)
            c.ClipRect.x += off.x, c.ClipRect.y += off.y, c.ClipRect.z += off.x, c.ClipRect.w += off.y;
}
// A window as drawn this frame, its child windows too
void tween_window(ImGuiWindow *w, float alpha, ImVec2 off) {
    tween_list(w->DrawList, 0, w->DrawList->VtxBuffer.Size, alpha, off, true);
    for (ImGuiWindow *c : w->DC.ChildWindows)
        if (c->Active)
            tween_window(c, alpha, off);
}
// What closed, fading: copies of a window's lists as last drawn (its child windows' after its own), when it closed,
// how far it slides as it goes
struct Ghost {
    std::vector<ImDrawList *> lists;
    double at;
    ImVec2 slide;
};
std::vector<Ghost> ghosts;
std::vector<ImDrawList *> ghost_shown;  // (this frame's faded copies, given to the draw data)
void ghost_copy(ImGuiWindow *w, bool now, std::vector<ImDrawList *> &out) {
    out.push_back(w->DrawList->CloneOutput());
    for (ImGuiWindow *c : w->DC.ChildWindows)
        if (now ? c->Active : c->WasActive)
            ghost_copy(c, now, out);
}
// w as drawn this frame (now), or as last drawn: a window left out of this frame still holds that one's lists
void ghost_take(ImGuiWindow *w, bool now, ImVec2 slide) {
    if (!w)
        return;
    Ghost g = {{}, anim_now(), slide};
    ghost_copy(w, now, g.lists);
    ghosts.push_back(std::move(g));
}
void ghosts_clear() {
    for (Ghost &g : ghosts)
        for (ImDrawList *l : g.lists)
            IM_DELETE(l);
    ghosts.clear();
}
#endif
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
#ifndef __ANDROID__
    ImGuiWindow *list = ImGui::GetCurrentWindow();
#endif
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
#ifndef __ANDROID__
    float in = anim_in(s.dropdown_at);  // (it comes out of its row: down from it, or up above it)
    if (in < 1)
        tween_window(list, in, ImVec2(0, (up ? 1 : -1) * (1 - in) * font * .6f));
    s.dropdown_up = up;
#endif
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
// Back (B, Esc, the Android Back key, F1 / the chord: request_back), and a click away from an open dropdown. A
// desktop's: a click on the game, away from every window of the menu, is Back too (a dialog or a page left, the menu
// closed - asking first when something is unsaved); the game never sees it (runtime/overlay.cpp keeps the mouse's
// events while the menu is open)
void back_keys(bool *open) {
    if (request_back.exchange(false) || pressed(ImGuiKey_GamepadFaceRight) || pressed(ImGuiKey_Escape) ||
        pressed(ImGuiKey_AppBack))
        ask_back(open);
    bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
#ifndef __ANDROID__
    bool right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
#else
    bool right = false;
#endif
    if (s.dropdown.open && (click || right) && !s.dropdown.hovered) {
        close_dropdown();  // (a click elsewhere only closes it, as a system's list does - the phone's picker covers the
        s.swallow = true;  // menu: the tap only closes it; a desktop's row under the click isn't chosen as well)
    }
#ifndef __ANDROID__
    else if ((click || right) && !touch_ui() && !ImGui::GetCurrentContext()->HoveredWindow) {
        ask_back(open);
        s.swallow = true;
    }
#endif
}
// The tabs' keys: Save from anywhere on a tab (the top face button, or Ctrl+S), L / R the tab before / after - on a
// desktop's keyboard Q / E, or Ctrl+Shift+Tab / Ctrl+Tab
void page_keys() {
    ImGuiIO &io = ImGui::GetIO();
    if ((pressed(ImGuiKey_GamepadFaceUp) || (io.KeyCtrl && pressed(ImGuiKey_S))) && dirty() && !s.dropdown.open)
        s.command = Command::Save;
    bool next = pressed(ImGuiKey_GamepadR1), before = pressed(ImGuiKey_GamepadL1);
#ifndef __ANDROID__
    bool ctrl_tab = io.KeyCtrl && pressed(ImGuiKey_Tab, true);
    next |= (ctrl_tab && !io.KeyShift) || (!io.KeyCtrl && !io.KeyAlt && pressed(ImGuiKey_E));
    before |= (ctrl_tab && io.KeyShift) || (!io.KeyCtrl && !io.KeyAlt && pressed(ImGuiKey_Q));
#endif
    if (next)
        first_page((s.page + 1) % PAGES);
    if (before)
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
    if (io.KeyCtrl && pressed(ImGuiKey_V))
        insert(clipboard_text());
    if (io.KeyCtrl && pressed(ImGuiKey_C) && !s.keyboard.buffer.empty())
        SDL_SetClipboardText(s.keyboard.buffer.c_str());
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
#ifndef __ANDROID__
    if (s.view == View::Remove)
        return removal_question();
#endif
    return s.view == View::Keyboard ? s.keyboard.title
           : s.view == View::KeyCapture && s.capture_bind >= 0
               ? fmt("Press a key or mouse button for %s (Esc: cancel)", tr(bind_titles[s.capture_bind]))
               : tr(s.view == View::Exit         ? "Exit game?"
                    : s.view == View::Restart    ? "Restart game?"
                    : s.view == View::Reset      ? "Reset this tab?"
                    : s.view == View::Move       ? "Move menu"
                    : s.view == View::Conflicts  ? "Mod conflicts"
                    : s.view == View::Keybinds   ? "Keybinds"
                    : s.view == View::TouchButtons ? (touch_rows() ? "Buttons on the screen" : "Combined buttons")
                    : s.view == View::ComboEdit  ? "Combined button"
                    : s.view == View::PadList    ? "Map a controller"
                    : s.view == View::PadMap     ? "Map a controller"
                    : s.view == View::RoomBrowser ? "Yuzu Room Browser"
                    : s.view == View::CitraBrowser ? "Citra Room Browser"
                    : s.view == View::HubsMenu   ? "Internet hubs"
                    : s.view == View::RyuMenu    ? "Ryujinx (RyuLdn)"
                    : s.view == View::YuzuMenu   ? "Yuzu Room"
                    : s.view == View::CitraMenu  ? "Citra Room"
                    : s.view == View::HubServers ? "Hub servers"
                    : s.view == View::KeyCapture ? "Press the key for chat (Esc: cancel)"
                    : s.view == View::ImportSave ? "Import this save? The characters here are replaced by its own and the game restarts; the current save is kept in a copy."
                    : s.view == View::Licenses   ? "Licenses"
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
    if (s.page == ABOUT_PAGE) {  // (the thank-you, as it showed after the credits of the first start: draw_thanks)
        notes.push_back({tr(THANKS_TEXT), false});
        notes.push_back({thanks_signature(), false});
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
    if (std::any_of(rows.begin(), rows.end(), [](const Row &row) { return row.cold; }))  // (only where a row has the *)
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
        char composing[256];  // (what the phone's keyboard is still composing, a word or Chinese pinyin: at the cursor)
        overlay_composition(composing, sizeof composing);
        if (s.keyboard.secret && !text.empty())
            text.assign(utf8_count(text), '*');
        text.insert(std::min(s.keyboard.cursor, text.size()), std::string(composing) + "|");
        float room = field.GetWidth() - f * 1.2f;
        size_t at = std::min(s.keyboard.cursor, s.keyboard.buffer.size()) + strlen(composing) + 1;
        if (s.keyboard.secret) at = std::min(text.size(), utf8_count(s.keyboard.buffer.substr(0, s.keyboard.cursor)) + strlen(composing) + 1);
        float upto = ImGui::CalcTextSize(text.c_str(), text.c_str() + std::min(at, text.size())).x;
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
    if (s.view == View::Licenses)
        licenses_text(look);
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
// ---------------------------------------------------------------- the credits
// Shown over the game for a few seconds when the app opens (again from Credits & Licenses): who made it and where the
// official one is, so a copy passed around elsewhere still says so
double g_credits_at = -1;
bool g_credits_asked = false;  // (Show the credits: over the menu too)
constexpr double kCreditsShown = 8;
void credits_show() {
    g_credits_at = SDL_GetTicks() / 1000.0;
    g_credits_asked = true;
}
// (only inside the overlay's ImGui frame: before its first one there is no font, and a draw list taken before NewFrame
// is the last frame's, cleared as the frame begins)
void draw_credits() {
    constexpr double kShown = kCreditsShown, kIn = .4, kFade = .9;
    const double now = SDL_GetTicks() / 1000.0;
    if (g_credits_at < 0)
        g_credits_at = now;
    const double t = now - g_credits_at;
    if (t > kShown || thanks_on(now))  // (the thank-you on the screen: never one on the other)
        return;
    const float a = (float)std::clamp(std::min(t / kIn, (kShown - t) / kFade), 0.0, 1.0);
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const Look k = touch_look();
    const Theme &th = theme();
    ImFont *font = ImGui::GetFont();
    const char *title = app_title();
    const std::string lines[] = {
        std::string(tr("Version")) + " " MHGU_VERSION "  \xC2\xB7  " + tr("by") + " khorne-hub",
        tr("Free, made by fans"),
        std::string(tr("Official download:")) + " github.com/khorne-hub/gu-native",
        tr("Based on MHGURecomp"),
    };
    const char *warns[] = {tr("Only the links khorne-hub gives are official."),
                           tr("Got it elsewhere, paid for it or it has ads? It is not the official one.")};
    // (sized to the screen's height, smaller when it doesn't fit its width)
    float f = std::floor(std::clamp(vp->Size.y * .034f, 12.f, 44.f)), w = 0;
    for (int pass = 0; pass < 2; pass++) {
        w = font->CalcTextSizeA(std::floor(f * 1.5f), FLT_MAX, 0, title).x;
        for (const std::string &l : lines)
            w = std::max(w, font->CalcTextSizeA(f, FLT_MAX, 0, l.c_str()).x);
        for (const char *l : warns)
            w = std::max(w, font->CalcTextSizeA(std::floor(f * .85f), FLT_MAX, 0, l).x);
        const float room = vp->Size.x * .9f - f * 2.6f;
        if (w <= room)
            break;
        f = std::floor(f * room / w);
    }
    const float tsz = std::floor(f * 1.5f), small = std::floor(f * .85f), gap = std::floor(f * .4f), pad = std::floor(f * 1.3f);
    const float line = std::max(1.f, std::floor(f * .09f));
    const float h = tsz + gap * 3 + 4 * (f + gap) + gap + small * 2 + gap * .5f;
    const ImVec2 c(vp->Pos.x + vp->Size.x * .5f, vp->Pos.y + vp->Size.y * .5f);
    const ImRect box(ImVec2(std::floor(c.x - w * .5f - pad), std::floor(c.y - h * .5f - pad)),
                     ImVec2(std::floor(c.x + w * .5f + pad), std::floor(c.y + h * .5f + pad)));
    // (the game's window: a wine-dark panel, a bronze edge, a gold rule inside and gold studs on its corners)
    dl->AddRectFilled(ImVec2(box.Min.x + line * 2, box.Min.y + line * 2), ImVec2(box.Max.x + line * 2, box.Max.y + line * 2),
                      fade(IM_COL32(0, 0, 0, 140), a));
    dl->AddRectFilled(box.Min, box.Max, fade(IM_COL32(50, 25, 24, 246), a));
    dl->AddRect(box.Min, box.Max, fade(IM_COL32(112, 76, 34, 255), a), 0, 0, line * 1.6f);
    const float in = line * 3.2f;
    const ImU32 gold = IM_COL32(222, 186, 96, 255);
    dl->AddRect(ImVec2(box.Min.x + in, box.Min.y + in), ImVec2(box.Max.x - in, box.Max.y - in), fade(gold, a * .75f), 0, 0, line * .7f);
    for (ImVec2 p : {box.Min, ImVec2(box.Max.x, box.Min.y), ImVec2(box.Min.x, box.Max.y), box.Max})
        diamond(dl, p, line * 2.6f, fade(gold, a));
    float y = box.Min.y + pad;
    ImVec2 ts = font->CalcTextSizeA(tsz, FLT_MAX, 0, title);
    shadow_text(dl, tsz, ImVec2(std::floor(c.x - ts.x * .5f), y), fade(IM_COL32(255, 232, 84, 255), a), title);
    y += tsz + gap * 1.5f;
    { // (a gold rule with a diamond in its middle)
        const float half = std::min(w * .5f, f * 9);
        dl->AddLine(ImVec2(c.x - half, y), ImVec2(c.x - f * .6f, y), fade(gold, a * .6f), line * .7f);
        dl->AddLine(ImVec2(c.x + f * .6f, y), ImVec2(c.x + half, y), fade(gold, a * .6f), line * .7f);
        diamond(dl, ImVec2(c.x, y), f * .3f, fade(gold, a * .9f));
        diamond(dl, ImVec2(c.x, y), f * .13f, fade(k.band, a));
    }
    y += gap * 1.5f;
    for (int i = 0; i < 4; i++) {
        ImVec2 ls = font->CalcTextSizeA(f, FLT_MAX, 0, lines[i].c_str());
        shadow_text(dl, f, ImVec2(std::floor(c.x - ls.x * .5f), y),
                    fade(i == 2 ? IM_COL32(236, 212, 44, 255) : IM_COL32(242, 238, 232, 255), a), lines[i].c_str());
        y += f + gap;
    }
    y += gap;
    for (const char *l : warns) {
        ImVec2 ws = font->CalcTextSizeA(small, FLT_MAX, 0, l);
        shadow_text(dl, small, ImVec2(std::floor(c.x - ws.x * .5f), y), fade(IM_COL32(196, 176, 166, 255), a), l);
        y += small + gap * .5f;
    }
    (void)th;
}
// ---------------------------------------------------------------- the thanks
// Once, the first time the game opens ([Application] ThanksShown Off), after the credits: a thank-you to the people who
// made the game and kept it alive, in the credits' look. It stays until the player closes it - Continue, a click (either
// button) or a tap anywhere, A, B, Esc, Back (Android's too), Enter, Space or F (a keyboard's A) - and meanwhile every input
// is its own: overlay.cpp counts it
// as a menu open (settings_panel_modal), so the keyboard, the mouse, the fingers (touch.cpp) and the controller
// (host_input.c) go to the overlay and none to the game, and here (thanks_frame, the first thing settings_panel does) it
// takes the frame's input before the menu or a plugin reads it. The overlay draws frames for it as for the credits
// (settings_panel_credits_up), also with nothing else on the screen. Closed, ThanksShown goes into the file at once.
// Credits & Licenses shows its text and opens it again, over the menu, which it goes back to. MHGU_THANKS=0 keeps it from
// showing by itself (a test run's input would wait for it), =1 shows it at every start; with MHGU_HIDDEN (no window to
// close it in) it doesn't show by itself.
enum class Thanks { Unknown, Wait, Open, Done };
std::atomic<Thanks> g_thanks{Thanks::Unknown};  // (read on the host event thread too: settings_panel_key)
constexpr double THANKS_AFTER_S = .3, THANKS_IN_S = .35, THANKS_OUT_S = .15;
constexpr ImGuiID THANKS_OWNER = 0x7468616e;  // (it holds every key for the rest of its frames: the menu under it gets none)
double g_thanks_at = -1, g_thanks_shut = -1;  // (when it opened, when it closed: its fades)
double g_thanks_arm = 1;  // (for this long after it opens it takes no answer: a press already under way, or buttons mashed
                          // through the logos, is not one)
// What closes it, as it lets go: a press made once it takes answers (so neither half of it reaches anything else). A
// keyboard's A and B too, as a desktop's keybinds have them at first (F, and Space), and the mouse's right button (B)
const ImGuiKey THANKS_KEYS[] = {ImGuiKey_MouseLeft, ImGuiKey_MouseRight, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
                                ImGuiKey_Escape,    ImGuiKey_AppBack,    ImGuiKey_Enter,           ImGuiKey_KeypadEnter,
                                ImGuiKey_Space,     ImGuiKey_F};
bool g_thanks_armed[IM_COUNTOF(THANKS_KEYS)];
void thanks_decide() {
    if (g_thanks != Thanks::Unknown)
        return;
    const char *env = getenv("MHGU_THANKS");  // (once: getenv walks the whole environment)
    const bool every = env && *env == '1', never = (env && *env == '0') || getenv("MHGU_HIDDEN");
    g_thanks = every || (!never && !config_editable().thanks_shown) ? Thanks::Wait : Thanks::Done;
    if (g_thanks == Thanks::Wait)
        fprintf(stderr, "[settings] the thank-you shows after the credits (%s)\n", every ? "MHGU_THANKS=1" : "ThanksShown = Off");
}
void thanks_open(double now, bool asked) {
    g_thanks = Thanks::Open;
    g_thanks_at = now, g_thanks_shut = -1;
    g_thanks_arm = 1.0;  // (asked for in the menu too: the key that asked for it, held, repeats at about .5 s and would answer)
    memset(g_thanks_armed, 0, sizeof g_thanks_armed);
    g_credits_asked = false;  // (credits asked for a moment before: not under it, nor back over the menu after it)
    fprintf(stderr, "[settings] the thank-you is open%s: every input goes to it until it is closed\n",
            asked ? " (Credits & Licenses)" : "");
}
void thanks_show() { thanks_open(SDL_GetTicks() / 1000.0, true); }
bool thanks_fading(double now) { return g_thanks == Thanks::Done && g_thanks_shut >= 0 && now - g_thanks_shut < THANKS_OUT_S; }
// [Application] ThanksShown into the file as it closes, with the settings as saved (those being edited in the menu are
// saved or not later, as the player chooses): no Save needed, the rest of the file as it was
void thanks_seen() {
    Config c = config_editable();
    if (c.thanks_shown)
        return;
    c.thanks_shown = 1;
    char error[512];
    if (!config_save(&c, error, sizeof error)) {
        fprintf(stderr, "[settings] ThanksShown could not be saved: %s\n", error);
        return;
    }
    s.draft.thanks_shown = s.saved.thanks_shown = 1;  // (a Save from the menu open under it keeps it)
    fprintf(stderr, "[settings] ThanksShown = On: the thank-you does not show by itself again\n");
}
// "— khorne-hub". ImGui's own font (a desktop's, without Forge's font) has no em dash and would show '?': there the
// banner draws the dash, and the menu's text has two hyphens
bool has_em_dash() {
    static ImFont *checked;
    static bool has;
    ImFont *font = ImGui::GetFont();
    if (font != checked)
        checked = font, has = font && font->IsGlyphInFont(0x2014);
    return has;
}
const char *thanks_signature() { return has_em_dash() ? "\xE2\x80\x94 khorne-hub" : "-- khorne-hub"; }
// Its answer this frame (t: since it opened); then every input of the frame is its own: each key held for whatever reads it
// after (the menu under it, a plugin's window), nothing under it pointed at
bool thanks_input(double t) {
    // (a click or a tap lets go on the screen: a finger the system took, touch.cpp to_menu, lets go far outside it, and a
    // desktop's mouse can let go outside the window)
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const bool on_screen = ImRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y)).Contains(ImGui::GetIO().MousePos);
    bool done = false;
    for (int i = 0; i < IM_COUNTOF(THANKS_KEYS); i++) {
        if (ImGui::IsKeyPressed(THANKS_KEYS[i], false))
            g_thanks_armed[i] = t >= g_thanks_arm;
        const bool mouse = THANKS_KEYS[i] == ImGuiKey_MouseLeft || THANKS_KEYS[i] == ImGuiKey_MouseRight;
        if (ImGui::IsKeyReleased(THANKS_KEYS[i]) && g_thanks_armed[i] && (!mouse || on_screen))
            done = true;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
        ImGui::SetKeyOwner((ImGuiKey)k, THANKS_OWNER, ImGuiInputFlags_LockThisFrame);
    ImGuiContext &g = *ImGui::GetCurrentContext();
    g.HoveredWindow = g.HoveredWindowUnderMovingWindow = nullptr;
    g.IO.InputQueueCharacters.resize(0);  // (typed text too: Space would type into the menu's keyboard under it)
    return done;
}
// The thank-you in the credits' look (draw_credits): a wine-dark panel, a bronze edge, a gold rule inside and gold studs
// on its corners, over the screen dimmed; the title, a gold rule, the text centred line by line, the signature at its
// right, Continue under it. Sized to the screen's height as the credits, smaller where it doesn't fit (a 1280x720 window,
// a phone, upright too; a 3440x1440 screen gets the credits' largest text). a: its fade; live: open, not fading out;
// armed: it takes answers (Continue lit, and under the mouse pointer or a finger)
void draw_thanks(float a, bool live, bool armed) {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    const ImGuiIO &io = ImGui::GetIO();
    const char *title = tr("Thank you"), *text = tr(THANKS_TEXT), *go = tr("Continue"), *name = "khorne-hub";
    const bool glyph = has_em_dash();
    // (the text broken into lines at a width that reads well; all of it smaller where it still doesn't fit)
    float f = std::floor(std::clamp(vp->Size.y * .034f, 12.f, 44.f));
    float tsz = 0, gap = 0, pad = 0, lead = 0, w = 0, h = 0, bw = 0, bh = 0, sw = 0, bar = 0, space = 0;
    std::vector<std::pair<const char *, const char *>> rows;
    auto lines = [&](float wrap, bool keep) {  // (how many lines at that width; keep: into rows)
        int n = 0;
        for (const char *p = text, *end = text + strlen(text); p < end; n++) {
            const char *eol = font->CalcWordWrapPosition(f, p, end, wrap), *e = eol;
            while (e > p && e[-1] == ' ')
                --e;
            if (keep)
                rows.push_back({p, e});
            for (p = eol; p < end && *p == ' ';)
                ++p;
        }
        return n;
    };
    for (int pass = 0; pass < 3; pass++) {
        tsz = std::floor(f * 1.5f), gap = std::floor(f * .4f), pad = std::floor(f * 1.3f), lead = std::floor(f * 1.32f);
        // (balanced: the narrowest width that still takes as many lines, so the last one isn't a word on its own)
        float wrap = std::max(f * 8, std::min(f * 24, vp->Size.x * .9f - pad * 2)), narrow = wrap * .5f;
        const int count = lines(wrap, false);
        for (int i = 0; i < 10; i++) {
            const float mid = (narrow + wrap) * .5f;
            (lines(mid, false) <= count ? wrap : narrow) = mid;
        }
        rows.clear();
        lines(wrap, true);
        w = font->CalcTextSizeA(tsz, FLT_MAX, 0, title).x;
        for (const auto &r : rows)
            w = std::max(w, font->CalcTextSizeA(f, FLT_MAX, 0, r.first, r.second).x);
        bar = std::floor(f * .8f), space = font->CalcTextSizeA(f, FLT_MAX, 0, " ").x;
        sw = glyph ? font->CalcTextSizeA(f, FLT_MAX, 0, thanks_signature()).x
                   : bar + space + font->CalcTextSizeA(f, FLT_MAX, 0, name).x;
        bh = std::floor(f * 2.3f);
        bw = std::floor(std::max(f * 8, font->CalcTextSizeA(f, FLT_MAX, 0, go).x + f * 3));
        w = std::max({w, sw, bw});
        h = tsz + gap * 3 + rows.size() * lead + gap * .25f + f + gap * 2.5f + bh;
        const float room_w = vp->Size.x * .96f - pad * 2, room_h = vp->Size.y * .94f - pad * 2;
        if ((w <= room_w && h <= room_h) || f <= 10)
            break;
        f = std::max(10.f, std::floor(f * std::min(room_w / w, room_h / h)));
    }
    const float line = std::max(1.f, std::floor(f * .09f));
    const ImVec2 c(vp->Pos.x + vp->Size.x * .5f, vp->Pos.y + vp->Size.y * .5f);
    const ImRect box(ImVec2(std::floor(c.x - w * .5f - pad), std::floor(c.y - h * .5f - pad)),
                     ImVec2(std::floor(c.x + w * .5f + pad), std::floor(c.y + h * .5f + pad)));
    const ImU32 gold = IM_COL32(222, 186, 96, 255);
    auto text_at = [&](float size, ImVec2 p, ImU32 col, const char *b, const char *e = nullptr) {  // (shadowed, as the credits')
        const float o = std::max(1.f, size * .06f);
        dl->AddText(font, size, ImVec2(p.x + o, p.y + o), fade(IM_COL32(0, 0, 0, 170), ((col >> 24) & 0xff) / 255.f), b, e);
        dl->AddText(font, size, p, col, b, e);
    };
    // (whatever is under it dimmed: the one thing that takes input now)
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), fade(IM_COL32(0, 0, 0, 150), a));
    dl->AddRectFilled(ImVec2(box.Min.x + line * 2, box.Min.y + line * 2), ImVec2(box.Max.x + line * 2, box.Max.y + line * 2),
                      fade(IM_COL32(0, 0, 0, 140), a));
    dl->AddRectFilled(box.Min, box.Max, fade(IM_COL32(50, 25, 24, 246), a));
    dl->AddRect(box.Min, box.Max, fade(IM_COL32(112, 76, 34, 255), a), 0, 0, line * 1.6f);
    const float in = line * 3.2f;
    dl->AddRect(ImVec2(box.Min.x + in, box.Min.y + in), ImVec2(box.Max.x - in, box.Max.y - in), fade(gold, a * .75f), 0, 0,
                line * .7f);
    for (ImVec2 p : {box.Min, ImVec2(box.Max.x, box.Min.y), ImVec2(box.Min.x, box.Max.y), box.Max})
        diamond(dl, p, line * 2.6f, fade(gold, a));
    float y = box.Min.y + pad;
    const ImVec2 ts = font->CalcTextSizeA(tsz, FLT_MAX, 0, title);
    text_at(tsz, ImVec2(std::floor(c.x - ts.x * .5f), y), fade(IM_COL32(255, 232, 84, 255), a), title);
    y += tsz + gap * 1.5f;
    {  // (the credits' gold rule, a diamond in its middle)
        const float half = std::min(w * .5f, f * 9);
        dl->AddLine(ImVec2(c.x - half, y), ImVec2(c.x - f * .6f, y), fade(gold, a * .6f), line * .7f);
        dl->AddLine(ImVec2(c.x + f * .6f, y), ImVec2(c.x + half, y), fade(gold, a * .6f), line * .7f);
        diamond(dl, ImVec2(c.x, y), f * .3f, fade(gold, a * .9f));
        diamond(dl, ImVec2(c.x, y), f * .13f, fade(IM_COL32(74, 34, 31, 255), a));
    }
    y += gap * 1.5f;
    for (const auto &r : rows) {
        const float lw = font->CalcTextSizeA(f, FLT_MAX, 0, r.first, r.second).x;
        text_at(f, ImVec2(std::floor(c.x - lw * .5f), y), fade(IM_COL32(242, 238, 232, 255), a), r.first, r.second);
        y += lead;
    }
    y += gap * .25f;
    {  // (the signature at the text's right)
        const float x = std::floor(c.x + w * .5f - sw);
        if (glyph) {
            text_at(f, ImVec2(x, y), fade(gold, a), thanks_signature());
        } else {  // (the dash drawn at the height of the font's hyphen, as long as an em dash, a stroke thick)
            ImFontBaked *baked = font->GetFontBaked(f);
            const ImFontGlyph *hyphen = baked ? baked->FindGlyphNoFallback('-') : nullptr;
            const float k = baked && baked->Size > 0 ? f / baked->Size : 1, o = std::max(1.f, f * .06f);
            const float mid = hyphen ? y + (hyphen->Y0 + hyphen->Y1) * .5f * k : y + f * .55f;
            const float stroke = std::max(1.f, std::floor(f * .08f + .5f));
            const float y0 = std::floor(mid - stroke * .5f), y1 = y0 + stroke;
            dl->AddRectFilled(ImVec2(x + o, y0 + o), ImVec2(x + bar + o, y1 + o), fade(IM_COL32(0, 0, 0, 170), a));
            dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + bar, y1), fade(gold, a));
            text_at(f, ImVec2(x + bar + space, y), fade(gold, a), name);
        }
    }
    y += f + gap * 2.5f;
    // Continue: a plate as the menu's and the touch controls' buttons (dark, a bronze edge, a gold rule inside, gold
    // studs), dim until it takes answers, lit under the mouse pointer or a finger
    const ImRect btn(ImVec2(std::floor(c.x - bw * .5f), std::floor(y)), ImVec2(std::floor(c.x + bw * .5f), std::floor(y + bh)));
    const bool over = live && armed && ImGui::IsMousePosValid(&io.MousePos) && btn.Contains(io.MousePos);
    const bool lit = over && (io.MouseSource == ImGuiMouseSource_Mouse || io.MouseDown[0]);
    const float on = armed ? a : a * .45f;
    dl->AddRectFilled(ImVec2(btn.Min.x, btn.Min.y + line), ImVec2(btn.Max.x, btn.Max.y + line), fade(IM_COL32(0, 0, 0, 110), a));
    dl->AddRectFilled(btn.Min, btn.Max, fade(lit ? IM_COL32(92, 62, 22, 255) : IM_COL32(28, 20, 15, 235), a));
    dl->AddRect(btn.Min, btn.Max, fade(IM_COL32(112, 76, 34, 255), on), 0, 0, line);
    const float bi = line * 1.8f;
    dl->AddRect(ImVec2(btn.Min.x + bi, btn.Min.y + bi), ImVec2(btn.Max.x - bi, btn.Max.y - bi), fade(gold, on * .7f), 0, 0,
                std::max(1.f, line * .45f));
    for (ImVec2 p : {btn.Min, ImVec2(btn.Max.x, btn.Min.y), ImVec2(btn.Min.x, btn.Max.y), btn.Max})
        diamond(dl, p, line * 1.5f, fade(gold, on));
    if (lit)
        dl->AddRect(ImVec2(btn.Min.x - line, btn.Min.y - line), ImVec2(btn.Max.x + line, btn.Max.y + line),
                    fade(IM_COL32(255, 214, 92, 255), a * .7f), 0, 0, line);
    const ImVec2 gs = font->CalcTextSizeA(f, FLT_MAX, 0, go);
    text_at(f, ImVec2(std::floor(c.x - gs.x * .5f), std::floor(btn.GetCenter().y - gs.y * .5f)),
            fade(lit ? IM_COL32(255, 214, 92, 255) : IM_COL32(244, 232, 200, 255), on), go);
}
// On the screen: open, or fading out (draw_credits waits for it: never one on the other)
bool thanks_on(double now) { return g_thanks == Thanks::Open || thanks_fading(now); }
// Waiting for the credits too: the overlay draws frames for it (settings_panel_credits_up), also with nothing else on the
// screen
bool thanks_up(double now) {
    thanks_decide();
    return g_thanks == Thanks::Wait || thanks_on(now);
}
// A frame of it, the first thing settings_panel does (only inside the overlay's frame): it opens once the credits of the
// first start are done; while it is open, its answer and the frame's input before anything else reads them; then its
// drawing, also as it fades out
void thanks_frame() {
    const double now = SDL_GetTicks() / 1000.0;
    thanks_decide();
    if (g_thanks == Thanks::Wait && g_credits_at >= 0 && now - g_credits_at > kCreditsShown + THANKS_AFTER_S)
        thanks_open(now, false);
    if (!thanks_on(now))
        return;
    bool live = g_thanks == Thanks::Open;
    const double t = now - g_thanks_at;
    if (live && thanks_input(t)) {  // (closed: input is the game's, or the menu's, from the next frame on)
        g_thanks = Thanks::Done, g_thanks_shut = now, live = false;
        fprintf(stderr, "[settings] the thank-you was closed after %.1f s\n", t);
        thanks_seen();
    }
    const float a = live ? (float)std::clamp(t / THANKS_IN_S, 0.0, 1.0)
                         : (float)std::clamp(1 - (now - g_thanks_shut) / THANKS_OUT_S, 0.0, 1.0);
    if (a > 0)
        draw_thanks(a, live, live && t >= g_thanks_arm);
}
} // namespace
// The game's thread, after ImGui::Render: the menu's and the dropdown's fading copies (ghost_take) into this frame's
// draw data, faded a little more each frame, under the foreground's notices and credits; dropped once faded out, or if
// the font atlas they were drawn with has since been replaced
void settings_panel_after_render(ImDrawData *dd) {
#ifndef __ANDROID__
    for (ImDrawList *l : ghost_shown)  // (last frame's: the overlay copied them for the render thread already)
        IM_DELETE(l);
    ghost_shown.clear();
    double now = anim_now();
    const ImTextureData *atlas = ImGui::GetIO().Fonts->TexRef._TexData;
    for (size_t i = 0; i < ghosts.size();) {
        Ghost &g = ghosts[i];
        float t = (float)((now - g.at) / ANIM_OUT);
        bool over = t >= 1;
        for (ImDrawList *l : g.lists)
            for (const ImDrawCmd &c : l->CmdBuffer)
                over |= c.ElemCount && c.TexRef._TexData != atlas;  // (compared, never read: it may be gone)
        if (over) {
            for (ImDrawList *l : g.lists)
                IM_DELETE(l);
            ghosts.erase(ghosts.begin() + i);
            continue;
        }
        float a = (1 - t) * (1 - t);
        ImVec2 off(g.slide.x * t, g.slide.y * t);
        for (ImDrawList *l : g.lists) {
            ImDrawList *copy = l->CloneOutput();
            copy->_PopUnusedDrawCmd();
            tween_list(copy, 0, copy->VtxBuffer.Size, a, off, true);
            ghost_shown.push_back(copy);
            // (added as ImDrawData::AddDrawList does, without its checks of a list being written: a copy has no writer)
            dd->CmdLists.push_back(copy);
            dd->CmdListsCount++;
            dd->TotalVtxCount += copy->VtxBuffer.Size;
            dd->TotalIdxCount += copy->IdxBuffer.Size;
        }
        i++;
    }
    if (ghost_shown.empty())
        return;
    for (int i = 0; i + 1 < dd->CmdLists.Size; i++)  // (the foreground's list back to the end)
        if (dd->CmdLists[i]->_OwnerName && !strcmp(dd->CmdLists[i]->_OwnerName, "##Foreground")) {
            ImDrawList *fg = dd->CmdLists[i];
            dd->CmdLists.erase(dd->CmdLists.Data + i);
            dd->CmdLists.push_back(fg);
            break;
        }
#else
    (void)dd;
#endif
}
// Something of the menu is still fading out: the overlay draws its frames until it is gone
bool settings_panel_fading() {
#ifndef __ANDROID__
    return !ghosts.empty() || !ghost_shown.empty();
#else
    return false;
#endif
}
void settings_panel_request_close() { request_exit.store(true); }
extern "C" void settings_panel_request_view(int view) { request_view.store(view); }
void settings_panel_request_back() { request_back.store(true); }
bool settings_panel_dirty() { return is_dirty.load(); }
int settings_panel_theme() { return s.editing ? s.draft.menu_theme : config_editable().menu_theme; }
bool settings_panel_confirming() { return is_confirming.load() || request_exit.load(); }
// (the thank-you over the menu choosing a key: the press is the thank-you's, not a key chosen)
bool settings_panel_key(int scancode) {
    if (!is_capturing.load() || g_thanks == Thanks::Open)
        return false;
    captured_key = scancode;
    return true;
}
bool settings_panel_mouse(int button) {
    if (!capture_mouse.load() || button < 1 || button > SDL_BUTTON_X2 || g_thanks == Thanks::Open)
        return false;
    captured_key = KEY_MOUSE + button;
    return true;
}
bool settings_panel_wheel(int notches) {
    if (!capture_wheel.load() || !notches || g_thanks == Thanks::Open)
        return false;
    captured_key = notches > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
    return true;
}
bool settings_panel_modal() { return g_thanks == Thanks::Open; }
static std::atomic<int> g_typing{0};  // (the menu's text page: 1 open, 2 a password's; for runtime/textinput.c)
bool settings_panel_typing(bool *secret) {
    int t = g_typing.load(std::memory_order_relaxed);
    if (secret) *secret = t == 2;
    return t != 0;
}
// Ownership has to precede NewFrame: native cancel/windowing runs there.
void settings_panel_before_frame(bool open) {
    // (the thank-you, the menu open under it or not: every key, so ImGui's own navigation neither moves nor activates the
    // menu's row under it - that is decided in NewFrame - and the wheel scrolls nothing; thanks_input still reads them, and
    // holds them for the rest of the frame)
    if (g_thanks == Thanks::Open) {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
            ImGui::SetKeyOwner((ImGuiKey)k, THANKS_OWNER);
        return;
    }
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
void settings_panel_idle(void) {
    s.editing = false;
    is_dirty = false;
    is_confirming = false;
    is_capturing = false;
    capture_mouse = false;
}
bool settings_panel_credits_up(void) {
    // (and the thank-you after them: it waits for them, shows and fades out over frames of its own. Asked for on Credits &
    // Licenses they need no more than their time: g_credits_asked stays set once asked, and kept frames coming for good)
    const double now = SDL_GetTicks() / 1000.0;
    return g_credits_at < 0 || now - g_credits_at <= kCreditsShown || thanks_up(now);
}
void settings_panel(bool *open) {
    thanks_frame();  // (first: while the thank-you is open, nothing after it gets the frame's input)
    g_typing.store(*open && s.view == View::Keyboard ? (s.keyboard.secret ? 2 : 1) : 0, std::memory_order_relaxed);
    if (!*open) {
        settings_panel_idle();
        draw_credits();
        return;
    }
    if (g_credits_asked)
        draw_credits();
    bool opening = !s.editing;
    if (opening) {
        s.draft = s.saved = config_editable();
#ifndef __ANDROID__  // (not when closing the window opened it on Exit game: the player didn't open it)
        if (!s.saved.menu_hint_seen && !request_exit.load())
            menu_hint_seen();
#endif
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
        s.exit_only = false;
        s.opened_at = anim_now(), s.view_at = -1;
#ifndef __ANDROID__
        ghosts_clear();  // (closed and opened again at once: no fading copy over it)
        s.dropdown_drawn = false;
#endif
        quest_keys();
        load_mods();
        load_cheats();
#ifndef __ANDROID__
        load_cheat_files();
#endif
    }
    if (request_exit.exchange(false) && s.view != View::Exit) {
        push(View::Exit, "dialog.cancel");
        s.exit_only = opening;  // (the menu was closed: the window's close button alone opened it)
        if (opening)
            s.view_at = -1;  // (it comes in with the menu)
    }
    switch (request_view.exchange(0)) {  // (a desktop's file dialog answered: runtime/pc_actions.c)
    case SETTINGS_VIEW_IMPORT_SAVE: if (s.view != View::ImportSave) push(View::ImportSave, "dialog.cancel"); break;
    case SETTINGS_VIEW_RESTART:  // (cheats or a mod added: the tabs list them now)
#ifndef __ANDROID__
        refresh_mods();
        load_cheat_files();
#endif
        if (s.view != View::Restart) push(View::Restart, "dialog.cancel");
        break;
    default: break;
    }
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
    s.input = overlay_last_input();  // (the same for the whole frame: the guides are measured, then drawn)
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
    auto *window = ImGui::GetCurrentWindow();
    int content_from = window->DrawList->VtxBuffer.Size;  // (after the panel: a view's contents, which come in)
    if (s.restore_scroll) {
        ImGui::SetScrollY(s.scroll);
        s.restore_scroll = false;
    }
    back_keys(open);
    View frame_view = s.view;  // (a view changed after this, by a row, shows from the next frame)
    int guide_items = view_guides();
    bool pinned_guides = guide_items != 0;
    bool clipped_body = false;
    float spacing = ImGui::GetStyle().ItemSpacing.y;
    GuideLayout guides = guide_layout(ImGui::GetContentRegionAvail().x, pinned_guides ? guide_items : GUIDE_ALL);
    // pinned above the guides: the help of the row focused or pointed at, with room for the longest of the tab
    float help_area = pinned_guides && s.view == View::Main && s.help_height > 0 ? s.help_height + spacing * 2 : 0;
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
        for (int i = 0; i < PAGES; i++) {
            if (i % columns)
                ImGui::SameLine();
            else
                ImGui::SetCursorPosX(tabs_start.x - ImGui::GetWindowPos().x + gutter);
            ImGui::PushID(i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool active = s.page == i;
            // (lit lighter under the mouse while the mouse is what moves, as the selected one while pressed)
            ImU32 tab = active ? look.tab_active : look.tab;
            ImGui::PushStyleColor(ImGuiCol_Button, vec(tab));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec(s.input == USED_MOUSE ? look.row_hover : tab));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, vec(look.row_focus));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, look.rounding);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::Button("###tab", ImVec2(tabw, tabh)) && !s.dragged && !s.swallow)
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
            ImGui::PopStyleColor(3);
            ImGui::PopID();
        }
        // At the tabs' sides, what changes the tab: the controller's L / R after a controller's press, else the keys Q /
        // E; a click on one changes it too
        for (int side = 0; side < 2; side++) {
            const char *key = side ? "E" : "Q";
            bool pad = s.input == USED_PAD;
            float gw = pad ? fw * 1.5f : SettingsIcons::key_width(fw, key);
            ImVec2 gp(side ? tabs_start.x + avail - gw : tabs_start.x, tabs_start.y + (header_height - fw) * .5f);
            ImGui::SetCursorScreenPos(gp);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::InvisibleButton(side ? "###next_tab" : "###tab_before", ImVec2(gw, fw)) && !s.dragged && !s.swallow)
                first_page((s.page + (side ? 1 : PAGES - 1)) % PAGES);
            bool lit = ImGui::IsItemActive() || (ImGui::IsItemHovered() && s.input == USED_MOUSE);
            ImGui::PopItemFlag();
            if (pad)
                SettingsIcons::draw(header, gp, fw, s.draft.button_icons, side ? SettingsIcons::Right : SettingsIcons::Left, 0);
            else
                SettingsIcons::key(header, gp, fw, key);
            if (lit)
                header->AddRect(ImVec2(gp.x - 2, gp.y - 2), ImVec2(gp.x + gw + 2, gp.y + fw + 2), look.accent_bright,
                                fw * .25f, 0, 1.5f);
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
    if (s.view == View::Licenses)
        licenses_text(look, pinned_guides ? guides.height + spacing * 2 : 0);
    float help_wrap = window->InnerRect.GetWidth() - 2 * ImGui::GetStyle().WindowPadding.x;
    std::string help;
    if (s.view == View::Main) {  // (the row moved to, or the one the mouse moved onto)
        static std::string last_focus, last_hovered;
        if (s.focus != last_focus)
            last_focus = s.help_row = s.focus;
        // (or that the mouse, while it is what moves, has just come onto: its first move after the menu opens has no
        // delta, ImGui gets the mouse's events only while the menu is open)
        ImVec2 moved = ImGui::GetIO().MouseDelta;
        if ((moved.x || moved.y || (s.input == USED_MOUSE && s.hovered != last_hovered)) && !s.hovered.empty())
            s.help_row = s.hovered;
        last_hovered = s.hovered;
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
        if (std::any_of(rows.begin(), rows.end(), [](const Row &row) { return row.cold; }))  // (only where a row has the *)
            ImGui::TextUnformatted(tr("* Restart required"));
        // Keep scroll space for the pinned hints so the last action can scroll fully above them.
        ImGui::Dummy(ImVec2(1, guides.height + help_area + ImGui::GetStyle().ItemSpacing.y));
    } else if (pinned_guides)  // (a dialog's or a page's guides: the same room under its rows)
        ImGui::Dummy(ImVec2(1, guides.height + spacing));
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
    if (pinned_guides)
        draw_guides(window, guides, guide_top);
    if (s.view != View::Main)
        s.header_bottom = ImGui::GetStyle().WindowPadding.y + ImGui::GetFontSize() * 1.5f;
    int content_to = window->DrawList->VtxBuffer.Size;  // (before the frame's rules and the grip)
    draw_frame(window, look);
    draw_grip(window, look);
    ImGui::End();
    bool dropdown_now = s.dropdown.open && s.dropdown.anchored && !options.empty();
    if (dropdown_now)
        draw_dropdown(options, scale, base);
    else
        s.dropdown.hovered = false;
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
#ifndef __ANDROID__
    // Coming in: the menu down into place, fading in; a dialog or a page, its contents over the panel that stays.
    // Going out: the menu (closed this frame) and the dropdown (closed since its last frame) fade from here, as copies
    float f = base * scale, opened = anim_in(s.opened_at), viewed = anim_in(s.view_at);
    if (opened < 1) {
        tween_window(window, opened, ImVec2(0, -(1 - opened) * f * .9f));
    } else if (viewed < 1 && s.view == frame_view) {
        float a = .25f + .75f * viewed;
        ImVec2 off(0, (1 - viewed) * f * .45f);
        tween_list(window->DrawList, content_from, content_to, a, off, false);
        for (ImGuiWindow *c : window->DC.ChildWindows)
            if (c->Active)
                tween_window(c, a, off);
    }
    ImGuiWindow *list = ImGui::FindWindowByName("Settings##dropdown");
    ImVec2 back_in(0, (s.dropdown_up ? 1 : -1) * f * .4f);  // (the dropdown: towards its row)
    if (!*open) {
        ghost_take(window, true, ImVec2(0, -f * .6f));
        if (dropdown_now)
            ghost_take(list, true, back_in);
    } else if (s.dropdown_drawn && !dropdown_now)
        ghost_take(list, false, back_in);
    s.dropdown_drawn = dropdown_now && *open;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        s.swallow = false;  // (the press it was for has let go)
#endif
    report(open);
}
