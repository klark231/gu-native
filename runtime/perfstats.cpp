// Performance readout ([Performance] ShowStats): frames per second, the frame time and, on a phone, its temperature, in
// a small plate at the top of the screen in the game's window style, as the touch controls draw it (runtime/touch.cpp):
// wine-dark, a black edge, red and green rules top and bottom, gold labels and cream values. It sits in the gap the
// touch HUD leaves at the top, between the health bar's end (~49% of the width) and the target camera's icons (~76%).
//   - frames: those presented (g_present_count: the renderer counts them), over half a second; the frame time is
//     theirs on average, and the longest gap between the game's frames in the last second (this is drawn once a frame,
//     on the game's update thread: runtime/overlay.cpp)
//   - temperature (Android): the battery's (the sticky ACTION_BATTERY_CHANGED intent: no permission needed) and
//     PowerManager's thermal status and headroom, from DeviceStats.java by JNI on the host main thread every 2 s while
//     shown (perfstats_poll), kept in atomics for the game thread
//   - a PC (Windows): On (the menu's Basic) adds the GPU's temperature; Full, a line on the GPU (use, temperature,
//     clock, video memory) and one on the CPU (the game's use and memory, the CPU's clock, the time the game's update
//     and render threads take per frame), from a sampler on a thread of its own (runtime/platform/win_stats.c), put in
//     words when it has a new snapshot. The plate reaches right as far as the target camera's icon and the lock-on's
//     padlock by it leave room, and down no further than the minimap under them (pc_bounds: the HUD sits in the
//     picture's middle 16:9 by default, so on a wider screen the icon is well inside the window's edge), at most 90% of
//     the width. It never goes past those bounds: Full puts the threads' times after the CPU's figures or on a fourth
//     line, whichever lets the text be larger, and the text gets as small as it must.
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "imgui.h"

#include "config.h"
#include "logwriter.h"
#include "perfstats.h"
#ifdef _WIN32
#include "platform/win_stats.h"
#include "settings_locale.h"
#include "touch.h"
#endif

#define LOG(...) rt_logf(__VA_ARGS__)  // (Windows: queued, the log's own thread writes it)

extern "C" volatile uint64_t g_present_count;  // (runtime/ui.c)

namespace {

#ifdef __ANDROID__
std::atomic<int> g_temp{INT_MIN};    // the battery's temperature, tenths of a degree C (INT_MIN: not known)
std::atomic<int> g_thermal{-1};      // PowerManager.THERMAL_STATUS_*: 0 none .. 6 shutdown (-1: not known)
std::atomic<float> g_headroom{NAN};  // getThermalHeadroom(10): 1 = the phone slows itself down (NaN: not known)

// A Java exception pending: logged and cleared (every JNI call after one would fail). true: there was one.
bool jfail(JNIEnv *env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

// com.mhgu.recomp.DeviceStats through the activity's class loader (FindClass from native code sees only the system's
// classes: runtime/ui.c restart_application found the same). A global reference, or NULL.
jclass device_stats_class(JNIEnv *env) {
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!act) return nullptr;
    jclass act_cls = env->GetObjectClass(act), loader_cls = nullptr, found = nullptr;
    jobject loader = nullptr, cls = nullptr;
    jstring name = nullptr;
    jmethodID get_loader = env->GetMethodID(act_cls, "getClassLoader", "()Ljava/lang/ClassLoader;"), load = nullptr;
    if (!jfail(env) && get_loader) loader = env->CallObjectMethod(act, get_loader);
    if (!jfail(env) && loader) loader_cls = env->FindClass("java/lang/ClassLoader");
    if (!jfail(env) && loader_cls) load = env->GetMethodID(loader_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!jfail(env) && load) name = env->NewStringUTF("com.mhgu.recomp.DeviceStats");
    if (!jfail(env) && name) cls = env->CallObjectMethod(loader, load, name);
    if (!jfail(env) && cls) found = (jclass)env->NewGlobalRef(cls);
    for (jobject o : {cls, (jobject)name, (jobject)loader_cls, loader, (jobject)act_cls, act})
        if (o) env->DeleteLocalRef(o);
    return found;
}
#endif

const ImU32 WINE = IM_COL32(50, 25, 24, 230), RED = IM_COL32(176, 34, 30, 255), MOSS = IM_COL32(80, 142, 54, 255),
            GOLD = IM_COL32(222, 186, 96, 255), DIM = IM_COL32(222, 186, 96, 150), INK = IM_COL32(244, 232, 200, 255),
            // (the FPS and the temperature: green, gold or red, lighter than the rules' to read well on the plate)
            GOOD = IM_COL32(132, 204, 96, 255), BAD = IM_COL32(236, 84, 64, 255);
const char *const SEP = " \xC2\xB7 ";  // (U+00B7)

// v with one decimal (or digits): a comma in Spanish
void decimal(char *buf, size_t size, double v, bool comma, int digits = 1) {
    snprintf(buf, size, "%.*f", digits, v);
    for (char *p = buf; comma && *p; p++)
        if (*p == '.') *p = ',';
}

struct Run { const char *text; ImU32 col; };

#ifdef _WIN32
// (a PC) the sampler's figures as runs: Basic, the GPU's temperature for line 1's end; Full, the GPU's line, the CPU's
// and the threads' times (after the CPU's figures, or on a line of their own where that lets the text be larger).
// Hidden when not known.
struct PcRuns {
    Run temp[4], lines[3][24];
    int n_temp, n[3], used;
    char text[20][16];  // (the numbers, and the labels that are words)
};
PcRuns g_pc;

// Where the plate must end, in the HUD's own geometry (nvn/vulkan/exec.c: the 1280x720 canvas over the picture, which
// follows the window's shape; on a wider picture the HUD is narrowed into its width, UIAnchor, and on a narrower one
// flattened toward the top and bottom):
//   - right: short of the target camera's icon and of the lock-on's padlock by it (touch.cpp lock_mark: up to .042 of
//     the height left of the icon's zone). Where the icon was drawn last (touch_hud_zone: the touch controls shown or
//     not, and wherever it was moved), else where the HUD puts it (x 1200, right-anchored; its art and margin 40 units
//     left of that; the touch HUD's, 250 units further left by the picture's edge); at most 90% of the width
//   - bottom: above the minimap, which begins under that icon at about y 108 (seen at 16:9 and at 3440x1440)
void pc_bounds(float w, float h, float *right, float *bottom) {
    float ra = g_render_aspect > 0 ? g_render_aspect : w / h, pw = w, ph = w / ra;
    if (ph > h) ph = h, pw = h * ra;
    float s = ra > 16.0f / 9.0f ? (16.0f / 9.0f) / ra : 1.0f, sy = ra < 16.0f / 9.0f ? ra / (16.0f / 9.0f) : 1.0f;
    float zone[4], left;
    if (touch_hud_zone(HP_TARGET, w, h, zone)) {
        left = zone[0];
    } else {
        float a = std::min(g_ui_anchor_aspect, ra) / ra;
        float x = g_touch_hud ? 1280 - (370 - g_hud_dx[HUD_TOUCH][HP_TARGET]) * s : 640 * (1 + a) - 120 * s;
        left = (w - pw) * .5f + x * pw / 1280;
    }
    *right = std::min(.9f * w, left - .045f * h);
    *bottom = (h - ph) * .5f + 108 * sy * ph / 720 - .005f * h;
    // (the minimap moved or sized by the player, beside or under it: down no further than its frame)
    int prof = g_hud_prof_now;
    if (prof >= 0 && prof < HUD_PROFILES && hud_custom(HP_MAP, prof) && touch_hud_part_rect(HP_MAP, w, h, zone) && zone[0] < *right)
        *bottom = std::min(*bottom, std::max(.05f * h, zone[1] - .005f * h));
}

ImU32 temp_colour(float c) { return c >= 90 ? BAD : c >= 80 ? GOLD : INK; }  // (a GPU: warm from 80 C, hot from 90)

void pc_format(const WinStats *s, int level, bool es) {
    PcRuns &p = g_pc;
    p.n_temp = p.n[0] = p.n[1] = p.n[2] = p.used = 0;
    if (!s) return;
    auto slot = [&]() { return p.text[std::min(p.used++, 19)]; };
    auto num = [&](double v, int digits) -> const char * {
        char *b = slot();
        decimal(b, sizeof p.text[0], v, es, digits);
        return b;
    };
    auto word = [&](const char *english) -> const char * {  // (settings_locale.h has it in Spanish; a space after it)
        char *b = slot();
        snprintf(b, sizeof p.text[0], "%s ", settings_text(es ? 1 : 0, english));
        return b;
    };
    const char *DEG = " \xC2\xB0" "C";  // (U+00B0)
    if (level == 1) {  // " · GPU 61 °C"
        if (std::isfinite(s->gpu_temp)) {
            p.temp[0] = {SEP, DIM}, p.temp[1] = {"GPU ", GOLD};
            p.temp[2] = {num(s->gpu_temp, 0), temp_colour(s->gpu_temp)}, p.temp[3] = {DEG, GOLD};
            p.n_temp = 4;
        }
        return;
    }
    for (int l = 0; l < 3; l++) {
        Run *r = p.lines[l];
        int &n = p.n[l];
        bool any = false;
        auto add = [&](const char *t, ImU32 c) { if (n < 24) r[n++] = {t, c}; };
        auto next = [&]() { if (any) add(SEP, DIM); any = true; };  // (before each figure: a dot between them)
        if (l == 0) {  // "GPU 52% · 61 °C · 1.85 GHz · VRAM 1.2/15.2 GB"
            if (std::isfinite(s->gpu_use) || std::isfinite(s->gpu_temp) || std::isfinite(s->gpu_ghz)) add("GPU ", GOLD);
            if (std::isfinite(s->gpu_use))  // (gold from 95%: the GPU holds the frame rate back)
                next(), add(num(s->gpu_use, 0), s->gpu_use >= 95 ? GOLD : INK), add("%", GOLD);
            if (std::isfinite(s->gpu_temp)) next(), add(num(s->gpu_temp, 0), temp_colour(s->gpu_temp)), add(DEG, GOLD);
            if (std::isfinite(s->gpu_ghz)) next(), add(num(s->gpu_ghz, 2), INK), add(" GHz", GOLD);
            if (std::isfinite(s->vram_gb) && s->vram_budget_gb > 0) {  // (red above 90% of the budget)
                next(), add("VRAM ", GOLD), add(num(s->vram_gb, 1), s->vram_gb > .9f * s->vram_budget_gb ? BAD : INK);
                add("/", DIM), add(num(s->vram_budget_gb, 1), INK), add(" GB", GOLD);
            }
        } else if (l == 1) {  // "CPU 13% · 4.32 GHz · RAM 1.0 GB"
            if (std::isfinite(s->cpu_use) || std::isfinite(s->cpu_ghz)) add("CPU ", GOLD);
            if (std::isfinite(s->cpu_use)) next(), add(num(s->cpu_use, 0), INK), add("%", GOLD);
            if (std::isfinite(s->cpu_ghz)) next(), add(num(s->cpu_ghz, 2), INK), add(" GHz", GOLD);
            if (std::isfinite(s->ram_gb)) next(), add("RAM ", GOLD), add(num(s->ram_gb, 1), INK), add(" GB", GOLD);
        } else {  // "game 3.1 ms · render 2.4 ms" (a thread busy 90% of the time or more: gold, it holds the frame rate back)
            if (std::isfinite(s->game_ms))
                next(), add(word("game"), GOLD), add(num(s->game_ms, 1), s->game_busy >= .9f ? GOLD : INK), add(" ms", GOLD);
            if (std::isfinite(s->render_ms))
                next(), add(word("render"), GOLD), add(num(s->render_ms, 1), s->render_busy >= .9f ? GOLD : INK),
                    add(" ms", GOLD);
        }
    }
}
#endif

}  // namespace

extern "C" void perfstats_poll(void) {
#ifdef __ANDROID__
    static uint64_t last;
    static bool failed;
    static jclass cls;
    static jmethodID m_temp, m_status, m_headroom;
    if (failed || !config_live_show_stats()) return;
    uint64_t now = SDL_GetTicksNS();
    if (last && now - last < 2000000000ull) return;
    last = now;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env) return;
    if (!cls) {
        cls = device_stats_class(env);
        if (cls) m_temp = env->GetStaticMethodID(cls, "batteryTempTenths", "()I");
        if (cls && !jfail(env)) m_status = env->GetStaticMethodID(cls, "thermalStatus", "()I");
        if (cls && !jfail(env)) m_headroom = env->GetStaticMethodID(cls, "thermalHeadroom", "()F");
        jfail(env);
        if (!cls || !m_temp || !m_status || !m_headroom) {
            failed = true;
            LOG("[perf] com.mhgu.recomp.DeviceStats not usable: no temperature on screen\n");
            return;
        }
    }
    jint t = env->CallStaticIntMethod(cls, m_temp);
    g_temp = jfail(env) ? INT_MIN : (int)t;
    jint s = env->CallStaticIntMethod(cls, m_status);
    g_thermal = jfail(env) ? -1 : (int)s;
    jfloat r = env->CallStaticFloatMethod(cls, m_headroom);
    g_headroom = jfail(env) ? NAN : (float)r;
#elif defined(_WIN32)
    winstats_level(config_live_show_stats());  // (the sampler started, told the level, or left idle: nothing while unchanged)
#endif
}

extern "C" bool perfstats_visible(void) { return config_live_show_stats() != 0; }

// The plate's lower edge as last drawn (display pixels) and when: the port's notices go under it (runtime/overlay.cpp)
static float g_plate_bottom;
static uint64_t g_plate_ns;
extern "C" float perfstats_bottom(void) {
    return g_plate_ns && SDL_GetTicksNS() - g_plate_ns < 200000000ull ? g_plate_bottom : 0.0f;
}

extern "C" void perfstats_draw(float w, float h) {
    // the measures: the frames presented in each half second; the longest gap between two of these calls (the game's
    // frames) in each second, the last whole second's shown (the one going on until there is one)
    static uint64_t last, win_t0, win_n0, sec_t0;
    static float fps = -1, avg_ms, worst_cur, worst_ms;
    uint64_t now = SDL_GetTicksNS(), n = g_present_count;
    if (!last || now - last > 5000000000ull) {  // (first shown, or after a long gap: hidden meanwhile, the app paused;
                                                // a stall of a second or more still shows)
        win_t0 = sec_t0 = now, win_n0 = n;
        fps = -1, worst_cur = worst_ms = 0;
    } else {
        worst_cur = std::max(worst_cur, (now - last) / 1e6f);
        if (now - win_t0 >= 500000000ull) {
            double s = (now - win_t0) / 1e9;
            fps = (float)((n - win_n0) / s);
            avg_ms = n > win_n0 ? (float)(s * 1e3 / (double)(n - win_n0)) : 0;
            win_t0 = now, win_n0 = n;
        }
        if (now - sec_t0 >= 1000000000ull) worst_ms = worst_cur, worst_cur = 0, sec_t0 = now;
    }
    last = now;
    if (fps < 0) return;  // (nothing measured yet)

    // the line: "60 FPS · 16.6 ms · max 24.1 · 38.5 °C (moderate)", the status only when there is one
    bool es = config_language() == 1;
    char f[16], ms[16], mx[16];
    snprintf(f, sizeof f, "%.0f", fps);
    decimal(ms, sizeof ms, avg_ms, es);
    decimal(mx, sizeof mx, worst_ms > 0 ? worst_ms : worst_cur, es);
    Run runs[64];
    int nr = 0;
    auto add = [&](const char *t, ImU32 c) { if (nr < 64) runs[nr++] = {t, c}; };
    float cap = config_live_fps();
    if (cap > FPS_PHONE_MAX) cap = 60;  // (Unlimited, or more than a phone takes: good from 60 up)
    add(f, fps >= cap * .95f ? GOOD : fps >= cap * .75f ? GOLD : BAD);
    add(" FPS", GOLD);
    add(SEP, DIM);
    add(avg_ms > 0 ? ms : "--", INK);
    add(" ms", GOLD);
    add(SEP, DIM);
    add(es ? "m\xC3\xA1x " : "max ", GOLD);
    add(mx, INK);
#ifdef __ANDROID__
    static const char *const STATUS_EN[7] = {"", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    static const char *const STATUS_ES[7] = {"", "leve", "moderado", "grave", "cr\xC3\xADtico", "emergencia", "apagando"};
    char tc[16];
    int t = g_temp, st = g_thermal;
    float room = g_headroom;  // (NaN: every comparison false)
    if (t != INT_MIN) {
        bool hot = st >= 2 || room >= 1.0f || t >= 450, warm = st == 1 || room >= .85f || t >= 420;
        decimal(tc, sizeof tc, t / 10.0, es);
        add(SEP, DIM);
        add(tc, hot ? BAD : warm ? GOLD : INK);
        add(" \xC2\xB0" "C", GOLD);  // (U+00B0)
    }
    if (st >= 1 && st <= 6) {
        if (t != INT_MIN) add(" (", GOLD); else add(SEP, DIM);
        add(es ? STATUS_ES[st] : STATUS_EN[st], st >= 2 ? BAD : GOLD);
        if (t != INT_MIN) add(")", GOLD);
    }
#endif
    // the plate: text 2.6% of the screen's height (smaller, down to 80%, if it would not fit the gap); as wide as its
    // widest line (a PC's Full readout has three or four)
    struct Span { int b, e; };  // (a line: runs[b] up to runs[e])
    Span lines[4] = {{0, nr}};
    int nl = 1;
    ImFont *font = ImGui::GetFont();
    float fs = .026f * h, line = .004f * h, padx = .5f * fs, pady = .22f * fs, lead = .18f * fs, tw, k = 1;
    auto width = [&](Span s) {
        float x = 0;
        for (int i = s.b; i < s.e; i++) x += font->CalcTextSizeA(fs, FLT_MAX, 0, runs[i].text).x;
        return x;
    };
    float lo = .49f * w, hi = .76f * w;
#ifdef _WIN32
    {   // the sampler's figures: put in words again with a new snapshot, level or language (about once a second)
        static WinStats ws;
        static uint32_t seen;
        static int seen_level = -1;
        static bool seen_es;
        int level = config_live_show_stats();
        uint32_t seq = winstats_get(&ws, seen);
        if (seq != seen || level != seen_level || es != seen_es) {
            seen = seq, seen_level = level, seen_es = es;
            pc_format(seq ? &ws : nullptr, level, es);
        }
        for (int i = 0; i < g_pc.n_temp; i++) add(g_pc.temp[i].text, g_pc.temp[i].col);
        lines[0].e = nr;
        Span part[3];  // the GPU's line, the CPU's figures, the threads' times
        int sep = -1;  // (the dot between those two when they share a line)
        for (int l = 0; l < 3; l++) {
            if (l == 2 && nr > part[1].b && g_pc.n[2]) sep = nr, add(SEP, DIM);
            part[l].b = nr;
            for (int i = 0; i < g_pc.n[l]; i++) add(g_pc.lines[l][i].text, g_pc.lines[l][i].col);
            part[l].e = nr;
        }
        // between the health bar's end and the target camera's icon, and above the minimap (pc_bounds), never past them:
        // the threads' times after the CPU's figures or on a line of their own, whichever lets the text be larger (the
        // fewer lines when the same), as small as it takes
        float right, bottom;
        pc_bounds(w, h, &right, &bottom);
        hi = std::max(right, lo + .1f * w);
        float tall = std::max(bottom - .01f * h - 2 * line * 1.7f, fs + 2 * pady);  // (the text's room: line 1's at least)
        auto fit = [&](float wide, int n) {
            return std::min({1.0f, (hi - lo) / (wide + 2 * padx), tall / (n * fs + (n - 1) * lead + 2 * pady)});
        };
        float w0 = width(lines[0]), wg = width(part[0]), wc = width(part[1]), wt = width(part[2]);
        float joined = std::max({w0, wg, wc + (sep >= 0 ? width({sep, sep + 1}) : 0) + wt}), split = std::max({w0, wg, wc, wt});
        int n = 1 + (part[0].e > part[0].b) + (part[2].e > part[1].b);  // (the lines with the threads' after the CPU's)
        float kj = fit(joined, n), ks = sep >= 0 ? fit(split, n + 1) : 0;
        bool own = ks > kj;
        tw = own ? split : joined, k = own ? ks : kj;
        if (part[0].e > part[0].b) lines[nl++] = part[0];
        if (own) lines[nl++] = part[1], lines[nl++] = part[2];
        else if (part[2].e > part[1].b) lines[nl++] = {part[1].b, part[2].e};
    }
#else
    tw = width(lines[0]);
    if (tw + 2 * padx > hi - lo) k = std::max((hi - lo) / (tw + 2 * padx), .8f);
#endif
    fs *= k, padx *= k, pady *= k, tw *= k, lead *= k;
    float pw = tw + 2 * padx, ph = nl * fs + (nl - 1) * lead + 2 * pady + 2 * line * 1.7f;
    float x0 = pw <= hi - lo ? std::clamp(.62f * w - pw * .5f, lo, hi - pw) : (lo + hi - pw) * .5f;
    ImVec2 p0(x0, .01f * h), p1(x0 + pw, .01f * h + ph);
    g_plate_bottom = p1.y, g_plate_ns = now;  // (perfstats_bottom: the port's notices go under it)
    ImDrawList *dl = ImGui::GetBackgroundDrawList();  // (under the port's menus and notices, as the touch controls)
    dl->AddRectFilled(p0, p1, WINE);
    dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 255), 0, 0, line);
    dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + line), RED);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + line), ImVec2(p1.x, p0.y + line * 1.7f), MOSS);
    dl->AddRectFilled(ImVec2(p0.x, p1.y - line * 1.7f), ImVec2(p1.x, p1.y - line), MOSS);
    dl->AddRectFilled(ImVec2(p0.x, p1.y - line), p1, RED);
    float y = p0.y + line * 1.7f + pady;
    for (int l = 0; l < nl; l++, y += fs + lead) {
        float x = p0.x + padx;
        for (int i = lines[l].b; i < lines[l].e; i++) {
            dl->AddText(font, fs, ImVec2(x + fs * .05f, y + fs * .06f), IM_COL32(0, 0, 0, 200), runs[i].text);
            dl->AddText(font, fs, ImVec2(x, y), runs[i].col, runs[i].text);
            x += font->CalcTextSizeA(fs, FLT_MAX, 0, runs[i].text).x;
        }
    }
}
