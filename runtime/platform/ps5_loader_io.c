// POSIX file reads and clocks for the native PS5 title's loader.
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *posix_path(const char *path, char buf[PLAT_PATH_MAX]) {
    if (!strchr(path, '\\')) return path;
    size_t length = strlen(path);
    if (length >= PLAT_PATH_MAX) return NULL;
    for (size_t i = 0; i <= length; ++i) buf[i] = path[i] == '\\' ? '/' : path[i];
    return buf;
}

static bool file_info(const char *path, struct stat *info) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    return path && stat(path, info) == 0;
}

int plat_file_exists(const char *path) {
    struct stat info;
    return file_info(path, &info) && S_ISREG(info.st_mode);
}

int plat_dir_exists(const char *path) {
    struct stat info;
    return file_info(path, &info) && S_ISDIR(info.st_mode);
}

int64_t plat_file_size(const char *path) {
    struct stat info;
    return file_info(path, &info) && S_ISREG(info.st_mode) ? info.st_size : -1;
}

void plat_file_mkdir_p(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path || !*path) return;
    if (path != buf && snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) return;
    for (char *p = buf + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    if (size) *size = 0;
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    uint8_t *data = NULL;
    struct stat info;
    if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= (1ll << 30)) {
        size_t bytes = (size_t)info.st_size, received = 0;
        data = malloc(bytes + 1);
        while (data && received < bytes) {
            ssize_t got = read(fd, data + received, bytes - received);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            received += (size_t)got;
        }
        if (received == bytes) {
            data[bytes] = 0;
            if (size) *size = bytes;
        } else {
            free(data);
            data = NULL;
        }
    }
    close(fd);
    return data;
}

int plat_exe_path(char *out, size_t cap) {
    static const char app[] = "/app0/eboot.bin";
    if (cap < sizeof app) return 0;
    memcpy(out, app, sizeof app);
    return sizeof app - 1;
}

#ifdef __PROSPERO__
uint64_t sceKernelGetTscFrequency(void);
#endif

static volatile int tsc_clock_on;
static uint64_t tsc_base_ns, tsc_base_c, tsc_mul;

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t mono_ns(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) abort();
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

int plat_tsc_clock_mode(int on) {
#ifdef __PROSPERO__
    if (on && !tsc_mul) {
        uint64_t hz = sceKernelGetTscFrequency();
        if (!hz) return -1;
        tsc_base_ns = mono_ns();
        tsc_base_c = read_tsc();
        tsc_mul = (uint64_t)(((__uint128_t)1000000000ull << 40) / hz);
    }
    __atomic_store_n(&tsc_clock_on, on ? 1 : 0, __ATOMIC_RELEASE);
    return on ? 1 : 0;
#else
    (void)on;
    return -1;
#endif
}

uint64_t plat_now_ns(void) {
    if (__atomic_load_n(&tsc_clock_on, __ATOMIC_ACQUIRE)) {
        uint64_t c = read_tsc();
        uint64_t dc = c > tsc_base_c ? c - tsc_base_c : 0;
        return tsc_base_ns + (uint64_t)(((__uint128_t)dc * tsc_mul) >> 40);
    }
    return mono_ns();
}

uint64_t plat_counter(void) { return plat_now_ns(); }
uint64_t plat_counter_hz(void) { return 1000000000u; }

void plat_sleep_ns(uint64_t ns) {
    struct timespec delay = {.tv_sec = ns / 1000000000u, .tv_nsec = ns % 1000000000u};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

void plat_sleep_ms(uint32_t ms) { plat_sleep_ns((uint64_t)ms * 1000000u); }

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *value = getenv(name);
    size_t length = value ? strlen(value) : 0;
    if (length >= cap) return 0;
    if (length) memcpy(out, value, length);
    out[length] = '\0';
    return length;
}

uint32_t plat_process_id(void) { return (uint32_t)getpid(); }

// The MIT template's CRT exit path closes the title through the shell.
void catchReturnFromMain(int status);

_Noreturn void plat_exit(int code) {
    fflush(stdout);
    fflush(stderr);
    catchReturnFromMain(code);
    abort();
}
