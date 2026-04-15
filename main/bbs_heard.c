//
//    Copyright (C) 2026 Robert Ambrose N7GET
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "bbs_heard.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "ax25_address.h"

static const char *TAG = "BBS_HEARD";
#define BBS_HEARD_BASE_PATH "/bbsfs/bbs"
#define BBS_HEARD_DATA_PATH BBS_HEARD_BASE_PATH "/heard.dat"
#define BBS_HEARD_TEMP_PATH BBS_HEARD_BASE_PATH "/heard.tmp"

/**
 * @brief Compute coarse bucket timestamp for dirty tracking
 * Rounds down to nearest BBS_HEARD_BUCKET_SECONDS boundary
 */
static uint32_t compute_bucket(uint32_t unix_time)
{
    return (unix_time / BBS_HEARD_BUCKET_SECONDS) * BBS_HEARD_BUCKET_SECONDS;
}

static void heard_on_frame(const ax25_frame_t *frame, void *user_data)
{
    bbs_heard_t *heard = (bbs_heard_t *)user_data;
    if (heard == NULL || frame == NULL || !heard->initialized) {
        return;
    }

    char src_full[16];
    ax25_address_to_string(&frame->source, src_full, sizeof(src_full));
    if (src_full[0] == '\0') {
        return;
    }

    /* Filter out the BBS's own callsign — don't hear yourself */
    if (heard->bbs_callsign[0] != '\0' && strcmp(src_full, heard->bbs_callsign) == 0) {
        return;
    }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t now_bucket = compute_bucket(now);

    xSemaphoreTake(heard->mutex, portMAX_DELAY);

    /* Search for existing entry with matching full callsign */
    for (size_t i = 0; i < heard->count; i++) {
        if (strcmp(heard->entries[i].callsign, src_full) == 0) {
            /* Update existing entry - check if bucket crossed a boundary */
            uint32_t old_bucket = heard->entries[i].bucket_timestamp;
            heard->entries[i].last_heard_unix = now;
            heard->entries[i].bucket_timestamp = now_bucket;
            
            /* Move this entry to the front (most recently heard position)
             * if it's not already there */
            if (i > 0) {
                bbs_heard_entry_t tmp = heard->entries[i];
                memmove(&heard->entries[1], &heard->entries[0], i * sizeof(bbs_heard_entry_t));
                heard->entries[0] = tmp;
                heard->dirty = true;  /* Order changed = structural change */
            } else if (old_bucket != now_bucket || old_bucket == UINT32_MAX) {
                /* Mark dirty if bucket changed, or if old value was the pre-boot
                 * sentinel (UINT32_MAX) meaning we're replacing a stale entry. */
                heard->dirty = true;
            }
            
            xSemaphoreGive(heard->mutex);
            return;
        }
    }

    /* Allocate new entry at position 0 and shift everything right */
    if (heard->count < BBS_HEARD_MAX) {
        /* Shift existing entries right to make room at position 0 */
        memmove(&heard->entries[1], &heard->entries[0], 
                heard->count * sizeof(bbs_heard_entry_t));
        heard->count++;
    } else {
        /* List is full: shift right, which drops the oldest (last entry) */
        memmove(&heard->entries[1], &heard->entries[0],
                (BBS_HEARD_MAX - 1) * sizeof(bbs_heard_entry_t));
        /* count stays at BBS_HEARD_MAX */
    }

    /* Insert new entry at position 0 */
    strlcpy(heard->entries[0].callsign, src_full,
            sizeof(heard->entries[0].callsign));
    heard->entries[0].last_heard_unix = now;
    heard->entries[0].bucket_timestamp = now_bucket;
    heard->dirty = true;  /* New entry = structural change */

    xSemaphoreGive(heard->mutex);
}

esp_err_t bbs_heard_init(bbs_heard_t *heard)
{
    if (heard == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(heard, 0, sizeof(*heard));

    heard->mutex = xSemaphoreCreateMutexStatic(&heard->mutex_buf);
    if (heard->mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    heard->promisc_port.mode = AX25_PORT_PROMISCUOUS;
    heard->promisc_port.on_tx_frame = heard_on_frame;
    heard->promisc_port.user_data = heard;

    esp_err_t err = ax25_router_register_port(&heard->promisc_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register heard port: %s", esp_err_to_name(err));
        return err;
    }

    heard->initialized = true;
    heard->dirty = false;
    heard->last_persist_time = 0;
    
    return ESP_OK;
}

void bbs_heard_set_filter_callsign(bbs_heard_t *heard, const char *bbs_callsign)
{
    if (heard == NULL) {
        return;
    }

    if (bbs_callsign == NULL || bbs_callsign[0] == '\0') {
        heard->bbs_callsign[0] = '\0';
        return;
    }

    strlcpy(heard->bbs_callsign, bbs_callsign, sizeof(heard->bbs_callsign));
}

void bbs_heard_deinit(bbs_heard_t *heard)
{
    if (heard == NULL || !heard->initialized) {
        return;
    }

    ax25_router_remove_port(&heard->promisc_port);
    heard->initialized = false;

    if (heard->mutex != NULL) {
        vSemaphoreDelete(heard->mutex);
        heard->mutex = NULL;
    }
}

size_t bbs_heard_snapshot(bbs_heard_t *heard, bbs_heard_entry_t *out, size_t max_out)
{
    if (heard == NULL || out == NULL || max_out == 0) {
        return 0;
    }

    xSemaphoreTake(heard->mutex, portMAX_DELAY);
    size_t n = heard->count < max_out ? heard->count : max_out;
    for (size_t i = 0; i < n; i++) {
        out[i] = heard->entries[i];
    }
    xSemaphoreGive(heard->mutex);

    return n;
}

esp_err_t bbs_heard_load(bbs_heard_t *heard)
{
    if (heard == NULL || !heard->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(BBS_HEARD_DATA_PATH, "rb");
    if (f == NULL) {
        /* File doesn't exist yet — not an error on first boot */
        ESP_LOGD(TAG, "No heard.dat file found (first boot?)");
        return ESP_OK;
    }

    xSemaphoreTake(heard->mutex, portMAX_DELAY);

    /* Read entries from file */
    heard->count = 0;
    bbs_heard_entry_t entry;
    while (heard->count < BBS_HEARD_MAX && fread(&entry, sizeof(entry), 1, f) == 1) {
        /* Validate: callsign must be non-empty */
        if (entry.callsign[0] != '\0') {
            /* Timestamps are seconds-since-boot and meaningless after a reboot.
             * Mark them with the UINT32_MAX sentinel so the formatter shows
             * "pre-boot" instead of a bogus elapsed time. */
            entry.last_heard_unix = UINT32_MAX;
            entry.bucket_timestamp = UINT32_MAX;
            heard->entries[heard->count++] = entry;
        }
    }

    fclose(f);
    xSemaphoreGive(heard->mutex);

    heard->dirty = false;
    heard->last_persist_time = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    ESP_LOGI(TAG, "Loaded heard list from persistent storage: %u entries", (unsigned)heard->count);
    return ESP_OK;
}

esp_err_t bbs_heard_persist(bbs_heard_t *heard, bool force_persist)
{
    if (heard == NULL || !heard->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    /* Rate-limiting: only persist once every 30 minutes (1800 seconds) */
    uint32_t elapsed_since_last = now - heard->last_persist_time;
    if (!force_persist && elapsed_since_last < 1800 && !heard->dirty) {
        /* Not dirty and rate limit not exceeded */
        return ESP_OK;
    }

    xSemaphoreTake(heard->mutex, portMAX_DELAY);

    /* Write to temporary file first */
    FILE *tmp = fopen(BBS_HEARD_TEMP_PATH, "wb");
    if (tmp == NULL) {
        ESP_LOGE(TAG, "Failed to open temp file for heard persistence");
        xSemaphoreGive(heard->mutex);
        return ESP_FAIL;
    }

    /* Write all entries */
    size_t wrote = fwrite(heard->entries, sizeof(bbs_heard_entry_t), heard->count, tmp);
    fclose(tmp);

    if (wrote != heard->count) {
        ESP_LOGE(TAG, "Failed to write heard entries to temp file (wanted %u got %u)",
                 (unsigned)heard->count, (unsigned)wrote);
        unlink(BBS_HEARD_TEMP_PATH);
        xSemaphoreGive(heard->mutex);
        return ESP_FAIL;
    }

    /* Atomic rename: temp file becomes the data file */
    if (rename(BBS_HEARD_TEMP_PATH, BBS_HEARD_DATA_PATH) != 0) {
        ESP_LOGE(TAG, "Failed to rename temp heard file to data file");
        unlink(BBS_HEARD_TEMP_PATH);
        xSemaphoreGive(heard->mutex);
        return ESP_FAIL;
    }

    /* Update tracking state */
    heard->dirty = false;
    heard->last_persist_time = now;

    xSemaphoreGive(heard->mutex);

    ESP_LOGD(TAG, "Persisted heard list to flash: %u entries (%u seconds since last persist)",
             (unsigned)heard->count, (unsigned)elapsed_since_last);

    return ESP_OK;
}
