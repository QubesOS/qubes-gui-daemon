#include "xen_ioctl.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <sys/ioctl.h>

#include <err.h>

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
    *fd = (uint32_t)-1;
    ioctl_arg->fd = 0;
    ioctl_arg->domid = domid;
    memcpy((char *)ioctl_arg + offsetof(struct ioctl_gntdev_dmabuf_exp_from_refs, refs),
           grants,
           SIZEOF_GRANT_REF * count);
    assert(ioctl_arg->flags == 0 &&
           ioctl_arg->count == count &&
           ioctl_arg->fd == 0 &&
           ioctl_arg->domid == domid);
    errno = 0;
    for (uint32_t i = 0; i < count; ++i) {
        assert(ioctl_arg->refs[i] == grants[i]);
        fprintf(stderr, "Mapping grant %" PRIu32 " using dma-buf\n", grants[i]);
    }
    switch (ioctl(gntdev_fd, IOCTL_GNTDEV_DMABUF_EXP_FROM_REFS, ioctl_arg)) {
    case 0:
        break;
    case -1:;
        int i = errno;
        warn("ioctl(IOCTL_GNTDEV_DMABUF_EXP_FROM_REFS): domid %" PRIu16 ", count %" PRIu32,
             ioctl_arg->domid, ioctl_arg->count);
        free(ioctl_arg);
        errno = i;
        return false;
    default:
        assert(!"Bogus return value from ioctl(IOCTL_GNTDEV_DMABUF_EXP_FROM_REFS)");
        abort();
    }
    assert(ioctl_arg->fd > 2);
    *fd = ioctl_arg->fd;
    free(ioctl_arg);
    return true;
}
