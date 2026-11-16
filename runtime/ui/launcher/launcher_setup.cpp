// The game files setup of the start screen (launcher_setup.h): prepare.py or prepare_ps5.py in a child process
// (SDL_Process), its event lines read by a thread of ours into the state the page shows and written to logs/setup.log.
// The steps and their order belong to the scripts; this side only maps their names.
// totk-pc-recomp's installer window works the same way over its CLI (installer/gui.py, MIT).
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/launcher/launcher_setup.h"

extern "C" {
#include "platform/platform.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char rt_port_version[];
}

static const char *const kDesktopSteps[SETUP_STEPS] = {"check", "exefs", "romfs", "generate", "compile", "shaders",
                                                       "warm", "cleanup"};
static const char *const kPs5Steps[SETUP_PS5_STEPS] = {"check", "nup", "exefs", "romfs", "generate", "compile",
                                                       "link", "shaders", "deploy"};

static SDL_Mutex *g_lock;
static SetupState g_state = {0, SETUP_MODE_DESKTOP, -1, SETUP_STEPS, {}, 0, 0, 0, {}, {}};
static SDL_Process *g_proc;
static char g_log_path[1200];

// The Python and prepare.py: the package's (setup/python, setup/src) beside the executable, else a source checkout's
// (the executable in build/: the repository's .venv and tools/). rel names the tool under tools/ ("prepare/prepare.py").
static bool find_tool(const char *exe_dir, const char *rel, char *python, char *script, size_t cap) {
#ifdef _WIN32
    snprintf(python, cap, "%s/setup/python/python.exe", exe_dir);
#else
    snprintf(python, cap, "%s/setup/python/bin/python3", exe_dir);
#endif
    snprintf(script, cap, "%s/setup/src/tools/%s", exe_dir, rel);
    if (plat_file_exists(python) && plat_file_exists(script)) return true;
#ifdef _WIN32
    snprintf(python, cap, "%s/../.venv/Scripts/python.exe", exe_dir);
#else
    snprintf(python, cap, "%s/../.venv/bin/python3", exe_dir);
#endif
    snprintf(script, cap, "%s/../tools/%s", exe_dir, rel);
    return plat_file_exists(python) && plat_file_exists(script);
}

// prepare_ps5.py and the PS5 link bundle (dist_ps5.py): the package's setup/ps5-bundle, else a build's
static bool find_ps5_tools(const char *exe_dir, char *python, char *script, char *bundle, size_t cap) {
    if (!find_tool(exe_dir, "ps5/prepare_ps5.py", python, script, cap)) return false;
    static const char *const where[] = {"setup/ps5-bundle", "ps5-bundle", "dist-ps5/ps5-bundle"};
    for (size_t i = 0; i < sizeof where / sizeof *where; i++) {
        snprintf(bundle, cap, "%s/%s/pitstop-runtime.json", exe_dir, where[i]);
        if (plat_file_exists(bundle)) {
            snprintf(bundle, cap, "%s/%s", exe_dir, where[i]);
            return true;
        }
    }
    return false;
}

int setup_available(const char *exe_dir) {
    char python[1200], script[1200];
    return find_tool(exe_dir, "prepare/prepare.py", python, script, sizeof python);
}

int setup_ps5_available(const char *exe_dir) {
    char python[1200], script[1200], bundle[1200];
    return find_ps5_tools(exe_dir, python, script, bundle, sizeof python);
}

// The package's Python and a tool under tools/ (import.py), for pages that run scripts like the setup does.
int setup_tool(const char *exe_dir, const char *rel, char *python, char *script, size_t cap) {
    return find_tool(exe_dir, rel, python, script, cap);
}

static void set_finished(int how, const char *code, const char *why) {
    SDL_LockMutex(g_lock);
    g_state.running = 0;
    g_state.finished = how;
    if (code) snprintf(g_state.code, sizeof g_state.code, "%s", code);
    if (why) snprintf(g_state.error, sizeof g_state.error, "%s", why);
    SDL_UnlockMutex(g_lock);
}

// One event or log line of prepare.py / prepare_ps5.py.
static void on_line(FILE *logf, char *line) {
    if (logf) {
        fprintf(logf, "%s\n", line);
        fflush(logf);
    }
    rt_log("setup: %s", line);
    SDL_LockMutex(g_lock);
    const char *const *names = g_state.mode == SETUP_MODE_PS5 ? kPs5Steps : kDesktopSteps;
    int count = g_state.step_count;
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(line, "step") || !strcmp(line, "skip")) {
        for (int i = 0; arg && i < count; i++)
            if (!strcmp(arg, names[i])) {
                if (g_state.step >= 0 && g_state.step != i && !g_state.done[g_state.step]) g_state.done[g_state.step] = 1;
                if (line[1] == 'k') g_state.done[i] = 2;
                else g_state.step = i, g_state.progress = g_state.total = 0;
            }
    } else if (!strcmp(line, "progress") && arg) {
        sscanf(arg, "%ld %ld", &g_state.progress, &g_state.total);
    } else if (!strcmp(line, "done")) {
        for (int i = 0; i < count; i++)
            if (!g_state.done[i]) g_state.done[i] = 1;
    } else if (!strcmp(line, "error") && arg) {
        char *why = strchr(arg, ' ');
        if (why) *why++ = 0;
        snprintf(g_state.code, sizeof g_state.code, "%s", arg);
        snprintf(g_state.error, sizeof g_state.error, "%s", why ? why : "");
    }
    SDL_UnlockMutex(g_lock);
}

static int SDLCALL reader(void *) {
    SDL_IOStream *out = SDL_GetProcessOutput(g_proc);
    char line[2048];
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
        on_line(nullptr, line);
    }
    if (n > 0) {
        line[n] = 0;
        on_line(nullptr, line);
    }
    int code = -1;
    SDL_WaitProcess(g_proc, true, &code);
    SDL_DestroyProcess(g_proc);
    g_proc = nullptr;
    SetupState s = setup_state();
    bool told = code == 0 || s.code[0];
    set_finished(code == 0 ? 1 : -1, told ? nullptr : "stopped", told ? nullptr : "");
    rt_log("setup: ended with code %d", code);
    return 0;
}

static void prepare_log_file(const char *exe_dir, const char *kind) {
    char logs_dir[1150];
    snprintf(logs_dir, sizeof logs_dir, "%s/logs", exe_dir);
    plat_file_mkdir_p(logs_dir);
    snprintf(g_log_path, sizeof g_log_path, "%s/setup.log", logs_dir);
    (void)kind;
}

void setup_start(const char *exe_dir, const char *game_dir, const char *source, int link_romfs) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    if (setup_state().running) return;
    char python[1200], script[1200];
    SDL_LockMutex(g_lock);
    g_state = SetupState{1, SETUP_MODE_DESKTOP, -1, SETUP_STEPS, {}, 0, 0, 0, {}, {}};
    SDL_UnlockMutex(g_lock);
    if (!find_tool(exe_dir, "prepare/prepare.py", python, script, sizeof python)) {
        set_finished(-1, "no_setup", "");
        return;
    }
    prepare_log_file(exe_dir, "desktop setup");
    // -I: no user site packages or PYTHON* variables, nothing from the working directory
    // without a source, prepare.py takes the last setup's; link_romfs < 0 leaves --romfs out too, so the RomFS mode
    // comes from that same record (0.1.11: the automatic set up after an update asked for "copy" and would have
    // copied 23 GB over a player who chose "link")
    const char *args[] = {python, "-I", script, "--game", game_dir, "--runtime", exe_dir, "--port", rt_port_version, "--source", source,
                          "--romfs", link_romfs ? "link" : "copy", nullptr};
    if (!source) args[9] = nullptr;
    if (link_romfs < 0) args[11] = nullptr;
    rt_log("setup: %s %s --game %s --runtime %s --source %s --romfs %s", python, script, game_dir, exe_dir, source ? source : "(last)",
           link_romfs < 0 ? "(last)" : link_romfs ? "link" : "copy");
    g_proc = SDL_CreateProcess(args, true);
    if (!g_proc) {
        set_finished(-1, "", SDL_GetError());
        return;
    }
    SDL_DetachThread(SDL_CreateThread(reader, "setup", nullptr));
}

void setup_ps5_start(const char *exe_dir, const char *source, const char *ps5_ip) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    if (setup_state().running) return;
    char python[1200], script[1200], bundle[1200], work_dir[1200], shader_cache[1200];
    SDL_LockMutex(g_lock);
    g_state = SetupState{1, SETUP_MODE_PS5, -1, SETUP_PS5_STEPS, {}, 0, 0, 0, {}, {}};
    SDL_UnlockMutex(g_lock);
    if (!find_ps5_tools(exe_dir, python, script, bundle, sizeof python)) {
        set_finished(-1, "no_setup", "the PS5 bundle tools are missing (setup/ps5-bundle/)");
        return;
    }
    if (!source || !source[0]) {
        set_finished(-1, "source", "choose a Mario Kart World 1.0.0 dump folder or .nup file first");
        return;
    }
    prepare_log_file(exe_dir, "PS5 build");
    snprintf(work_dir, sizeof work_dir, "%s/ps5-work", exe_dir);
    snprintf(shader_cache, sizeof shader_cache, "%s/shaders", exe_dir);

    const char *args[16];
    int n = 0;
    args[n++] = python;
    args[n++] = "-I";
    args[n++] = script;
    args[n++] = "--source";
    args[n++] = source;
    args[n++] = "--bundle";
    args[n++] = bundle;
    args[n++] = "--work";
    args[n++] = work_dir;
    args[n++] = "--log";
    args[n++] = g_log_path;
    if (plat_file_exists(shader_cache)) {
        args[n++] = "--shader-cache";
        args[n++] = shader_cache;
    }
    if (ps5_ip && ps5_ip[0]) {
        args[n++] = "--send";
        args[n++] = ps5_ip;
    }
    args[n++] = nullptr;
    rt_log("setup-ps5: %s %s --source %s --bundle %s --send %s", python, script, source, bundle,
           (ps5_ip && ps5_ip[0]) ? ps5_ip : "(none)");
    g_proc = SDL_CreateProcess(args, true);
    if (!g_proc) {
        set_finished(-1, "", SDL_GetError());
        return;
    }
    SDL_DetachThread(SDL_CreateThread(reader, "setup_ps5", nullptr));
}

void setup_cancel(void) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (g_proc) SDL_KillProcess(g_proc, true);
    snprintf(g_state.code, sizeof g_state.code, "cancelled");
    g_state.error[0] = 0;
    SDL_UnlockMutex(g_lock);
}

SetupState setup_state(void) {
    if (!g_lock) return g_state;
    SDL_LockMutex(g_lock);
    SetupState s = g_state;
    SDL_UnlockMutex(g_lock);
    return s;
}
