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

#include "bbs_storage.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stddef.h>

#include "esp_log.h"
#include "esp_littlefs.h"

#define BBS_FS_BASE_PATH "/bbsfs"
#define BBS_DIR_PATH BBS_FS_BASE_PATH "/bbs"
#define BBS_INDEX_PATH BBS_DIR_PATH "/index.dat"

static const char *TAG = "BBS_STORAGE";

static void make_message_path(uint32_t id, char *out, size_t out_len)
{
    snprintf(out, out_len, BBS_DIR_PATH "/msg_%04lu.txt", (unsigned long)id);
}

static void str_copy_safe(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static esp_err_t ensure_dirs(void)
{
    struct stat st;
    if (stat(BBS_DIR_PATH, &st) == 0) {
        return ESP_OK;
    }
    if (mkdir(BBS_DIR_PATH, 0775) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t index_save(const bbs_storage_t *st)
{
    FILE *f = fopen(BBS_INDEX_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open index for write");
        return ESP_FAIL;
    }

    size_t wrote = fwrite(st->records, sizeof(bbs_message_index_t), st->count, f);
    fclose(f);

    if (wrote != st->count) {
        ESP_LOGE(TAG, "Failed to write full index (wanted %u got %u)",
                 (unsigned)st->count, (unsigned)wrote);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t index_load(bbs_storage_t *st)
{
    st->count = 0;
    st->next_id = 1;

    FILE *f = fopen(BBS_INDEX_PATH, "rb");
    if (f == NULL) {
        return index_save(st);
    }

    bbs_message_index_t rec;
    while (st->count < st->max_messages &&
           fread(&rec, sizeof(rec), 1, f) == 1) {
        if (rec.id == 0) {
            continue;
        }
        st->records[st->count++] = rec;
        if (rec.id >= st->next_id) {
            st->next_id = rec.id + 1;
        }
    }

    fclose(f);
    return ESP_OK;
}

static int find_record_idx(const bbs_storage_t *st, uint32_t id)
{
    for (size_t i = 0; i < st->count; i++) {
        if (st->records[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t bbs_storage_init(bbs_storage_t *st, const char *partition_label, uint16_t max_messages)
{
    if (st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(st, 0, sizeof(*st));
    st->max_messages = max_messages;
    if (st->max_messages == 0 || st->max_messages > CONFIG_BBS_MAX_MESSAGES) {
        st->max_messages = CONFIG_BBS_MAX_MESSAGES;
    }

    str_copy_safe(st->partition_label, sizeof(st->partition_label),
                  partition_label ? partition_label : CONFIG_BBS_PARTITION_LABEL);

    esp_vfs_littlefs_conf_t conf = {
        .base_path = BBS_FS_BASE_PATH,
        .partition_label = st->partition_label,
        .partition = NULL,
        .format_if_mount_failed = true,
        .dont_mount = false,
        .grow_on_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "LittleFS mount failed (label=%s): %s",
                 st->partition_label, esp_err_to_name(err));
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "LittleFS already mounted for label=%s, reusing mount",
                 st->partition_label);
    }

    st->mounted = true;

    err = ensure_dirs();
    if (err != ESP_OK) {
        bbs_storage_deinit(st);
        return err;
    }

    err = index_load(st);
    if (err != ESP_OK) {
        bbs_storage_deinit(st);
        return err;
    }

    ESP_LOGI(TAG, "Mounted LittleFS (%s), index has %u messages",
             st->partition_label, (unsigned)st->count);
    return ESP_OK;
}

void bbs_storage_deinit(bbs_storage_t *st)
{
    if (st == NULL || !st->mounted) {
        return;
    }

    esp_vfs_littlefs_unregister(st->partition_label);
    st->mounted = false;
}

esp_err_t bbs_storage_store_message(bbs_storage_t *st,
                                    const char *from_base,
                                    const char *to_base,
                                    const char *subject,
                                    const char *body,
                                    uint8_t flags,
                                    uint32_t *out_id)
{
    if (st == NULL || !st->mounted || body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bbs_message_index_t rec;
    memset(&rec, 0, sizeof(rec));

    rec.id = st->next_id++;
    rec.flags = flags;
    rec.timestamp = (uint32_t)(esp_log_timestamp() / 1000);

    str_copy_safe(rec.from, sizeof(rec.from), from_base);
    str_copy_safe(rec.to, sizeof(rec.to), to_base);
    str_copy_safe(rec.subject, sizeof(rec.subject), subject ? subject : "(no subject)");

    char msg_path[64];
    make_message_path(rec.id, msg_path, sizeof(msg_path));

    FILE *mf = fopen(msg_path, "wb");
    if (mf == NULL) {
        ESP_LOGE(TAG, "Failed to create message file %s", msg_path);
        return ESP_FAIL;
    }

    size_t body_len = strlen(body);
    size_t wrote = fwrite(body, 1, body_len, mf);
    fclose(mf);

    if (wrote != body_len) {
        ESP_LOGE(TAG, "Message write incomplete");
        return ESP_FAIL;
    }

    rec.size = (uint32_t)body_len;

    if (st->count >= st->max_messages) {
        size_t oldest_idx = 0;
        uint32_t oldest_id = st->records[0].id;
        for (size_t i = 1; i < st->count; i++) {
            if (st->records[i].id < oldest_id) {
                oldest_id = st->records[i].id;
                oldest_idx = i;
            }
        }

        char old_path[64];
        make_message_path(oldest_id, old_path, sizeof(old_path));
        unlink(old_path);

        st->records[oldest_idx] = rec;
    } else {
        st->records[st->count++] = rec;
    }

    esp_err_t err = index_save(st);
    if (err != ESP_OK) {
        return err;
    }

    if (out_id != NULL) {
        *out_id = rec.id;
    }

    return ESP_OK;
}

const bbs_message_index_t *bbs_storage_find(const bbs_storage_t *st, uint32_t id)
{
    if (st == NULL) {
        return NULL;
    }
    int idx = find_record_idx(st, id);
    if (idx < 0) {
        return NULL;
    }
    return &st->records[idx];
}

esp_err_t bbs_storage_read_message(const bbs_storage_t *st,
                                   uint32_t id,
                                   char *out,
                                   size_t out_size,
                                   size_t *out_len)
{
    if (st == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const bbs_message_index_t *rec = bbs_storage_find(st, id);
    if (rec == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char path[64];
    make_message_path(rec->id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_FAIL;
    }

    size_t n = fread(out, 1, out_size - 1, f);
    out[n] = '\0';
    fclose(f);

    if (out_len != NULL) {
        *out_len = n;
    }

    return ESP_OK;
}

esp_err_t bbs_storage_delete_message(bbs_storage_t *st, uint32_t id)
{
    if (st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int idx = find_record_idx(st, id);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char path[64];
    make_message_path(id, path, sizeof(path));
    unlink(path);

    for (size_t i = (size_t)idx; i + 1 < st->count; i++) {
        st->records[i] = st->records[i + 1];
    }
    st->count--;

    return index_save(st);
}

esp_err_t bbs_storage_free_bytes(const bbs_storage_t *st, size_t *out_free)
{
    if (!st || !st->mounted || !out_free) {
        return ESP_FAIL;
    }
    size_t total = 0, used = 0;
    esp_err_t err = esp_littlefs_info(st->partition_label, &total, &used);
    if (err != ESP_OK) {
        *out_free = 0;
        return ESP_FAIL;
    }
    *out_free = (total > used) ? (total - used) : 0;
    return ESP_OK;
}
