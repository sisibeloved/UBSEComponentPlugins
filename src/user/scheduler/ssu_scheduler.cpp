#include "ssu_api.h"

ssu_err_t ssu_scheduler_check_reliability(ssu_reliability_t reliability)
{
    if (reliability != SSU_RELIABILITY_STRIPE) {
        return SSU_ERR_UNSUPPORTED;
    }

    return SSU_OK;
}
