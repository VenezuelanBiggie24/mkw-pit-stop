// Placeholder TCP file receiver (PIT1 protocol) for Mario Kart World: Pit Stop (PPSA99193).
// Runs only while the placeholder / "game files missing" screen is displayed; never started once the game runs.
#define _GNU_SOURCE
#include "ps5_recv.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"

#define MANIFEST_NAME ".pitstop-manifest.tsv"
#define PART_SUFFIX ".pitstop.part"
#define FTP_SUFFIX ".pitstop-ftp"   // PIT1A: the name the builder uploads a file under by FTP
#define MAGIC_STR "PIT1"
#define MAX_BATCH 16384u
#define IO_BUF_SIZE (256u * 1024u)
// File data is received into PIPE_SLOTS buffers of PIPE_SLOT_SIZE while a writer thread hashes and writes the ones
// already full: received, hashed and written one after another, the socket sat idle for every write() to the console's
// disk and the sender stalled on a full window. 4 x 1 MiB keeps a write's worth of data in flight.
#define PIPE_SLOTS 4u
#define PIPE_SLOT_SIZE (1024u * 1024u)
// The kernel's receive buffer, set on the listening socket so the accepted one negotiates a window scale large enough
// for it: the default left the sender waiting on acknowledgements between bursts.
#define SOCK_BUF_SIZE (4 * 1024 * 1024)
#define HASH_CHECK_LIMIT (256ull << 20)

#ifdef __PROSPERO__
#define SCE_NET_CTL_INFO_IP_ADDRESS 14
int sceNetInit(void);
int sceNetCtlInit(void);
typedef union {
    char ip_address[16];
    uint8_t raw[256];
} SceNetCtlInfo;
int sceNetCtlGetInfo(int code, SceNetCtlInfo *info);
#endif

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Ps5RecvStatus g_status;
static uint64_t g_begin_ns;   // PIT1B's time: g_status.send_ns counts from it

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static pthread_t g_thread;
static int g_started;
static int g_listen_fd = -1;
static volatile int g_stop;
static char g_root[512];
static char g_port_ver[64];
static char g_game_id[64];
static int g_has_game_code;

// ---- manifest (in-memory table persisted to <root>/.pitstop-manifest.tsv on CMD_DONE)
typedef struct {
    char *path;
    uint64_t size;
    uint64_t mtime_ns;
    uint8_t sha256[32];
} ManifestEntry;

static ManifestEntry *g_man;
static size_t g_man_n;
static size_t g_man_cap;
// A hash index over the paths (open addressing, entry index + 1, 0 free): the RomFS is ~206,000 files, and a lookup
// that walked the whole table made each received file, each "have you got it" and the manifest's load cost up to
// 206,000 string compares, some 2x10^10 over one send (18/11/2026).
static size_t *g_idx;
static size_t g_idx_cap;   // a power of two, at least twice the entries

static void manifest_free(void) {
    for (size_t i = 0; i < g_man_n; i++) free(g_man[i].path);
    free(g_man);
    free(g_idx);
    g_man = NULL;
    g_idx = NULL;
    g_man_n = 0;
    g_man_cap = 0;
    g_idx_cap = 0;
}

static uint64_t path_hash(const char *s) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    while (*s) h = (h ^ (uint8_t)*s++) * 1099511628211ull;
    return h;
}

// The slot of rel: its entry's, or the free one where it would go.
static size_t idx_slot(const char *rel) {
    size_t mask = g_idx_cap - 1, i = (size_t)path_hash(rel) & mask;
    while (g_idx[i] && strcmp(g_man[g_idx[i] - 1].path, rel)) i = (i + 1) & mask;
    return i;
}

static int idx_rebuild(size_t want) {
    size_t cap = 1024;
    while (cap < want * 2) cap *= 2;
    size_t *n = calloc(cap, sizeof *n);
    if (!n) return 0;
    free(g_idx);
    g_idx = n;
    g_idx_cap = cap;
    for (size_t e = 0; e < g_man_n; e++) g_idx[idx_slot(g_man[e].path)] = e + 1;
    return 1;
}

static uint64_t stat_mtime_ns(const struct stat *st) {
#ifdef __PROSPERO__
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim.tv_nsec;
#else
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim.tv_nsec;
#endif
}

static int hex_decode32(const char *hex, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[i * 2 + j];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return 0;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}

static void manifest_set(const char *rel, uint64_t size, uint64_t mtime_ns, const uint8_t sha[32]) {
    if ((g_man_n + 1) * 2 > g_idx_cap && !idx_rebuild(g_man_n + 1)) return;
    size_t slot = idx_slot(rel);
    if (g_idx[slot]) {
        ManifestEntry *m = &g_man[g_idx[slot] - 1];
        m->size = size;
        m->mtime_ns = mtime_ns;
        memcpy(m->sha256, sha, 32);
        return;
    }
    if (g_man_n == g_man_cap) {
        size_t ncap = g_man_cap ? g_man_cap * 2 : 1024;
        ManifestEntry *p = realloc(g_man, ncap * sizeof *p);
        if (!p) return;
        g_man = p;
        g_man_cap = ncap;
    }
    char *copy = strdup(rel);
    if (!copy) return;
    g_man[g_man_n++] = (ManifestEntry){copy, size, mtime_ns, {0}};
    memcpy(g_man[g_man_n - 1].sha256, sha, 32);
    g_idx[slot] = g_man_n;
}

static const ManifestEntry *manifest_find(const char *rel) {
    if (!g_idx_cap) return NULL;
    size_t slot = idx_slot(rel);
    return g_idx[slot] ? &g_man[g_idx[slot] - 1] : NULL;
}

// Drops rel and everything under it (a removed folder) from the table.
static void manifest_remove_tree(const char *rel) {
    size_t len = strlen(rel), out = 0;
    for (size_t i = 0; i < g_man_n; i++) {
        const char *p = g_man[i].path;
        if (!strncmp(p, rel, len) && (p[len] == 0 || p[len] == '/')) free(g_man[i].path);
        else g_man[out++] = g_man[i];
    }
    g_man_n = out;
    idx_rebuild(g_man_n);
}

static void manifest_load(const char *root) {
    manifest_free();
    char path[768];
    if (snprintf(path, sizeof path, "%s/%s", root, MANIFEST_NAME) >= (int)sizeof path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        char *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
        if (!t1 || !t2 || !t3) continue;
        *t1 = *t2 = *t3 = 0;
        if (!ps5_recv_validate_path(line) || strlen(t3 + 1) != 64) continue;
        uint64_t sz = strtoull(t1 + 1, NULL, 10);
        uint64_t mt = strtoull(t2 + 1, NULL, 10);
        uint8_t sha[32];
        if (hex_decode32(t3 + 1, sha)) manifest_set(line, sz, mt, sha);
    }
    fclose(f);
}

static void manifest_save(const char *root) {
    char tmp[768], dst[768];
    if (snprintf(tmp, sizeof tmp, "%s/%s%s", root, MANIFEST_NAME, PART_SUFFIX) >= (int)sizeof tmp) return;
    if (snprintf(dst, sizeof dst, "%s/%s", root, MANIFEST_NAME) >= (int)sizeof dst) return;
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    for (size_t i = 0; i < g_man_n; i++) {
        char hex[65];
        for (int j = 0; j < 32; j++) sprintf(hex + j * 2, "%02x", g_man[i].sha256[j]);
        hex[64] = 0;
        fprintf(f, "%s\t%llu\t%llu\t%s\n", g_man[i].path, (unsigned long long)g_man[i].size,
                (unsigned long long)g_man[i].mtime_ns, hex);
    }
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, dst) != 0) unlink(tmp);
}

// ---- path validation & safe file operations
int ps5_recv_validate_path(const char *rel) {
    if (!rel || !*rel) return 0;
    size_t len = strlen(rel);
    if (len >= 512 || rel[0] == '/' || rel[len - 1] == '/') return 0;
    if (!strcmp(rel, MANIFEST_NAME)) return 0;
    size_t part_len = strlen(PART_SUFFIX);
    if (len >= part_len && !strcmp(rel + len - part_len, PART_SUFFIX)) return 0;
    const char *seg_start = rel;
    for (size_t i = 0; i <= len; i++) {
        unsigned char ch = (unsigned char)rel[i];
        if (ch != 0 && (ch < 0x20 || ch == 0x7f || ch == '\\')) return 0;
        if (ch == '/' || ch == 0) {
            size_t slen = (size_t)(rel + i - seg_start);
            if (slen == 0) return 0;
            if (slen == 1 && seg_start[0] == '.') return 0;
            if (slen == 2 && seg_start[0] == '.' && seg_start[1] == '.') return 0;
            seg_start = rel + i + 1;
        }
    }
    return 1;
}

// lstat, else stat: inside the title lstat fails where stat answers (ps5_sys.c does the same). With lstat alone the
// manifest stayed empty on the console (each received file is recorded after an lstat: .pitstop-manifest.tsv was 0
// bytes, 23/11/2026), so every send sent everything again, and PIT1A found no upload. Symbolic links are refused by
// O_NOFOLLOW where files are opened; the S_ISLNK checks keep lstat.
static int path_stat(const char *path, struct stat *st) {
    return lstat(path, st) == 0 || stat(path, st) == 0 ? 0 : -1;
}

static int ensure_parent_dirs(const char *root, const char *rel) {
    char buf[1280];
    if (snprintf(buf, sizeof buf, "%s/%s", root, rel) >= (int)sizeof buf) return 0;
    size_t root_len = strlen(root);
    for (char *p = buf + root_len + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        struct stat st;
        if (path_stat(buf, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return 0;   // reject symlinks or regular files in directory position
        } else if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
            return 0;
        } else {
            chmod(buf, 0777);
        }
        *p = '/';
    }
    return 1;
}

static int sha256_file(const char *path, uint8_t out[32]) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    uint8_t *buf = malloc(IO_BUF_SIZE);
    if (!buf) {
        close(fd);
        return 0;
    }
    Sha256 ctx;
    sha256_init(&ctx);
    for (;;) {
        ssize_t k = read(fd, buf, IO_BUF_SIZE);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) {
            free(buf);
            close(fd);
            return 0;
        }
        if (k == 0) break;
        sha256_update(&ctx, buf, (size_t)k);
    }
    sha256_final(&ctx, out);
    free(buf);
    close(fd);
    return 1;
}

static void refresh_shadowmount_sce_sys(const char *root) {
    static const char *const files[] = {"param.json", "icon0.png"};
    const char *app_dir = "/user/app/PPSA99193/sce_sys";
    char probe[256];
    snprintf(probe, sizeof probe, "%s/param.json", app_dir);
    if (access(probe, W_OK) != 0) return;   // sandboxed without Lapy elevation: placeholder sce_sys already matches
    uint8_t *buf = malloc(IO_BUF_SIZE);
    if (!buf) return;
    for (int i = 0; i < 2; i++) {
        char src[768], dst[256], tmp[256];
        snprintf(src, sizeof src, "%s/sce_sys/%s", root, files[i]);
        snprintf(dst, sizeof dst, "%s/%s", app_dir, files[i]);
        snprintf(tmp, sizeof tmp, "%s/%s.tmp", app_dir, files[i]);
        int in = open(src, O_RDONLY | O_CLOEXEC);
        if (in < 0) continue;
        int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (out < 0) {
            close(in);
            continue;
        }
        int ok = 1;
        for (;;) {
            ssize_t k = read(in, buf, IO_BUF_SIZE);
            if (k < 0 && errno == EINTR) continue;
            if (k < 0) { ok = 0; break; }
            if (k == 0) break;
            ssize_t off = 0;
            while (off < k) {
                ssize_t w = write(out, buf + off, (size_t)(k - off));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { ok = 0; break; }
                off += w;
            }
            if (!ok) break;
        }
        ok = close(out) == 0 && ok;
        close(in);
        if (!ok || rename(tmp, dst) != 0) unlink(tmp);
    }
    free(buf);
}

// ---- socket helpers
static int read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t k = recv(fd, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static int write_exact(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static uint16_t get_u16le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64le(const uint8_t *p) {
    return (uint64_t)get_u32le(p) | ((uint64_t)get_u32le(p + 4) << 32);
}
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void set_error(const char *msg) {
    pthread_mutex_lock(&g_lock);
    g_status.state = PS5_RECV_ERROR;
    snprintf(g_status.error, sizeof g_status.error, "%s", msg ? msg : "transfer error");
    pthread_mutex_unlock(&g_lock);
}

static void send_err(int fd, const char *msg) {
    uint16_t mlen = (uint16_t)strlen(msg ? msg : "error");
    if (mlen > 250) mlen = 250;
    uint8_t hdr[3] = {'E', (uint8_t)mlen, (uint8_t)(mlen >> 8)};
    write_exact(fd, hdr, 3);
    if (mlen) write_exact(fd, msg, mlen);
    set_error(msg);
}

static int handle_info(int fd) {
    char why[128];
    int ready = ps5_game_files_ready(g_root, g_game_id, why, sizeof why);
    uint64_t free_bytes = 0;
    struct statvfs vfs;
    // Not a measurement, so not sent as one ("free_real":0): inside the title statvfs answers exactly 16.00 GiB free
    // for /app0 and for /data alike, on a console with 231 GiB free, and the builder refused a 22.5 GiB send
    // (18/11/2026) or deleted folders to make room (20/11/2026). A full disk still fails the write with its error.
    if (statvfs(strcmp(g_root, "/app0") ? g_root : "/data", &vfs) == 0)
        free_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    pthread_mutex_lock(&g_lock);
    Ps5RecvStatus t = g_status;
    pthread_mutex_unlock(&g_lock);
    char json[1024];
    // "timing": the current or last send's (Ps5RecvStatus send_ns...), which send_ps5.py asks for before PIT1D and
    // prints, so a player's log says where the time went
    int n = snprintf(json, sizeof json,
                     "{\"title_id\":\"PPSA99193\",\"port_version\":\"%s\",\"rt_game_id\":\"%s\","
                     "\"has_game_code\":%d,\"game_files_ready\":%d,\"free_bytes\":%llu,\"free_real\":0,\"status\":1,\"resume\":1,\"remove\":1,"
                     "\"ftp\":1,\"root\":\"%s\",\"timing\":{\"bytes\":%llu,\"send_ms\":%llu,\"net_ms\":%llu,\"slot_ms\":%llu,"
                     "\"write_ms\":%llu,\"hash_ms\":%llu,\"rcvbuf\":%d}}",
                     g_port_ver, g_game_id, g_has_game_code ? 1 : 0, ready ? 1 : 0, (unsigned long long)free_bytes,
                     g_root,
                     (unsigned long long)t.bytes_done, (unsigned long long)(t.send_ns / 1000000),
                     (unsigned long long)(t.net_ns / 1000000), (unsigned long long)(t.slot_ns / 1000000),
                     (unsigned long long)(t.write_ns / 1000000), (unsigned long long)(t.hash_ns / 1000000), t.rcvbuf);
    if (n < 0 || n >= (int)sizeof json) return 0;
    uint8_t hdr[4];
    put_u32le(hdr, (uint32_t)n);
    return write_exact(fd, hdr, 4) && write_exact(fd, json, (size_t)n);
}

// PIT1T: the PC builder's step for the screen (send_ps5.py StatusNotifier): u8 index, u8 count, u8 failed, u32
// progress, u32 total, u16 length and the step's name or the failure (UTF-8, at most 200 bytes). Only shown: the
// transfer's state is not touched. The builder sends it only to a receiver whose PIT1I reply has "status".
static int handle_status(int fd) {
    uint8_t b[13];
    if (!read_exact(fd, b, sizeof b)) return 0;
    uint16_t n = get_u16le(b + 11);
    if (n > 200) {
        send_err(fd, "status text too long");
        return 0;
    }
    char label[201];
    if (n && !read_exact(fd, label, n)) return 0;
    label[n] = 0;
    for (uint16_t i = 0; i < n; i++)
        if ((unsigned char)label[i] < 0x20) label[i] = ' ';   // one line on the screen
    pthread_mutex_lock(&g_lock);
    g_status.pc_index = b[0];
    g_status.pc_count = b[1];
    g_status.pc_failed = b[2] != 0;
    g_status.pc_progress = get_u32le(b + 3);
    g_status.pc_total = get_u32le(b + 7);
    memcpy(g_status.pc_label, label, (size_t)n + 1);
    if (g_status.state == PS5_RECV_ERROR) {   // a new build after a failed transfer: its error is old news
        g_status.state = PS5_RECV_WAITING;
        g_status.error[0] = 0;
    }
    pthread_mutex_unlock(&g_lock);
    uint8_t ok = 'K';
    return write_exact(fd, &ok, 1);
}

// A file, or a folder with everything in it, never following a symbolic link (lstat; a link itself is removed).
static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;   // never stat: a link to a folder must not be followed here
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    int ok = 1;
    struct dirent *e;
    char child[1400];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) { ok = 0; continue; }
        ok = remove_tree(child) && ok;
    }
    closedir(d);
    return rmdir(path) == 0 && ok;
}

// PIT1X: remove a path in the title folder (u16 length, the path): the loose romfs/ once romfs.nup has arrived in
// its place (23 GB the console would otherwise keep twice). Only inside the title folder (ps5_recv_validate_path),
// only for a sender whose PIT1I saw "remove".
static int handle_remove(int fd) {
    uint8_t lb[2];
    if (!read_exact(fd, lb, 2)) return 0;
    uint16_t plen = get_u16le(lb);
    if (plen == 0 || plen >= 512) {
        send_err(fd, "invalid path length");
        return 0;
    }
    char rel[512], full[1280];
    if (!read_exact(fd, rel, plen)) return 0;
    rel[plen] = 0;
    if (!ps5_recv_validate_path(rel) || !strcmp(rel, MANIFEST_NAME)) {
        send_err(fd, "rejected path outside title folder");
        return 0;
    }
    snprintf(full, sizeof full, "%s/%s", g_root, rel);
    if (!remove_tree(full)) {
        char msg[200];
        snprintf(msg, sizeof msg, "remove %s failed: %s", rel, strerror(errno));
        send_err(fd, msg);
        return 0;
    }
    manifest_remove_tree(rel);
    uint8_t ok = 'K';
    return write_exact(fd, &ok, 1);
}

static int handle_stat_batch(int fd) {
    uint8_t cnt_buf[4];
    if (!read_exact(fd, cnt_buf, 4)) return 0;
    uint32_t count = get_u32le(cnt_buf);
    if (count > MAX_BATCH) {
        send_err(fd, "stat batch too large");
        return 0;
    }
    uint8_t *ans = malloc(count ? count : 1);
    if (!ans) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t rec_hdr[2];
        if (!read_exact(fd, rec_hdr, 2)) { free(ans); return 0; }
        uint16_t plen = get_u16le(rec_hdr);
        if (plen == 0 || plen >= 512) {
            free(ans);
            send_err(fd, "invalid path length");
            return 0;
        }
        char rel[512];
        uint8_t meta[40];
        if (!read_exact(fd, rel, plen) || !read_exact(fd, meta, 40)) { free(ans); return 0; }
        rel[plen] = 0;
        if (!ps5_recv_validate_path(rel)) {
            free(ans);
            send_err(fd, "rejected path outside title folder");
            return 0;
        }
        uint64_t want_sz = get_u64le(meta);
        const uint8_t *want_sha = meta + 8;
        int have = 0;
        char full[1280];
        if (snprintf(full, sizeof full, "%s/%s", g_root, rel) < (int)sizeof full) {
            struct stat st;
            if (path_stat(full, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == want_sz) {
                uint64_t mt = stat_mtime_ns(&st);
                const ManifestEntry *me = manifest_find(rel);
                if (me && me->size == want_sz && me->mtime_ns == mt && !memcmp(me->sha256, want_sha, 32)) {
                    have = 1;
                } else if (want_sz <= HASH_CHECK_LIMIT) {
                    uint8_t got[32];
                    if (sha256_file(full, got) && !memcmp(got, want_sha, 32)) {
                        manifest_set(rel, want_sz, mt, got);
                        have = 1;
                    }
                }
            }
        }
        ans[i] = (uint8_t)have;
    }
    int ok = write_exact(fd, ans, count);
    free(ans);
    return ok;
}

static int handle_begin(int fd) {
    uint8_t buf[16];
    if (!read_exact(fd, buf, 16)) return 0;
    uint64_t files = get_u64le(buf);
    uint64_t bytes = get_u64le(buf + 8);
    pthread_mutex_lock(&g_lock);
    g_status.state = PS5_RECV_RECEIVING;
    g_status.files_done = 0;
    g_status.files_total = files;
    g_status.bytes_done = 0;
    g_status.bytes_total = bytes;
    g_status.current_file[0] = 0;
    g_status.error[0] = 0;
    g_status.send_ns = g_status.net_ns = g_status.slot_ns = g_status.write_ns = g_status.hash_ns = 0;
    int rcvbuf = 0;
    socklen_t rlen = sizeof rcvbuf;
    g_status.rcvbuf = getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &rlen) == 0 ? rcvbuf : -1;
    g_begin_ns = now_ns();
    pthread_mutex_unlock(&g_lock);
    uint8_t ok = 'K';
    return write_exact(fd, &ok, 1);
}

// ---- file data pipeline: the connection's thread receives, the writer thread hashes and writes (see PIPE_SLOTS)
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    pthread_t thread;
    uint8_t *buf[PIPE_SLOTS];
    size_t len[PIPE_SLOTS];
    unsigned head, tail;   // filled up to head, written up to tail (both count up; slot = n % PIPE_SLOTS)
    int out_fd;
    Sha256 *sha;
    int err;               // a write failed: the rest of the file is not written
    int quit;
} Pipe;

static void *pipe_writer(void *arg) {
    Pipe *p = arg;
    pthread_mutex_lock(&p->m);
    for (;;) {
        while (p->tail == p->head && !p->quit) pthread_cond_wait(&p->c, &p->m);
        if (p->tail == p->head) break;   // quit with nothing left
        unsigned slot = p->tail % PIPE_SLOTS;
        int fd = p->out_fd, failed = p->err;
        Sha256 *sha = p->sha;
        pthread_mutex_unlock(&p->m);
        const uint8_t *b = p->buf[slot];
        size_t n = p->len[slot], off = 0;
        uint64_t t0 = now_ns();
        sha256_update(sha, b, n);
        uint64_t t1 = now_ns();
        while (!failed && off < n) {
            ssize_t w = write(fd, b + off, n - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) failed = 1;
            else off += (size_t)w;
        }
        uint64_t t2 = now_ns();
        pthread_mutex_lock(&g_lock);
        g_status.hash_ns += t1 - t0;
        g_status.write_ns += t2 - t1;
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_lock(&p->m);
        if (failed) p->err = 1;
        p->tail++;
        pthread_cond_broadcast(&p->c);
    }
    pthread_mutex_unlock(&p->m);
    return NULL;
}

static int pipe_open(Pipe *p) {
    memset(p, 0, sizeof *p);
    for (unsigned i = 0; i < PIPE_SLOTS; i++) {
        p->buf[i] = malloc(PIPE_SLOT_SIZE);
        if (!p->buf[i]) {
            while (i--) free(p->buf[i]);
            return 0;
        }
    }
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->c, NULL);
    if (pthread_create(&p->thread, NULL, pipe_writer, p) != 0) {
        for (unsigned i = 0; i < PIPE_SLOTS; i++) free(p->buf[i]);
        return 0;
    }
    return 1;
}

static void pipe_close(Pipe *p) {
    pthread_mutex_lock(&p->m);
    p->quit = 1;
    pthread_cond_broadcast(&p->c);
    pthread_mutex_unlock(&p->m);
    pthread_join(p->thread, NULL);
    for (unsigned i = 0; i < PIPE_SLOTS; i++) free(p->buf[i]);
}

// A free slot to receive into (waits for the writer while all are full).
static uint8_t *pipe_slot(Pipe *p) {
    pthread_mutex_lock(&p->m);
    while (p->head - p->tail >= PIPE_SLOTS) pthread_cond_wait(&p->c, &p->m);
    uint8_t *b = p->buf[p->head % PIPE_SLOTS];
    pthread_mutex_unlock(&p->m);
    return b;
}

static void pipe_push(Pipe *p, size_t n) {
    pthread_mutex_lock(&p->m);
    p->len[p->head % PIPE_SLOTS] = n;
    p->head++;
    pthread_cond_broadcast(&p->c);
    pthread_mutex_unlock(&p->m);
}

// Waits until every pushed slot is written; returns the write error flag.
static int pipe_drain(Pipe *p) {
    pthread_mutex_lock(&p->m);
    while (p->tail != p->head) pthread_cond_wait(&p->c, &p->m);
    int err = p->err;
    pthread_mutex_unlock(&p->m);
    return err;
}

static int pipe_failed(Pipe *p) {
    pthread_mutex_lock(&p->m);
    int err = p->err;
    pthread_mutex_unlock(&p->m);
    return err;
}

static int handle_put(int fd, Pipe *pipe) {
    uint8_t *io_buf = pipe->buf[0];   // the writer is idle between files: slot 0 serves the resume check
    uint8_t hdr[3];
    if (!read_exact(fd, hdr, 3)) return 0;
    uint8_t flags = hdr[0];
    uint16_t plen = get_u16le(hdr + 1);
    if (plen == 0 || plen >= 512) {
        send_err(fd, "invalid path length");
        return 0;
    }
    char rel[512];
    uint8_t meta[40];
    if (!read_exact(fd, rel, plen) || !read_exact(fd, meta, 40)) return 0;
    rel[plen] = 0;
    if (!ps5_recv_validate_path(rel)) {
        send_err(fd, "rejected path outside title folder");
        return 0;
    }
    uint64_t fsize = get_u64le(meta);
    const uint8_t *want_sha = meta + 8;

    pthread_mutex_lock(&g_lock);
    g_status.state = PS5_RECV_RECEIVING;
    snprintf(g_status.current_file, sizeof g_status.current_file, "%s", rel);
    pthread_mutex_unlock(&g_lock);

    if (!ensure_parent_dirs(g_root, rel)) {
        send_err(fd, "could not create target directory");
        return 0;
    }
    char final_path[1280], part_path[1320];
    if (snprintf(final_path, sizeof final_path, "%s/%s", g_root, rel) >= (int)sizeof final_path ||
        snprintf(part_path, sizeof part_path, "%s%s", final_path, PART_SUFFIX) >= (int)sizeof part_path) {
        send_err(fd, "path too long");
        return 0;
    }
    struct stat existing;
    if (lstat(final_path, &existing) == 0 && S_ISLNK(existing.st_mode)) {
        send_err(fd, "refusing to write through symlink");
        return 0;
    }
    // flags & 2 (a sender whose PIT1I saw "resume"): a large file whose transfer broke goes on where its .part ends.
    // The .part's .want names the file it belongs to (size and SHA-256); its bytes are hashed again here, and the
    // offset to go on from is answered before the data. romfs.nup is 23 GB in one file: a dropped connection would
    // otherwise start it over.
    Sha256 sha;
    sha256_init(&sha);
    char want_path[1340];
    snprintf(want_path, sizeof want_path, "%s.want", part_path);
    uint64_t resume_at = 0;
    int resumable = (flags & 2) != 0;
    if (resumable) {
        char want_line[100] = "", expect[100];
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", want_sha[i]);
        snprintf(expect, sizeof expect, "%llu %s", (unsigned long long)fsize, hex);
        FILE *wf = fopen(want_path, "r");
        if (wf) {
            if (!fgets(want_line, sizeof want_line, wf)) want_line[0] = 0;
            fclose(wf);
            want_line[strcspn(want_line, "\r\n")] = 0;
        }
        struct stat ps;
        if (!strcmp(want_line, expect) && path_stat(part_path, &ps) == 0 && S_ISREG(ps.st_mode) &&
            (uint64_t)ps.st_size <= fsize) {
            int in_fd = open(part_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            uint64_t have = 0;
            while (in_fd >= 0 && have < (uint64_t)ps.st_size) {
                size_t want = (uint64_t)ps.st_size - have > IO_BUF_SIZE ? IO_BUF_SIZE : (size_t)((uint64_t)ps.st_size - have);
                ssize_t k = read(in_fd, io_buf, want);
                if (k <= 0) break;
                sha256_update(&sha, io_buf, (size_t)k);
                have += (uint64_t)k;
            }
            if (in_fd >= 0) close(in_fd);
            if (have == (uint64_t)ps.st_size) resume_at = have;
            else sha256_init(&sha);
        }
        if (!resume_at) {
            FILE *nf = fopen(want_path, "w");
            if (nf) fprintf(nf, "%s\n", expect), fclose(nf);
        }
        uint8_t off_le[8];
        for (int i = 0; i < 8; i++) off_le[i] = (uint8_t)(resume_at >> (8 * i));
        if (!write_exact(fd, off_le, 8)) return 0;
    }
    int out_fd = open(part_path, O_WRONLY | O_CREAT | (resume_at ? 0 : O_TRUNC) | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (out_fd >= 0 && resume_at && lseek(out_fd, (off_t)resume_at, SEEK_SET) != (off_t)resume_at) {
        close(out_fd);
        out_fd = -1;
    }
    if (out_fd < 0) {
        char msg[200];
        snprintf(msg, sizeof msg, "open %s failed: %s", rel, strerror(errno));
        send_err(fd, msg);
        return 0;
    }
    // As an FTP upload leaves them. With 0644 (no execute bit) the title stopped starting after a send, with
    // PRX_SCE_MODULE_LOAD_ERROR: its sce_module/libc.prx was byte for byte the placeholder's, only the mode and owner
    // differed (18/11/2026).
    fchmod(out_fd, 0777);
    if (resume_at) {
        pthread_mutex_lock(&g_lock);
        g_status.bytes_done += resume_at;
        pthread_mutex_unlock(&g_lock);
    }

    uint64_t rem = fsize - resume_at;
    int io_err = 0;
    pthread_mutex_lock(&pipe->m);
    pipe->out_fd = out_fd;
    pipe->sha = &sha;
    pipe->err = 0;
    pthread_mutex_unlock(&pipe->m);
    while (rem > 0) {
        if (pipe_failed(pipe)) {
            io_err = 2;
            break;
        }
        size_t chunk = rem > PIPE_SLOT_SIZE ? PIPE_SLOT_SIZE : (size_t)rem;
        uint64_t t0 = now_ns();
        uint8_t *b = pipe_slot(pipe);
        uint64_t t1 = now_ns();
        if (!read_exact(fd, b, chunk)) {
            io_err = 1;
            break;
        }
        uint64_t t2 = now_ns();
        pipe_push(pipe, chunk);
        rem -= chunk;
        pthread_mutex_lock(&g_lock);
        g_status.bytes_done += chunk;
        g_status.slot_ns += t1 - t0;
        g_status.net_ns += t2 - t1;
        g_status.send_ns = t2 - g_begin_ns;
        pthread_mutex_unlock(&g_lock);
    }
    // the writer finishes what was received before the file is closed (and, on a broken connection, the .part keeps
    // every byte that came: the resume hashes it again)
    uint64_t td = now_ns();
    int drain_err = pipe_drain(pipe);
    uint64_t te = now_ns();
    pthread_mutex_lock(&g_lock);
    g_status.slot_ns += te - td;   // waiting on the writer, as a full pipe is
    g_status.send_ns = te - g_begin_ns;
    pthread_mutex_unlock(&g_lock);
    if (drain_err && io_err != 1) io_err = 2;
    if (!io_err && !strcmp(rel, "eboot.bin") && fsync(out_fd) != 0) io_err = 2;
    if (close(out_fd) != 0 && !io_err) io_err = 2;
    if (io_err) {
        if (io_err == 1 && resumable) {   // the connection broke: keep the part for the next try
            set_error("connection lost during file transfer (the next send goes on from here)");
            return 0;
        }
        unlink(part_path);
        unlink(want_path);
        if (io_err == 2) send_err(fd, "write failed (disk full or permission denied)");
        else set_error("connection lost during file transfer");
        return 0;
    }
    uint8_t got_sha[32];
    sha256_final(&sha, got_sha);
    if (memcmp(got_sha, want_sha, 32) != 0) {
        unlink(part_path);
        unlink(want_path);
        send_err(fd, "SHA-256 mismatch");
        return 0;
    }
    unlink(want_path);
    if (rename(part_path, final_path) != 0) {
        char msg[200];
        snprintf(msg, sizeof msg, "replace %s failed: %s", rel, strerror(errno));
        unlink(part_path);
        send_err(fd, msg);
        return 0;
    }
    struct stat st;
    if (path_stat(final_path, &st) == 0) manifest_set(rel, fsize, stat_mtime_ns(&st), got_sha);

    pthread_mutex_lock(&g_lock);
    g_status.files_done++;
    pthread_mutex_unlock(&g_lock);

    if (flags & 1) {
        uint8_t ok = 'K';
        if (!write_exact(fd, &ok, 1)) return 0;
    }
    return 1;
}

static int adopt_reply(int fd, char tag, const char *msg) {
    uint16_t n = (uint16_t)(msg ? strlen(msg) : 0);
    if (n > 250) n = 250;
    uint8_t hdr[3] = {(uint8_t)tag, (uint8_t)n, (uint8_t)(n >> 8)};
    return write_exact(fd, hdr, tag == 'N' ? 3 : 1) && (tag != 'N' || !n || write_exact(fd, msg, n));
}

// Hashes what the open upload has beyond *done, at most `cap` bytes (into buf, IO_BUF_SIZE). Returns the bytes read,
// 0 when caught up, -1 on a read error.
static int64_t adopt_hash_more(int in_fd, Sha256 *sha, uint64_t *done, uint64_t cap, uint8_t *buf) {
    struct stat st;
    if (fstat(in_fd, &st) != 0) return -1;
    uint64_t have = (uint64_t)st.st_size, got = 0;
    while (*done < have && got < cap) {
        size_t want = have - *done > IO_BUF_SIZE ? IO_BUF_SIZE : (size_t)(have - *done);
        ssize_t k = pread(in_fd, buf, want, (off_t)*done);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return k < 0 ? -1 : (int64_t)got;
        sha256_update(sha, buf, (size_t)k);
        *done += (uint64_t)k;
        got += (uint64_t)k;
    }
    return (int64_t)got;
}

// The folder FTP wrote into, as the title sees it: a file another process (the FTP server) creates in the title folder
// never appears under /app0 while the title runs (23/11/2026: the probe uploaded to /data/homebrew/PPSA99193 was not
// there for /app0 through a whole upload), though what the title writes through /app0 is there at once for FTP. So
// PIT1A works in that folder by its own path, once it proves to be the title folder: its eboot.bin has the same size
// and modification time as /app0's. Returns NULL, or why not (into why).
static const char *same_folder(const char *dir, char *why, size_t cap) {
    char pa[600], pb[600];
    struct stat sa, sb, ds;
    if (dir[0] != '/' || strstr(dir, "/..") || strstr(dir, "//")) {
        snprintf(why, cap, "%s: not a plain absolute path", dir);
        return why;
    }
    if (stat(dir, &ds) != 0 || !S_ISDIR(ds.st_mode)) {
        snprintf(why, cap, "%s: no such folder for the title (%s)", dir, strerror(errno));
        return why;
    }
    snprintf(pa, sizeof pa, "%s/eboot.bin", g_root);
    snprintf(pb, sizeof pb, "%s/eboot.bin", dir);
    if (stat(pa, &sa) != 0) {
        snprintf(why, cap, "%s: %s", pa, strerror(errno));
        return why;
    }
    if (stat(pb, &sb) != 0) {
        snprintf(why, cap, "%s: %s", pb, strerror(errno));
        return why;
    }
    if (sa.st_size != sb.st_size || stat_mtime_ns(&sa) != stat_mtime_ns(&sb)) {
        snprintf(why, cap, "%s is another folder: eboot.bin %lld bytes, time %llu there, %lld and %llu in %s", dir,
                 (long long)sb.st_size, (unsigned long long)stat_mtime_ns(&sb), (long long)sa.st_size,
                 (unsigned long long)stat_mtime_ns(&sa), g_root);
        return why;
    }
    return NULL;
}

// PIT1A: a large file the builder puts on the console by FTP instead of PIT1P. The title's own writes run at ~20 MiB/s
// for about 2 GB, then at ~2 MiB/s for the rest of the process's life (measured 23/11/2026: 3 GiB in 554 s, in /app0
// and in /data alike, one file or two, with or without fsync; the FTP payload, another process, held 20.3 MiB/s), and
// romfs.nup is 23 GB: some three hours. Here the title only reads.
// u8 flags (1: a probe, removed once checked), u16 length, the path, u64 size, SHA-256, u16 length and the title
// folder as FTP wrote into it (same_folder). Answers 'K' (or 'N' as below: not the title folder), then the builder
// appends to <path>.pitstop-ftp, made here empty, there by FTP while this follows the upload (the screen's progress) and hashes what
// has arrived; then the builder sends 'U' (uploaded) or 'C' (the upload failed: it sends the file by PIT1P instead).
// After 'U': 'p' bytes while the rest is hashed, then 'K' (checked, renamed into place, in the manifest) or 'N', u16
// length, why (nothing changed; the connection goes on, and no error screen: the builder falls back to PIT1P).
static int handle_adopt(int fd, Pipe *pipe) {
    uint8_t *buf = pipe->buf[0];   // the writer is idle between files
    uint8_t hdr[3];
    if (!read_exact(fd, hdr, 3)) return 0;
    uint8_t flags = hdr[0];
    uint16_t plen = get_u16le(hdr + 1);
    if (plen == 0 || plen >= 512) {
        send_err(fd, "invalid path length");
        return 0;
    }
    char rel[512];
    uint8_t meta[40];
    if (!read_exact(fd, rel, plen) || !read_exact(fd, meta, 40)) return 0;
    rel[plen] = 0;
    if (!ps5_recv_validate_path(rel)) {
        send_err(fd, "rejected path outside title folder");
        return 0;
    }
    uint8_t dl[2];
    char dir[512];
    if (!read_exact(fd, dl, 2)) return 0;
    uint16_t dlen = get_u16le(dl);
    if (dlen == 0 || dlen >= sizeof dir) {
        send_err(fd, "invalid folder length");
        return 0;
    }
    if (!read_exact(fd, dir, dlen)) return 0;
    dir[dlen] = 0;
    while (dlen > 1 && dir[dlen - 1] == '/') dir[--dlen] = 0;
    const int probe = flags & 1;
    const uint64_t fsize = get_u64le(meta);
    const uint8_t *want_sha = meta + 8;
    char final_path[1280], staged[1320], msg[300];
    if (same_folder(dir, msg, sizeof msg)) return adopt_reply(fd, 'N', msg);
    if (snprintf(final_path, sizeof final_path, "%s/%s", dir, rel) >= (int)sizeof final_path ||
        snprintf(staged, sizeof staged, "%s%s", final_path, FTP_SUFFIX) >= (int)sizeof staged) {
        send_err(fd, "path too long");
        return 0;
    }
    struct stat existing;
    if (lstat(final_path, &existing) == 0 && S_ISLNK(existing.st_mode)) {
        send_err(fd, "refusing to write through symlink");
        return 0;
    }
    if (!ensure_parent_dirs(dir, rel)) {   // so the FTP upload finds its folder
        send_err(fd, "could not create target directory");
        return 0;
    }
    // The file is the title's own, made here, empty, and the builder appends to it by FTP (APPE): a file the FTP server
    // creates stays out of the title's sight, even by the folder's own path (23/11/2026), and it may be another
    // process's file in the same folder that the title cannot see change. 0777: the FTP server writes it.
    int in_fd = open(staged, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0777);
    if (in_fd < 0) {
        snprintf(msg, sizeof msg, "%s: making %s failed: %s", rel, staged, strerror(errno));
        return adopt_reply(fd, 'N', msg);
    }
    fchmod(in_fd, 0777);
    struct stat in_st;
    fstat(in_fd, &in_st);
    pthread_mutex_lock(&g_lock);
    const uint64_t base = g_status.bytes_done;
    if (!probe) {
        g_status.state = PS5_RECV_RECEIVING;
        snprintf(g_status.current_file, sizeof g_status.current_file, "%s (by FTP)", rel);
    }
    pthread_mutex_unlock(&g_lock);
    if (!adopt_reply(fd, 'K', NULL)) return 0;

    Sha256 sha;
    sha256_init(&sha);
    uint64_t hashed = 0;
    char end = 0;
    while (!end) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int64_t got = 0;
        if (in_fd < 0 && (in_fd = open(staged, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) >= 0) fstat(in_fd, &in_st);
        if (in_fd >= 0) got = adopt_hash_more(in_fd, &sha, &hashed, 64ull << 20, buf);
        if (got < 0) {   // tried again from where it stopped; on 'U' a failing read is the answer
            close(in_fd);
            in_fd = -1;
            got = 0;
        }
        if (!probe && in_fd >= 0) {
            struct stat now;
            uint64_t seen = fstat(in_fd, &now) == 0 ? (uint64_t)now.st_size : hashed;
            pthread_mutex_lock(&g_lock);
            g_status.bytes_done = base + (seen < fsize ? seen : fsize);
            g_status.send_ns = now_ns() - g_begin_ns;
            pthread_mutex_unlock(&g_lock);
        }
        int r = poll(&pfd, 1, got > 0 ? 0 : 250);
        if (r < 0 && errno != EINTR) {
            if (in_fd >= 0) close(in_fd);
            return 0;
        }
        if (r > 0) {
            ssize_t k = recv(fd, &end, 1, 0);
            if (k <= 0) {
                if (in_fd >= 0) close(in_fd);
                return 0;   // the builder is gone: its .pitstop-ftp stays for it to remove
            }
            if (end != 'U' && end != 'C') {
                if (in_fd >= 0) close(in_fd);
                send_err(fd, "bad FTP upload notice");
                return 0;
            }
        }
    }
    const char *why = NULL;
    if (end == 'C') {
        why = "cancelled";
    } else {
        struct stat st;
        if (path_stat(staged, &st) != 0 || !S_ISREG(st.st_mode)) {
            snprintf(msg, sizeof msg, "%s: no FTP upload at %s (%s)", rel, staged, strerror(errno));
            why = msg;
        } else if ((uint64_t)st.st_size != fsize) {
            snprintf(msg, sizeof msg, "%s: the FTP upload has %llu bytes, not %llu", rel,
                     (unsigned long long)st.st_size, (unsigned long long)fsize);
            why = msg;
        } else {
            if (in_fd < 0 || st.st_ino != in_st.st_ino || st.st_dev != in_st.st_dev) {   // replaced: from the start
                if (in_fd >= 0) close(in_fd);
                in_fd = open(staged, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                sha256_init(&sha);
                hashed = 0;
            }
            uint64_t last = now_ns();
            int64_t got = 1;
            while (in_fd >= 0 && hashed < fsize && (got = adopt_hash_more(in_fd, &sha, &hashed, 16ull << 20, buf)) > 0) {
                if (now_ns() - last >= 1000000000ull) {   // the builder waits on a 30 s timeout
                    last = now_ns();
                    if (!write_exact(fd, "p", 1)) {
                        close(in_fd);
                        return 0;
                    }
                }
            }
            uint8_t got_sha[32];
            sha256_final(&sha, got_sha);
            if (in_fd < 0 || hashed != fsize) {
                snprintf(msg, sizeof msg, "%s: reading the FTP upload failed: %s", rel, strerror(errno));
                why = msg;
            } else if (memcmp(got_sha, want_sha, 32) != 0) {
                snprintf(msg, sizeof msg, "%s: the FTP upload's SHA-256 differs", rel);
                why = msg;
            } else if (rename(staged, final_path) != 0) {
                snprintf(msg, sizeof msg, "%s: putting the FTP upload in place failed: %s", rel, strerror(errno));
                why = msg;
            } else if (probe) {
                unlink(final_path);
            } else {
                if (path_stat(final_path, &st) == 0) manifest_set(rel, fsize, stat_mtime_ns(&st), got_sha);
                pthread_mutex_lock(&g_lock);
                g_status.files_done++;
                g_status.bytes_done = base + fsize;
                pthread_mutex_unlock(&g_lock);
            }
        }
    }
    if (in_fd >= 0) close(in_fd);
    if (why) {
        unlink(staged);
        pthread_mutex_lock(&g_lock);
        if (!probe) g_status.bytes_done = base;   // the file comes again by PIT1P
        pthread_mutex_unlock(&g_lock);
        return end == 'C' ? adopt_reply(fd, 'K', NULL) : adopt_reply(fd, 'N', why);
    }
    return adopt_reply(fd, 'K', NULL);
}

static void handle_client(int fd, Pipe *pipe) {
    struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);

    for (;;) {
        uint8_t hdr[5];
        ssize_t got = recv(fd, hdr, 5, MSG_WAITALL);
        if (got == 0) break;   // clean close
        if (got != 5 || memcmp(hdr, MAGIC_STR, 4) != 0) {
            if (got > 0) send_err(fd, "bad protocol header");
            break;
        }
        uint8_t op = hdr[4];
        if (op == 'I') {
            if (!handle_info(fd)) break;
        } else if (op == 'X') {
            if (!handle_remove(fd)) break;
        } else if (op == 'T') {
            if (!handle_status(fd)) break;
        } else if (op == 'S') {
            if (!handle_stat_batch(fd)) break;
        } else if (op == 'B') {
            if (!handle_begin(fd)) break;
        } else if (op == 'P') {
            if (!handle_put(fd, pipe)) break;
        } else if (op == 'A') {
            if (!handle_adopt(fd, pipe)) break;
        } else if (op == 'D') {
            manifest_save(g_root);
            refresh_shadowmount_sce_sys(g_root);
            pthread_mutex_lock(&g_lock);
            g_status.state = PS5_RECV_DONE;
            g_status.current_file[0] = 0;
            pthread_mutex_unlock(&g_lock);
            uint8_t ok = 'K';
            write_exact(fd, &ok, 1);
            g_stop = 1;
            int lfd = __atomic_exchange_n(&g_listen_fd, -1, __ATOMIC_ACQ_REL);
            if (lfd >= 0) {
                shutdown(lfd, SHUT_RDWR);
                close(lfd);
            }
            break;
        } else {
            send_err(fd, "unknown command");
            break;
        }
    }
    // If client dropped mid-transfer before DONE, persist partial manifest so resume skips completed files.
    pthread_mutex_lock(&g_lock);
    int st = g_status.state;
    if (st == PS5_RECV_RECEIVING) {
        g_status.state = PS5_RECV_ERROR;
        if (!g_status.error[0]) snprintf(g_status.error, sizeof g_status.error, "connection closed before completion");
    }
    pthread_mutex_unlock(&g_lock);
    if (st == PS5_RECV_RECEIVING || st == PS5_RECV_ERROR) manifest_save(g_root);
}

static void *recv_main(void *unused) {
    (void)unused;
    Pipe pipe;
    if (!pipe_open(&pipe)) return NULL;
    manifest_load(g_root);
    while (!g_stop) {
        int lfd = __atomic_load_n(&g_listen_fd, __ATOMIC_ACQUIRE);
        if (lfd < 0) break;
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            if (errno == EINTR && !g_stop) continue;
            break;
        }
        handle_client(cfd, &pipe);
        close(cfd);
    }
    pipe_close(&pipe);
    manifest_free();
    return NULL;
}

int ps5_console_ip(char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
#ifdef __PROSPERO__
    SceNetCtlInfo info;
    memset(&info, 0, sizeof info);
    if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_IP_ADDRESS, &info) != 0) {
        (void)sceNetInit();
        (void)sceNetCtlInit();
        memset(&info, 0, sizeof info);
        if (sceNetCtlGetInfo(SCE_NET_CTL_INFO_IP_ADDRESS, &info) != 0) {
            snprintf(out, cap, "(offline)");
            return 0;
        }
    }
    if (info.ip_address[0]) {
        snprintf(out, cap, "%s", info.ip_address);
        return 1;
    }
    snprintf(out, cap, "(offline)");
    return 0;
#else
    snprintf(out, cap, "127.0.0.1");
    return 1;
#endif
}

static char g_main_sha[65];

void ps5_game_set_main_sha256(const char *hex) { snprintf(g_main_sha, sizeof g_main_sha, "%s", hex ? hex : ""); }

// The RomFS of a game folder: romfs.nup, the whole RomFS as one package the runtime reads in place (the PC builder
// writes it: one file to send instead of ~206,000), else the loose romfs/ folder. 0 when neither is there.
int ps5_game_romfs(const char *dir, char *out, size_t cap) {
    struct stat st;
    snprintf(out, cap, "%s/romfs.nup", dir);
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) return 1;
    snprintf(out, cap, "%s/romfs", dir);
    return stat(out, &st) == 0 && S_ISDIR(st.st_mode);
}

static int has_game(const char *dir) {
    char p[768];
    struct stat st;
    snprintf(p, sizeof p, "%s/exefs/main", dir);
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return 0;
    return ps5_game_romfs(dir, p, sizeof p);
}

static int main_matches(const char *dir) {
    char p[768], hex[65];
    uint8_t d[32];
    snprintf(p, sizeof p, "%s/exefs/main", dir);
    if (!sha256_file(p, d)) return 0;
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return strcmp(hex, g_main_sha) == 0;
}

static int by_version_desc(const void *a, const void *b) {
    unsigned long x = strtoul(*(char *const *)a, NULL, 10), y = strtoul(*(char *const *)b, NULL, 10);
    return x < y ? 1 : x > y ? -1 : 0;
}

// The game folders under base: its updates/<title version>/ (newest first: 917504, 1.8.0, before an older update),
// then base itself, appended to cands (at most cap).
static int add_game_tree(const char *base, char **cands, int n, int cap) {
    char *vers[33];
    int nv = 0;
    char up[640];
    snprintf(up, sizeof up, "%s/updates", base);
    DIR *d = opendir(up);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && nv < 32) {
            if (e->d_name[0] == '.') continue;
            char c[768];
            snprintf(c, sizeof c, "%s/%s", up, e->d_name);
            if (has_game(c)) vers[nv++] = strdup(e->d_name);
        }
        closedir(d);
    }
    qsort(vers, (size_t)nv, sizeof *vers, by_version_desc);
    for (int i = 0; i < nv; i++) {
        char c[768];
        snprintf(c, sizeof c, "%s/%s", up, vers[i]);
        free(vers[i]);
        if (n < cap) cands[n++] = strdup(c);
    }
    if (n < cap && has_game(base)) cands[n++] = strdup(base);
    return n;
}

const char *ps5_game_dir(const char *root_dir, char *why, size_t why_cap) {
    // once per boot: the placeholder check, the receiver's status and the paths ask the same (each hash of a
    // candidate's main, ~55 MB, would cost a read of it again)
    static char found[768], done_for[512];
    static int done;
    if (why && why_cap) why[0] = 0;
    if (done && !strcmp(done_for, root_dir)) return found[0] ? found : NULL;
    enum { CAP = 64 };
    char *cands[CAP];
    int n = add_game_tree(root_dir, cands, 0, CAP);
    // pitstop-game-dirs.txt: more folders that hold the game, one path a line (the PC builder writes it when the
    // player's copy is already on the console, so the 23 GB need not be sent again); the title's own folder first
    char list[640];
    snprintf(list, sizeof list, "%s/pitstop-game-dirs.txt", root_dir);
    FILE *lf = fopen(list, "r");
    if (lf) {
        char line[600];
        while (fgets(line, sizeof line, lf)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '/' ) n = add_game_tree(line, cands, n, CAP);
        }
        fclose(lf);
    }
    found[0] = 0;
    for (int i = 0; i < n; i++) {
        // one candidate: the loader's hash check says if it is the wrong version; several: the one this build matches
        if (!found[0] && (n == 1 || !g_main_sha[0] || main_matches(cands[i]))) snprintf(found, sizeof found, "%s", cands[i]);
        free(cands[i]);
    }
    if (!found[0] && why && why_cap)
        snprintf(why, why_cap, n ? "no exefs/main of the game version this build was made for"
                                 : "missing exefs/main, or romfs/ or romfs.nup");
    snprintf(done_for, sizeof done_for, "%s", root_dir);
    done = 1;
    return found[0] ? found : NULL;
}

int ps5_game_files_ready(const char *root_dir, const char *expected_game_id, char *why, size_t why_cap) {
    if (why && why_cap) why[0] = 0;
    if (!root_dir || !*root_dir) return 0;
    char p[768];
    struct stat st;
    if (!ps5_game_dir(root_dir, why, why_cap)) return 0;
    snprintf(p, sizeof p, "%s/spirv.nup", root_dir);   // the shaders as one package (gfx_shader.c), else the folder
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(p, sizeof p, "%s/spirv", root_dir);
        if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) {
            if (why && why_cap) snprintf(why, why_cap, "missing spirv.nup or spirv/");
            return 0;
        }
    }
    if (expected_game_id && *expected_game_id) {
        snprintf(p, sizeof p, "%s/pitstop-game.id", root_dir);
        FILE *f = fopen(p, "rb");
        if (f) {
            char got[64] = "";
            if (!fgets(got, sizeof got, f)) got[0] = 0;
            fclose(f);
            got[strcspn(got, "\r\n \t")] = 0;
            if (strcmp(got, expected_game_id) != 0) {
                if (why && why_cap) snprintf(why, why_cap, "game files were built for another runtime version");
                return 0;
            }
        }
    }
    return 1;
}

// ---- after a ProsperoStore update
// ProsperoStore (github.com/darkxex/ProsperoStore, src/install/transaction.cpp at 0e1f031, keep_previous) installs a
// clean update by replacing the app's whole folder with the release zip, and moves the old folder to
// <drive>/prosperostore/previous/<TITLEID>. Our folder holds what the PC builder sent (romfs.nup, 23 GB; spirv.nup;
// exefs/), the saves (profile/) and mkw.ini, none of which is in the zip: without this the player would build and send
// everything again and lose their saves. Its "quick update" lays the zip over the folder instead and needs nothing.

// The store's drive for an install location (its system::drive_root): /data, /mnt/ext0-1 or /mnt/usb0-7.
static int store_drive(const char *location, const char *store_root, char *out, size_t cap) {
    static const char *const drives[] = {"/data", "/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2",
                                         "/mnt/usb3", "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"};
    char base[512];
    // store_root's parent is the internal drive (/data on the console; a scratch folder in the host test)
    snprintf(base, sizeof base, "%s", store_root);
    char *slash = strrchr(base, '/');
    if (slash && slash != base) *slash = 0;
    size_t n = strlen(base);
    if (!strncmp(location, base, n) && (location[n] == '/' || !location[n])) return snprintf(out, cap, "%s", base) < (int)cap;
    for (size_t i = 0; i < sizeof drives / sizeof *drives; i++) {
        n = strlen(drives[i]);
        if (!strncmp(location, drives[i], n) && (location[n] == '/' || !location[n]))
            return snprintf(out, cap, "%s", drives[i]) < (int)cap;
    }
    return 0;
}

// "location" from the store's receipt for our title: the folder it installed us in, such as /data/homebrew.
static int store_location(const char *receipt, char *out, size_t cap) {
    char buf[4096];
    FILE *f = fopen(receipt, "rb");
    if (!f) return 0;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    const char *p = strstr(buf, "\"location\":\"");
    if (!p) return 0;
    p += strlen("\"location\":\"");
    const char *end = strchr(p, '"');
    // a plain absolute path only: an escaped character (\") or a .. would need a real JSON parser to trust
    if (!end || p[0] != '/' || memchr(p, '\\', (size_t)(end - p)) || (size_t)(end - p) >= cap) return 0;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = 0;
    return strstr(out, "/..") == NULL;
}

// lstat fails on paths that exist from inside the title (console test, 22/11/2026: the restore saw none of the new
// folder's entries and renamed the old eboot.bin over the new one), so stat answers too, as in ps5_sys.c
static int path_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 || stat(path, &st) == 0;
}

static int same_file(const char *a, const char *b) {
    struct stat x, y;
    // /app0 is ShadowMountPlus's mount of the folder: the device differs, the size and modification time do not
    return stat(a, &x) == 0 && stat(b, &y) == 0 && x.st_size == y.st_size && x.st_mtime == y.st_mtime;
}

int ps5_store_restore(const char *store_root, const char *title_id, const char *app_dir) {
    char receipt[640], location[512], drive[512], target[768], kept[768], p[1024];
    snprintf(receipt, sizeof receipt, "%s/receipts/%s.json", store_root, title_id);
    if (!store_location(receipt, location, sizeof location)) return 0;   // not installed by the store
    if (!store_drive(location, store_root, drive, sizeof drive)) return 0;
    snprintf(target, sizeof target, "%s/%s", location, title_id);
    snprintf(kept, sizeof kept, "%s/prosperostore/previous/%s", drive, title_id);
    // the receipt's folder must be the one running (a copy installed by hand elsewhere is not ours to fill)
    char a[1024];
    snprintf(p, sizeof p, "%s/eboot.bin", target);
    snprintf(a, sizeof a, "%s/eboot.bin", app_dir);
    if (!same_file(p, a)) return 0;
    // only a folder fresh from the zip: anything the builder, the game or the player puts there means it is in use,
    // and what the previous folder still holds stays for the player to look at
    static const char *const used[] = {".pitstop-manifest.tsv", "profile", "mkw.ini", "exefs", "romfs", "romfs.nup",
                                       "spirv", "spirv.nup", "pitstop-game-dirs.txt", "pitstop-game.id"};
    for (size_t i = 0; i < sizeof used / sizeof *used; i++) {
        snprintf(p, sizeof p, "%s/%s", target, used[i]);
        if (path_exists(p)) return 0;
    }
    // the release's own entries: never taken from the old folder, whatever a check of the new one says
    static const char *const release[] = {"eboot.bin", "sce_sys", "sce_module", "licenses", "LEGAL.txt",
                                          "pitstop-runtime.json"};
    DIR *d = opendir(kept);
    if (!d) return 0;
    int moved = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char from[1024], to[1024];
        if (snprintf(from, sizeof from, "%s/%s", kept, e->d_name) >= (int)sizeof from ||
            snprintf(to, sizeof to, "%s/%s", target, e->d_name) >= (int)sizeof to)
            continue;
        int own = 0;
        for (size_t i = 0; i < sizeof release / sizeof *release; i++) own |= !strcmp(e->d_name, release[i]);
        // what the new release brings is the new version's
        if (own || path_exists(to)) continue;
        // the same drive, so a rename: the 23 GB romfs.nup moves at once, nothing is copied
        if (rename(from, to) == 0) moved++;
        else fprintf(stderr, "ps5 store restore: %s -> %s failed (%s)\n", from, to, strerror(errno));
    }
    closedir(d);
    if (moved) fprintf(stderr, "ps5 store restore: moved %d entries back from %s to %s\n", moved, kept, target);
    return moved;
}

int ps5_recv_start(const char *root_dir, uint16_t port, const char *port_version, const char *rt_game_id,
                   int has_game_code) {
    if (g_started) return 1;
    if (!port) {
        const char *env = getenv("MKW_PS5_RECV_PORT");
        if (env && *env) {
            int p = atoi(env);
            port = (p >= 0 && p <= 65535) ? (uint16_t)p : PS5_RECV_PORT;
        } else {
            port = PS5_RECV_PORT;
        }
    }
    snprintf(g_root, sizeof g_root, "%s", root_dir ? root_dir : "/app0");
    snprintf(g_port_ver, sizeof g_port_ver, "%s", port_version ? port_version : "dev");
    snprintf(g_game_id, sizeof g_game_id, "%s", rt_game_id ? rt_game_id : "");
    g_has_game_code = has_game_code ? 1 : 0;
    g_stop = 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    int rcvbuf = SOCK_BUF_SIZE;   // a refusal only leaves the default
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return 0;
    }
    socklen_t alen = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) == 0) port = ntohs(addr.sin_port);

    pthread_mutex_lock(&g_lock);
    memset(&g_status, 0, sizeof g_status);
    g_status.state = PS5_RECV_WAITING;
    g_status.port = port;
    ps5_console_ip(g_status.ip, sizeof g_status.ip);
    pthread_mutex_unlock(&g_lock);

    g_listen_fd = fd;
    if (pthread_create(&g_thread, NULL, recv_main, NULL) != 0) {
        close(fd);
        g_listen_fd = -1;
        return 0;
    }
    g_started = 1;
    return 1;
}

void ps5_recv_stop(void) {
    if (!g_started) return;
    g_stop = 1;
    int lfd = __atomic_exchange_n(&g_listen_fd, -1, __ATOMIC_ACQ_REL);
    if (lfd >= 0) {
        shutdown(lfd, SHUT_RDWR);
        close(lfd);
    }
    pthread_join(g_thread, NULL);
    g_started = 0;
}

void ps5_recv_status(Ps5RecvStatus *out) {
    if (!out) return;
    pthread_mutex_lock(&g_lock);
    *out = g_status;
    pthread_mutex_unlock(&g_lock);
}
