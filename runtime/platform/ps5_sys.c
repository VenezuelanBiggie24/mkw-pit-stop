// POSIX file writes, system queries, and unsupported desktop stubs for the PS5 platform.
#define _GNU_SOURCE
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void arc4random_buf(void *buf, size_t nbytes);

struct PlatLoad { int unused; };

static const char *posix_path(const char *path, char buf[PLAT_PATH_MAX]) {
    if (!strchr(path, '\\')) return path;
    size_t length = strlen(path);
    if (length >= PLAT_PATH_MAX) return NULL;
    for (size_t i = 0; i <= length; ++i) buf[i] = path[i] == '\\' ? '/' : path[i];
    return buf;
}

static int write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static void sync_dir_of(const char *path) {
    char dir[PLAT_PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) return;
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(dir, ".");
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

void plat_wait_ns(uint64_t ns) { plat_sleep_ns(ns); }

int plat_full_path(const char *path, char *out, size_t cap) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return 0;
    if (path[0] == '/') return snprintf(out, cap, "%s", path) < (int)cap;
    char cwd[PLAT_PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    return snprintf(out, cap, "%s/%s", cwd, path) < (int)cap;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n);
    return close(fd) == 0 && ok;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return 0;
    char tmp[PLAT_PATH_MAX + 48];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", path, plat_process_id(), plat_thread_id());
    if (len < 0 || len >= (int)sizeof tmp) return 0;
    if (access(path, W_OK) && errno != ENOENT) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n) && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok && !rename(tmp, path)) {
        sync_dir_of(path);
        return 1;
    }
    unlink(tmp);
    return 0;
}

int plat_file_sync(FILE *f) { return !fflush(f) && !fsync(fileno(f)); }

void plat_file_delete(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (path) unlink(path);
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    char buf[PLAT_PATH_MAX];
    dir = posix_path(dir, buf);
    if (!dir) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t el = strlen(ext);
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < el || strcmp(e->d_name + n - el, ext) || e->d_name[0] == '.') continue;
        if (!cb(e->d_name, ctx)) break;
    }
    closedir(d);
    return 1;
}

void plat_local_time(struct tm *out) {
    time_t now = time(NULL);
    if (!localtime_r(&now, out)) memset(out, 0, sizeof *out);
}

// libSceUserService, as the template's pad code uses it (PS5_VulkanTemplate ps5/src/platform.c): the user who started
// the title and their online ID (16 characters at most).
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int sceUserServiceGetUserName(int32_t user_id, char *name, size_t size);

int plat_user_name(char *out, size_t n) {
    int32_t user = -1;
    // The service may not be up yet: the pads start it (pad_open), after the plugins have asked for the player's name
    // (plugin.c) at boot, which then got a random one for the run. A second start only answers "already started".
    (void)sceUserServiceInitialize(NULL);
    if (!out || n < 2 || sceUserServiceGetInitialUser(&user) < 0 || user < 0) return 0;
    memset(out, 0, n);
    if (sceUserServiceGetUserName(user, out, n - 1) < 0) return 0;
    return out[0] != 0;
}

int plat_random(void *out, size_t n) {
    arc4random_buf(out, n);
    return 1;
}

// The image's load address (lld defines __ehdr_start at the image's start, VA 0 in llvm-pie.elf): logged offsets
// (exe+0x...) are then llvm-pie.elf's addresses. Run 16 printed absolute addresses as exe+ (the title loads at
// 0x400000: lifted code at exe+0x5b0020 is llvm-pie.elf's 0x1b0020). Weak: 0 (absolute) if the linker gave none.
extern const char __ehdr_start[] __attribute__((weak, visibility("hidden")));
uintptr_t plat_exe_base(void) { return (uintptr_t)__ehdr_start; }

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    (void)addr; (void)name; (void)cap; (void)base;
    return 0;
}

int plat_process_alive(uint32_t pid) { return pid == plat_process_id(); }

int plat_stderr_path(char *out, size_t cap) {
    (void)out; (void)cap;
    return 0;
}

int plat_os_version(char *out, size_t cap) { return snprintf(out, cap, "PS5") < (int)cap; }
const char *plat_ui_language(void) { return "en-US"; }

int plat_font_path(const char *lang, char *out, size_t cap) {
    (void)lang; (void)out; (void)cap;
    return 0;
}

int plat_open_folder(const char *path) {
    (void)path;
    return 0;
}

int plat_clipboard_set(const char *utf8) {
    (void)utf8;
    return 0;
}

void *plat_lib_open(const char *name) {
    (void)name;
    return NULL;
}

void *plat_lib_sym(void *lib, const char *name) {
    (void)lib; (void)name;
    return NULL;
}

PlatLoad *plat_load_open(const uint8_t *gpu_luid) {
    (void)gpu_luid;
    return calloc(1, sizeof(PlatLoad));
}

void plat_load_close(PlatLoad *l) { free(l); }

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    (void)l;
    *total_pct = -1.0f;
    *proc_pct = -1.0f;
    *temp_c = -1.0f;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    (void)l;
    *load_pct = -1.0f;
    *vram_used_mb = -1.0f;
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = -1.0f;
    *proc_mb = -1.0f;
    int64_t direct = sceKernelGetDirectMemorySize();
    *total_mb = direct > 0 ? (float)(direct / (1024.0 * 1024.0)) : -1.0f;
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    *resident = 0;
    *private_bytes = 0;
}

// PS5's post-link check rejects weak undefined symbols when zstd's optional tracer is absent.
__attribute__((used, retain)) unsigned long long ZSTD_trace_compress_begin(const void *cctx) {
    (void)cctx;
    return 0;
}
__attribute__((used, retain)) void ZSTD_trace_compress_end(unsigned long long ctx, const void *trace) {
    (void)ctx;
    (void)trace;
}
__attribute__((used, retain)) unsigned long long ZSTD_trace_decompress_begin(const void *dctx) {
    (void)dctx;
    return 0;
}
__attribute__((used, retain)) void ZSTD_trace_decompress_end(unsigned long long ctx, const void *trace) {
    (void)ctx;
    (void)trace;
}

struct PlatFile { int fd; };
struct PlatAppend { int fd; };

static int fs_err_from_errno(int e) {
    switch (e) {
    case 0: return PLAT_FS_OK;
    case ENOENT:
    case ENOTDIR: return PLAT_FS_NOT_FOUND;
    case EEXIST: return PLAT_FS_EXISTS;
    case ENOTEMPTY: return PLAT_FS_NOT_EMPTY;
    case EBUSY:
    case ETXTBSY: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

void plat_timer_1ms(void) {}

int plat_setenv(const char *name, const char *value) {
    if (!name || !*name) return 0;
    return (value && *value) ? setenv(name, value, 1) == 0 : unsetenv(name) == 0;
}

uint64_t plat_process_uptime_ns(void) { return plat_now_ns(); }

uint64_t plat_unix_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return localtime_r(&t, out) != NULL;
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return gmtime_r(&t, out) != NULL;
}

long plat_tz_west(void) {
    tzset();
    return 0;
}

int plat_time_zone(char *out, size_t cap) {
    if (!out || !cap) return 0;
    return snprintf(out, cap, "UTC") < (int)cap;
}

// libSceSystemService: the console's system language (Settings > Language), as a BCP 47 tag the game's language
// follows when MKW_LANG is unset (ov_system.c switch_language), as the Switch follows its own. Was "en-US" whatever
// the console's language. The ids are SCE_SYSTEM_SERVICE_PARAM_ID_LANG's values; an unknown one (Finnish, Arabic,
// ...) stays English, as the game ships none of them.
int sceSystemServiceParamGetInt(int param_id, int32_t *value);

const char *plat_user_locale(void) {
    static const char *const tags[] = {
        "ja", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL", "pt-PT", "ru-RU", "ko-KR", "zh-Hant", "zh-Hans",
        "fi-FI", "sv-SE", "da-DK", "nb-NO", "pl-PL", "pt-BR", "en-GB", "tr-TR", "es-419", "ar", "fr-CA", "cs-CZ",
        "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID", "uk-UA",
    };
    enum { PARAM_ID_LANG = 1 };
    int32_t id = -1;
    if (sceSystemServiceParamGetInt(PARAM_ID_LANG, &id) < 0 || id < 0 || id >= (int32_t)(sizeof tags / sizeof *tags))
        return "en-US";
    return tags[id];
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    (void)env_name; (void)env_value; (void)out; (void)cap; (void)timeout_ms;
    return -1;
}

int plat_file_replace(const char *from, const char *to) {
    char fbuf[PLAT_PATH_MAX], tbuf[PLAT_PATH_MAX];
    from = posix_path(from, fbuf);
    to = posix_path(to, tbuf);
    if (!from || !to || rename(from, to) != 0) return 0;
    sync_dir_of(to);
    return 1;
}

int plat_file_set_readonly(const char *path, int on) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    return path && chmod(path, on ? 0444 : 0644) == 0;
}

PlatAppend *plat_append_open(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return NULL;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;
    PlatAppend *f = malloc(sizeof *f);
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    return f;
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    return f && write_all(f->fd, data, n);
}

void plat_append_sync(PlatAppend *f) {
    if (f) fsync(f->fd);
}

PlatFile *plat_file_open(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    PlatFile *f = malloc(sizeof *f);
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    return f;
}

PlatFile *plat_file_open_unbuffered(const char *path) { return plat_file_open(path); }

void plat_file_close(PlatFile *f) {
    if (!f) return;
    close(f->fd);
    free(f);
}

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    if (!f) return -1;
    if (!plat_mem_back_hold((uintptr_t)buf, n)) return -2;
    uint8_t *p = buf;
    size_t done = 0;
    int efault_retries = 0;
    int64_t ret = 0;
    plat_sample_hold(1);   // the sampler's signal off this kernel copy into watched memory (sigsample.c)
    while (done < n) {
        ssize_t k = pread(f->fd, p + done, n - done, (off_t)(off + done));
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && errno == EFAULT) {
            static int logged;
            if (!__atomic_exchange_n(&logged, 1, __ATOMIC_ACQ_REL)) {
                fprintf(stderr, "PS5 pread EFAULT at %p+%zu (watch=%#x)\n",
                        (void *)(p + done), n - done, (unsigned)plat_mem_watch_info((uintptr_t)(p + done)));
            }
            plat_mem_early_resolve((uintptr_t)(p + done), n - done);   // a watch set during the read
            if (efault_retries++ < 3 && plat_mem_back((uintptr_t)(p + done), n - done)) continue;
            ret = -2;
            goto out;
        }
        if (k < 0) { ret = -1; goto out; }
        if (k == 0) break;
        done += (size_t)k;
    }
    ret = (int64_t)done;
out:
    plat_sample_hold(0);
    plat_mem_back_release((uintptr_t)buf, n);
    return ret;
}

int plat_file_id(PlatFile *f, PlatFileId *out) {
    if (!f || !out) return 0;
    struct stat st;
    if (fstat(f->fd, &st) != 0) return 0;
    out->volume = (uint64_t)st.st_dev;
    out->file_id = (uint64_t)st.st_ino;
    out->size = (uint64_t)st.st_size;
    out->write_time = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    out->create_time = (uint64_t)st.st_ctim.tv_sec * 1000000000ull + (uint64_t)st.st_ctim.tv_nsec;
    return 1;
}

int plat_fs_kind(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return PLAT_FS_MISSING;
    struct stat st;
    if (lstat(path, &st) != 0 && stat(path, &st) != 0) return PLAT_FS_MISSING;
    if (S_ISLNK(st.st_mode)) return PLAT_FS_LINK;
    if (S_ISDIR(st.st_mode)) return PLAT_FS_DIR;
    return PLAT_FS_FILE;
}

int plat_fs_mkdir(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return PLAT_FS_ERROR;
    if (mkdir(path, 0755) == 0) return PLAT_FS_OK;
    int e = errno;
    if (e != EEXIST) fprintf(stderr, "plat_fs_mkdir(%s) failed: errno=%d (%s)\n", path, e, strerror(e));
    return fs_err_from_errno(e);
}

int plat_fs_rmdir(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return PLAT_FS_ERROR;
    return rmdir(path) == 0 ? PLAT_FS_OK : fs_err_from_errno(errno);
}

int plat_fs_unlink(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return PLAT_FS_ERROR;
    if (unlink(path) == 0) return PLAT_FS_OK;
    if (errno == EACCES || errno == EPERM) {
        chmod(path, 0644);
        if (unlink(path) == 0) return PLAT_FS_OK;
    }
    return fs_err_from_errno(errno);
}

int plat_fs_rename(const char *from, const char *to, int durable) {
    char fbuf[PLAT_PATH_MAX], tbuf[PLAT_PATH_MAX];
    from = posix_path(from, fbuf);
    to = posix_path(to, tbuf);
    if (!from || !to) return PLAT_FS_ERROR;
    struct stat st;
    if (lstat(to, &st) == 0) return PLAT_FS_EXISTS;
    if (rename(from, to) != 0) return fs_err_from_errno(errno);
    if (durable) sync_dir_of(to);
    return PLAT_FS_OK;
}

int plat_fs_link(const char *existing, const char *path) {
    char ebuf[PLAT_PATH_MAX], pbuf[PLAT_PATH_MAX];
    existing = posix_path(existing, ebuf);
    path = posix_path(path, pbuf);
    if (!existing || !path) return PLAT_FS_ERROR;
    return link(existing, path) == 0 ? PLAT_FS_OK : fs_err_from_errno(errno);
}

int plat_fs_links(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return 0;
    struct stat st;
    return (lstat(path, &st) == 0 || stat(path, &st) == 0) ? (int)st.st_nlink : 0;
}

void plat_fs_sync_dir(const char *path) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) return;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}

int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    char buf[PLAT_PATH_MAX];
    dir = posix_path(dir, buf);
    if (!dir) return PLAT_FS_ERROR;
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "plat_fs_list: opendir(%s) failed: errno=%d (%s)\n", dir, errno, strerror(errno));
        return fs_err_from_errno(errno);
    }
    struct dirent *e;
    static int stat_err_logged = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[PLAT_PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full) continue;
        struct stat st;
        if (lstat(full, &st) != 0 && stat(full, &st) != 0) {
            if (!stat_err_logged) {
                stat_err_logged = 1;
                fprintf(stderr, "plat_fs_list: lstat/stat(%s) failed: errno=%d (%s)\n", full, errno, strerror(errno));
            }
            continue;
        }
        int kind = S_ISLNK(st.st_mode) ? PLAT_FS_LINK : (S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE);
        uint64_t size = kind == PLAT_FS_FILE ? (uint64_t)st.st_size : 0;
        if (!cb(e->d_name, kind, size, ctx)) break;
    }
    closedir(d);
    return PLAT_FS_OK;
}

// The kind from d_type, no lstat: the RomFS index's 205,660 lstat calls cost 29-31 s at every boot (~145 us each,
// a full path lookup per file). An entry the file system leaves DT_UNKNOWN is stat'ed as plat_fs_list does.
int plat_fs_list_names(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    char buf[PLAT_PATH_MAX];
    dir = posix_path(dir, buf);
    if (!dir) return PLAT_FS_ERROR;
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "plat_fs_list_names: opendir(%s) failed: errno=%d (%s)\n", dir, errno, strerror(errno));
        return fs_err_from_errno(errno);
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int kind;
        uint64_t size = PLAT_FS_SIZE_UNKNOWN;
        if (e->d_type == DT_DIR) kind = PLAT_FS_DIR;
        else if (e->d_type == DT_LNK) kind = PLAT_FS_LINK;
        else if (e->d_type == DT_REG) kind = PLAT_FS_FILE;
        else if (e->d_type != DT_UNKNOWN) continue;   // sockets, devices, pipes: never game files
        else {
            char full[PLAT_PATH_MAX];
            struct stat st;
            if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full || lstat(full, &st) != 0)
                continue;
            kind = S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
            size = kind == PLAT_FS_FILE ? (uint64_t)st.st_size : 0;
        }
        if (kind != PLAT_FS_FILE) size = 0;
        if (!cb(e->d_name, kind, size, ctx)) break;
    }
    closedir(d);
    return PLAT_FS_OK;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) { if (err) *err = PLAT_FS_ERROR; return NULL; }
    int fd = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) { if (err) *err = fs_err_from_errno(errno); return NULL; }
    PlatFile *f = malloc(sizeof *f);
    if (!f) { close(fd); if (err) *err = PLAT_FS_ERROR; return NULL; }
    f->fd = fd;
    if (err) *err = PLAT_FS_OK;
    return f;
}

PlatFile *plat_fs_create(const char *path, int *err) {
    char buf[PLAT_PATH_MAX];
    path = posix_path(path, buf);
    if (!path) { if (err) *err = PLAT_FS_ERROR; return NULL; }
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) { if (err) *err = fs_err_from_errno(errno); return NULL; }
    PlatFile *f = malloc(sizeof *f);
    if (!f) { close(fd); if (err) *err = PLAT_FS_ERROR; return NULL; }
    f->fd = fd;
    if (err) *err = PLAT_FS_OK;
    return f;
}

// The kernel reads the buffer, which can be guest memory (a save's write, fs_host.c write_file): a unit with no backing
// (never touched, or packed by MKW_PACK, plat_mem_pack) answers EFAULT instead of faulting, so its units are backed
// first, as plat_file_pread does, and backed again on an EFAULT.
int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    if (!f) return -1;
    if (!plat_mem_back((uintptr_t)buf, n)) return -1;
    const uint8_t *p = buf;
    size_t done = 0;
    int efault_retries = 0;
    while (done < n) {
        ssize_t k = pwrite(f->fd, p + done, n - done, (off_t)(off + done));
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && errno == EFAULT && efault_retries++ < 3 && plat_mem_back((uintptr_t)(p + done), n - done))
            continue;
        if (k < 0) return -1;
        done += (size_t)k;
    }
    return (int64_t)done;
}

int plat_file_set_size(PlatFile *f, uint64_t size) {
    if (!f) return PLAT_FS_ERROR;
    return ftruncate(f->fd, (off_t)size) == 0 ? PLAT_FS_OK : fs_err_from_errno(errno);
}

int plat_file_fsync(PlatFile *f) { return f && fsync(f->fd) == 0; }

int plat_temp_dir(char *out, size_t cap) {
    if (!out || !cap) return 0;
    mkdir("/data/mkw_tmp", 0755);
    return snprintf(out, cap, "/data/mkw_tmp") < (int)cap;
}

// Discord's IPC (core/presence): no Discord client on the console, so nobody listens.
PlatIpc *plat_ipc_open(const char *name) { (void)name; return NULL; }
int plat_ipc_send(PlatIpc *c, const void *data, size_t n) { (void)c, (void)data, (void)n; return 0; }
long plat_ipc_recv(PlatIpc *c, void *buf, size_t cap) { (void)c, (void)buf, (void)cap; return -1; }
void plat_ipc_close(PlatIpc *c) { (void)c; }

// Windows' lazy commit (core/memory/memory.c): the PS5 backs guest memory its own way (ps5_guest_memory.c).
void plat_mem_lazy_hook(int (*fn)(uint64_t addr)) { (void)fn; }

int plat_fs_os_error(void) { return errno; }   // the PS5's plat_fs_* calls set errno as Linux's do

// Only Windows refuses to rename a directory with a file open under it: nothing to say elsewhere.
void plat_fs_busy_reason(const char *dir, char *out, size_t cap) {
    (void)dir;
    if (cap) out[0] = 0;
}

// ---- the platform calls upstream's 22/11/2026 merge brought (memory census, spill, shader pack, VRAM and heap lines)

// Last write time in ns (as linux_fs.c): spirv/shaders.pack against its folder (gfx_shader_pack.c).
uint64_t plat_path_write_time(const char *path) {
    char buf[PLAT_PATH_MAX];
    const char *p = posix_path(path, buf);
    struct stat st;
    return !p || stat(p, &st) ? 0 : (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec;
}

// MKW_MEM_SPILL (core/memory/memory_spill.c) is a Linux feature: the console's memory is direct memory, given back by
// ps5_guest_memory.c's own decommit and MKW_PACK, not by dropping pages to a file. Without a scratch file and a drop,
// the spill stays off.
PlatFile *plat_file_scratch(const char *dir) { (void)dir; return NULL; }
int plat_mem_drop(uint64_t addr, uint64_t n) { (void)addr, (void)n; return 0; }

// No /proc/self/smaps or mincore here: the census labels are a no-op and residency is unknown (-1).
void plat_mem_name(const void *p, size_t n, const char *name) { (void)p, (void)n, (void)name; }
int64_t plat_mem_resident(const void *p, size_t n) { (void)p, (void)n; return -1; }

// Windows-only figures: CET shadow stacks, the OS's per-process VRAM count.
int plat_shadow_stack(void) { return -1; }
uint64_t plat_gpu_process_vram_mib(void) { return 0; }

// The C heap is the SDK's title heap (ps5platform/heap.h: malloc and its family wrapped to dlmalloc mspaces in DIRECT
// memory, the pool the guest and the GPU share), so what it maps counts against the same 12 GiB as everything else.
// The SDK reports the direct memory it has mapped, not the bytes in use: in_use and free stay 0.
void plat_heap_stats(uint64_t *in_use, uint64_t *free_bytes, uint64_t *mapped) {
    struct ps5_heap_stats st = {0};
    ps5_heap_stats(&st);
    if (in_use) *in_use = 0;
    if (free_bytes) *free_bytes = 0;
    if (mapped) *mapped = st.mapped_bytes;
}

// The title heap gives a free top or an empty segment back only past a threshold and every few thousand frees; the
// SDK's newer platform layer has ps5_heap_trim and its header asks a title to call it "every 10 s, say" so that what a
// burst took goes back to the pool it shares with its GPU and its guest (mihawk-99/PS5_PayloadSDK 55a2de04, heap.h).
// Ported from extoverflow/mkw-recomp ac72ec5 (MIT), where the title's memory thread calls it; here the memory
// governor's thread does (vk_headroom.c). Weak: the SDK this tree builds with (611893fc) has no ps5_heap_trim, and then
// the trim does nothing and says so once (plat_heap_trim_available).
__attribute__((weak)) void ps5_heap_trim(void);
void plat_heap_trim(void) {
    if (ps5_heap_trim) ps5_heap_trim();
}
int plat_heap_trim_available(void) { return ps5_heap_trim != NULL; }
