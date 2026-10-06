/*
 * libmol-cow.so —— 让在农场里运行的游戏与工具（经 Wine/Proton）拥有写时复制（COW）语义，
 * 游戏目录与 mod 里的原文件永远不被修改、移动或删除。
 *
 * 农场里的文件是指向原文件的符号链接。Wine（多数文件操作由 wineserver 代为执行）有两类危险行为：
 *   1. 以写方式打开农场里的链接 → 内核顺着链接改写原文件；
 *   2. 删除 / 改名 / 移动农场里的文件 → Wine 先把链接解析成真实路径，直接 unlink / rename **原文件**
 *      （实测：`del a.txt` 删掉 mod 里的 a.txt，`move S.exe bak\S.exe` 把游戏目录里的 exe 挪进农场）。
 * 本库经 LD_PRELOAD 注入 mo-linux 启动的进程（含 wineserver），改为：
 *   - 写打开农场里的链接：先换成真实副本（见下）再打开；
 *   - 写打开 / truncate 受保护目录里的原文件（Wine 用解析后的路径时）：找到指向它的农场链接，对它做同样的
 *     复制后改为打开副本；找不到就 EACCES；
 *   - 删除受保护的原文件：改为删除指向它的农场链接（虚拟删除，下次 apply 会恢复）；
 *   - 改名 / 移动受保护的原文件：改为复制到新位置，再删除指向它的农场链接（虚拟移动）；
 *   - 改名到受保护目录里、修改受保护文件的权限：拒绝 / 忽略。
 * 复制顺序：ioctl(FICLONE)（btrfs/xfs 的 reflink，瞬间完成、不占额外空间）→ copy_file_range → read/write。
 * 每次把链接换成副本都在 MOL_COW_LOG 记一行「农场相对路径 \t 原目标 \t reflink|copy」，`overwrite capture` 据此丢弃没被真正
 * 改动的副本，其余收进 overwrite（Data/ 下）或 overwrite-root（根目录，作为最高层参与合并）。
 *
 * 环境变量：
 *   MOL_COW_ROOT     农场根（绝对路径；未设置则本库什么都不做）
 *   MOL_COW_LOG      日志文件
 *   MOL_COW_PROTECT  受保护目录，冒号分隔（游戏目录、overwrite、overwrite-root……）
 *   MOL_COW_MODS     mods 目录（受保护；其下第一级是 mod 名，映射时去掉）
 * 内部 I/O 直接走 syscall，不经过被拦截的 libc 函数，也不依赖新版本 glibc 的符号（最高 GLIBC_2.14）。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif

#define COW_EXPORT __attribute__((visibility("default")))

/* glibc 头文件里没有声明的入口 */
int __open_2(const char* path, int flags);
int __open64_2(const char* path, int flags);
int __openat_2(int dirfd, const char* path, int flags);
int __openat64_2(int dirfd, const char* path, int flags);
int renameat2(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, unsigned int flags);

/* dlsym 在 glibc 2.34 移进了 libc 并换了符号版本；钉到老版本，保证老系统上也能加载 */
#if defined(__x86_64__)
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
#endif

#define MAX_PROTECT 16

static char g_root[PATH_MAX]; /* 农场根的规范路径；空 = 未启用 */
static size_t g_root_len;
static char g_log[PATH_MAX];
static char g_protect[MAX_PROTECT][PATH_MAX];
static int g_nprotect;
static char g_mods[PATH_MAX];
static int g_inited;

/* ---- 系统调用包装（绕开被拦截的 libc 入口） ------------------------------- */

static int sys_openat(int dirfd, const char* p, int flags, mode_t mode) { return (int)syscall(SYS_openat, dirfd, p, flags, mode); }
static int sys_close(int fd) { return (int)syscall(SYS_close, fd); }
static int sys_lstat(const char* p, struct stat* st) { return (int)syscall(SYS_newfstatat, AT_FDCWD, p, st, AT_SYMLINK_NOFOLLOW); }
static int sys_stat(const char* p, struct stat* st) { return (int)syscall(SYS_newfstatat, AT_FDCWD, p, st, 0); }
static int sys_fstat(int fd, struct stat* st) { return (int)syscall(SYS_newfstatat, fd, "", st, AT_EMPTY_PATH); }
static int sys_unlink(const char* p) { return (int)syscall(SYS_unlinkat, AT_FDCWD, p, 0); }
static int sys_rename(const char* a, const char* b) { return (int)syscall(SYS_renameat, AT_FDCWD, a, AT_FDCWD, b); }

/* ---- 路径工具 ---------------------------------------------------------------- */

static void trim_slash(char* p) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = '\0';
}

static void cow_init(void) {
    if (g_inited) return;
    g_inited = 1;
    const char* r = getenv("MOL_COW_ROOT");
    if (!r || !*r || !realpath(r, g_root)) {
        g_root[0] = '\0';
        return;
    }
    trim_slash(g_root);
    g_root_len = strlen(g_root);
    const char* l = getenv("MOL_COW_LOG");
    if (l && *l && strlen(l) < sizeof(g_log)) strcpy(g_log, l);
    const char* m = getenv("MOL_COW_MODS");
    if (m && *m && realpath(m, g_mods)) trim_slash(g_mods);
    else g_mods[0] = '\0';
    const char* p = getenv("MOL_COW_PROTECT");
    char one[PATH_MAX];
    while (p && *p && g_nprotect < MAX_PROTECT) {
        const char* e = strchr(p, ':');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n > 0 && n < sizeof(one)) {
            memcpy(one, p, n);
            one[n] = '\0';
            if (realpath(one, g_protect[g_nprotect])) {
                trim_slash(g_protect[g_nprotect]);
                ++g_nprotect;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    if (g_mods[0] && g_nprotect < MAX_PROTECT) strcpy(g_protect[g_nprotect++], g_mods);
}

static int under(const char* path, const char* root) {
    size_t n = strlen(root);
    return n && strncmp(path, root, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

/* (dirfd, path) → 绝对路径（不解析任何链接） */
static int absolute_of(int dirfd, const char* path, char* out, size_t cap) {
    if (path[0] == '/') {
        if (strlen(path) >= cap) return -1;
        strcpy(out, path);
        return 0;
    }
    char base[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!getcwd(base, sizeof(base))) return -1;
    } else {
        char proc[64];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", dirfd);
        ssize_t n = readlink(proc, base, sizeof(base) - 1);
        if (n <= 0) return -1;
        base[n] = '\0';
    }
    if ((size_t)snprintf(out, cap, "%s/%s", base, path) >= cap) return -1;
    return 0;
}

/* 父目录解析成规范路径、最后一级原样保留（不跟随最后一级的链接）。成功返回 0。 */
static int canon_parent(int dirfd, const char* path, char* out, size_t cap) {
    char abs[PATH_MAX];
    if (absolute_of(dirfd, path, abs, sizeof(abs)) != 0) return -1;
    trim_slash(abs);
    char* slash = strrchr(abs, '/');
    if (!slash || !slash[1]) return -1;
    char name[NAME_MAX + 1];
    if (strlen(slash + 1) > NAME_MAX) return -1;
    strcpy(name, slash + 1);
    if (slash == abs) strcpy(abs, "/");
    else *slash = '\0';
    char rp[PATH_MAX];
    if (!realpath(abs, rp)) return -1;
    if (strcmp(rp, "/") == 0) rp[0] = '\0';
    if ((size_t)snprintf(out, cap, "%s/%s", rp, name) >= cap) return -1;
    return 0;
}

static int in_farm(const char* canon) { return g_root_len && under(canon, g_root) && canon[g_root_len] == '/'; }

static int protected_path(const char* canon) {
    for (int i = 0; i < g_nprotect; ++i)
        if (under(canon, g_protect[i])) return 1;
    return 0;
}

/* dir 下与 name 大小写不敏感地匹配的条目，写进 out（完整路径）。 */
static int match_ci(const char* dir, const char* name, char* out, size_t cap) {
    if ((size_t)snprintf(out, cap, "%s/%s", dir, name) >= cap) return -1;
    struct stat st;
    if (sys_lstat(out, &st) == 0) return 0;
    DIR* d = opendir(dir);
    if (!d) return -1;
    int found = -1;
    for (struct dirent* e; (e = readdir(d));) {
        if (strcasecmp(e->d_name, name) == 0) {
            found = (size_t)snprintf(out, cap, "%s/%s", dir, e->d_name) < cap ? 0 : -1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* 农场下的 rel（'/' 分隔）逐级大小写不敏感地解析 */
static int resolve_ci(const char* rel, char* out, size_t cap) {
    char cur[PATH_MAX], next[PATH_MAX], comp[NAME_MAX + 1];
    strcpy(cur, g_root);
    const char* p = rel;
    while (*p) {
        const char* e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 0 || n > NAME_MAX) return -1;
        memcpy(comp, p, n);
        comp[n] = '\0';
        if (match_ci(cur, comp, next, sizeof(next)) != 0) return -1;
        strcpy(cur, next);
        if (!e) break;
        p = e + 1;
    }
    if (strlen(cur) >= cap) return -1;
    strcpy(out, cur);
    return 0;
}

/* 指向受保护原文件 target 的农场链接。按目录对应关系生成候选（农场根 / Data/），readlink 核对。 */
static int find_farm_link(const char* target, char* out, size_t cap) {
    for (int i = 0; i < g_nprotect; ++i) {
        const char* root = g_protect[i];
        if (!under(target, root) || target[strlen(root)] != '/') continue;
        const char* rel = target + strlen(root) + 1;
        if (g_mods[0] && strcmp(root, g_mods) == 0) { /* mods/<mod 名>/… */
            rel = strchr(rel, '/');
            if (!rel) continue;
            ++rel;
        }
        char cand[PATH_MAX], drel[PATH_MAX];
        const char* tries[2] = {rel, NULL};
        if ((size_t)snprintf(drel, sizeof(drel), "Data/%s", rel) < sizeof(drel)) tries[1] = drel;
        for (int k = 0; k < 2; ++k) {
            if (!tries[k] || resolve_ci(tries[k], cand, sizeof(cand)) != 0) continue;
            char lt[PATH_MAX], rt[PATH_MAX];
            ssize_t n = readlink(cand, lt, sizeof(lt) - 1);
            if (n <= 0) continue;
            lt[n] = '\0';
            if (strcmp(lt, target) == 0 || (realpath(lt, rt) && strcmp(rt, target) == 0)) {
                if (strlen(cand) >= cap) return -1;
                strcpy(out, cand);
                return 0;
            }
        }
    }
    return -1;
}

/* ---- 复制 ------------------------------------------------------------------- */

/* 0 = 复制完成，1 = reflink 完成，-1 = 失败 */
static int copy_fd(int src, int dst, off_t size) {
    if (ioctl(dst, FICLONE, src) == 0) return 1; /* reflink */
    off_t left = size;
    while (left > 0) {
        ssize_t n = (ssize_t)syscall(SYS_copy_file_range, src, NULL, dst, NULL, (size_t)(left > (1 << 30) ? (1 << 30) : left), 0u);
        if (n <= 0) break;
        left -= n;
    }
    if (left == 0) return 0;
    if (lseek(src, 0, SEEK_SET) < 0 || lseek(dst, 0, SEEK_SET) < 0 || ftruncate(dst, 0) != 0) return -1;
    char buf[1 << 16];
    for (;;) {
        ssize_t r = read(src, buf, sizeof(buf));
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        for (ssize_t off = 0; off < r;) {
            ssize_t w = write(dst, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            off += w;
        }
    }
}

/* src 的内容（跟随链接）原子地放到 dst（同目录临时文件 + rename）。成功返回 0（*reflinked 表示走了 reflink）。 */
static int clone_to(const char* src, const char* dst, int* reflinked) {
    int s = sys_openat(AT_FDCWD, src, O_RDONLY | O_CLOEXEC, 0);
    if (s < 0) return -1;
    struct stat st;
    if (sys_fstat(s, &st) != 0 || !S_ISREG(st.st_mode)) {
        sys_close(s);
        errno = EISDIR;
        return -1;
    }
    char tmp[PATH_MAX];
    const char* slash = strrchr(dst, '/');
    if (!slash || (size_t)snprintf(tmp, sizeof(tmp), "%.*s/.mol-cow.%d.tmp", (int)(slash - dst), dst, (int)getpid()) >= sizeof(tmp)) {
        sys_close(s);
        return -1;
    }
    int d = sys_openat(AT_FDCWD, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (d < 0) {
        sys_close(s);
        return -1;
    }
    int rc = copy_fd(s, d, st.st_size);
    sys_close(d);
    sys_close(s);
    if (reflinked) *reflinked = rc == 1;
    if (rc >= 0 && sys_rename(tmp, dst) == 0) return 0;
    sys_unlink(tmp);
    return -1;
}

static void log_copy(const char* canon, const char* target, int reflinked) {
    if (!g_log[0]) return;
    int fd = sys_openat(AT_FDCWD, g_log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return;
    char line[PATH_MAX * 2 + 4];
    int n = snprintf(line, sizeof(line), "%s\t%s\t%s\n", canon + g_root_len + 1, target, reflinked ? "reflink" : "copy");
    if (n > 0 && (size_t)n < sizeof(line)) (void)!write(fd, line, (size_t)n); /* O_APPEND 的小写入是原子的 */
    sys_close(fd);
}

static int lock_farm(void) {
    char p[PATH_MAX];
    if ((size_t)snprintf(p, sizeof(p), "%s/.mol-cow.lock", g_root) >= sizeof(p)) return -1;
    int fd = sys_openat(AT_FDCWD, p, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) flock(fd, LOCK_EX);
    return fd; /* 关闭即解锁 */
}

/* 农场里的链接 canon（指向普通文件）换成真实副本。已经是真实文件 → 0；出错 → -1。 */
static int copy_up(const char* canon) {
    struct stat lst;
    if (sys_lstat(canon, &lst) != 0 || !S_ISLNK(lst.st_mode)) return 0;
    int lk = lock_farm();
    int rc = 0;
    char target[PATH_MAX];
    ssize_t tl;
    if (sys_lstat(canon, &lst) != 0 || !S_ISLNK(lst.st_mode)) goto done; /* 等锁期间别人复制好了 */
    tl = readlink(canon, target, sizeof(target) - 1);
    if (tl <= 0) {
        rc = -1;
        goto done;
    }
    target[tl] = '\0';
    int reflinked = 0;
    if (clone_to(canon, canon, &reflinked) != 0) { /* 读的是链接目标，写到同目录临时文件后 rename 覆盖链接 */
        rc = -1;
        goto done;
    }
    log_copy(canon, target, reflinked);
done:
    if (lk >= 0) sys_close(lk);
    return rc;
}

/* ---- 各操作的策略 -------------------------------------------------------------- */

/* 写打开 / truncate：返回实际应打开的路径（可能改成农场副本），或 NULL 表示拒绝（errno 已设置）。 */
static const char* prepare_write(int dirfd, const char* path, char* buf, size_t cap) {
    cow_init();
    if (!g_root_len || !path || !*path) return path;
    char canon[PATH_MAX];
    if (canon_parent(dirfd, path, canon, sizeof(canon)) != 0) return path;
    if (in_farm(canon)) {
        struct stat lst;
        if (sys_lstat(canon, &lst) == 0 && S_ISLNK(lst.st_mode)) {
            struct stat st;
            if (sys_stat(canon, &st) != 0) {
                /* 悬空链接：写打开会在链接目标处新建文件（可能就是受保护目录）。先删掉链接，在农场里新建 */
                sys_unlink(canon);
            } else if (copy_up(canon) != 0) {
                errno = EACCES; /* 换不成副本就不放行：宁可失败也不改原文件 */
                return NULL;
            }
        }
        return path;
    }
    /* 不在农场里：若（解析后）落在受保护目录，说明 Wine 用的是链接目标的真实路径 */
    char full[PATH_MAX];
    if (!realpath(canon, full)) {
        if (protected_path(canon)) { /* 往原目录里新建文件 */
            errno = EACCES;
            return NULL;
        }
        return path;
    }
    if (!protected_path(full)) return path;
    char link[PATH_MAX];
    if (find_farm_link(full, link, sizeof(link)) == 0 && copy_up(link) == 0 && strlen(link) < cap) {
        strcpy(buf, link);
        return buf;
    }
    errno = EACCES;
    return NULL;
}

/* ---- 拦截的 libc 入口 ------------------------------------------------------------ */

#define NEXT(name) \
    static __typeof__(name)* real_##name; \
    if (!real_##name) real_##name = (__typeof__(name)*)dlsym(RTLD_NEXT, #name)

static int wants_write(int flags) { return (flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC); }
static mode_t mode_arg(int flags, va_list ap) {
    return (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE ? (mode_t)va_arg(ap, int) : 0;
}

#define OPEN_PREP(dirfd, path)                                           \
    char redirect_[PATH_MAX];                                            \
    const char* use_ = path;                                             \
    if (wants_write(flags)) {                                            \
        use_ = prepare_write(dirfd, path, redirect_, sizeof(redirect_)); \
        if (!use_) return -1;                                            \
    }

COW_EXPORT int open(const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t m = mode_arg(flags, ap);
    va_end(ap);
    OPEN_PREP(AT_FDCWD, path);
    NEXT(open);
    return real_open(use_, flags, m);
}
COW_EXPORT int open64(const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t m = mode_arg(flags, ap);
    va_end(ap);
    OPEN_PREP(AT_FDCWD, path);
    NEXT(open64);
    return real_open64(use_, flags, m);
}
COW_EXPORT int openat(int dirfd, const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t m = mode_arg(flags, ap);
    va_end(ap);
    OPEN_PREP(dirfd, path);
    NEXT(openat);
    return real_openat(use_ == path ? dirfd : AT_FDCWD, use_, flags, m);
}
COW_EXPORT int openat64(int dirfd, const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t m = mode_arg(flags, ap);
    va_end(ap);
    OPEN_PREP(dirfd, path);
    NEXT(openat64);
    return real_openat64(use_ == path ? dirfd : AT_FDCWD, use_, flags, m);
}
/* _FORTIFY_SOURCE 编译的程序会调用这些 */
COW_EXPORT int __open_2(const char* path, int flags) {
    OPEN_PREP(AT_FDCWD, path);
    NEXT(__open_2);
    return real___open_2(use_, flags);
}
COW_EXPORT int __open64_2(const char* path, int flags) {
    OPEN_PREP(AT_FDCWD, path);
    NEXT(__open64_2);
    return real___open64_2(use_, flags);
}
COW_EXPORT int __openat_2(int dirfd, const char* path, int flags) {
    OPEN_PREP(dirfd, path);
    NEXT(__openat_2);
    return real___openat_2(use_ == path ? dirfd : AT_FDCWD, use_, flags);
}
COW_EXPORT int __openat64_2(int dirfd, const char* path, int flags) {
    OPEN_PREP(dirfd, path);
    NEXT(__openat64_2);
    return real___openat64_2(use_ == path ? dirfd : AT_FDCWD, use_, flags);
}
COW_EXPORT int creat(const char* path, mode_t mode) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    OPEN_PREP(AT_FDCWD, path);
    NEXT(creat);
    return real_creat(use_, mode);
}
COW_EXPORT int creat64(const char* path, mode_t mode) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    OPEN_PREP(AT_FDCWD, path);
    NEXT(creat64);
    return real_creat64(use_, mode);
}
COW_EXPORT int truncate(const char* path, off_t len) {
    int flags = O_WRONLY;
    OPEN_PREP(AT_FDCWD, path);
    NEXT(truncate);
    return real_truncate(use_, len);
}
COW_EXPORT int truncate64(const char* path, off64_t len) {
    int flags = O_WRONLY;
    OPEN_PREP(AT_FDCWD, path);
    NEXT(truncate64);
    return real_truncate64(use_, len);
}
/* glibc 的 fopen 内部直接走系统调用，不会经过上面的 open：单独拦 */
static int mode_writes(const char* mode) { return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')); }
COW_EXPORT FILE* fopen(const char* path, const char* mode) {
    char redirect_[PATH_MAX];
    const char* use_ = path;
    if (mode_writes(mode) && !(use_ = prepare_write(AT_FDCWD, path, redirect_, sizeof(redirect_)))) return NULL;
    NEXT(fopen);
    return real_fopen(use_, mode);
}
COW_EXPORT FILE* fopen64(const char* path, const char* mode) {
    char redirect_[PATH_MAX];
    const char* use_ = path;
    if (mode_writes(mode) && !(use_ = prepare_write(AT_FDCWD, path, redirect_, sizeof(redirect_)))) return NULL;
    NEXT(fopen64);
    return real_fopen64(use_, mode);
}
COW_EXPORT FILE* freopen(const char* path, const char* mode, FILE* f) {
    char redirect_[PATH_MAX];
    const char* use_ = path;
    if (path && mode_writes(mode) && !(use_ = prepare_write(AT_FDCWD, path, redirect_, sizeof(redirect_)))) return NULL;
    NEXT(freopen);
    return real_freopen(use_, mode, f);
}

/* 删除：受保护的原文件 → 改为删除指向它的农场链接（虚拟删除）；找不到链接也假装成功，原文件不动。 */
static int guard_unlink(int dirfd, const char* path, int* handled) {
    *handled = 0;
    cow_init();
    if (!g_root_len || !path || !*path) return 0;
    char canon[PATH_MAX];
    if (canon_parent(dirfd, path, canon, sizeof(canon)) != 0 || in_farm(canon) || !protected_path(canon)) return 0;
    *handled = 1;
    char link[PATH_MAX];
    if (find_farm_link(canon, link, sizeof(link)) == 0) return sys_unlink(link);
    return 0;
}
COW_EXPORT int unlink(const char* path) {
    int handled;
    int rc = guard_unlink(AT_FDCWD, path, &handled);
    if (handled) return rc;
    NEXT(unlink);
    return real_unlink(path);
}
COW_EXPORT int unlinkat(int dirfd, const char* path, int flags) {
    if (flags & AT_REMOVEDIR) {
        /* 删除受保护目录本身：假装成功（农场里的目录都是真实目录，删它们不受影响） */
        char canon[PATH_MAX];
        cow_init();
        if (g_root_len && canon_parent(dirfd, path, canon, sizeof(canon)) == 0 && !in_farm(canon) && protected_path(canon)) return 0;
    } else {
        int handled;
        int rc = guard_unlink(dirfd, path, &handled);
        if (handled) return rc;
    }
    NEXT(unlinkat);
    return real_unlinkat(dirfd, path, flags);
}
COW_EXPORT int remove(const char* path) {
    struct stat st;
    if (sys_lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        NEXT(remove);
        return real_remove(path);
    }
    return unlink(path);
}

/* 改名 / 移动：源是受保护的原文件 → 复制到新位置并删除指向它的农场链接（虚拟移动）；
 * 目标落在受保护目录里 → 拒绝。返回 -2 表示不处理（交给真正的 rename）。 */
static int guard_rename(int odir, const char* oldp, int ndir, const char* newp, unsigned flags) {
    cow_init();
    if (!g_root_len || !oldp || !newp) return -2;
    char o[PATH_MAX], n[PATH_MAX];
    if (canon_parent(odir, oldp, o, sizeof(o)) != 0 || canon_parent(ndir, newp, n, sizeof(n)) != 0) return -2;
    const int src_prot = !in_farm(o) && protected_path(o);
    const int dst_prot = !in_farm(n) && protected_path(n);
    if (!src_prot && !dst_prot) return -2;
    if (dst_prot || (flags & RENAME_EXCHANGE)) {
        errno = dst_prot ? EACCES : EXDEV;
        return -1;
    }
    struct stat st;
    if ((flags & RENAME_NOREPLACE) && sys_lstat(n, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (sys_stat(o, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) {
        errno = EXDEV; /* 目录：让调用方按跨设备处理（逐个复制），原目录保持不动 */
        return -1;
    }
    if (clone_to(o, n, NULL) != 0) return -1;
    char link[PATH_MAX];
    if (find_farm_link(o, link, sizeof(link)) == 0 && strcmp(link, n) != 0) sys_unlink(link);
    return 0;
}
COW_EXPORT int rename(const char* oldp, const char* newp) {
    int rc = guard_rename(AT_FDCWD, oldp, AT_FDCWD, newp, 0);
    if (rc != -2) return rc;
    NEXT(rename);
    return real_rename(oldp, newp);
}
COW_EXPORT int renameat(int od, const char* oldp, int nd, const char* newp) {
    int rc = guard_rename(od, oldp, nd, newp, 0);
    if (rc != -2) return rc;
    NEXT(renameat);
    return real_renameat(od, oldp, nd, newp);
}
COW_EXPORT int renameat2(int od, const char* oldp, int nd, const char* newp, unsigned int flags) {
    int rc = guard_rename(od, oldp, nd, newp, flags);
    if (rc != -2) return rc;
    NEXT(renameat2);
    return real_renameat2(od, oldp, nd, newp, flags);
}

/* 权限：受保护的原文件不改（Wine 的只读属性等），假装成功 */
static int protected_target(int dirfd, const char* path, int follow) {
    cow_init();
    if (!g_root_len || !path || !*path) return 0;
    char canon[PATH_MAX], full[PATH_MAX];
    if (canon_parent(dirfd, path, canon, sizeof(canon)) != 0) return 0;
    if (!follow) return !in_farm(canon) && protected_path(canon);
    return realpath(canon, full) && protected_path(full);
}
COW_EXPORT int chmod(const char* path, mode_t mode) {
    if (protected_target(AT_FDCWD, path, 1)) return 0;
    NEXT(chmod);
    return real_chmod(path, mode);
}
COW_EXPORT int fchmodat(int dirfd, const char* path, mode_t mode, int flags) {
    if (protected_target(dirfd, path, !(flags & AT_SYMLINK_NOFOLLOW))) return 0;
    NEXT(fchmodat);
    return real_fchmodat(dirfd, path, mode, flags);
}
