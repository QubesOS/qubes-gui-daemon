/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

// #define DEBUG

#define _GNU_SOURCE 1
#define XC_WANT_COMPAT_MAP_FOREIGN_API
#include <stdlib.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>

#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/file.h>
#include <xenctrl.h>
#include <xengnttab.h>
#include <assert.h>
#include "shm-args.h"
#include <qubes-gui-protocol.h>

#define QUBES_STRINGIFY(x) QUBES_STRINGIFY_(x)
#define QUBES_STRINGIFY_(x) #x

#ifdef _STAT_VER
# define FSTAT __fxstat
# define FSTAT64 __fxstat64
# define VER_ARG int ver,
# define VER _STAT_VER,
#else
# define FSTAT fstat
# define FSTAT64 fstat64
# define VER_ARG
# define VER
#endif

#define ASM_DEF(ret, name, ...) \
    __attribute__((visibility("default"))) \
    ret name(__VA_ARGS__) __asm__(QUBES_STRINGIFY_(name)); \
    __attribute__((visibility("default"))) \
    ret name(__VA_ARGS__)

static void *(*real_mmap)(void *shmaddr, size_t len, int prot, int flags,
           int fd, off_t offset);
static int (*real_munmap) (void *shmaddr, size_t len);
static int (*real_fstat64) (VER_ARG int fd, struct stat64 *buf);
static int (*real_fstat)(VER_ARG int fd, struct stat *buf);

static int try_init(void);

static struct stat global_buf;
static int gntdev_fd = -1;

static struct shm_args_hdr *shm_args = NULL;
#ifdef XENCTRL_HAS_XC_INTERFACE
static xc_interface *xc_hnd;
#else
static int xc_hnd;
#endif
static xengnttab_handle *xgt;
static char __shmid_filename[SHMID_FILENAME_LEN];
static char *shmid_filename = NULL;
static int idfd = -1, display = -1, init_called = 0;

static uint8_t *mmap_mfns(struct shm_args_hdr *shm_args) {
    uint8_t *map;
    xen_pfn_t *pfntable;
    uint32_t i;
    struct shm_args_mfns *shm_args_mfns = (struct shm_args_mfns *) (
            ((uint8_t *) shm_args) + sizeof(struct shm_args_hdr));

    pfntable = calloc(shm_args_mfns->count, sizeof(xen_pfn_t));
    if (!pfntable)
        return NULL;
    for (i = 0; i < shm_args_mfns->count; i++)
        pfntable[i] = shm_args_mfns->mfns[i];

    map = xc_map_foreign_pages(xc_hnd, shm_args->domid, PROT_READ,
                                pfntable, shm_args_mfns->count);
    free(pfntable);
    if (map == NULL)
        return NULL;

    map += shm_args_mfns->off;

    return map;
}

static uint8_t *mmap_grant_refs(void *shmaddr,
                                int fd,
                                size_t len,
                                struct shm_args_hdr *shm_args) {
    struct shm_args_grant_refs *shm_args_grant = (struct shm_args_grant_refs *) (
            ((uint8_t *) shm_args) + sizeof(struct shm_args_hdr));

    return real_mmap(shmaddr, len, PROT_READ, MAP_SHARED, fd, shm_args_grant->off);
}

static size_t shm_segsz_mfns(struct shm_args_hdr *shm_args) {
    struct shm_args_mfns *shm_args_mfns = (struct shm_args_mfns *) (
            ((uint8_t *) shm_args) + sizeof(struct shm_args_hdr));
    if (shm_args_mfns->count > MAX_MFN_COUNT)
        return 0; // this is considered an error
    return shm_args_mfns->count * XC_PAGE_SIZE - shm_args_mfns->off;
}

static size_t shm_segsz_grant_refs(struct shm_args_hdr *shm_args) {
    struct shm_args_grant_refs *shm_args_grant = (struct shm_args_grant_refs *) (
            ((uint8_t *) shm_args) + sizeof(struct shm_args_hdr));
    if (shm_args_grant->count > MAX_GRANT_REFS_COUNT)
        return 0; // this is considered an error
    return shm_args_grant->count * XC_PAGE_SIZE;
}

_Thread_local static bool in_shmoverride = false;
ASM_DEF(void *, mmap,
        void *shmaddr, size_t len, int prot, int flags,
        int fd, off_t offset)
{
    struct stat64 buf;
    if (0) {
        // These are purely for type-checking by the C compiler; they are not
        // executed at runtime
        real_mmap = mmap64;
        real_mmap = mmap;
        real_fstat64 = FSTAT64;
        real_fstat = FSTAT;
    }

    try_init();

#if defined MAP_ANON && defined MAP_ANONYMOUS && (MAP_ANONYMOUS) != (MAP_ANON)
# error header bug (def mismatch)
#endif
#ifndef MAP_ANON
# define MAP_ANON 0
#endif
#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS 0
#endif
    if ((flags & (MAP_ANON|MAP_ANONYMOUS)) || in_shmoverride)
        return real_mmap(shmaddr, len, prot, flags, fd, offset);

    if (real_fstat64(
#ifdef _STAT_VER
                _STAT_VER,
#endif
                fd, &buf))
        return MAP_FAILED;

    if (!S_ISCHR(buf.st_mode) ||
        buf.st_dev != global_buf.st_dev ||
        buf.st_ino != global_buf.st_ino ||
        buf.st_rdev != global_buf.st_rdev) {
        return real_mmap(shmaddr, len, prot, flags, fd, offset);
    }

    if ((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) != PROT_READ ||
        flags != MAP_SHARED ||
        offset != 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    in_shmoverride = true;
    uint8_t *fakeaddr = MAP_FAILED;

    switch (shm_args->type) {
    case SHM_ARGS_TYPE_MFNS:
        if (len == shm_segsz_mfns(shm_args))
            fakeaddr = mmap_mfns(shm_args);
        else
            errno = EINVAL;
        break;
    case SHM_ARGS_TYPE_GRANT_REFS:
        if (len == shm_segsz_grant_refs(shm_args))
            fakeaddr = mmap_grant_refs(shmaddr, fd, len, shm_args);
        else
            errno = EINVAL;
        break;
    default:
        errno = EINVAL;
    }
    if (!fakeaddr)
        fakeaddr = MAP_FAILED;
    in_shmoverride = false;
    return fakeaddr;
}

__attribute__((alias("mmap"))) ASM_DEF(void *, mmap64,
        void *shmaddr, size_t len, int prot, int flags,
        int fd, off_t offset);

ASM_DEF(int, munmap, void *addr, size_t len)
{
    if (len > SIZE_MAX - XC_PAGE_SIZE)
        abort();

    try_init();

    const uintptr_t addr_int = (uintptr_t)addr;
    const uintptr_t rounded_addr = addr_int & ~(uintptr_t)(XC_PAGE_SIZE - 1);
    return real_munmap((void *)rounded_addr, len + (addr_int - rounded_addr));
}

static const char* const opts_with_args[] = {
    "+extension",
    "-a",
    "-allowNonLocalXvidtune",
    "-ardelay",
    "-arinterval",
    "-audit",
    "-auth",
    "-background",
    "-bgamma",
    "-cc",
    "-class",
    "-config",
    "-configdir",
    "-cookie",
    "-deferglyphs",
    "-depth",
    "-displayID",
    "-displayfd",
    "-dpi",
    "-extension",
    "-f",
    "-fakescreenfps",
    "-fbbpp",
    "-fc",
    "-fn",
    "-fp",
    "-from",
    "-gamma",
    "-ggamma",
    "-indirect",
    "-initfd",
    "-isolateDevice",
    "-keybd",
    "-keyboard",
    "-layout",
    "-ld",
    "-lf",
    "-listen",
    "-listenfd",
    "-logfile",
    "-logverbose",
    "-ls",
    "-masterfd",
    "-maxbigreqsize",
    "-maxclients",
    "-modulepath",
    "-mouse",
    "-multicast",
    "-name",
    "-nolisten",
    "-origin",
    "-output",
    "-p",
    "-parent",
    "-pointer",
    "-port",
    "-query",
    "-render",
    "-rgamma",
    "-rgba",
    "-s",
    "-schedInterval",
    "-screen",
    "-seat",
    "-showDefaultModulePath",
    "-t",
    "-title",
    "-to",
    "-verbose",
    "-verbosity",
    "-weight",
    "-wm",
    "-x",
    "-xkbdir",
    "-xkbmap",
    "c",
};

static int cmp_strings(const void *a, const void *b) { return strcmp(a, *(const char **)b); }

static bool parse_display_name(const char *const ptr, int *display_num)
{
    unsigned long l_display = ULONG_MAX;
    const char *p = ptr + 1, *period_pointer = NULL;
    if (!isdigit((unsigned char)*p)) {
        fprintf(stderr, "Bad display name %s: colon not followed by ASCII digit\n", ptr);
        return false;
    }
    for (p++; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            if (*p != '.') {
                fprintf(stderr, "Bad display name %s: invalid character %d\n", ptr, *p);
                return false;
            }
            if (period_pointer) {
                fprintf(stderr, "Bad display name %s: more than one period ('.') character\n", ptr);
                return false;
            }
            period_pointer = p;
        }
    }

    if (period_pointer) {
        if (p - period_pointer <= 1) {
            fprintf(stderr, "Bad display name %s: period at end of name\n", ptr);
            return false;
        }

        if (p - period_pointer > 3) {
            fprintf(stderr, "Bad display name %s: more than 2 bytes after period\n", ptr);
            return false;
        }
    }

    errno = 0;
    l_display = strtoul(ptr + 1, NULL, 10);
    if (errno || l_display > INT_MAX) {
        fprintf(stderr, "Bad display name %s: exceeds INT_MAX (%d)\n", ptr, INT_MAX);
        /* Xorg will correctly reject this later */
        return false;
    }

    /* X happily accepts multiple display names and ignores all but the last */
    *display_num = (int)l_display;
    return true;
}

static int get_display(void)
{
    ssize_t res;
    int fd, rc = -1, display = 0;

    fd = open("/proc/self/cmdline", O_RDONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        perror("cmdline open");
        return rc;
    }
    char *ptr = NULL;
    size_t size = 0;
    FILE *f = fdopen(fd, "r");
    if (!f) {
        perror("fdopen()");
        close(fd);
        return rc;
    }

    bool skip = true; /* Skip argv[0] (the program name) */
    while(true) {
        errno = 0;
        res = getdelim(&ptr, &size, 0, f);
        if (res <= 0) {
            if (res == -1 && errno == 0)
                break;
            perror("cmdline read");
            goto cleanup;
        }
        size_t length = (size_t)res;
        assert(ptr && ptr[length] == '\0');

        /*
         * Skip option arguments.  Some options take more than one argument,
         * but the extra arguments are always optional and display names
         * will not be interpreted as arguments to these options.  Since
         * skip is true at the start of the loop, this also skips argv[0].
         */
        if (skip) {
            skip = false;
            continue;
        }

        if (!strcmp(ptr, "-I"))
            break; /* all remaining arguments ignored */

        /* Check if this is an option that takes a mandatory argument */
        if (bsearch(ptr, opts_with_args, sizeof(opts_with_args)/sizeof(opts_with_args[0]),
                    sizeof(opts_with_args[0]), cmp_strings)) {
            skip = true;
            continue;
        }

        if (ptr[0] == ':' && !parse_display_name(ptr, &display)) {
            fprintf(stderr, "Bad display name %s\n", ptr);
            goto cleanup;
        }
    }
    rc = display;
cleanup:
    free(ptr);
    fclose(f);
    return rc;
}

static int assign_off(off_t *off) {
    size_t s;
    switch (shm_args->type) {
    case SHM_ARGS_TYPE_MFNS:
        s = shm_segsz_mfns(shm_args);
        break;
    case SHM_ARGS_TYPE_GRANT_REFS:
        s = shm_segsz_grant_refs(shm_args);
        break;
    default:
        s = 0;
    }
    if (s) {
        *off = (off_t)s;
        return 0;
    } else {
        errno = EINVAL;
        return -1;
    }
}

#define STAT(id)                                          \
ASM_DEF(int, f ## id, int filedes, struct id *buf) {      \
    try_init();                                           \
    int res = real_f ## id(VER filedes, buf);             \
    if (res ||                                            \
        !S_ISCHR(buf->st_mode) ||                         \
        buf->st_dev != global_buf.st_dev ||               \
        buf->st_ino != global_buf.st_ino ||               \
        buf->st_rdev != global_buf.st_rdev)               \
        return res;                                       \
    return assign_off(&buf->st_size);                     \
}
STAT(stat)
STAT(stat64)
#undef STAT

#ifdef _STAT_VER
#define STAT(id)                                                    \
ASM_DEF(int, __fx ## id, int ver, int filedes, struct id *buf) {    \
    try_init();                                                     \
    if (ver != _STAT_VER) {                                         \
        fprintf(stderr,                                             \
                "Wrong _STAT_VER: got %d, expected %d, libc has incompatibly changed\n", \
                ver, _STAT_VER);                                    \
        abort();                                                    \
    }                                                               \
    return f ## id(filedes, buf);                                   \
}
STAT(stat)
STAT(stat64)
#undef STAT
#endif

static int try_init(void)
{
    // Ideally it is being called in constructor, if something is calling this before
    // constructor - we're assuming it is not multi-threaded code.
    if (__builtin_expect(init_called, 1)) return 0;
    init_called = 1;

    unsetenv("LD_PRELOAD");
    fprintf(stderr, "shmoverride constructor running\n");
    dlerror();
    if (!(real_mmap = dlsym(RTLD_NEXT, "mmap64"))) {
        fprintf(stderr, "shmoverride: no mmap64?: %s\n", dlerror());
        abort();
    } else if (!(real_fstat = dlsym(RTLD_NEXT, QUBES_STRINGIFY(FSTAT)))) {
        fprintf(stderr, "shmoverride: no " QUBES_STRINGIFY(FSTAT) "?: %s\n", dlerror());
        abort();
    } else if (!(real_fstat64 = dlsym(RTLD_NEXT, QUBES_STRINGIFY(FSTAT64)))) {
        fprintf(stderr, "shmoverride: no " QUBES_STRINGIFY(FSTAT64) "?: %s\n", dlerror());
        abort();
    } else if (!(real_munmap = dlsym(RTLD_NEXT, "munmap"))) {
        fprintf(stderr, "shmoverride: no munmap?: %s\n", dlerror());
        abort();
    }

    if ((display = get_display()) < 0)
        goto cleanup;

    if ((gntdev_fd = open("/dev/xen/gntdev", O_PATH | O_CLOEXEC | O_NOCTTY)) == -1) {
        perror("open /dev/xen/gntdev");
        goto cleanup;
    } else if (real_fstat(VER gntdev_fd, &global_buf)) {
        perror("stat /dev/xen/gntdev");
        goto cleanup;
    } else if (!S_ISCHR(global_buf.st_mode)) {
        fprintf(stderr, "/dev/xen/gntdev is not a character special file");
        goto cleanup;
    }

#ifdef XENCTRL_HAS_XC_INTERFACE
    xc_hnd = xc_interface_open(NULL, NULL, 0);
    if (!xc_hnd) {
#else
    xc_hnd = xc_interface_open();
    if (xc_hnd < 0) {
#endif
        perror("shmoverride xc_interface_open");
        goto cleanup;  //allow it to run when not under Xen
    }

    xgt = xengnttab_open(NULL, 0);
    if (xgt == NULL) {
        perror("shmoverride: xengnttab_open failed");
        goto cleanup; // Allow it to run when not under Xen.
    }

    if ((unsigned int)snprintf(__shmid_filename, sizeof __shmid_filename,
        SHMID_FILENAME_PREFIX "%d", display) >= sizeof __shmid_filename) {
        fputs("snprintf() failed!\n", stderr);
        abort();
    }
    fprintf(stderr, "shmoverride: running with shm file %s\n", __shmid_filename);

    /* Try to lock the shm.id file (don't rely on whether it exists, a previous
     * process might have crashed).
     */
    idfd = open(__shmid_filename, O_RDWR | O_CLOEXEC | O_CREAT | O_NOCTTY, 0600);
    if (idfd < 0) {
        fprintf(stderr, "shmoverride opening %s: %s\n",
            __shmid_filename, strerror(errno));
        goto cleanup;
    }
    if (flock(idfd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "shmoverride flock %s: %s\n",
                __shmid_filename, strerror(errno));
        /* There is probably an alive process holding the file, give up. */
        goto cleanup;
    }
    /* Save shmid file for cleanup only after taking the lock */
    shmid_filename = __shmid_filename;

    if (ftruncate(idfd, SHM_ARGS_SIZE) < 0) {
        perror("shmoverride ftruncate");
        goto cleanup;
    }
    if (fchmod(idfd, 0660)) {
        perror("shmoverride chmod");
        goto cleanup;
    }
    _Static_assert(SHM_ARGS_SIZE % XC_PAGE_SIZE == 0, "bug");
    shm_args = mmap(NULL, SHM_ARGS_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED_VALIDATE, idfd, 0);
    if (shm_args == MAP_FAILED) {
        perror("mmap");
        goto cleanup;
    }
    return 0;

cleanup:
    fprintf(stderr, "shmoverride: running without override\n");
#ifdef XENCTRL_HAS_XC_INTERFACE
    if (!xc_hnd) {
        xc_interface_close(xc_hnd);
        xc_hnd = NULL;
    }
#else
    if (xc_hnd >= 0) {
        xc_interface_close(xc_hnd);
        xc_hnd = -1;
    }
#endif
    if (idfd >= 0) {
        close(idfd);
        idfd = -1;
    }
    if (gntdev_fd >= 0) {
        close(gntdev_fd);
        gntdev_fd = -1;
    }
    if (shmid_filename) {
        unlink(shmid_filename);
        shmid_filename = NULL;
    }
    shm_args = NULL;
    return 0;
}
int __attribute__ ((constructor)) initfunc(void)
{
    return try_init();
}

int __attribute__ ((destructor)) descfunc(void)
{
    if (shm_args) {
        assert(shmid_filename);
        assert(idfd >= 0);

        close(idfd);
        close(gntdev_fd);
        unlink(shmid_filename);
    }

    if (xgt != NULL)
        xengnttab_close(xgt);

    return 0;
}
