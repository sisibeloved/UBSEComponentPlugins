#ifndef SSU_PLUGIN_LBC_MOCK_H
#define SSU_PLUGIN_LBC_MOCK_H

#include <stdint.h>

#include "ssu_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *prefix;
    const char *dev_ip;
    uint16_t port;
    const char *subnqn;
    const char *dev_dir;
    const char *configfs_dir;
} ssu_lbc_mock_config_t;

ssu_err_t ssu_lbc_mock_configure(const ssu_lbc_mock_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
