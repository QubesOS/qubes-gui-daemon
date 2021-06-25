#include "xen_ioctl.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/ioctl.h>

#include <xen/grant_table.h>
#include <xen/gntdev.h>

#include <qubes-gui-protocol.h>

static_assert(sizeof(struct ioctl_gntdev_grant_ref) == 8, "bug");
static_assert(MAX_GRANT_REFS_COUNT <
        (SIZE_MAX - offsetof(struct ioctl_gntdev_map_grant_ref, refs)) /
        SIZEOF_GRANT_REF,
        "MAX_GRANT_REFS_COUNT too large");

bool map_grant_references(int gntdev_fd,
                          domid_t domid,
                          uint32_t count,
                          const uint32_t *grants,
                          uint32_t *fd) {
    assert(grants && "NULL grants");
    assert(fd && "NULL fd");
    assert(count && "cannot map zero grants");
    assert(count <= MAX_GRANT_REFS_COUNT && "excessive count not caught earlier");
    const size_t ioctl_params_len =
        offsetof(struct ioctl_gntdev_dmabuf_exp_from_refs, refs) +
                 SIZEOF_GRANT_REF * count;
    struct ioctl_gntdev_dmabuf_exp_from_refs *ioctl_arg = malloc(ioctl_params_len);
    if (!ioctl_arg) {
        perror("malloc");
        return false;
    }
    ioctl_arg->flags = 0;
    ioctl_arg->count = count;
    ioctl_arg->fd = *fd = (uint32_t)-1;
    ioctl_arg->domid = domid;
    memcpy(ioctl_arg->refs, grants, SIZEOF_GRANT_REF * count);
    if (ioctl(gntdev_fd, IOCTL_GNTDEV_DMABUF_EXP_FROM_REFS, ioctl_arg)) {
        const int i = errno;
        perror("ioctl(IOCTL_GNTDEV_DMABUF_EXP_FROM_REFS)");
        free(ioctl_arg);
        errno = i;
        return false;
    }
    *fd = ioctl_arg->fd;
    free(ioctl_arg);
    return true;
}
