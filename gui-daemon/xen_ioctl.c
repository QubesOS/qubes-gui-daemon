#include "xen_ioctl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/ioctl.h>

#include <xen/grant_table.h>
#include <xen/gntdev.h>

#include <qubes-gui-protocol.h>

static_assert(sizeof(struct ioctl_gntdev_grant_ref) == 8, "bug");
static_assert(MAX_GRANT_REFS_COUNT <
        (SIZE_MAX - offsetof(struct ioctl_gntdev_map_grant_ref, refs)) /
        sizeof(struct ioctl_gntdev_grant_ref),
        "MAX_GRANT_REFS_COUNT too large");

bool map_grant_references(int gntdev_fd,
                          domid_t domid,
                          uint32_t count,
                          const uint32_t *grants,
                          uint64_t *offset) {
    assert(offset && "NULL offset");
    assert(grants && "NULL grants");
    assert(count && "cannot map zero grants");
    assert(count <= MAX_GRANT_REFS_COUNT && "excessive count not caught earlier");
    *offset = UINT64_MAX;
    const size_t ioctl_params_len = offsetof(struct ioctl_gntdev_map_grant_ref, refs) +
                    sizeof(struct ioctl_gntdev_grant_ref) * count;
    struct ioctl_gntdev_map_grant_ref *ioctl_arg = malloc(ioctl_params_len);
    if (!ioctl_arg) {
        perror("malloc failed");
        return false;
    }
    struct ioctl_gntdev_grant_ref *ioctl_refs = (struct ioctl_gntdev_grant_ref *) (
            ((uint8_t *) ioctl_arg) + offsetof(struct ioctl_gntdev_map_grant_ref, refs));
    ioctl_arg->count = count;
    ioctl_arg->pad = 0;
    ioctl_arg->index = 0;
    for (size_t i = 0; i < count; ++i) {
        ioctl_refs[i].domid = domid;
        ioctl_refs[i].ref = grants[i];
    }
    if (ioctl(gntdev_fd, IOCTL_GNTDEV_MAP_GRANT_REF, ioctl_arg)) {
        perror("ioctl(IOCTL_GNTDEV_MAP_GRANT_REF)");
        return false;
    }
    *offset = ioctl_arg->index;
    free(ioctl_arg);
    return true;
}

bool unmap_grant_references(int gntdev_fd,
                            uint64_t offset,
                            uint32_t count) {
    struct ioctl_gntdev_unmap_grant_ref refs = {
        .index = offset,
        .count = count,
        .pad = 0,
    };
    if (ioctl(gntdev_fd, IOCTL_GNTDEV_UNMAP_GRANT_REF, &refs)) {
        perror("ioctl(IOCTL_GNTDEV_UNMAP_GRANT_REF)");
        return false;
    }
    return true;
}
