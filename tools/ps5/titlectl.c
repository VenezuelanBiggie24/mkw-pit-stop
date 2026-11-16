// titlectl: a resident PS5 payload that starts, stops and reports an installed title, so the PC can run the game
// unattended (tools/ps5/bench.py: kill the old process, launch, wait for the result, kill on a hang).
//
// Why a payload: the title's own receiver (runtime/platform/ps5_recv.c) runs only on the placeholder screen, never while
// the game runs, so it cannot stop it. This runs beside everything, sent once to the ELF loader (port 9021), and
// listens on TITLECTL_PORT for one-line text commands; each gets one line back ("ok ..." or "err ..."):
//   ping                 ok titlectl <version>
//   status <TITLE>       ok running pid=<pid> app_id=<hex> focused=<0|1> | ok stopped
//   launch <TITLE>       ok launched pid=<pid> via=<call> focused=<0|1> | err ...   (refuses when it already runs)
//   kill <TITLE>         ok killed pid=<pid> | ok stopped
//   exit                 ok bye, and the payload ends
// A second copy finds the port taken and exits, so sending it again is harmless.
//
// The approach (not the code) follows phantomptr/ps5upload at ef47933 (GPL-3, ideas only per AGENTS.md): launching is
// asynchronous and some firmwares accept only some of Sony's launch calls, so they are tried in turn, each followed by
// a check that the title's process really appeared, and no further request once it has: the shell answers a second
// launch request for a starting app by sending it to the background (16/11/2026, version 1: sceLncUtilLaunchApp one
// second after a SIGKILL of the previous instance started the game behind the home screen). sceSystemServiceLaunchApp
// with the logged-in user (an unpacked block, unlike the Lnc one) goes first; the Lnc calls need that user too (0 fails
// with 0x8094000F, "no foreground user"). The app that has the screen is the low 32 bits of the shell's event flag
// SceShellCoreUtilAppFocus (ps5upload's notes: 0x6018 with a game up, 0x?007 for the home screen, whose id changes),
// so launch and status say whether the title is in front; sceLncUtilSetAppFocus returns 0 and changes nothing (same
// notes), so nothing here tries to force it. Stopping is SIGKILL to the process whose app info names the title: it skips the
// game's own exit, so the end of its log can be lost (bench.py lets the game exit by itself and kills only on a hang).
// Never sends signals to pid 0/1, itself, or system titles (NPXS*).
//
// Build: uv run tools/ps5/titlectl.py build   (or PS5_PAYLOAD_SDK=... make-free: see titlectl.py)
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#define TITLECTL_VERSION 2
#define TITLECTL_PORT 9031
#define LAUNCH_WAIT_MS 15000   // the launch call returns before the process exists; a cold start takes seconds
#define KILL_WAIT_MS 5000
#define KILL_SETTLE_MS 4000    // after the process is gone, before the shell takes a new launch as a fresh one
#define FOCUS_WAIT_MS 20000    // a cold start takes the screen after its splash

// Sony's interfaces (libSceSystemService, libSceUserService, libkernel). The app info is the kernel's record of a
// process: the app id first, the title id at 0x10.
typedef struct {
    uint32_t app_id;
    uint32_t pad0;
    uint64_t unknown0;
    char title_id[10];
    char rest[0x46];
} AppInfo;
typedef struct __attribute__((packed)) {
    uint32_t size;
    int32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint64_t check_flag;
} LncAppParam;
int sceKernelGetAppInfo(pid_t pid, AppInfo *info);
int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int32_t *user);
int sceLncUtilInitialize(void);
int sceLncUtilLaunchApp(const char *title_id, const char **argv, LncAppParam *param);
typedef struct {   // sceSystemServiceLaunchApp's block: natural alignment, not packed
    uint32_t size;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} LaunchCtx;
int sceSystemServiceLaunchApp(const char *title_id, const char **argv, LaunchCtx *param);
int sceKernelOpenEventFlag(int *ef, const char *name);
int sceKernelPollEventFlag(int ef, uint64_t pattern, uint32_t mode, uint64_t *result);
#define EVF_WAITMODE_OR 0x02u

static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static int valid_title(const char *t) {
    if (strlen(t) != 9 || !strncmp(t, "NPXS", 4)) return 0;
    for (int i = 0; i < 9; i++)
        if (i < 4 ? (t[i] < 'A' || t[i] > 'Z') : (t[i] < '0' || t[i] > '9')) return 0;
    return 1;
}

// The process running title, or 0; *app_id gets its app id.
static pid_t find_title(const char *title, uint32_t *app_id) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0)) return 0;
    size += size / 4;   // processes may start between the two calls
    char *buf = malloc(size);
    if (!buf) return 0;
    pid_t found = 0;
    if (!sysctl(mib, 4, buf, &size, NULL, 0)) {
        for (size_t off = 0; off + sizeof(int) <= size;) {
            struct kinfo_proc *kp = (struct kinfo_proc *)(buf + off);
            if (kp->ki_structsize <= 0) break;
            AppInfo info;
            memset(&info, 0, sizeof info);
            if (kp->ki_pid > 1 && !sceKernelGetAppInfo(kp->ki_pid, &info) && !strncmp(info.title_id, title, 9)) {
                found = kp->ki_pid;
                if (app_id) *app_id = info.app_id;
                break;
            }
            off += (size_t)kp->ki_structsize;
        }
    }
    free(buf);
    return found;
}

// The app id that has the screen, or -1 when the shell's flag cannot be read.
static int64_t focused_app(void) {
    static int ef = -1;
    if (ef < 0 && sceKernelOpenEventFlag(&ef, "SceShellCoreUtilAppFocus")) ef = -1;
    uint64_t bits = 0;
    if (ef < 0 || sceKernelPollEventFlag(ef, ~0ull, EVF_WAITMODE_OR, &bits)) return -1;
    return (int64_t)(bits & 0xffffffffu);
}

static pid_t wait_title(const char *title, int ms) {
    for (int t = 0; t < ms; t += 250) {
        pid_t p = find_title(title, NULL);
        if (p) return p;
        sleep_ms(250);
    }
    return 0;
}

static void cmd_launch(const char *title, char *out, size_t cap) {
    if (find_title(title, NULL)) {
        snprintf(out, cap, "err already running");
        return;
    }
    static int inited;
    if (!inited) {
        sceUserServiceInitialize(NULL);   // fails harmlessly when this process already did it
        sceLncUtilInitialize();
        inited = 1;
    }
    int32_t user = -1;
    int ur = sceUserServiceGetForegroundUser(&user);
    LaunchCtx ctx = {sizeof ctx, (uint32_t)user, 0, 0, 0};
    LncAppParam param = {sizeof param, user, 0, 0, 0};
    const char *via = "systemservice";
    int r1 = sceSystemServiceLaunchApp(title, NULL, ur ? NULL : &ctx), r2 = 0, r3 = 0;   // the new app id, < 0 fails
    pid_t p = wait_title(title, LAUNCH_WAIT_MS);
    if (!p) { via = "lnc-param"; r2 = sceLncUtilLaunchApp(title, NULL, &param); p = wait_title(title, LAUNCH_WAIT_MS); }
    if (!p) { via = "lnc"; r3 = sceLncUtilLaunchApp(title, NULL, NULL); p = wait_title(title, LAUNCH_WAIT_MS); }
    if (!p) {
        snprintf(out, cap, "err no process after launch: user=%#x (%#x) systemservice=%#x lnc-param=%#x lnc=%#x",
                 (unsigned)user, (unsigned)ur, (unsigned)r1, (unsigned)r2, (unsigned)r3);
        return;
    }
    uint32_t app = 0;
    int focused = 0;
    for (int t = 0; t < FOCUS_WAIT_MS && !focused; t += 250) {
        if (find_title(title, &app) && focused_app() == (int64_t)app) focused = 1;
        else sleep_ms(250);
    }
    snprintf(out, cap, "ok launched pid=%d via=%s app_id=%#x focused=%d focus=%#llx", (int)p, via, app, focused,
             (long long)focused_app());
}

static void cmd_kill(const char *title, char *out, size_t cap) {
    pid_t p = find_title(title, NULL);
    if (!p) { snprintf(out, cap, "ok stopped"); return; }
    if (p <= 1 || p == getpid()) { snprintf(out, cap, "err refusing pid %d", (int)p); return; }
    if (kill(p, SIGKILL)) { snprintf(out, cap, "err kill %d: %s", (int)p, strerror(errno)); return; }
    for (int t = 0; t < KILL_WAIT_MS; t += 100) {
        if (find_title(title, NULL) != p) {
            sleep_ms(KILL_SETTLE_MS);
            snprintf(out, cap, "ok killed pid=%d", (int)p);
            return;
        }
        sleep_ms(100);
    }
    snprintf(out, cap, "err pid %d still there after SIGKILL", (int)p);
}

// One command line into out; returns 0 for "exit".
static int run(char *line, char *out, size_t cap) {
    line[strcspn(line, "\r\n")] = 0;
    char cmd[16] = "", title[16] = "";
    sscanf(line, "%15s %15s", cmd, title);
    if (!strcmp(cmd, "ping")) { snprintf(out, cap, "ok titlectl %d", TITLECTL_VERSION); return 1; }
    if (!strcmp(cmd, "exit")) { snprintf(out, cap, "ok bye"); return 0; }
    if (strcmp(cmd, "status") && strcmp(cmd, "launch") && strcmp(cmd, "kill")) {
        snprintf(out, cap, "err unknown command");
        return 1;
    }
    if (!valid_title(title)) { snprintf(out, cap, "err bad title id"); return 1; }
    if (!strcmp(cmd, "launch")) cmd_launch(title, out, cap);
    else if (!strcmp(cmd, "kill")) cmd_kill(title, out, cap);
    else {
        uint32_t app = 0;
        pid_t p = find_title(title, &app);
        int64_t f = focused_app();
        if (p) snprintf(out, cap, "ok running pid=%d app_id=%#x focused=%d focus=%#llx", (int)p, app, f == (int64_t)app,
                        (long long)f);
        else snprintf(out, cap, "ok stopped");
    }
    return 1;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(TITLECTL_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) {
        printf("titlectl: port %d taken (already running?): %s\n", TITLECTL_PORT, strerror(errno));
        return 0;
    }
    printf("titlectl %d: listening on %d\n", TITLECTL_VERSION, TITLECTL_PORT);
    for (int alive = 1; alive;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        struct timeval tv = {30, 0};   // a client that connects and says nothing does not block the others for long
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char line[128], out[256];
        ssize_t n = recv(c, line, sizeof line - 1, 0);
        if (n > 0) {
            line[n] = 0;
            alive = run(line, out, sizeof out);
            printf("titlectl: %s -> %s\n", line, out);
            size_t len = strlen(out);
            out[len++] = '\n';
            send(c, out, len, 0);
        }
        close(c);
    }
    close(s);
    return 0;
}
