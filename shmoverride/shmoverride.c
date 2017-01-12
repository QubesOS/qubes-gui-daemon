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
#include <sys/mman.h>
#include <alloca.h>
#include <errno.h>
#include <unistd.h>
#include "list.h"
#include <qubes-gui-protocol.h>

static void *(*real_shmat) (int shmid, const void *shmaddr, int shmflg);
static int (*real_shmdt) (const void *shmaddr);
static int (*real_shmctl) (int shmid, int cmd, struct shmid_ds * buf);

static int local_shmid = 0xabcdef;
static struct shm_cmd *cmd_pages = NULL;
static struct genlist *addr_list;
#ifdef XENCTRL_HAS_XC_INTERFACE
static xc_interface *xc_hnd;
#else
static int xc_hnd;
#endif
static int list_len;

void *shmat(int shmid, const void *shmaddr, int shmflg)
{
    unsigned int i;
    xen_pfn_t *pfntable;
    char *fakeaddr;
    long fakesize;
    if (!cmd_pages || (uint32_t)shmid != cmd_pages->shmid)
        return real_shmat(shmid, shmaddr, shmflg);
    if (cmd_pages->off >= 4096 || cmd_pages->num_mfn > MAX_MFN_COUNT
            || cmd_pages->num_mfn == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    pfntable = alloca(sizeof(xen_pfn_t) * cmd_pages->num_mfn);
#ifdef DEBUG
    fprintf(stderr, "size=%d table=%p\n", cmd_pages->num_mfn,
            pfntable);
#endif
    for (i = 0; i < cmd_pages->num_mfn; i++)
        pfntable[i] = cmd_pages->mfns[i];
#ifdef BACKEND_VMM_xen
    fakeaddr =
        xc_map_foreign_pages(xc_hnd, cmd_pages->domid, PROT_READ,
                pfntable, cmd_pages->num_mfn);
#else
#error "shmoverride implemented only for Xen"
#endif
    fakesize = 4096 * cmd_pages->num_mfn;
#ifdef DEBUG
    fprintf(stderr, "num=%d, addr=%p, len=%d\n",
            cmd_pages->num_mfn, fakeaddr, list_len);
#endif
    if (fakeaddr && fakeaddr != MAP_FAILED) {
        list_insert(addr_list, (long) fakeaddr, (void *) fakesize);
        list_len++;
        return fakeaddr + cmd_pages->off;
    } else {
        errno = ENOMEM;
        return MAP_FAILED;
    }
}

int shmdt(const void *shmaddr)
{
    unsigned long addr = ((long) shmaddr) & (-4096UL);
    struct genlist *item = list_lookup(addr_list, addr);
    if (!item)
        return real_shmdt(shmaddr);
    munmap((void *) addr, (long) item->data);
    list_remove(item);
    list_len--;
    return 0;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
    if (!cmd_pages || (uint32_t)shmid != cmd_pages->shmid || cmd != IPC_STAT)
        return real_shmctl(shmid, cmd, buf);
    memset(&buf->shm_perm, 0, sizeof(buf->shm_perm));
    buf->shm_segsz = cmd_pages->num_mfn * 4096 - cmd_pages->off;
    return 0;
}

#define SHMID_FILENAME "/var/run/shm.id"
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
    idfd = open(SHMID_FILENAME, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (idfd < 0) {
        perror("shmoverride creating " SHMID_FILENAME);
        xc_interface_close(xc_hnd);
        return 0;
    }
    local_shmid =
        shmget(IPC_PRIVATE, SHM_CMD_NUM_PAGES * 4096,
                IPC_CREAT | 0700);
    if (local_shmid == -1) {
        unlink(SHMID_FILENAME);
        perror("shmoverride shmget");
        exit(1);
    }
    sprintf(idbuf, "%d", local_shmid);
    len = strlen(idbuf);
    if (write(idfd, idbuf, len) != len) {
        unlink(SHMID_FILENAME);
        perror("shmoverride writing " SHMID_FILENAME);
        exit(1);
    }
    if (close(idfd) < 0) {
        unlink(SHMID_FILENAME);
        perror("shmoverride closing " SHMID_FILENAME);
        exit(1);
    }
    cmd_pages = real_shmat(local_shmid, 0, 0);
    if (!cmd_pages) {
        unlink(SHMID_FILENAME);
        perror("real_shmat");
        exit(1);
    }
    cmd_pages->shmid = local_shmid;
    return 0;
}

int __attribute__ ((destructor)) descfunc()
{
    if (cmd_pages) {
        real_shmdt(cmd_pages);
        real_shmctl(local_shmid, IPC_RMID, 0);
        unlink(SHMID_FILENAME);
    }
    return 0;
}
