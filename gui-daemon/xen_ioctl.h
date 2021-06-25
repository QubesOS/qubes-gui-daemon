#ifndef QUBES_GUI_DAEMON_XEN_IOCTL_H
#define QUBES_GUI_DAEMON_XEN_IOCTL_H QUBES_GUI_DAEMON_XEN_IOCTL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <xen/xen.h>

/**
 * @brief Map \a count grants from remote domain \a domid.
 * @param gntdev_fd File descriptor to `/dev/xen/gntdev`.
 * @param domid Remote domain to map grants from.
 * @param count Number of grants pointed to by \a grants.
 * @param[in] grants Array of grants of length \a count.
 * @param[out] File descriptor of the returned dma-buf.
 * @return true on success, false on error.
 */
bool map_grant_references(int gntdev_fd,
                          domid_t domid,
                          uint32_t count,
                          const uint32_t *grants,
                          uint32_t *fd);
#endif
