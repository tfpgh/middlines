/*
 * Copyright (c) 2022 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __cplusplus
extern "C"
{
#endif

#pragma once

#include <stdbool.h>

#include <golioth/client.h>
#include <golioth/ota.h>

struct golioth_fw_update_config
{
    const char *current_version;
    const char *fw_package_name;
};

void golioth_fw_update_init(struct golioth_client *client, const char *current_version);

void golioth_fw_update_init_with_config(struct golioth_client *client,
                                        const struct golioth_fw_update_config *config);

typedef void (*golioth_fw_update_state_change_callback)(enum golioth_ota_state state,
                                                        enum golioth_ota_reason reason,
                                                        void *user_arg);

void golioth_fw_update_register_state_change_callback(
    golioth_fw_update_state_change_callback callback,
    void *user_arg);

#ifdef __cplusplus
}
#endif
