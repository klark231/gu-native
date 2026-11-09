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
#include "settings_locale.h"
#include "perfstats.h"

#define LOG(...) fprintf(stderr, __VA_ARGS__)

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

// v with one decimal: a comma in Spanish
void decimal(char *buf, size_t size, double v, bool comma) {
    snprintf(buf, size, "%.1f", v);
    for (char *p = buf; comma && *p; p++)
        if (*p == '.') *p = ',';
}

struct Run { const char *text; ImU32 col; };

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
#endif
}

extern "C" bool perfstats_visible(void) { return config_live_show_stats() != 0; }

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
    const int lang = config_language();
    bool es = lang == MENU_LANG_ES;
    char f[16], ms[16], mx[16];
    snprintf(f, sizeof f, "%.0f", fps);
    decimal(ms, sizeof ms, avg_ms, es);
    decimal(mx, sizeof mx, worst_ms > 0 ? worst_ms : worst_cur, es);
    const char *SEP = " \xC2\xB7 ";  // (U+00B7)
    Run runs[16];
    int nr = 0;
    auto add = [&](const char *t, ImU32 c) { if (nr < 16) runs[nr++] = {t, c}; };
    float cap = config_live_fps();
    add(f, fps >= cap * .95f ? GOOD : fps >= cap * .75f ? GOLD : BAD);
    add(" FPS", GOLD);
    add(SEP, DIM);
    add(avg_ms > 0 ? ms : "--", INK);
    add(" ms", GOLD);
    add(SEP, DIM);
    add(es ? "m\xC3\xA1x " : lang == MENU_LANG_ZH ? settings_text(lang, "max ") : "max ", GOLD);
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
        add(es ? STATUS_ES[st] : settings_text(lang, STATUS_EN[st]), st >= 2 ? BAD : GOLD);
        if (t != INT_MIN) add(")", GOLD);
    }
#endif

    // the plate: text 2.6% of the screen's height (smaller, down to 80%, if it would not fit the gap)
    ImFont *font = ImGui::GetFont();
    float fs = .026f * h, line = .004f * h, padx = .5f * fs, pady = .22f * fs, tw = 0;
    for (int i = 0; i < nr; i++) tw += font->CalcTextSizeA(fs, FLT_MAX, 0, runs[i].text).x;
    float lo = .49f * w, hi = .76f * w;
    if (tw + 2 * padx > hi - lo) {
        float k = std::max((hi - lo) / (tw + 2 * padx), .8f);
        fs *= k, padx *= k, pady *= k, tw *= k;
    }
    float pw = tw + 2 * padx, ph = fs + 2 * pady + 2 * line * 1.7f;
    float x0 = pw <= hi - lo ? std::clamp(.62f * w - pw * .5f, lo, hi - pw) : (lo + hi - pw) * .5f;
    ImVec2 p0(x0, .01f * h), p1(x0 + pw, .01f * h + ph);
    ImDrawList *dl = ImGui::GetBackgroundDrawList();  // (under the port's menus and notices, as the touch controls)
    dl->AddRectFilled(p0, p1, WINE);
    dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 255), 0, 0, line);
    dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + line), RED);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + line), ImVec2(p1.x, p0.y + line * 1.7f), MOSS);
    dl->AddRectFilled(ImVec2(p0.x, p1.y - line * 1.7f), ImVec2(p1.x, p1.y - line), MOSS);
    dl->AddRectFilled(ImVec2(p0.x, p1.y - line), p1, RED);
    float x = p0.x + padx, y = p0.y + line * 1.7f + pady;
    for (int i = 0; i < nr; i++) {
        dl->AddText(font, fs, ImVec2(x + fs * .05f, y + fs * .06f), IM_COL32(0, 0, 0, 200), runs[i].text);
        dl->AddText(font, fs, ImVec2(x, y), runs[i].col, runs[i].text);
        x += font->CalcTextSizeA(fs, FLT_MAX, 0, runs[i].text).x;
    }
}
