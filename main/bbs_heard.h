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

#ifndef BBS_HEARD_H
#define BBS_HEARD_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ax25_router.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BBS_HEARD_MAX 20

/** Coarse bucket size for dirty tracking (15 minutes = 900 seconds) */
#define BBS_HEARD_BUCKET_SECONDS 900

typedef struct {
    char callsign[16];          // "CALLSIGN-SS\0" — full callsign with SSID (e.g., "N7GET-5", "N0TST-8")
    uint32_t last_heard_unix;
    /** Coarse bucket tracking: last-heard time rounded down to BBS_HEARD_BUCKET_SECONDS */
    uint32_t bucket_timestamp;
} bbs_heard_entry_t;

typedef struct {
    bool initialized;
    bool dirty;                     /**< Set when heard table changes structurally */
    uint32_t last_persist_time;     /**< Unix time of last persist to flash */
    char bbs_callsign[16];          /**< BBS's own callsign — entries matching this are filtered out */
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_buf;
    ax25_router_port_t promisc_port;
    /** Heard list entries maintained in MRU (most-recently-used) order:
     *  - entries[0] is the most recently heard
     *  - entries[count-1] is the least recently heard
     *  - When a frame is heard from an existing or new callsign, it moves to position 0 */
    bbs_heard_entry_t entries[BBS_HEARD_MAX];
    size_t count;
} bbs_heard_t;

esp_err_t bbs_heard_init(bbs_heard_t *heard);
void bbs_heard_deinit(bbs_heard_t *heard);

/**
 * @brief Set the BBS's own callsign to filter it from the heard list
 * 
 * Should be called after bbs_heard_init() but before the router starts
 * sending frames to the heard port. Prevents the BBS from hearing itself.
 * 
 * @param heard Pointer to bbs_heard_t (must be initialized first)
 * @param bbs_callsign Full callsign with SSID (e.g., "N7GET-2")
 */
void bbs_heard_set_filter_callsign(bbs_heard_t *heard, const char *bbs_callsign);

size_t bbs_heard_snapshot(bbs_heard_t *heard, bbs_heard_entry_t *out, size_t max_out);

/**
 * @brief Load heard list from persistent storage (/bbsfs/bbs/heard.dat)
 * @param heard Pointer to bbs_heard_t (must be initialized first)
 * @return ESP_OK on success, ESP_FAIL if file doesn't exist or corrupted
 */
esp_err_t bbs_heard_load(bbs_heard_t *heard);

/**
 * @brief Persist heard list to flash atomically using temp-file rename
 * 
 * Writes to /bbsfs/bbs/heard.tmp then renames to /bbsfs/bbs/heard.dat.
 * Only persists if dirty flag is set or if coarse bucket times have crossed thresholds.
 * 
 * @param heard Pointer to bbs_heard_t
 * @param force_persist If true, skip dirty check and persist regardless
 * @return ESP_OK on success, ESP_FAIL on I/O error
 */
esp_err_t bbs_heard_persist(bbs_heard_t *heard, bool force_persist);

#ifdef __cplusplus
}
#endif

#endif
