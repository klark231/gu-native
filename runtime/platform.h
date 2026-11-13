// Host OS layer: everything the runtime needs that differs between Windows and Linux.
// Implementations: runtime/platform/windows.c, runtime/platform/linux.c (the Makefile picks one).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#else
// POSIX has no text / binary file modes, and O_BINARY does not exist: the runtime opens everything with O_BINARY
// (on Windows it is what keeps CRLF translation away from game data).
#ifndef O_BINARY
#define O_BINARY 0
#endif
#include <sys/types.h>  // (ssize_t, off_t: plat_pread)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- guest address space
// Claim the 4 GB guest window at GUEST_BASE (pages are backed lazily, on first touch).
bool plat_reserve_guest(uint64_t base, uint64_t size);
// Make sure host [p, p+size) inside the window is backed before the OS itself writes there (file reads), and writable
// (Android: its page write tracking leaves pages the renderer watches read-only).
void plat_commit(uint64_t host_addr, uint64_t size);
// pread into memory that may be guest memory. Android: the kernel can't write a page the write tracking keeps read-only
// (EFAULT, or a short read, where a store would just fault and go on): this one gets it written, and reads on past a
// short read (short only at the end of the file). Elsewhere: pread.
#ifdef __ANDROID__
ssize_t plat_pread(int fd, void *buf, size_t n, off_t off);
#else
#define plat_pread pread
#endif
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_RW = 3 };
int plat_protect(uint64_t host_addr, uint64_t size, int prot);
#define PLAT_PAGE 0x1000ull
// Pages of host [addr, addr+size) inside the window written since the last call for them (the tracking is reset):
// their host addresses into pages (at most max; a full array means "maybe more": treat all as written), *page = the
// tracking granularity. -1: no write tracking.
// Is host addr in committed, readable memory? *end = where that region (committed or not) ends. (Debug scans.)
bool plat_committed(uint64_t addr, uint64_t *end);int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);

// ---- time
uint64_t plat_now_ns(void);                // monotonic
void plat_sleep_until_ns(uint64_t t);      // precise wait for plat_now_ns() >= t
#ifdef __ANDROID__
// (the perf pass's MHGU_PERF_SLEEP_COARSE) one sleep to t, the thread's timer slack as it is and no spin at the end:
// for waits that need no precision. It may end some tens of microseconds after t.
void plat_sleep_coarse_until_ns(uint64_t t);
#endif
#ifdef _WIN32
// An event waited for with a precise timeout (nvn/vulkan/exec.c: a draw's short wait for its shader build;
// runtime/kernel.c kwait: a guest thread's waits). winpthreads' timed waits end on the system's 15.6 ms tick whatever
// is asked (a 1 ms pthread_cond_timedwait took 13-17 ms, timeBeginPeriod(1) or not); this one waits on the thread's
// high-resolution timer.
void *plat_event_new(void);                       // auto-reset: a set wakes one waiter, or the next wait if none waits
void plat_event_set(void *e);
// Until set (true, and it is reset), or plat_now_ns() >= t (false); t past: only a look; t UINT64_MAX: no timeout.
bool plat_event_wait_until(void *e, uint64_t t);
#endif

// ---- process
void plat_init(void);
bool plat_exe_path(char *buf, size_t size);
unsigned long plat_process_id(void);
bool plat_wait_process(unsigned long pid); // Restart child waits for old host to release saves and logs.
// Started without a game folder: work in the executable's folder; log to log.txt there when there is no terminal
// (plat_fatal_notice then tells the player about a fatal error).
void plat_standalone(void);
// The player's own folder for this game, created if missing (save data lives there, away from the game folder so a
// rebuilt or deleted build folder can't take it along): Windows %APPDATA%\MHGURecomp.
// TODO(Linux): $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp (see docs/STATUS.md "Save data").
bool plat_user_dir(char *buf, size_t size);
// The files in a folder and its subfolders, 0 if it can't be read (main.c: whether the player's romfs is complete;
// the system's own listing, not a stat per file: ~80 ms for the game's 35531 on Windows).
int plat_count_files(const char *dir);
// A UTF-8 path (SDL's dialogs, game_folder.txt) in the spelling the runtime's file functions take: Windows' narrow
// ones use the system's code page, so a path it can't spell comes back in its 8.3 short form (that path must exist).
bool plat_path_from_utf8(const char *utf8, char *out, size_t size);
// The other way: a path of the runtime's (the system's code page on Windows) in UTF-8, for SDL's file functions.
bool plat_path_to_utf8(const char *native, char *out, size_t size);
void plat_error_box(const char *msg);
void plat_fatal_notice(void);
// A thread running fn(arg) with stack bytes of stack, nobody waiting for it (runtime/kernel.c: a guest thread); 0 or
// the error. Windows only reserves the stack and commits it as it is used: pthread_create committed all of it.
int plat_thread_create(void *(*fn)(void *), void *arg, size_t stack);
#ifdef _WIN32
void plat_thread_below_normal(void);  // the calling thread gives way to the game's (nvn/vulkan/exec.c: shader builds)
#endif
// The calling thread's name as the system's tools show it (top, simpleperf, /proc/<pid>/task/<tid>/comm; up to 15
// characters): "vsync", "upload-2", "g44-50". Without it every thread the runtime makes is "SDLThread" on Android, the
// name of the one that made it. MHGU_PERF_THREADNAMES (Android, on by default; read once, by plat_init); =0, and elsewhere, nothing.
#ifdef __ANDROID__
void plat_thread_name(const char *name);
#else
static inline void plat_thread_name(const char *name) { (void)name; }
#endif
const char *rt_out_of_memory_text(void);  // (runtime/kernel.c) the message when the system has no memory left

// ---- thread priorities (Windows, runtime/platform/windows.c; elsewhere every thread keeps the system's default)
// The calling thread's priority for what it does: PLAT_PRIO_BACKGROUND below normal (shader and pipeline builds, the
// log's writer, the readout's sampler: nothing waits for them this frame), PLAT_PRIO_HIGH above normal (the game's update
// thread, the presenting one, the vsync tick, the audio mixer). It is a floor: it never takes a guest thread under its
// guest priority's level, and a later SetThreadPriority from the game never takes the thread under it. false: nothing
// changed (MHGU_PRIO=0, or not Windows).
enum { PLAT_PRIO_NONE = -128, PLAT_PRIO_BACKGROUND = -1, PLAT_PRIO_NORMAL = 0, PLAT_PRIO_HIGH = 1 };
// A guest thread's Horizon priority (0 highest .. 63; the game's main thread runs at 44), for the calling thread: up to
// 43 above normal, 44 and past it normal, never under (runtime/kernel.c: at its start and when it changes).
// A thread waiting on a guest mutex lends the owner its level (runtime/kernel.c prio_lend), as Horizon raises an owner
// to its highest waiter's priority: plat_prio_self is the calling thread's record for that (NULL: nothing is lent,
// MHGU_PRIO=0 or MHGU_PRIO_LEND=0), plat_prio_level a record's level (the highest of its guest priority's, its floor and
// what it is lent), plat_prio_lend the level lent to it now, PLAT_PRIO_NONE for none (> 0: its level rose, < 0: fell;
// the calling thread's own fall waits for plat_prio_sync, called once it holds the kernel's lock no more), plat_prio_end
// the calling thread's last call, once nothing can lend to it any more.
#ifdef _WIN32
bool plat_thread_priority(int level);
void plat_guest_priority(int prio);
void *plat_prio_self(void);
int plat_prio_level(void *who);
int plat_prio_lend(void *who, int level);
void plat_prio_sync(void);
void plat_prio_end(void);
#else
static inline bool plat_thread_priority(int level) { (void)level; return false; }
static inline void plat_guest_priority(int prio) { (void)prio; }
static inline void *plat_prio_self(void) { return NULL; }
static inline int plat_prio_level(void *who) { (void)who; return PLAT_PRIO_NONE; }
static inline int plat_prio_lend(void *who, int level) { (void)who, (void)level; return 0; }
static inline void plat_prio_sync(void) {}
static inline void plat_prio_end(void) {}
#endif

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset);
void plat_print_backtrace(void);
void plat_crash_handler_install(void);
// Called (when set) after a guest page nobody mapped was first touched and got backed with zeros: guest address,
// 1 write / 0 read / -1 unknown. Runs in the fault handler: Forge uses it to name a plugin's bad pointer.
extern void (*plat_touch_hook)(uint64_t guest_addr, int write);
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) __attribute__((noreturn));

#if defined(__ANDROID__) || defined(_WIN32)
// ---- performance hints (runtime/platform/android_perf.c: an ADPF session for the threads the frame rate rests on;
// Windows, runtime/platform/win_stats.c: the game and render threads whose time per frame the performance readout shows)
enum { PERF_GAME, PERF_RENDER, PERF_SUBMIT };
void perf_hint_thread(int role);  // the calling thread is that one (after its first call: a thread-local load, Windows
                                  // its id against the role's; for PERF_SUBMIT also a store: the frames it submits in)
void perf_hint_frame(void);       // a frame was presented (the presenting thread, once per frame)
#endif

#ifdef __cplusplus
}
#endif
