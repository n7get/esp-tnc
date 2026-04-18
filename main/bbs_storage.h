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

#ifndef BBS_STORAGE_H
#define BBS_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

 #ifdef __cplusplus
 extern "C" {
 #endif

#ifndef CONFIG_BBS_MAX_MESSAGES
#define CONFIG_BBS_MAX_MESSAGES 100
#endif

#ifndef CONFIG_BBS_PARTITION_LABEL
#define CONFIG_BBS_PARTITION_LABEL "bbs_data"
#endif

#define BBS_CALL_BASE_LEN 6
#define BBS_SUBJECT_LEN   48

#define BBS_MSG_FLAG_PRIVATE  0x01
#define BBS_MSG_FLAG_BULLETIN 0x02

typedef struct {
    uint32_t id;
    char from[BBS_CALL_BASE_LEN + 1];
    char to[BBS_CALL_BASE_LEN + 1];
    char subject[BBS_SUBJECT_LEN + 1];
    uint32_t size;
    uint32_t timestamp;
    uint8_t flags;
    uint8_t reserved[3];
} bbs_message_index_t;

typedef struct {
    bool mounted;
    uint16_t max_messages;
    uint32_t next_id;
    size_t count;
    bbs_message_index_t records[CONFIG_BBS_MAX_MESSAGES];
    char partition_label[16];
} bbs_storage_t;

esp_err_t bbs_storage_init(bbs_storage_t *st, const char *partition_label, uint16_t max_messages);
void bbs_storage_deinit(bbs_storage_t *st);

esp_err_t bbs_storage_store_message(bbs_storage_t *st,
                                    const char *from_base,
                                    const char *to_base,
                                    const char *subject,
                                    const char *body,
                                    uint8_t flags,
                                    uint32_t *out_id);

esp_err_t bbs_storage_read_message(const bbs_storage_t *st,
                                   uint32_t id,
                                   char *out,
                                   size_t out_size,
                                   size_t *out_len);

esp_err_t bbs_storage_delete_message(bbs_storage_t *st, uint32_t id);
const bbs_message_index_t *bbs_storage_find(const bbs_storage_t *st, uint32_t id);

/**
 * @brief Get the number of free bytes available in the message store partition.
 *
 * @param st Pointer to initialized bbs_storage_t.
 * @param out_free Pointer to size_t to receive free bytes.
 * @return ESP_OK on success, ESP_FAIL if not mounted or error.
 */
esp_err_t bbs_storage_free_bytes(const bbs_storage_t *st, size_t *out_free);

#ifdef __cplusplus
}
#endif

#endif
