// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"
#include "gamelib_lse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "rt.h"
#ifdef _WIN32
#include "gamefolder.h"
#include "settings_locale.h"
#endif

static const GameLibTables *g_tables;

#ifdef _WIN32
// The game code read in ahead, on a thread of its own in background mode (its reads the disk's last): otherwise each
// page of it is read from the library's file the first time that code runs - a monster's new move, a first visit -
// in the middle of a frame, 10 to 50 ms each on a hard disk (MHGU_HITCHLOG=1 saw the update thread wait with the
// process idle). PrefetchVirtualMemory (Windows 8 on) asks for the whole image in large reads and leaves the pages on
// standby, still the file's: memory needed elsewhere takes them back. MHGU_PREFETCH=0: not read ahead (to compare).
typedef struct { void *addr; SIZE_T size; } PrefetchRange;
typedef BOOL(WINAPI *PrefetchFn)(HANDLE, ULONG_PTR, PrefetchRange *, ULONG);
static DWORD WINAPI prefetch_image(void *base) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const IMAGE_DOS_HEADER *dos = base;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)base + dos->e_lfanew);
    PrefetchRange range = {base, nt->OptionalHeader.SizeOfImage};
    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    PrefetchFn prefetch = (PrefetchFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "PrefetchVirtualMemory");
    bool asked = prefetch && prefetch(GetCurrentProcess(), 1, &range, 0);
    if (!asked) {  // (without it: a byte of each page read, which brings it in the same way, in smaller reads)
        volatile const char *p = base;
        unsigned sum = 0;
        for (SIZE_T o = 0; o < range.size; o += 4096) sum += p[o];
        (void)sum;
    }
    QueryPerformanceCounter(&t1), QueryPerformanceFrequency(&hz);
    LOG("[gamelib] the game code read ahead (%s): %.0f MB in %.2f s\n", asked ? "PrefetchVirtualMemory" : "page by page",
        range.size / 1048576.0, (double)(t1.QuadPart - t0.QuadPart) / (double)hz.QuadPart);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return 0;
}
#endif

bool gamelib_load(char *why, size_t why_size) {
    if (g_tables) return true;
    char path[1100];
    const char *env = getenv("MHGU_GAME_LIB");
    if (env && *env) {
        snprintf(path, sizeof path, "%s", env);
    } else {
        char exe[1024];
        if (!plat_exe_path(exe, sizeof exe)) exe[0] = 0;
        char *slash = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
        if (bs > slash) slash = bs;
        if (slash) slash[1] = 0; else exe[0] = 0;
        snprintf(path, sizeof path, "%s%s", exe, GAMELIB_NAME);
#ifdef _WIN32
        // the player's own, made by tools/build_game.py --pc in the game folder (beside exefs and romfs), as on Android
        char own[1100];
        snprintf(own, sizeof own, "%s/%s", g_game_dir, GAMELIB_NAME);
        if (GetFileAttributesA(own) != INVALID_FILE_ATTRIBUTES) snprintf(path, sizeof path, "%s", own);
#endif
    }
#ifdef _WIN32
    // (tools/build_game.py compiles it for x86-64-v2: SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT, CMPXCHG16B; without any of
    // them its first such instruction would fault. Never -march=native: a library made on a newer processor and copied to
    // an older one could use LZCNT, which older ones run as BSR, with wrong results and no fault)
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("x86-64-v2")) {
        snprintf(why, why_size, "%s", settings_text(gamefolder_language(), "This processor lacks instructions the game code "
                 "needs (SSE4.2): it needs a 64-bit processor from 2009 on (Intel Core i3/i5/i7, AMD FX or newer)."));
        return false;
    }
    // (it imports the runtime from a module named MHGU.exe: under another name this program isn't that one)
    if (GetModuleHandleA("MHGU.exe") != GetModuleHandleA(NULL)) {
        snprintf(why, why_size, "the game code imports from MHGU.exe: this program must have that name");
        return false;
    }
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        snprintf(why, why_size, "can't load %s (error %lu): make it with tools/build_game.py --pc <game folder>", path,
                 GetLastError());
        return false;
    }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    if (!gamelib_fit_cpu(path, sizeof path, why, why_size)) return false;
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(why, why_size, "%s", dlerror()); return false; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(why, why_size, "%s: not game code", path); return false; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, GAMELIB_ID)) {
        snprintf(why, why_size, "%s was built for another version of the runtime: make it again with this version's "
                 "tools/build_game.py", path);
        return false;
    }
    g_tables = t;
    LOG("[gamelib] %s (%u + %u functions)\n", path, *t->main_count, *t->sdk_count);
#ifdef _WIN32
    const char *ahead = getenv("MHGU_PREFETCH");
    if (!ahead || *ahead != '0') {
        HANDLE th = CreateThread(NULL, 64 << 10, prefetch_image, (void *)h, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (th) CloseHandle(th);
    }
#endif
    return true;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
