// Player settings: mhgurecomp.ini in the game folder (written with the defaults when it is missing).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DISPLAY_WINDOWED, DISPLAY_FULLSCREEN, DISPLAY_BORDERLESS };
enum { FACE_POSITION, FACE_LABEL };
enum { COMBO_LENIENT, COMBO_STRICT };
enum { TOUCH_AUTO, TOUCH_ON, TOUCH_OFF };  // [Touch] Controls (runtime/touch.cpp)  // [Controls] ComboInputWindow (runtime/hle/combowin.c)
enum { TS_ADAPTIVE, TS_TRADITIONAL };  // [Touch] Layout: the controls change with the action (menus, bows) / the same ones always
enum { MENU_CONFIRM_ORIGINAL, MENU_CONFIRM_SOUTH, MENU_CONFIRM_EAST };
enum { FXAA_ORIGINAL, FXAA_311 };  // [Anti-aliasing] FXAAMode (runtime/nvn/vulkan/exec.c)

// [Keybinds]: the keyboard and mouse controls (runtime/host_input.c), in the file's order. Each takes up to two keys
// or mouse buttons: an SDL scancode, KEY_MOUSE + SDL_BUTTON_*, or the wheel's KEY_WHEEL_UP / KEY_WHEEL_DOWN (0: none).
// BIND_CAM_* tilt the right stick all the way. BIND_ITEM_LEFT / RIGHT turn the item bar directly (runtime/hle/itembar.c),
// not a Switch button. BIND_COMBO1.. press a combined button's buttons ([Touch] Combo1.., TOUCH_COMBOS of them).
enum {
    BIND_MOVE_UP, BIND_MOVE_DOWN, BIND_MOVE_LEFT, BIND_MOVE_RIGHT, BIND_WALK,
    BIND_CAM_UP, BIND_CAM_DOWN, BIND_CAM_LEFT, BIND_CAM_RIGHT,
    BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS, BIND_MINUS,
    BIND_DPAD_UP, BIND_DPAD_DOWN, BIND_DPAD_LEFT, BIND_DPAD_RIGHT, BIND_LSTICK, BIND_RSTICK,
    BIND_ITEM_LEFT, BIND_ITEM_RIGHT,
    BIND_COMBO1, BIND_COMBO2, BIND_COMBO3, BIND_COMBO4, BIND_COMBO5, BIND_COMBO6, BINDS
};
#define BIND_KEYS 2
#define KEY_MOUSE 0x1000
#define KEY_WHEEL_UP (KEY_MOUSE + 0x10)
#define KEY_WHEEL_DOWN (KEY_MOUSE + 0x11)

// [Monster HP]: factors on the HP multiplier of Hub-type quests' large monsters
#define HP_QUESTS_MAX 256
#define HP_SCALE_MIN 0.05f
#define HP_SCALE_MAX 20.0f
// [Multiplayer] RecentServer<n> / RecentKey<n>: how many hub servers and matchmaking keys the menu remembers (the MH4U
// port's lobbies too: RecentLobby<n>)
#define LDN_RECENT 24
// The game's buttons, for showing each on the screen or not ([Touch] Show*: runtime/touch.cpp); and two of the touch
// controls' ways, kept with them: [Touch] StickRun (the stick pushed far out runs, locked; Off: a Run button instead) and
// GuardSlide (a thumb from Guard keeps R as it slides onto the attacks)
enum { TB_ATTACKS, TB_DODGE, TB_GUARD, TB_SHEATHE, TB_PLUS, TB_MINUS, TB_DPAD, TB_L, TB_ZL, TB_ZR, TB_L3, TB_R3, TB_STICK,
       TB_RANGED, TB_FIRE, TB_SECOND, TB_AIM, TB_RSPECIAL, TB_AMMO, TB_ITEM_ARROWS, TB_MENU_ZL, TB_MENU_ZR, TB_MENU_PAD,
       TB_STICK_RUN, TB_GUARD_SLIDE, TB_COUNT };
// (the item bar's arrows on by default, as in the MH4U port)
#define TOUCH_SHOW_DEFAULT {1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0}
#define TOUCH_SHOW_TRAD_DEFAULT {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}  // (Traditional: every button on)
#define TOUCH_COMBOS 6
// a controller's buttons a combined button can be on (the less used ones)
enum { PADB_NONE, PADB_L3, PADB_R3, PADB_MINUS, PADB_GUIDE, PADB_MISC, PADB_PADDLE1, PADB_PADDLE2, PADB_PADDLE3, PADB_PADDLE4,
       PADB_TOUCHPAD, PADB_COUNT };
typedef struct {
    float scale[4];  // by the number of hunters (index: hunters - 1)
    float village;   // Village quests posted in the Hub / Pub: this first, then scale ([Village Quests] HubHP)
    int quests;
    struct { uint32_t quest; float scale[4]; } quest[HP_QUESTS_MAX];  // per quest, in place of scale
} HpScale;

typedef struct {
    int display_mode;         // DISPLAY_*
    int width, height;        // screen / window size (Fullscreen: the monitor mode)
    int render_w, render_h;   // the game's 3D resolution (16:9)
    int fps;                  // frame rate cap: 30..240 (a desktop: up to FPS_UNLIMITED)
    int vsync;                // 1: present without tearing (mailbox / FIFO, at most the refresh rate), 0: immediate
    int msaa;                 // samples of the 3D scene: 1 (off), 2, 4, 8
    int fxaa;                 // the game's own FXAA pass: 1 on, 0 off
    int face_buttons;         // FACE_*: controller face buttons by position (the Switch layout) or by their labels
    int ldn_port;             // local play: the port a hosted hub listens on (TCP; UDP for LAN search)
    char ldn_join[256];       // local play: hosts to search besides the LAN (host[:port], comma separated)
    int sync_small_monsters;  // multiplayer: small monsters synced like large ones (runtime/net/emsync.c)
    int bonus_content;        // free downloads copied from a donor save into saves without any (runtime/services/bonus.c)
    int village_in_hub;       // L / R at the Hub's / Pub's quest counter switch to Village quests (runtime/hle/villagehub.c)
    HpScale hp;               // large monster HP in Hub-type quests (runtime/hle/hpscale.c)
    float mouse_sensitivity;  // mouse look: times the hunt camera's angle per count (runtime/hle/mousecam.c), and
                              // the speed for full right stick tilt elsewhere is divided by it (runtime/host_input.c)
    float aspect_ratio;       // the picture's shape (width / height), 0: the window's; never below 16:10 (config.c)
    float fov;                // 3D camera field of view factor (1: the game's)
    int ui_anchor;            // where the HUD goes on a picture wider than 16:9: UI_*
    int background_input;     // the controller keeps working while the window is in the background (runtime/ui.c)
    int volume;               // host master volume, 0..100; independent of the game's music / SFX settings
    int button_icons;         // 0: Xbox, 1: Switch, 2: PlayStation (visual guides only)
    int language;             // 0: English, 1: Spanish (host menu only)
    int menu_scale;           // host UI text/layout scale, 50..200 percent
    int menu_confirm;         // independent confirmation policy for the host settings menu
    int confirm_exit;         // ask before closing the application
    int fsr;                  // [Upscaler] FSR 3.1: 1 on (runtime/nvn/vulkan/fsr.c), 0 off
    int fsr_quality;          // FSR_*: the render scale
    float fsr_sharpness;      // FSR's RCAS sharpening, 0..1
    int fsr_objmotion;        // FSR: motion vectors of moving objects (characters, monsters): 1 on, 0 camera only
    float fsr_reactive;       // FSR: reactive mask strength on the game's moving-object mask, 0 (off) .. 1
    int fsr_animsurf;         // FSR: surfaces animated through their texture (banners, water): 1 detected, 0 off
    int fsr_upscaler;         // [Upscaler] Upscaler: FSR_UP_3 (FSR 3.1), FSR_UP_4 (FSR 4, runtime/nvn/vulkan/fsr4.c) or
                              // FSR_UP_XESS (XeSS-SR, Windows: runtime/nvn/vulkan/xess.c)
    int updates;              // [Application] Updates: UPDATES_*: Android's start looks for a newer release once a day (Notices.java)
    int shader_prewarm;       // [Performance] ShaderPrewarm: translate the game's shaders at start-up (nvn/vulkan/exec.c)
    float draw_distance;      // [Graphics] DrawDistance: town NPCs' fade distance factor, 0 no limit (config.c npc_fade_dist2)
    int mute_in_background;   // [Audio] MuteInBackground: no sound while the window is not focused (config_audio_gain)
    int combo_window;         // [Controls] ComboInputWindow: COMBO_*, how long a press waits for a second button
    char ldn_server[256];     // [Multiplayer] Server: hub server for internet hubs (host[:port]); "" the default, "Off" none
    char ldn_key[64];         // [Multiplayer] Key: matchmaking key, hubs seen only by players with the same; "" public
    char chat_key[32];        // [Controls] ChatKey: the key that opens a line of chat (runtime/hle/chat.c), by SDL's
                              // name for it ("/", "Y"...); "" none
    int fxaa_mode;            // [Anti-aliasing] FXAAMode: FXAA_*, which FXAA draws while FXAA is on
    int smooth_gradients;     // [Graphics] SmoothGradients: the game's RGBA8 render targets as RGBA16 (nvn/vulkan/textures.c)
    int binds[BINDS][BIND_KEYS];  // [Keybinds]: BIND_* -> its keys (an SDL scancode or KEY_MOUSE + button; 0 none)
    int menu_theme;           // [Application] Theme: THEME_*, the settings menu's colours (runtime/settings_panel.cpp)
    int discord;              // [Discord] RichPresence: what you play, on your Discord profile (runtime/net/discord.c)
    int discord_hunter;       // [Discord] ShowHunter: your hunter's name and HR there too (runtime/hle/presence.c)
    int fill_screen;          // [Display] FillScreen: movies and menu backdrops cover a wider screen (nvn/vulkan/exec.c)
    int touch_controls;       // [Touch] Controls: TOUCH_*, the controls on the screen (runtime/touch.cpp)
    int touch_hud;            // [Touch] MoveHUD: the HUD moves out of the thumbs' way while they show (nvn/vulkan/exec.c)
    float touch_camera;       // [Touch] CameraSpeed: the camera's turn for a drag (1: about 90 degrees a screen height)
    float touch_opacity;      // [Touch] Opacity: of the controls, 0.2 .. 1
    int render_scale;         // [Display] RenderScale: RenderResolution Auto's 3D size, percent of the screen's (25..100)
    int touch_vibration;      // [Touch] Vibration: a short tick when a touch control is pressed (runtime/haptics.c)
    int phone_rumble;         // [Controls] PhoneRumble: the game's rumble on the phone's vibrator too (runtime/haptics.c)
    int show_stats;           // [Performance] ShowStats: the frame rate and the phone's temperature on screen (perfstats);
                              // a desktop: 0 off, 1 (On, the menu's Basic) with the GPU's temperature, 2 (Full) also
                              // the GPU's and the CPU's lines
    int pause_background;     // [Application] PauseInBackground: the game stops while the app is in the background
    int low_end;              // [Performance] LowEnd: 0 off, 1 a weak GPU's frame, 2 Toaster (LOW_END_TOASTER: all of it and more)
    int lock_on;              // [Controls] LockOn: the camera stays on the monster, and with it a bow's or gun's shots (touch.cpp)
    // (every field's default is in config.c's CONFIG_DEFAULT_VALUES, by position: a new field needs one there)
    int shadows;              // [Performance] Shadows: 0 none drawn (nvn/vulkan/exec.c shadow_skipped)
    int adreno_fix;           // [Performance] AdrenoFix: 0 Auto, 1 Off (nvn/vulkan/device.c msaa_floor)
    int bloom;                // [Performance] Bloom: 0 the bloom and depth of field chain is left out (nvn/vulkan/exec.c lowend_skipped)
    int reverb;               // [Audio] Reverb: 0 the game's reverb effects let the sound through dry (services/audio.c)
    int touch_show[TB_COUNT]; // [Touch] Show*: which of the game's buttons are on the screen in a hunt (runtime/touch.cpp)
    int thanks_shown;         // [Application] ThanksShown: the thank-you shown once, after the credits of the first start,
                              // was closed (settings_panel.cpp draw_thanks); from then on only Credits & Licenses shows it
    char touch_combo[TOUCH_COMBOS][32];  // [Touch] Combo1..: the game's buttons one screen button presses together
                                         // ("A+X"; config_combo_mask); "" none
    int touch_combo_pad[TOUCH_COMBOS];   // [Touch] Combo1Pad..: PADB_*, a controller's button that does the same
    char gpu[256];            // [Display] GPU: the Vulkan device's name (nvn/vulkan/device.c pick_device); "Auto" or "" the
                              // best one (MHGU_GPU=<index> overrides)
    int menu_hint_seen;       // [Application] MenuHintSeen (a desktop): the settings menu was opened once, so the notice
                              // at the start that says how (overlay.cpp menu_hint) no longer shows
    int mouse_menus;          // [Controls] MouseMenus (a desktop): in the game's menus the mouse points and clicks
                              // (runtime/hle/mousemenu.c, host_input.c); off, it turns the camera there as elsewhere
    int grass;                // [Performance] Grass: 0 the game's grass fields (its uGrass2 units) are not drawn
                              // (runtime/hle/grass.c; the Marshland's area 8 slowed weak phones down: #85, #121, #159)
    int lock_buttons;         // [Touch] LockOnButtons: L and the target button were shown once for the lock-on (its
                              // padlock and long press are L's, the target button changes its monster: touch.cpp); a
                              // file without it (0.1.8 and before, both off by default) gets both shown (config.c)
    int online_notice;        // [Multiplayer] OnlineNoticeShown: the notice about playing online (which server, and that it
                              // sees the player's IP address) was closed; until then nothing reaches a hub server
                              // (runtime/net/ldn.c, settings_panel.cpp's card)
    int pause_menu;           // [Application] PauseInMenu (Android): the game waits while the settings menu is open and the
                              // player plays alone (runtime/overlay.cpp; #94)
    char ldn_recent_server[LDN_RECENT][256];  // [Multiplayer] RecentServer1..: the hub servers typed and saved, the last
                                              // first, none of the short list's (the menu's list: settings_panel.cpp)
    char ldn_recent_key[LDN_RECENT][64];      // [Multiplayer] RecentKey1..: the matchmaking keys typed and saved, the same
    int gyro_aim;             // [Controls] GyroAim: aiming a bow or a bowgun, turning the phone (or a controller with a gyro)
                              // turns the aim too, from where it was as the aim began (runtime/host_input.c gyro_update)
    float gyro_sensitivity;   // [Controls] GyroSensitivity: the aim's turn for a turn of the phone (GYRO_SENS_MIN..MAX)
    // From here on the defaults are set by name in CONFIG_DEFAULT_VALUES (config.c), after the positional ones.
    int touch_scheme;         // [Touch] Layout: TS_*, how the controls on the screen are laid out (Adaptive by default)
    int touch_show_trad[TB_COUNT];  // [Touch] TradShow*: the same for the Traditional layout (TB_ATTACKS to TB_STICK; all on by default)
    int touch_auto_run;       // [Touch] AutoRun: Traditional layout only: the thumb past the stick's rim runs (R held); off (default): only the R button runs
    int share_urgent;         // [Multiplayer] ShareUrgentCredit: an Urgent quest posted by someone else counts for you too (runtime/hle/urgentcredit.c)
    int hub_full_list;        // [Multiplayer] FullHubList: each Join Hub search asks each hub server several times at once, to list all their hubs
    // Crossplay (runtime/net/ldn.c, crates/mhgu-crossplay)
    int crossplay;            // [Multiplayer] Crossplay: XP_* (Ryujinx LDN server, Yuzu rooms, Mixed, Citra rooms, All Rooms)
    char ryu_servers[256];    // [Multiplayer] RyuServers: RyuLdn servers (host[:port], comma separated)
    int ryu_host;             // [Multiplayer] RyuHost: run a RyuLdn server in this process
    int ryu_port;             // [Multiplayer] RyuPort: that server's TCP port
    char ryu_password[64];    // [Multiplayer] RyuPassword: the lobby's passphrase (empty: public)
    char room_server[256];    // [Multiplayer] RoomServer: Yuzu-family room server (host[:port]); empty: none
    int room_host;            // [Multiplayer] RoomHost: run a room server in this process
    int room_port;            // [Multiplayer] RoomPort: that room server's port
    char room_name[64];       // [Multiplayer] RoomName: the nickname in a room, and the name of a room this device hosts
    char room_password[64];   // [Multiplayer] RoomPassword: the room's password (empty: open)
    char web_api_url[256];    // [Multiplayer] WebApiUrl: the Yuzu (Eden) Web API that lists public rooms (the Yuzu Room Browser)
    char last_room[96];       // [Multiplayer] LastRoom: "address:port" of the room joined last (pinned first in the Room Browser)
    char last_room_name[64];  // [Multiplayer] LastRoomName: and its name (empty: any name)
    char citra_room_server[256];    // [Multiplayer] CitraRoomServer: Citra-family (Azahar) room, host[:port]; empty: none
    char citra_room_password[64];   // [Multiplayer] CitraRoomPassword: that room's password (empty: open)
    char citra_web_api_url[256];    // [Multiplayer] CitraWebApiUrl: the Citra Web API that lists public rooms (Citra Room Browser)
    char citra_web_api_url_2[256];  // [Multiplayer] CitraWebApiUrl2: a second one, its rooms are added to the list (empty: off)
    char citra_token[256];          // [Multiplayer] CitraToken: the token sent with a join request to a Citra room (empty: anonymous)
    char last_citra_room[96];       // [Multiplayer] LastCitraRoom: "address:port" of the Citra room joined last
    char last_citra_room_name[64];  // [Multiplayer] LastCitraRoomName: and its name (empty: any name)
} Config;
enum { XP_OFF, XP_RYU, XP_ROOM, XP_MIXED, XP_CITRA, XP_ALL };  // (XP_ROOM: Yuzu-family rooms; XP_CITRA: Citra-family / Azahar
// rooms, MH XX; XP_ALL: a hosted hub is on the LAN / hub server, in the Yuzu room and in the Citra room at once)
#define WEB_API_DEFAULT_URL "api.ynet-fun.xyz"  // the Eden build's default (its web_api_url setting)
#define CITRA_WEB_API_DEFAULT_URL "http://88.198.47.46:5000"    // AzaharPlus's defaults (its web_api_url and web_api_url_2)
#define CITRA_WEB_API_DEFAULT_URL_2 "http://3ds.mgn.pub:3000"
// The remembered lists (Config's ldn_recent_*: n entries of `width` bytes, the last first, no empty one between): add puts
// a value at the front once (nocase: compared as host names are; an empty one is nothing), the last one dropped when it
// is full; remove takes it out, the rest moving up (false: it wasn't there)
void config_recent_add(char *list, int n, size_t width, const char *value, bool nocase);
bool config_recent_remove(char *list, int n, size_t width, const char *value, bool nocase);
uint32_t config_combo_mask(const char *text);  // "A+X" -> the BTN_* bits, 0 if there is nothing or anything else in it
void config_combo_text(uint32_t mask, char *out, size_t size);  // and back
// The hub server used when ini Server is empty (runtime/net/ldn.c, server/): a community server that speaks this port's
// protocol (PROTO_VERSION 2, LOBBY_PROTO 1), where players of 0.1.6 and later already meet. The project runs none.
// LDN_SERVERS: the menu's short list, the default first ("Custom..." types any other, Off is none).
// (several: the hubs of each are listed, a hub made is listed on each; the menu's Hub servers view edits the list)
#define LDN_DEFAULT_SERVER "hunterhubid-gu-hub.duckdns.org morerokk-mhgurecomp-ldnserver.duckdns.org"
#define LDN_SERVERS {"hunterhubid-gu-hub.duckdns.org", "morerokk-mhgurecomp-ldnserver.duckdns.org"}
#define LOW_END_SCALE 40  // what the menu's Low-end mode sets: RenderScale, FPS
#define LOW_END_FPS 30
// [Performance] FPS: the most a phone takes; a desktop also takes more, up to FPS_UNLIMITED ("Unlimited" in the file
// and the menu: as many frames as the PC draws, the engine's limiter at 1 ms a frame, which no frame here is faster than)
#define FPS_PHONE_MAX 240
#define FPS_UNLIMITED 1000
#ifdef __ANDROID__
#define FPS_MAX FPS_PHONE_MAX
#else
#define FPS_MAX FPS_UNLIMITED
#endif
#define LOW_END_TOASTER 2   // LowEnd = Toaster: everything for speed and the battery (config.c, textures.c, the menu)
#define TOASTER_SCALE 25    // what the menu's Toaster sets: RenderScale (and LOW_END_FPS)
#define TOASTER_HUD_LINES 540  // Toaster: the HUD's picture at most this tall (config.c, fsr.c's output)
// (a desktop) how tall the menu's Low-end mode and Toaster make the 3D picture, whatever the screen: config_lowend_scale
#define LOW_END_LINES 720
#define TOASTER_LINES 540
enum { UPDATES_OFF, UPDATES_ON };  // (an older file's Ask and Auto: On; Never: Off)
enum { THEME_GOLD, THEME_BLUE, THEME_HUNTER };  // [Application] Theme
enum { UI_CENTER = 1, UI_21_9, UI_32_9, UI_EDGES };  // the middle 16:9, 21:9, 32:9, the picture's sides
enum { FSR_UP_3, FSR_UP_4, FSR_UP_XESS };  // [Upscaler] Upscaler (a start-up setting: the device is made for it)
enum { FSR_NATIVE, FSR_QUALITY, FSR_BALANCED, FSR_PERFORMANCE, FSR_ULTRA_PERFORMANCE };  // output / render: 1, 1.5, 1.7, 2, 3

extern Config g_config;
extern uint32_t g_render_size[2];  // read by the game code (tools/gen.py PATCHES: its docked render size)
extern uint32_t g_gpu_pool_size;   // the game's GPU pool for render targets and textures, bigger with the render size (gen.py PATCHES)
extern float g_render_aspect;      // the picture's shape, 16:10 or wider: the 3D cameras' aspect (tools/gen.py PATCHES)
extern float g_fov_scale;          // boot diagnostic value; camera_fovy reads config_live_fov()
extern uint32_t g_ui_size[2];      // the game's 2D canvas size (tools/gen.py PATCHES: 0xdbcd48, 0xdbcd54)
extern float g_ui_anchor_aspect;   // the HUD's width / height, 16:9 .. g_render_aspect (nvn/vulkan/exec.c)
extern uint32_t g_fsr_output[2];   // FSR on: the size it upscales to (what g_render_size would be without it), else 0
extern float g_fsr_lod_bias;       // FSR on: texture LOD bias for the lower render size, log2(render / output); else 0
extern const char *g_fsr4_fallback;  // Upscaler = FSR4 but FSR 3.1 runs: why, for the settings panel (nvn/vulkan/fsr4.c); else NULL
extern const char *g_xess_fallback;  // Upscaler = XeSS but FSR 3.1 runs: why, for the settings panel (nvn/vulkan/xess.c); else NULL
float camera_fovy(float fovy, float aspect);  // a 3D camera's vertical field of view for its aspect (gen.py PATCHES)
// (a desktop) the window's new size in pixels: with AspectRatio = Auto the picture takes its shape at once (config.c)
void config_window_shape(int w, int h);
#ifndef __ANDROID__
// (a desktop) the RenderScale the menu's Low-end mode (1) or Toaster (LOW_END_TOASTER) sets for these settings: the 3D
// picture LOW_END_LINES or TOASTER_LINES tall on the screen it is fitted to (config.c)
int config_lowend_scale(const Config *c, int mode);
#endif

void config_load(void);  // before the window and the guest start
void config_path(char *buf, size_t size);  // the settings file (MHGU_CONFIG, else mhgurecomp.ini)

// The settings of the file, one row each (config.c g_settings): what reads, writes and checks them, and what the
// settings menu (settings_panel.cpp) marks as needing a restart. Quest<number> entries ([Monster HP]) are apart: hp.quest.
enum { SET_BOOL, SET_INT, SET_FLOAT, SET_FACTOR, SET_CHOICE, SET_SIZE, SET_ASPECT, SET_TEXT, SET_KEYS };
typedef struct {
    const char *section, *key;  // [section] and key; "FPS|FrameRate": other spellings of the key it reads too
    int kind;                   // SET_*
    size_t field, field2;       // offsetof(Config, ...): the value (SET_SIZE: width; field2 its height)
    float min, max;             // SET_INT, SET_FLOAT, SET_FACTOR: the range
    // SET_CHOICE: the values as the file writes them, ',' between them, each with other spellings it reads after '|'
    // ('*' at the end: anything starting so); SET_SIZE: the spelling of 0x0 ("Desktop|Auto")
    const char *choices;
    const int *values;          // SET_CHOICE: each choice's value (NULL: 0, 1, 2...)
    bool restart;               // takes effect at the next start, not at once
    const char *error;          // config_validate's message for a bad value (English: settings_locale.h translates)
} ConfigSetting;
extern const ConfigSetting g_settings[];
extern const int g_nsettings;
const ConfigSetting *config_setting(const char *key);  // by key or another spelling of it; NULL: none
bool config_parse(const ConfigSetting *s, const char *value, Config *c);  // a value as the file has it into *c
void config_format(const ConfigSetting *s, const Config *c, char *buf, size_t size);  // as the file writes it
bool config_valid(const ConfigSetting *s, const Config *c);
bool config_equal(const ConfigSetting *s, const Config *a, const Config *b);
bool config_restart_key(const char *key);  // the setting takes effect at the next start
bool config_range(const char *key, float *min, float *max);  // a number setting's range
bool config_help(const char *key, char *buf, size_t size);  // what the default ini's comments say of it (the menu's help)
// [Keybinds]: a key's or mouse button's name as the file writes it and the menu shows it (SDL's, "Mouse Left"...;
// "" for one that has none), and the code a name is for (0: none of them)
const char *config_key_name(int code);
int config_key_code(const char *name);
const ConfigSetting *config_bind_setting(int bind);  // the [Keybinds] row of BIND_*

// File values, before environment overrides and automatic resolution are resolved.
void config_store_loaded(const Config *config);
Config config_editable(void);
Config config_defaults(void);
bool config_save(const Config *config, char *error, size_t error_size);
bool config_validate(const Config *config, char *error, size_t error_size);
// Immutable boot settings stay in g_config. These getters are atomic live values.
void config_runtime_init(const Config *effective); // After environment/Auto resolution at startup.
float config_live_fps(void);
float config_live_fov(void);
float config_live_mouse_sensitivity(void);
float config_live_draw_distance(void);
int config_live_fxaa(void);
int config_live_msaa(void);
int config_live_vsync(void);
int config_live_face_buttons(void);
int config_live_menu_confirm(void);
int config_live_background_input(void);
int config_live_combo_window(void);
int config_live_chat_key(void);  // [Controls] ChatKey as an SDL scancode (0: none)
int config_live_bind(int bind, int slot);  // [Keybinds]: one of BIND_*'s keys (Config.binds)
int config_live_fxaa_mode(void);
int config_live_smooth_gradients(void);
int config_live_discord(void);         // [Discord] RichPresence
int config_live_discord_hunter(void);  // [Discord] ShowHunter
int config_live_touch_controls(void);  // [Touch] (runtime/touch.cpp)
int config_live_touch_hud(void);
int config_live_lock_on(void);
int config_live_gyro_aim(void);           // [Controls] GyroAim
float config_live_gyro_sensitivity(void); // [Controls] GyroSensitivity
int config_live_mouse_menus(void);  // [Controls] MouseMenus (a desktop's; 0 on a phone)
int config_live_touch_scheme(void);    // [Touch] Layout: TS_*
int config_live_touch_auto_run(void);  // [Touch] AutoRun
int config_live_touch_show_trad(int button);   // TB_*, in the Traditional layout
int config_live_touch_show(int button);        // TB_*
uint32_t config_live_touch_combo(int index);   // its buttons (config_combo_mask)
int config_live_touch_combo_pad(int index);    // PADB_*
float config_live_touch_camera(void);
float config_live_touch_opacity(void);
int config_live_touch_vibration(void);  // [Touch] Vibration
int config_live_phone_rumble(void);     // [Controls] PhoneRumble
int config_live_show_stats(void);       // [Performance] ShowStats
int config_live_generation(void);      // counts the live settings' publishes (runtime/touch.cpp: a frame's key)
int config_live_pause_background(void); // [Application] PauseInBackground
int config_live_pause_menu(void);       // [Application] PauseInMenu
int config_live_shadows(void);          // [Performance] Shadows
int config_live_bloom(void);            // [Performance] Bloom
int config_live_grass(void);            // [Performance] Grass
int config_live_reverb(void);           // [Audio] Reverb
void config_live_test(const Config *c);  // (config.c: a test run's overrides of Shadows, Bloom, Grass, FPS; not saved)
int config_live_share_urgent(void);   // [Multiplayer] ShareUrgentCredit
int config_live_village_in_hub(void); // [Village Quests] VillageQuestsInHub (hle/villagehub.c)
int config_live_hub_full_list(void);   // [Multiplayer] FullHubList
void config_live_crossplay(Config *out);  // coherent snapshot of the saved Crossplay / Ryu* / Room* settings
void config_live_ldn(char *server, size_t server_size, char *key, size_t key_size);  // [Multiplayer] Server, Key
int config_live_online_notice(void);  // [Multiplayer] OnlineNoticeShown
bool config_restart_required(const Config *draft); // Raw cold fields vs this process's startup INI values.
float config_audio_gain(void);  // atomic, safe on audio threads; 0 while muted in the background
void config_window_focus(bool focused);  // the window got / lost the focus (runtime/host_input.c)
void config_refresh_system_language(void);  // the phone's language may have changed (SDL_EVENT_LOCALE_CHANGED)
int config_language(void);     // last saved UI language, atomic across host threads
bool config_confirm_exit(void); // atomic, safe on the host main thread

#ifdef __cplusplus
}
#endif
