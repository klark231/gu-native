// Typed text for the game's text entries. On the Switch a text entry brings up the system keyboard (on screen, or a
// USB keyboard). Here, while the game asks for text, the physical keyboard types: characters go into the entry (the
// game shows them in its own text box as they change), Backspace deletes, Ctrl+V pastes, Enter confirms, Escape
// cancels. Meanwhile the keys don't act as controller buttons (L = A, WASD...): runtime/host_input.c asks
// textinput_event first. A controller can still confirm the entry as it is with Plus (Start), or cancel with B.
#include <pthread.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "overlay.h"
#include "rt.h"
#include "textinput.h"

#define TEXT_MAX 0x7d0  // bytes (the Switch keyboard's own limit is 500 characters)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_active;
static int g_window_on;  // text input on: 0 no, 1 for an entry, 2 for the port's menu (no screen keyboard), 3 for the
                         // port's menu with the phone's keyboard (its keyboard view asked for it: textinput_menu_keyboard)
// The port's menu (its thread): wants the phone's keyboard; its key's presses, counted (and as far as the window went:
// the main thread's); Back put the keyboard away, not told to the menu yet (meanwhile what it wants is not asked again;
// told, the menu stops wanting it until its key is pressed again)
static int g_menu_keyboard, g_menu_show, g_menu_closed;
#ifdef __ANDROID__
static int g_menu_shown;
#endif
static char g_text[TEXT_MAX + 4];
static int g_max_chars, g_event;  // g_event: TI_* waiting for the game's next poll (ENTER / CANCEL win over CHANGED)

static int utf8_chars(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

static void post(int ev) {
    if (ev > g_event) g_event = ev;
}

void textinput_begin(const char *initial, int max_chars) {
    pthread_mutex_lock(&g_lock);
    g_active = true;
    g_max_chars = max_chars;
    g_event = TI_NONE;
    snprintf(g_text, sizeof g_text, "%s", initial ? initial : "");
    pthread_mutex_unlock(&g_lock);
    LOG("[textinput] text entry (up to %d characters, \"%s\"): type, Enter confirms, Escape cancels\n", max_chars,
        initial ? initial : "");
}

void textinput_end(void) {
    pthread_mutex_lock(&g_lock);
    g_active = false;
    g_event = TI_NONE;
    pthread_mutex_unlock(&g_lock);
}

int textinput_poll(char *out, size_t size, int *chars) {
    pthread_mutex_lock(&g_lock);
    int ev = g_event;
    g_event = TI_NONE;
    snprintf(out, size, "%s", g_text);
    if (chars) *chars = utf8_chars(g_text);
    pthread_mutex_unlock(&g_lock);
    return ev;
}

void textinput_peek(char *out, size_t size) {
    pthread_mutex_lock(&g_lock);
    snprintf(out, size, "%s", g_text);
    pthread_mutex_unlock(&g_lock);
}

bool textinput_active(void) { return __atomic_load_n(&g_active, __ATOMIC_RELAXED); }

void textinput_menu_keyboard(bool want) { __atomic_store_n(&g_menu_keyboard, want ? 1 : 0, __ATOMIC_RELAXED); }
void textinput_menu_keyboard_show(void) { __atomic_add_fetch(&g_menu_show, 1, __ATOMIC_RELAXED); }
bool textinput_menu_keyboard_closed(void) {
    if (!__atomic_load_n(&g_menu_closed, __ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&g_menu_keyboard, 0, __ATOMIC_RELEASE);  // (first: the window doesn't bring it up again meanwhile)
    __atomic_store_n(&g_menu_closed, 0, __ATOMIC_RELEASE);
    return true;
}

void textinput_type(const char *s) {
    pthread_mutex_lock(&g_lock);
    if (g_active) {
        size_t len = strlen(g_text);
        int chars = utf8_chars(g_text);
        bool added = false;
        while (*s) {
            size_t n = 1;  // one character (its UTF-8 bytes)
            while (s[n] && ((unsigned char)s[n] & 0xC0) == 0x80) n++;
            bool control = (unsigned char)*s < 0x20 || *s == 0x7f;  // (pasted line breaks, tabs)
            if (!control) {
                if ((g_max_chars > 0 && chars >= g_max_chars) || len + n > TEXT_MAX) break;
                memcpy(g_text + len, s, n);
                g_text[len += n] = 0;
                chars++;
                added = true;
            }
            s += n;
        }
        if (added) post(TI_CHANGED);
    }
    pthread_mutex_unlock(&g_lock);
}

void textinput_key(int what) {
    pthread_mutex_lock(&g_lock);
    if (g_active) {
        if (what == TI_NONE) {  // Backspace: the last character
            size_t len = strlen(g_text);
            if (len) {
                while (len > 0 && ((unsigned char)g_text[len - 1] & 0xC0) == 0x80) len--;
                if (len > 0) len--;
                g_text[len] = 0;
                post(TI_CHANGED);
            }
        } else {
            post(what);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

bool textinput_event(const SDL_Event *e) {
    if (!textinput_active()) return false;
    switch (e->type) {
    case SDL_EVENT_TEXT_INPUT:
        textinput_type(e->text.text);
        return true;
    case SDL_EVENT_KEY_DOWN: {
        SDL_Scancode sc = e->key.scancode;
        if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return false;  // (screenshots, MSAA...)
        if ((sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) && (e->key.mod & SDL_KMOD_ALT)) return false;
        if ((sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) && !e->key.repeat) textinput_key(TI_ENTER);
        else if ((sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_AC_BACK) && !e->key.repeat) textinput_key(TI_CANCEL);
        else if (sc == SDL_SCANCODE_BACKSPACE) textinput_key(TI_NONE);
        else if (sc == SDL_SCANCODE_V && (e->key.mod & SDL_KMOD_CTRL)) {
            char *clip = SDL_GetClipboardText();
            if (clip) textinput_type(clip);
            SDL_free(clip);
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
        return true;
    }
    return false;
}

#ifdef __ANDROID__
// The phone's keyboard for the port's menu: a server's name or a key, as typed (no capitals, no corrections)
static void start_menu_keyboard(SDL_Window *w) {
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER, SDL_CAPITALIZE_NONE);
    SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, false);
    SDL_StartTextInputWithProperties(w, props);
    SDL_DestroyProperties(props);
}
#endif

void textinput_update(void) {
    int want = textinput_active() ? 1 : overlay_menu_open() ? 2 : 0;  // (Forge's menu takes typed text too)
#ifdef __ANDROID__
    int show = __atomic_load_n(&g_menu_show, __ATOMIC_RELAXED);
    // (Back's mark read before what the menu wants: the menu clears what it wants before the mark, so one taking the mark
    // meanwhile never brings the keyboard up again for a pass - textinput_menu_keyboard_closed)
    if (want == 2 && !__atomic_load_n(&g_menu_closed, __ATOMIC_ACQUIRE) && __atomic_load_n(&g_menu_keyboard, __ATOMIC_RELAXED))
        want = 3;
    // (the menu's: Back put the phone's keyboard away and SDL turned text input off by itself - the menu keeps its view and
    // its text, a physical keyboard types into it again, and its key brings the phone's keyboard back; or the keyboard's
    // own button hid it, and the menu's key asks for it again)
    if (want == 3 && g_window_on == 3) {
        SDL_Window *w = ui_window();
        if (w && !SDL_TextInputActive(w)) {
            __atomic_store_n(&g_menu_closed, 1, __ATOMIC_RELAXED);
            LOG("[textinput] the phone's keyboard put away (Back): the menu's typing stays\n");
            g_window_on = 0;
            want = 2;
        } else if (w && show != g_menu_shown) {  // (SDL can't tell that the keyboard's own button hid it)
            SDL_StopTextInput(w);
            start_menu_keyboard(w);
        }
    }
    g_menu_shown = show;
    // (Back hid the screen keyboard and SDL turned text input off by itself: on again for a physical keyboard, the
    // screen keyboard still hidden; a tap brings it back, textinput_tap)
    if (want == 1 && g_window_on == 1) {
        SDL_Window *w = ui_window();
        if (w && !SDL_TextInputActive(w) && !SDL_ScreenKeyboardShown(w)) {
            SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
            SDL_StartTextInput(w);
            SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");
        }
    }
#endif
    if (want == g_window_on) return;
    SDL_Window *w = ui_window();
    if (!w) return;
    if (g_window_on) SDL_StopTextInput(w);
#ifdef __ANDROID__
    // (there text input shows the screen keyboard: for the game's entries; the port's menu has its own keyboard view,
    // a physical keyboard still types into it, and the phone's keyboard only when that view's key asks for it)
    if (want) SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, want == 2 ? "0" : "1");
    if (want == 3) start_menu_keyboard(w);
    else
#endif
    if (want) SDL_StartTextInput(w);
    g_window_on = want;
}

// (Android) a tap while an entry is open brings the screen keyboard back if the player had hidden it
void textinput_tap(void) {
#ifdef __ANDROID__
    SDL_Window *w = ui_window();
    if (g_window_on == 1 && w && !SDL_ScreenKeyboardShown(w)) {
        SDL_StopTextInput(w);
        SDL_StartTextInput(w);
    }
#endif
}
