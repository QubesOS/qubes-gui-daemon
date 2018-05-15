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
#include <dlfcn.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <malloc.h>
#include <xenctrl.h>
#include <xengnttab.h>
#include <sys/mman.h>
#include <alloca.h>
#include <errno.h>
#include <unistd.h>
#include "list.h"
#include "shm-args.h"
#include <qubes-gui-protocol.h>

static void *(*real_shmat) (int shmid, const void *shmaddr, int shmflg);
static int (*real_shmdt) (const void *shmaddr);
static int (*real_shmctl) (int shmid, int cmd, struct shmid_ds * buf);

static int local_shmid = 0xabcdef;
static struct shm_args_hdr *shm_args = NULL;
static struct genlist *addr_list;
#ifdef XENCTRL_HAS_XC_INTERFACE
static xc_interface *xc_hnd;
#else
static int xc_hnd;
#endif
static xengnttab_handle *xgt;
static int list_len;
static char __shmid_filename[SHMID_FILENAME_LEN];
static char *shmid_filename = NULL;
static char display_str[SHMID_DISPLAY_MAXLEN+1] = "";

struct mfns_info {
    uint32_t count;
    uint32_t off;
};

struct grant_refs_info {
    uint32_t count;
    bool is_dummy;
};

struct info {
    uint32_t type;
    union {
        struct mfns_info mfns;
        struct grant_refs_info grant;
    } u;
};

static uint8_t *shmat_mfns(struct shm_args_hdr *shm_args, struct info *info) {
    uint8_t *map;
    xen_pfn_t *pfntable;
    uint32_t i;
    struct shm_args_mfns *shm_args_mfns = (struct shm_args_mfns *) (
            ((uint8_t *) shm_args) + sizeof(struct shm_args_hdr));

    pfntable = alloca(sizeof(xen_pfn_t) * shm_args_mfns->count);
    for (i = 0; i < shm_args_mfns->count; i++)
        pfntable[i] = shm_args_mfns->mfns[i];

    info->u.mfns.count = shm_args_mfns->count;
    info->u.mfns.off = shm_args_mfns->off;

    map = xc_map_foreign_pages(xc_hnd, shm_args->domid, PROT_READ,
                                pfntable, shm_args_mfns->count);
    if (map == NULL)
        return NULL;

    map += shm_args_mfns->off;

    return map;
}

static uint8_t *shmat_grant_refs(struct shm_args_hdr *shm_args,
                                 struct info *info) {
    uint8_t *map;
    struct shm_args_grant_refs *shm_args_grant = (struct shm_args_grant_refs *) (
            ((uint8_t *) shm_args) + sizeof(struct shm_args_hdr));

    info->u.grant.count = shm_args_grant->count;

    map = xengnttab_map_domain_grant_refs(xgt,
            shm_args_grant->count,
            shm_args->domid,
            &shm_args_grant->refs[0],
            PROT_READ);

    if (map != NULL)
        return map;

    // Something failed. Most likely the other domain already destroyed the
    // buffer and thereby invalidated the refs. So create a dummy buffer. This
    // mapping will probably be unmapped very soon anyway.

    map = calloc(1, info->u.mfns.count * XC_PAGE_SIZE);
    info->u.grant.is_dummy = true;
    return map;
}

void *shmat(int shmid, const void *shmaddr, int shmflg)
{
    uint8_t *fakeaddr = NULL;
    struct info *info;

    if (!shm_args || (uint32_t)shmid != shm_args->shmid)
        return real_shmat(shmid, shmaddr, shmflg);

    info = calloc(1, sizeof(struct info));
    if (info == NULL)
        return MAP_FAILED;

    switch (shm_args->type) {
    case SHM_ARGS_TYPE_MFNS:
        fakeaddr = shmat_mfns(shm_args, info);
        break;
    case SHM_ARGS_TYPE_GRANT_REFS:
        fakeaddr = shmat_grant_refs(shm_args, info);
        break;
    default:
        errno = EINVAL;
    }
    info->type = shm_args->type;

    if (fakeaddr == NULL) {
        free(info);
        // errno set by shmat_*
        return MAP_FAILED;
    }

    list_insert(addr_list, (long) fakeaddr, info);
    list_len++;
    return fakeaddr;
}

static int shmdt_mfns(void *map, struct info *info) {
    return munmap(map - info->u.mfns.off, info->u.mfns.count * XC_PAGE_SIZE);
}

static int shmdt_grant_refs(void *map, struct info *info) {
    if (info->u.grant.is_dummy) {
        free(map);
        return 0;
    }

    return xengnttab_unmap(xgt, map, info->u.grant.count);
}

int shmdt(const void *shmaddr)
{
    void *addr = (void *) shmaddr; // drop const qualifier
    struct genlist *item = list_lookup(addr_list, (long) addr);
    struct info *info;
    int rc;
    if (!item)
        return real_shmdt(shmaddr);

    info = item->data;
    switch (info->type) {
    case SHM_ARGS_TYPE_MFNS:
        rc = shmdt_mfns(addr, info);
        break;
    case SHM_ARGS_TYPE_GRANT_REFS:
        rc = shmdt_grant_refs(addr, info);
        break;
    default:
        errno = EINVAL;
        rc = -1;
    }

    list_remove(item);
    list_len--;
    return rc;
}

static size_t shm_segsz_mfns(struct shm_args_hdr *shm_args) {
    struct shm_args_mfns *shm_args_mfns = (struct shm_args_mfns *) (
            ((uint8_t *) shm_args) + sizeof(struct shm_args_hdr));

    return shm_args_mfns->count * XC_PAGE_SIZE - shm_args_mfns->off;
}

static size_t shm_segsz_grant_refs(struct shm_args_hdr *shm_args) {
    struct shm_args_grant_refs *shm_args_grant = (struct shm_args_grant_refs *) (
            ((uint8_t *) shm_args) + sizeof(struct shm_args_hdr));

    return shm_args_grant->count * XC_PAGE_SIZE;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
    size_t segsz = 0;

    if (!shm_args || (uint32_t)shmid != shm_args->shmid || cmd != IPC_STAT)
        return real_shmctl(shmid, cmd, buf);

    switch (shm_args->type) {
    case SHM_ARGS_TYPE_MFNS:
        segsz = shm_segsz_mfns(shm_args);
        break;
    case SHM_ARGS_TYPE_GRANT_REFS:
        segsz = shm_segsz_grant_refs(shm_args);
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    memset(&buf->shm_perm, 0, sizeof(buf->shm_perm));
    buf->shm_segsz = segsz;

    return 0;
}

void get_display()
{
    int fd;
    ssize_t res;
    char ch;
    int in_arg = -1;

    fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) {
        perror("cmdline open");
        exit(1);
    }

    while(1) {
        res = read(fd, &ch, 1);
        if (res < 0) {
            perror("cmdline read");
            exit(1);
        }
        if (res == 0)
            break;

        if (in_arg == 0 && ch != ':')
            in_arg = -1;
        if (ch == '\0') {
            in_arg = 0;
        } else if (in_arg >= 0) {
            if (in_arg >= SHMID_DISPLAY_MAXLEN)
                break;
            if (in_arg > 0 && (ch < '0' || ch > '9')) {
                if (in_arg == 1) {
                    fprintf(stderr, "cmdline DISPLAY parsing failed\n");
                    exit(1);
                }
                in_arg = -1;
                continue;
            }
            display_str[in_arg++] = ch;
            display_str[in_arg] = '\0';
        }
    }
    close(fd);

    if (display_str[0] != ':') {
        display_str[0] = ':';
        display_str[1] = '0';
        display_str[2] = '\0';
    } else if (display_str[1] == '\0') {
        fprintf(stderr, "cmdline DISPLAY parsing failed\n");
        exit(1);
    }

    /* post-processing: drop leading ':' */
    res = strlen(display_str);
    for (in_arg = 0; in_arg < res; in_arg++)
        display_str[in_arg] = display_str[in_arg+1];
}

int __attribute__ ((constructor)) initfunc()
{
    int idfd, len;
    char idbuf[20];
    unsetenv("LD_PRELOAD");
    fprintf(stderr, "shmoverride constructor running\n");
    real_shmat = dlsym(RTLD_NEXT, "shmat");
    real_shmctl = dlsym(RTLD_NEXT, "shmctl");
    real_shmdt = dlsym(RTLD_NEXT, "shmdt");
    if (!real_shmat || !real_shmctl || !real_shmdt) {
        perror("shmoverride: missing shm API");
        exit(1);
    }
    addr_list = list_new();
#ifdef XENCTRL_HAS_XC_INTERFACE
    xc_hnd = xc_interface_open(NULL, NULL, 0);
    if (!xc_hnd) {
#else
    xc_hnd = xc_interface_open();
    if (xc_hnd < 0) {
#endif
        perror("shmoverride xc_interface_open");
        return 0;  //allow it to run when not under Xen
    }

    xgt = xengnttab_open(NULL, 0);
    if (xgt == NULL) {
        perror("shmoverride: xengnttab_open failed");
        return 0; // Allow it to run when not under Xen.
    }

    get_display();
    snprintf(__shmid_filename, SHMID_FILENAME_LEN,
        SHMID_FILENAME_PREFIX "%s", display_str);
    shmid_filename = __shmid_filename;
    idfd = open(shmid_filename, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (idfd < 0) {
        fprintf(stderr, "shmoverride creating %s: %s\n",
            shmid_filename, strerror(errno));
        xc_interface_close(xc_hnd);
        return 0;
    }
    local_shmid =
        shmget(IPC_PRIVATE, SHM_ARGS_SIZE,
                IPC_CREAT | 0700);
    if (local_shmid == -1) {
        unlink(shmid_filename);
        perror("shmoverride shmget");
        exit(1);
    }
    sprintf(idbuf, "%d", local_shmid);
    len = strlen(idbuf);
    if (write(idfd, idbuf, len) != len) {
        unlink(shmid_filename);
        fprintf(stderr, "shmoverride writing %s: %s\n",
            shmid_filename, strerror(errno));
        exit(1);
    }
    if (close(idfd) < 0) {
        unlink(shmid_filename);
        fprintf(stderr, "shmoverride closing %s: %s\n",
            shmid_filename, strerror(errno));
        exit(1);
    }
    shm_args = real_shmat(local_shmid, 0, 0);
    if (!shm_args) {
        unlink(shmid_filename);
        perror("real_shmat");
        exit(1);
    }
    shm_args->shmid = local_shmid;
    return 0;
}

int __attribute__ ((destructor)) descfunc()
{
    if (shm_args) {
        real_shmdt(shm_args);
        real_shmctl(local_shmid, IPC_RMID, 0);
        if (shmid_filename != NULL)
            unlink(shmid_filename);
    }

    if (xgt != NULL)
        xengnttab_close(xgt);

    return 0;
}
