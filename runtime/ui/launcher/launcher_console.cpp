// The PS5 found and checked (launcher_console.h): tools/ps5/console_probe.py in the package's Python, its lines read
// by a thread of ours as they come (a search prints each console as soon as it answers), as launcher_finddumps.cpp does.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "ui/launcher/launcher_console.h"
#include "ui/launcher/launcher_setup.h"

extern "C" {
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static SDL_Mutex *g_lock;
static ConsoleState g_st;

typedef struct {
    SDL_Process *proc;
    int find;   // 1 find, 0 check
} Job;

static char *field(char **rest) {   // the next tab-separated field of *rest
    char *f = *rest;
    if (!f) return nullptr;
    char *t = strchr(f, '\t');
    if (t) *t++ = 0;
    *rest = t;
    return f;
}

static void on_line(int find, char *line) {
    char *rest = line, *tag = field(&rest);
    if (!tag) return;
    SDL_LockMutex(g_lock);
    if (find && !strcmp(tag, "found") && g_st.found_count < CONSOLE_FOUND_MAX) {
        char *ip = field(&rest), *kind = field(&rest), *what = field(&rest);
        if (ip && kind) {
            ConsoleFound *c = &g_st.found[g_st.found_count++];
            snprintf(c->ip, sizeof c->ip, "%s", ip);
            snprintf(c->kind, sizeof c->kind, "%s", kind);
            snprintf(c->what, sizeof c->what, "%s", what ? what : "");
        }
    } else if (!find && !strcmp(tag, "kind")) {
        snprintf(g_st.kind, sizeof g_st.kind, "%s", rest ? rest : "none");
    } else if (!find && !strcmp(tag, "line") && rest && g_st.line_count < CONSOLE_LINES_MAX) {
        snprintf(g_st.lines[g_st.line_count++], sizeof g_st.lines[0], "%s", rest);
    } else if (!find && !strcmp(tag, "game") && g_st.game_count < CONSOLE_GAMES_MAX) {
        char *version = field(&rest), *dir = field(&rest);
        snprintf(g_st.game_version[g_st.game_count], sizeof g_st.game_version[0], "%s", version ? version : "?");
        snprintf(g_st.game_dir[g_st.game_count], sizeof g_st.game_dir[0], "%s", dir ? dir : ".");
        g_st.game_count++;
    }
    SDL_UnlockMutex(g_lock);
}

static int SDLCALL reader(void *raw) {
    Job *job = (Job *)raw;
    SDL_IOStream *out = SDL_GetProcessOutput(job->proc);
    char line[1024];
    size_t n = 0;
    char c;
    for (;;) {
        if (SDL_ReadIO(out, &c, 1) != 1) {   // the pipe does not block: no data yet is not the end
            if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;
            SDL_Delay(20);
            continue;
        }
        if (c == '\r') continue;
        if (c != '\n') {
            if (n + 1 < sizeof line) line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        on_line(job->find, line);
    }
    if (n) {
        line[n] = 0;
        on_line(job->find, line);
    }
    SDL_WaitProcess(job->proc, true, nullptr);
    SDL_DestroyProcess(job->proc);
    SDL_LockMutex(g_lock);
    if (job->find) g_st.finding = 0;
    else {
        g_st.checking = 0;
        if (!g_st.kind[0]) snprintf(g_st.kind, sizeof g_st.kind, "none");
    }
    SDL_UnlockMutex(g_lock);
    SDL_free(job);
    return 0;
}

static void start(const char *exe_dir, int find, const char *ip) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    char python[1200], script[1200];
    if (!setup_tool(exe_dir, "ps5/console_probe.py", python, script, sizeof python)) return;
    SDL_LockMutex(g_lock);
    int busy = find ? g_st.finding : g_st.checking;
    if (!busy) {
        if (find) g_st.finding = 1, g_st.found_count = 0;
        else {
            g_st.checking = 1, g_st.kind[0] = 0, g_st.line_count = g_st.game_count = 0;
            snprintf(g_st.checked_ip, sizeof g_st.checked_ip, "%s", ip);
        }
    }
    SDL_UnlockMutex(g_lock);
    if (busy) return;
    const char *args[] = {python, "-I", script, find ? "find" : "check", find ? nullptr : ip, nullptr};
    SDL_Process *proc = SDL_CreateProcess(args, true);
    Job *job = proc ? (Job *)SDL_malloc(sizeof *job) : nullptr;
    SDL_Thread *t = nullptr;
    if (job) {
        *job = {proc, find};
        t = SDL_CreateThread(reader, find ? "console find" : "console check", job);
    }
    if (t) {
        SDL_DetachThread(t);
        return;
    }
    rt_log("launcher: cannot ask for the PS5: %s", SDL_GetError());
    if (proc) SDL_DestroyProcess(proc);
    SDL_free(job);
    SDL_LockMutex(g_lock);
    if (find) g_st.finding = 0;
    else g_st.checking = 0, snprintf(g_st.kind, sizeof g_st.kind, "none");
    SDL_UnlockMutex(g_lock);
}

void console_find_start(const char *exe_dir) { start(exe_dir, 1, nullptr); }

void console_check_start(const char *exe_dir, const char *ip) {
    if (ip && *ip) start(exe_dir, 0, ip);
}

ConsoleState console_state(void) {
    if (!g_lock) return ConsoleState{};
    SDL_LockMutex(g_lock);
    ConsoleState st = g_st;
    SDL_UnlockMutex(g_lock);
    return st;
}
