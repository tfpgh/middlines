#pragma once

#include "esp_err.h"

#include <golioth/client.h>

esp_err_t init_nvs(void);

esp_err_t load_golioth_credentials(struct golioth_client_config *config,
                                   char *psk_id,
                                   size_t psk_id_size,
                                   char *psk,
                                   size_t psk_size);
