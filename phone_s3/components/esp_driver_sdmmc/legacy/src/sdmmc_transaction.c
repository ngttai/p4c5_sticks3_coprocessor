/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "soc/soc_caps.h"
#include "driver/sdmmc_types.h"
#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "driver/sd_host.h"
#include "sdmmc_internal.h"

static const char* TAG = "sdmmc_req";

/* Local change (see README_LOCAL.md): counted multiple block writes */
#define SCF_NO_AUTO_STOP 0x4000     // same value as src/sd_trans_sdmmc.c

enum { COUNTED_OFF, COUNTED_ON, COUNTED_UNSUPPORTED };
static volatile int s_counted[SOC_SDMMC_NUM_SLOTS];
static int s_cmd23_timeouts[SOC_SDMMC_NUM_SLOTS];

void sdmmc_host_set_counted_writes(int slot, bool enable)
{
    if (slot >= 0 && slot < SOC_SDMMC_NUM_SLOTS) {
        s_counted[slot] = enable ? COUNTED_ON : COUNTED_OFF;
        s_cmd23_timeouts[slot] = 0;
    }
}

static esp_err_t transaction(sd_host_slot_handle_t slot_hdl, sdmmc_command_t *cmd)
{
    esp_err_t ret = sd_host_slot_do_transaction(slot_hdl, cmd);
    return ret != ESP_OK ? ret : cmd->error;
}

/* SET_BLOCK_COUNT (CMD23), then WRITE_MULTIPLE_BLOCK without the automatic STOP_TRANSMISSION: the
 * card leaves the receive state by itself after the last block. A failed write still gets a CMD12,
 * which the driver never sent, so the card does not stay in the receive state. */
static esp_err_t counted_write(int slot, sd_host_slot_handle_t slot_hdl, sdmmc_command_t *cmdinfo)
{
    sdmmc_command_t setc = {
        .opcode = MMC_SET_BLOCK_COUNT,
        .arg = cmdinfo->datalen / cmdinfo->blklen,
        .flags = SCF_CMD_AC | SCF_RSP_R1,
        .timeout_ms = 1000,
    };
    esp_err_t err = transaction(slot_hdl, &setc);
    if (err == ESP_ERR_TIMEOUT) {
        /* no answer: this write goes with the automatic CMD12; after 3 in a row the card is taken
         * as one that does not know CMD23 (a single lost response must not turn it off) */
        if (++s_cmd23_timeouts[slot] >= 3) {
            ESP_LOGW(TAG, "slot %d: no CMD23, multiple block writes with CMD12", slot);
            s_counted[slot] = COUNTED_UNSUPPORTED;
        }
        return sd_host_slot_do_transaction(slot_hdl, cmdinfo);
    }
    s_cmd23_timeouts[slot] = 0;
    if (err != ESP_OK) {
        cmdinfo->error = err;
        return ESP_OK;      // the error is reported in cmdinfo->error, like a failed transfer
    }
    const int flags = cmdinfo->flags;
    cmdinfo->flags |= SCF_NO_AUTO_STOP;
    esp_err_t ret = sd_host_slot_do_transaction(slot_hdl, cmdinfo);
    cmdinfo->flags = flags;
    if (ret != ESP_OK || cmdinfo->error != ESP_OK) {
        sdmmc_command_t stop = {
            .opcode = MMC_STOP_TRANSMISSION,
            .flags = SCF_CMD_AC | SCF_RSP_R1B,
            .timeout_ms = 1000,
        };
        esp_err_t stop_err = transaction(slot_hdl, &stop);
        ESP_LOGW(TAG, "slot %d: CMD25 failed (0x%x), CMD12: 0x%x", slot,
                 ret != ESP_OK ? ret : cmdinfo->error, stop_err);
    }
    return ret;
}

esp_err_t sdmmc_host_do_transaction(int slot, sdmmc_command_t* cmdinfo)
{
    sd_host_slot_handle_t slot_hdl = sdmmc_get_slot_handle(slot);
    esp_err_t ret;
    if (slot >= 0 && slot < SOC_SDMMC_NUM_SLOTS && s_counted[slot] == COUNTED_ON &&
            cmdinfo->opcode == MMC_WRITE_BLOCK_MULTIPLE && cmdinfo->blklen > 0 && cmdinfo->datalen > 0 &&
            !(cmdinfo->flags & SCF_NO_AUTO_STOP)) {
        ret = counted_write(slot, slot_hdl, cmdinfo);
    } else {
        ret = sd_host_slot_do_transaction(slot_hdl, cmdinfo);
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to do SD transaction");

    return ESP_OK;
}
