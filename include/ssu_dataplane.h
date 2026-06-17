#ifndef SSU_DATAPLANE_H
#define SSU_DATAPLANE_H

#include <stddef.h>
#include <stdint.h>

#include "ssu_api.h"

#ifdef __cplusplus
extern "C" {
#endif

ssu_err_t ssu_dataplane_write(const ssu_logdev_info_t *logdev,
                              uint64_t logical_offset,
                              const void *buf,
                              size_t len);

ssu_err_t ssu_dataplane_read(const ssu_logdev_info_t *logdev,
                             uint64_t logical_offset,
                             void *buf,
                             size_t len);

#ifdef __cplusplus
}
#endif

#endif
