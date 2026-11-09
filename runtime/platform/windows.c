// Windows host layer (runtime/platform.h).
#include <windows.h>
#include <shlobj.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <timeapi.h>

#include "../hitchlog.h"
#include "../logwriter.h"
#include "../platform.h"

static bool oom_error(DWORD e) { return e == ERROR_COMMITMENT_LIMIT || e == ERROR_NOT_ENOUGH_MEMORY || e == ERROR_OUTOFMEMORY; }

// ---- guest address space
// The 4 GB window is reserved up front; pages are committed on their first touch by the exception handler below
// (64 KB at a time), so only memory the game uses is backed.
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull
// Chunks Windows would not back for want of memory (ERROR_COMMITMENT_LIMIT: RAM plus the page file used up). The access
// that touched one ends the game right after, on its own thread, and unhandled_filter tells that crash from any other
// by its address (the last few kept: threads fault together when memory runs out). plat_commit's failures are not
// here: they are survivable, and a later crash elsewhere is not memory's.
#define REFUSED 4
static volatile uint64_t g_refused[REFUSED];
static volatile LONG g_refused_n;
static volatile DWORD g_refused_error;

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    if (VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE)) return true;
    DWORD e = GetLastError();
    if (oom_error(e)) g_refused_error = e, g_refused[(ULONG)InterlockedIncrement(&g_refused_n) % REFUSED] = base;
    return false;
}

void (*plat_touch_hook)(uint64_t guest_addr, int write);

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    if (lazy_commit(fa)) {
        if (HITCH_ON) hitch_add(HC_MEM_COMMIT, plat_now_ns() - t0, COMMIT_CHUNK);  // ((debug) MHGU_HITCHLOG)
        if (plat_touch_hook) plat_touch_hook(fa - g_lo, er->ExceptionInformation[0] == 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// The window keeps write tracking (MEM_WRITE_WATCH): the renderer re-uploads only textures / buffers whose pages the
// CPU wrote (plat_written_pages). Refused: no tracking, the renderer checks contents instead.
static bool g_write_watch;

bool plat_reserve_guest(uint64_t base, uint64_t size) {
    // (MHGU_NO_WRITE_WATCH=1: none, as on Linux - the renderer's content checks, for tests)
    void *p = getenv("MHGU_NO_WRITE_WATCH") ? NULL
            : VirtualAlloc((void *)(uintptr_t)base, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p == (void *)(uintptr_t)base) g_write_watch = true;
    else p = VirtualAlloc((void *)(uintptr_t)base, size, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)base) return false;
    g_lo = base;
    g_hi = base + size;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        rt_logf("[plat] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr, (unsigned long long)size, GetLastError());
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi || max <= 0) return -1;
    ULONG_PTR n = (ULONG_PTR)max;
    DWORD gran = 4096;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, (SIZE_T)size, (PVOID *)pages, &n, &gran) != 0) return -1;
    *page = gran;
    if (n >= (ULONG_PTR)max) ResetWriteWatch((void *)(uintptr_t)addr, (SIZE_T)size);  // (more than fit: caller takes all)
    return (int)n;
}

bool plat_committed(uint64_t addr, uint64_t *end) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof mi)) { *end = addr + 0x1000; return false; }
    *end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
    return mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time
static LARGE_INTEGER qpf;

uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

static HANDLE thread_timer(void) {  // (the calling thread's; high resolution where the system has them)
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    return timer;
}

void plat_sleep_until_ns(uint64_t t) {
    HANDLE timer = thread_timer();
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

void *plat_event_new(void) { return CreateEventW(NULL, FALSE, FALSE, NULL); }
void plat_event_set(void *e) { SetEvent((HANDLE)e); }

bool plat_event_wait_until(void *e, uint64_t t) {
    if (t == UINT64_MAX) return WaitForSingleObject((HANDLE)e, INFINITE) == WAIT_OBJECT_0;
    HANDLE timer = thread_timer(), h[2] = {(HANDLE)e, timer};
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return WaitForSingleObject((HANDLE)e, 0) == WAIT_OBJECT_0;
        // (setting the timer clears a signal left from a wait the event won)
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((t - now + 99) / 100)};
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))  // (whole ms, never past t)
            return WaitForSingleObject((HANDLE)e, (DWORD)((t - now) / 1000000)) == WAIT_OBJECT_0;
        DWORD r = WaitForMultipleObjects(2, h, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) return true;
        if (r != WAIT_OBJECT_0 + 1) return false;
        // (the timer: t is reached, unless it rang a hair before plat_now_ns's clock says so)
    }
}

// ---- process
// MHGU.exe is a GUI program, so a double-click opens no console window. The log is stderr: one redirected to a file or
// a pipe stays; else it goes to the terminal that started the game (its console, attached); with neither,
// plat_standalone sends it to log.txt. Its lines are written there by a thread of its own (runtime/logwriter.c).
static bool g_log_free;  // (no file, pipe or terminal took stderr)

static bool std_redirected(DWORD which) {
    DWORD type = GetFileType(GetStdHandle(which));
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

// The stream on path, with its descriptor and the system's standard handle: a GUI program starts with neither, and
// SDL's own messages go to the handle.
static bool std_reopen(FILE *f, int fd, DWORD which, const char *path) {
    if (!freopen(path, "w", f)) return false;
    if (_fileno(f) != fd) _dup2(_fileno(f), fd);
    SetStdHandle(which, (HANDLE)_get_osfhandle(_fileno(f)));
    return true;
}

static void box_start(void);
static void qos_high(void);
static int g_prio_on = 1;  // (MHGU_PRIO=0: no thread's priority changed, plat_thread_priority)
static int g_lend_on = 1;  // (MHGU_PRIO_LEND=0: none lent to a mutex's owner, plat_prio_self)

void plat_init(void) {
    _fmode = _O_BINARY;
    if (!std_redirected(STD_ERROR_HANDLE)) {
        if (AttachConsole(ATTACH_PARENT_PROCESS) && std_reopen(stderr, 2, STD_ERROR_HANDLE, "CONOUT$")) {
            if (!std_redirected(STD_OUTPUT_HANDLE)) std_reopen(stdout, 1, STD_OUTPUT_HANDLE, "CONOUT$");
        } else {
            g_log_free = true;
        }
    }
    if (_fileno(stdout) >= 0) _setmode(_fileno(stdout), _O_BINARY);  // (-2: a GUI program started without one)
    if (_fileno(stderr) >= 0) _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);
    const char *e = getenv("MHGU_PRIO");
    g_prio_on = !(e && *e == '0');
    e = getenv("MHGU_PRIO_LEND");
    g_lend_on = !(e && *e == '0');
    qos_high();
    box_start();
}

unsigned long plat_process_id(void) { return GetCurrentProcessId(); }
bool plat_wait_process(unsigned long pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    DWORD result = WaitForSingleObject(process, 30000);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
}

bool plat_exe_path(char *buf, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    return n > 0 && n < size;
}

// Started on its own (no game folder given): run in the executable's folder (MHGU_HOME: in that folder instead, created
// if missing - the settings, caches, mods and log there, the build folder untouched). With no terminal or redirect for
// the log (a double-click in Explorer, a shortcut) it goes to log.txt there, the last one kept as log_prev.txt (so the
// log of a session that crashed survives the next start), and fatal errors show a message box.
static bool g_own_log;

void plat_standalone(void) {
    char exe[MAX_PATH];
    const char *home = getenv("MHGU_HOME");
    if (home && *home) {
        CreateDirectoryA(home, NULL);
        SetCurrentDirectoryA(home);
    } else if (plat_exe_path(exe, sizeof exe)) {
        char *s = strrchr(exe, '\\');
        if (s) { *s = 0; SetCurrentDirectoryA(exe); }
    }
    if (g_log_free) {
        MoveFileExA("log.txt", "log_prev.txt", MOVEFILE_REPLACE_EXISTING);
        if (std_reopen(stderr, 2, STD_ERROR_HANDLE, "log.txt")) { setvbuf(stderr, NULL, _IONBF, 0); g_own_log = true; }
    }
}

// %APPDATA%\MHGURecomp (Roaming). The runtime opens files with the narrow (ANSI code page) functions, so a path the
// code page can't spell (a user name in another script) is handed out in its 8.3 short form instead.
bool plat_user_dir(char *buf, size_t size) {
    PWSTR base = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, KF_FLAG_CREATE, NULL, &base) != S_OK) {
        CoTaskMemFree(base);
        return false;
    }
    wchar_t dir[MAX_PATH];
    int n = _snwprintf(dir, MAX_PATH, L"%ls\\MHGURecomp", base);
    CoTaskMemFree(base);
    if (n < 0 || n >= MAX_PATH) return false;
    CreateDirectoryW(dir, NULL);
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return false;
    BOOL lossy = FALSE;
    int len = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, dir, -1, buf, (int)size, NULL, &lossy);
    if (len > 0 && !lossy) return true;
    wchar_t shrt[MAX_PATH];
    DWORD m = GetShortPathNameW(dir, shrt, MAX_PATH);
    if (!m || m >= MAX_PATH) return false;
    len = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, shrt, -1, buf, (int)size, NULL, &lossy);
    return len > 0 && !lossy;
}

static int count_files(const char *dir, int depth) {
    char pat[MAX_PATH * 2];
    if (depth > 32 || snprintf(pat, sizeof pat, "%s\\*", dir) >= (int)sizeof pat) return 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileExA(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { n++; continue; }
        char sub[MAX_PATH * 2];
        if (snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName) < (int)sizeof sub) n += count_files(sub, depth + 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
int plat_count_files(const char *dir) { return count_files(dir, 0); }

bool plat_path_from_utf8(const char *utf8, char *out, size_t size) {
    wchar_t w[MAX_PATH * 2], shrt[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, MAX_PATH * 2)) return false;
    BOOL lossy = FALSE;
    int len = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, (int)size, NULL, &lossy);
    if (len > 0 && !lossy) return true;
    DWORD m = GetShortPathNameW(w, shrt, MAX_PATH * 2);
    if (!m || m >= MAX_PATH * 2) return false;
    len = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, shrt, -1, out, (int)size, NULL, &lossy);
    return len > 0 && !lossy;
}

bool plat_path_to_utf8(const char *native, char *out, size_t size) {
    wchar_t w[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_ACP, 0, native, -1, w, MAX_PATH * 2)) return false;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, NULL, NULL) > 0;
}

// ---- message boxes
// A thread that is not the main one (the window's) has its box shown by a thread of its own, started by plat_init with
// its stack committed. The out-of-memory notices come on a guest thread, whose stack is only reserved
// (plat_thread_create): right when Windows has no memory left, MessageBoxW there can find no page to grow into.
static HANDLE g_box_go, g_box_done;  // (auto-reset events)
static const char *volatile g_box_msg;
static DWORD g_box_thread, g_main_thread;
static SRWLOCK g_box_lock = SRWLOCK_INIT;

static void box_show(const char *msg) {
    // (UTF-8 text, as the rest of the runtime's: the wide box shows accents right)
    wchar_t w[2048];
    UINT type = MB_OK | MB_ICONERROR | MB_SETFOREGROUND;
    if (MultiByteToWideChar(CP_UTF8, 0, msg, -1, w, 2048)) MessageBoxW(NULL, w, L"MHGU", type);
    else MessageBoxA(NULL, msg, "MHGU", type);
}

static unsigned __stdcall box_thread(void *arg) {
    (void)arg;
    IsGUIThread(TRUE);  // (made a window thread now, with its message queue: not when memory has run out)
    MSG m;
    PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE);
    for (;;) {
        WaitForSingleObject(g_box_go, INFINITE);
        box_show(g_box_msg);
        SetEvent(g_box_done);
    }
    return 0;
}

static void box_start(void) {
    g_main_thread = GetCurrentThreadId();
    g_box_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_box_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    unsigned id = 0;
    // (without STACK_SIZE_PARAM_IS_A_RESERVATION the size is what is committed at once: 256 KB)
    uintptr_t h = g_box_go && g_box_done ? _beginthreadex(NULL, 256 << 10, box_thread, NULL, 0, &id) : 0;
    if (h) g_box_thread = id, CloseHandle((HANDLE)h);
}

static BOOL CALLBACK box_on_screen(HWND w, LPARAM shown) {
    if (IsWindowVisible(w)) *(bool *)shown = true;
    return !*(bool *)shown;
}

// (holding g_box_lock) the box thread shows it, this one waits. No box on the screen after 5 s: the box thread is stuck
// on something this one holds (a crash inside the loader or the heap), and the game would hang with no word: shown here.
static void box_hand_over(const char *msg) {
    g_box_msg = msg;
    SetEvent(g_box_go);
    if (WaitForSingleObject(g_box_done, 5000) != WAIT_TIMEOUT) return;
    bool shown = false;
    EnumThreadWindows(g_box_thread, box_on_screen, (LPARAM)&shown);
    if (shown) WaitForSingleObject(g_box_done, INFINITE);
    else box_show(msg);
}

// One box at a time: a thread that crashes while one is up waits for it. A crash's notice (fatal) shows none once a box
// was shown: the process is on its way out with the player told why (exit right after a box can itself crash when
// memory has run out).
static bool g_box_shown;            // (under g_box_lock)
static volatile DWORD g_box_owner;  // the thread holding g_box_lock (its own box's message loop may ask for another)

static void box(const char *msg, bool fatal) {
    DWORD me = GetCurrentThreadId();
    if (me == g_box_thread || me == g_box_owner) return;  // (the box thread crashed showing one; a box inside a box)
    AcquireSRWLockExclusive(&g_box_lock);
    g_box_owner = me;
    if (!fatal || !g_box_shown) {
        g_box_shown = true;
        if (g_box_thread && me != g_main_thread) box_hand_over(msg);
        else box_show(msg);
    }
    g_box_owner = 0;
    ReleaseSRWLockExclusive(&g_box_lock);
}

void plat_error_box(const char *msg) { box(msg, false); }

// The crash being reported is memory running out (unhandled_filter): plat_fatal_notice says so instead of a bare error
static volatile bool g_fault_oom;

void plat_fatal_notice(void) {
    rt_log_flush();
    if (g_fault_oom) box(rt_out_of_memory_text(), true);  // (whoever has the log: the player has to be told what to do)
    else if (g_own_log) box("The game stopped because of an error. The details are in log.txt, next to MHGU.exe.", true);
}

typedef struct { void *(*fn)(void *); void *arg; } ThreadStart;
static unsigned __stdcall thread_trampoline(void *p) {
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
int plat_thread_create(void *(*fn)(void *), void *arg, size_t stack) {
    ThreadStart *s = malloc(sizeof *s);
    if (!s) return ENOMEM;
    s->fn = fn, s->arg = arg;
    // (STACK_SIZE_PARAM_IS_A_RESERVATION: the size is address space, committed page by page as the stack grows into it.
    // winpthreads' pthread_create passes it as the commit: 256 MB of every guest thread, 16-22 of them, counted against
    // the commit limit while about 1 MB of each was ever touched)
    errno = 0;
    _doserrno = 0;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack, thread_trampoline, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) {
        // (the CRT turns the system's error into errno, the commit limit's 1455 into EINVAL: the system's own is logged)
        unsigned long os = _doserrno ? _doserrno : GetLastError();
        int e = errno;
        free(s);
        rt_logf("[plat] a thread could not start: Windows error %lu (errno %d)\n", os, e);
        return oom_error(os) ? ENOMEM : e ? e : EAGAIN;
    }
    CloseHandle((HANDLE)h);
    return 0;
}

// ---- thread priorities (platform.h)
// A guest thread is above normal when its Horizon priority is over the main thread's 44 (the ones the game wants
// answered first ask for less: 34 to 40 here), normal otherwise. Never under normal, where the runtime's own normal
// threads (the shader builds) could keep one off the CPU while it held a lock the frame's threads waited on; never
// highest, over the vsync tick and the audio mixer. The frame's own threads are above normal whatever their guest
// priority says, and work nothing waits for this frame is below normal. A thread waiting on a guest mutex lends its
// level to the owner (runtime/kernel.c prio_lend), as Horizon raises an owner to its highest waiter's priority: an above
// normal waiter is not left behind a normal owner that other above normal threads keep off every core. A thread's
// levels are set by itself and lent by its waiters, under g_prio_lock. MHGU_PRIO=0: no thread's priority changed;
// MHGU_PRIO_LEND=0: none lent.
typedef struct {
    HANDLE h;                             // (plat_prio_self) its waiters set its priority with it
    signed char guest, floor, lent, set;  // PLAT_PRIO_* levels, PLAT_PRIO_NONE: none; set: the one Windows has
    bool fall;                            // its level fell by its own hand, still under the kernel's lock (plat_prio_sync)
} Prio;
static __thread Prio t_prio = {NULL, PLAT_PRIO_NONE, PLAT_PRIO_NONE, PLAT_PRIO_NONE, PLAT_PRIO_NONE, false};
static SRWLOCK g_prio_lock = SRWLOCK_INIT;

static int prio_level(const Prio *p) {
    int l = p->guest > p->floor ? p->guest : p->floor;
    return p->lent > l ? p->lent : l;
}

// (g_prio_lock held) h: GetCurrentThread() from the thread itself, p->h from another
static void prio_apply(Prio *p, HANDLE h) {
    int l = prio_level(p);
    if (l == PLAT_PRIO_NONE) l = PLAT_PRIO_NORMAL;  // (all taken back: the system's default)
    int w = l > 0 ? THREAD_PRIORITY_ABOVE_NORMAL : l < 0 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
    if (l != p->set && SetThreadPriority(h, w)) p->set = (signed char)l;
}

bool plat_thread_priority(int level) {
    if (!g_prio_on) return false;
    AcquireSRWLockExclusive(&g_prio_lock);
    t_prio.floor = (signed char)level;
    prio_apply(&t_prio, GetCurrentThread());
    ReleaseSRWLockExclusive(&g_prio_lock);
    return true;
}

void plat_guest_priority(int prio) {
    if (!g_prio_on) return;
    AcquireSRWLockExclusive(&g_prio_lock);
    t_prio.guest = prio < 44 ? PLAT_PRIO_HIGH : PLAT_PRIO_NORMAL;
    prio_apply(&t_prio, GetCurrentThread());
    ReleaseSRWLockExclusive(&g_prio_lock);
}

void *plat_prio_self(void) {
    if (!g_prio_on || !g_lend_on) return NULL;
    if (!t_prio.h) {
        HANDLE h;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                             THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
            return NULL;
        t_prio.h = h;
    }
    return &t_prio;
}

int plat_prio_level(void *who) {
    AcquireSRWLockShared(&g_prio_lock);
    int l = prio_level(who);
    ReleaseSRWLockShared(&g_prio_lock);
    return l;
}

int plat_prio_lend(void *who, int level) {
    Prio *p = who;
    AcquireSRWLockExclusive(&g_prio_lock);
    int was = prio_level(p);
    p->lent = (signed char)level;
    int moved = prio_level(p) - was;
    // (the owner giving back what it was lent, as it lets a mutex go: lowered only once it lets the kernel's lock go
    // too, not while the waiter it just woke may take its core)
    if (moved < 0 && p == &t_prio) p->fall = true;
    else if (moved) prio_apply(p, p->h);
    ReleaseSRWLockExclusive(&g_prio_lock);
    return moved;
}

void plat_prio_sync(void) {
    if (!t_prio.fall) return;  // (set and cleared by this thread only)
    AcquireSRWLockExclusive(&g_prio_lock);
    t_prio.fall = false;
    prio_apply(&t_prio, GetCurrentThread());
    ReleaseSRWLockExclusive(&g_prio_lock);
}

void plat_prio_end(void) {
    AcquireSRWLockExclusive(&g_prio_lock);
    if (t_prio.h) CloseHandle(t_prio.h);
    t_prio.h = NULL;
    t_prio.lent = PLAT_PRIO_NONE;
    ReleaseSRWLockExclusive(&g_prio_lock);
}

// Windows may run a process it takes for a background one (minimized, covered, not focused: BackgroundInput lets one
// play so) on efficient cores at low clocks, and ignore its timer resolution, which the guest's timed waits rest on:
// both opted out (Windows 10 1709 and later; 10 has no timer part). MHGU_QOS=0: left to Windows.
static const char *g_qos = "not available";  // (for the log: plat_log_device)

static void qos_high(void) {
    const char *e = getenv("MHGU_QOS");
    if (e && *e == '0') { g_qos = "left to Windows (MHGU_QOS=0)"; return; }
    typedef BOOL(WINAPI * SetInfo)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
    SetInfo set = (SetInfo)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    if (!set) return;
    PROCESS_POWER_THROTTLING_STATE s = {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                        PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
    if (set(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s)) { g_qos = "off"; return; }
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    if (set(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s)) g_qos = "off (its timer part not known here)";
}

// (main.c, once the log is in place) how this run treats its threads
void plat_log_device(void) {
    rt_logf("[plat] thread priorities: %s%s; power throttling %s\n",
            g_prio_on ? "guest threads by guest priority (up to 43 above normal, 44 and past it normal), the update and "
                        "presenting threads, the vsync tick and the audio mixer at least above normal, background work below "
                        "normal (MHGU_PRIO=0: all normal)"
                      : "all normal (MHGU_PRIO=0)",
            !g_prio_on  ? ""
            : g_lend_on ? ", a guest mutex's owner raised to its waiters' level (MHGU_PRIO_LEND=0: not raised)"
                        : ", a guest mutex's owner not raised to its waiters' level (MHGU_PRIO_LEND=0)",
            g_qos);
}

// (nvn/vulkan/exec.c: the shader build workers) under the game's threads, through the same levels, so MHGU_PRIO=0
// leaves them as they are too
void plat_thread_below_normal(void) { plat_thread_priority(PLAT_PRIO_BACKGROUND); }

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[MAX_PATH];
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, sizeof path)) return NULL;
    const char *b = strrchr(path, '\\');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)m;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    USHORT n = RtlCaptureStackBackTrace(0, 64, bt, NULL);
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = rt_host_fn_name(pc, &off);
        if (!s) s = plat_symbol(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx mhgurecomp+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(pc - exe));
    }
}

// Is this crash memory running out? A guest chunk Windows refused to back (lazy_commit) at the faulting address, or a
// page of this thread's stack: a guest thread's stack is only reserved (plat_thread_create), so a page of it Windows
// cannot back raises a stack overflow far above the reservation's end, where runaway recursion overflows within its
// last pages; with next to no commit left (RAM plus the page file), an overflow anywhere is taken for it too.
static bool out_of_memory(const EXCEPTION_POINTERS *ep, uint64_t fa, char *why, size_t size) {
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    uint64_t left = GlobalMemoryStatusEx(&ms) ? ms.ullAvailPageFile : UINT64_MAX, limit = ms.ullTotalPageFile;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && fa >= g_lo && fa < g_hi) {
        uint64_t chunk = fa & ~(COMMIT_CHUNK - 1);
        for (int i = 0; i < REFUSED; i++) {
            if (g_refused[i] != chunk) continue;
            snprintf(why, size, "Windows could not back guest memory at %llx (error %lu), commit %llu of %llu MB left",
                     (unsigned long long)(fa - g_lo), g_refused_error, (unsigned long long)(left >> 20),
                     (unsigned long long)(limit >> 20));
            return true;
        }
    } else if (code == EXCEPTION_STACK_OVERFLOW) {
        MEMORY_BASIC_INFORMATION mi;
        uint64_t sp = ep->ContextRecord->Rsp, above = 0;
        if (VirtualQuery((void *)(uintptr_t)sp, &mi, sizeof mi)) above = sp - (uint64_t)(uintptr_t)mi.AllocationBase;
        if (above > (1u << 20) || left < (64u << 20)) {
            snprintf(why, size, "Windows could not back this thread's stack (%llu KB above its end), commit %llu of %llu MB left",
                     (unsigned long long)(above >> 10), (unsigned long long)(left >> 20), (unsigned long long)(limit >> 20));
            return true;
        }
    }
    return false;
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    rt_log_flush();  // (the lines queued until now first, then the report: the log goes straight out from here on)
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint64_t fa = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    char why[200];
    if (out_of_memory(ep, fa, why, sizeof why)) {
        g_fault_oom = true;
        rt_logf("[plat] out of memory: %s\n", why);
    }
    char what[48];
    snprintf(what, sizeof what, "exception %08lx", er->ExceptionCode);
    uint64_t lr = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)ep->ContextRecord->Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT)
        lr = *(uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
    rt_fault_report(what, fa, ep->ContextRecord->Rip, lr);
}

void plat_crash_handler_install(void) {
    ULONG guarantee = 64 << 10;
    SetThreadStackGuarantee(&guarantee);
    static bool installed;
    if (installed) return;
    installed = true;
    SetUnhandledExceptionFilter(unhandled_filter);
}

// ---- POSIX shims (win_compat.h)
static ssize_t prw(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        BOOL ok = write ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov)
                        : ReadFile(h, (char *)buf + done, chunk, &got, &ov);
        if (!ok) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? (ssize_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, true); }
