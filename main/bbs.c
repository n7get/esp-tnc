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

#include "bbs.h"
#include "ax25_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#ifdef __INTELLISENSE__
#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 100
#endif
#ifndef CONFIG_LOG_MAXIMUM_LEVEL
#define CONFIG_LOG_MAXIMUM_LEVEL 5
#endif
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BBS";
static const char CMD_BYE[] = "B";
static const char CMD_CONFIG[] = "CONFIG";
static const char CMD_TERMINAL[] = "TERM";
static const char CMD_HEARD[] = "J";
static const char CMD_HELP_ALIAS[] = "?";
static const char CMD_HELP[] = "H";
static const char CMD_INFO[] = "I";
static const char CMD_KILL[] = "K";
static const char CMD_LIST_LAST[] = "LL";
static const char CMD_LIST_MINE[] = "LM";
static const char CMD_LIST[] = "L";
static const char CMD_READ[] = "R";
static const char CMD_SB[] = "SB";
static const char CMD_SEND[] = "S";
static const char CMD_SP[] = "SP";
static const char CMD_SYSOP[] = "SYSOP";
static const char CR[] = "\r";
static const char EMPTY_CMD[] = "<empty>";
static const char MESSAGE_NOT_FOUND_TEXT[] = "*** Message not found\r";
static const char MESSAGE_NOT_FOUND[] = "message not found";
static const char NO_MESSAGES[] = "no messages";
static const char NO_READABLE_MESSAGES[] = "no readable messages";
static const char NVS_LAST_READ_KEY_FMT[] = "lr_%s";
static const char READ_SEPARATOR[] = "----------------------------------------\r";
static const char USAGE_ERROR[] = "usage error";

/* Retry window: 200 × 10 ms = 2000 ms, must exceed the remote T2 timer
 * (1000 ms) + round-trip latency so the AX.25 window clears in time. */
#define BBS_SEND_RETRY_MAX        200
#define BBS_SEND_RETRY_DELAY_MS   10
#define BBS_TXBUF_SIZE            (AX25_MAX_INFO_LEN + 1)

static void bbs_log_command(const char *cmd, const char *arg)
{
#if CONFIG_BBS_CMD_TRACE_LOG
    ESP_LOGI(TAG, "CMD: %s%s%s", cmd ? cmd : "", (arg && arg[0]) ? " " : "", (arg && arg[0]) ? arg : "");
#else
    (void)cmd;
    (void)arg;
#endif
}

static void bbs_log_response(const char *cmd, const char *summary)
{
#if CONFIG_BBS_CMD_TRACE_LOG
    ESP_LOGI(TAG, "RESP: %s -> %s", cmd ? cmd : "", summary ? summary : "");
#else
    (void)cmd;
    (void)summary;
#endif
}

static bool equals_ignore_case_ascii(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;

        if (ca >= 'a' && ca <= 'z') {
            ca = (char)(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = (char)(cb - 'a' + 'A');
        }

        if (ca != cb) {
            return false;
        }

        a++;
        b++;
    }

    return (*a == '\0') && (*b == '\0');
}

static void format_elapsed_since(uint32_t then_unix, char *out, size_t out_size)
{
    uint32_t now_unix;
    uint32_t delta;
    uint32_t days;
    uint32_t hours;
    uint32_t minutes;

    if (out == NULL || out_size == 0) {
        return;
    }

    /* UINT32_MAX is the sentinel for "heard before current boot, time unknown" */
    if (then_unix == UINT32_MAX) {
        snprintf(out, out_size, "pre-boot");
        return;
    }

    now_unix = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    delta = (then_unix <= now_unix) ? (now_unix - then_unix) : 0;
    days = delta / 86400U;
    hours = (delta % 86400U) / 3600U;
    minutes = (delta % 3600U) / 60U;

    snprintf(out, out_size, "%lud %02lu:%02lu",
             (unsigned long)days,
             (unsigned long)hours,
             (unsigned long)minutes);
}

static void bbs_send_bytes(bbs_t *bbs, const uint8_t *data, size_t len)
{
    if (bbs == NULL || bbs->conn == NULL || data == NULL || len == 0) {
        return;
    }

    const uint8_t *p = data;
    size_t remaining = len;

    while (remaining > 0) {
        size_t chunk = (remaining > AX25_MAX_INFO_LEN) ? AX25_MAX_INFO_LEN : remaining;
        esp_err_t err = ESP_FAIL;
        int attempts = 0;

        while (attempts < BBS_SEND_RETRY_MAX) {
            err = ax25_conn_send_data(bbs->conn, p, chunk);
            if (err == ESP_OK) {
                break;
            }

            if (err == ESP_ERR_NO_MEM) {
                attempts++;
                vTaskDelay(pdMS_TO_TICKS(BBS_SEND_RETRY_DELAY_MS));
                continue;
            }

            if (err == ESP_ERR_INVALID_STATE) {
                return;
            }

            ESP_LOGW(TAG, "send_data failed: %s", esp_err_to_name(err));
            return;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send_data retry exhausted (%d attempts)", BBS_SEND_RETRY_MAX);
            return;
        }

        p += chunk;
        remaining -= chunk;
    }
}

static void bbs_send_text(bbs_t *bbs, const char *text)
{
    if (text == NULL) {
        return;
    }

    bbs_send_bytes(bbs, (const uint8_t *)text, strlen(text));
}

static void bbs_cfg_output_cb(const char *text, size_t len, void *arg)
{
    bbs_t *bbs = (bbs_t *)arg;
    bbs_send_bytes(bbs, (const uint8_t *)text, len);
}

typedef struct {
    bbs_t *bbs;
    uint8_t buf[BBS_TXBUF_SIZE];
    size_t len;
} bbs_txbuf_t;

static void bbs_txbuf_append(bbs_txbuf_t *tx, const uint8_t *data, size_t data_len);

static const char *bbs_get_prompt_text(const bbs_t *bbs)
{
    if (bbs == NULL) {
        return CONFIG_BBS_PROMPT_TEXT;
    }

    if (bbs->input_mode == BBS_INPUT_MODE_CONFIG) {
        return bbs->config_prompt_text;
    }

    if (bbs->input_mode == BBS_INPUT_MODE_TERMINAL) {
        return bbs->terminal_prompt_text;
    }

    return bbs->prompt_text;
}

static void bbs_txbuf_append_text(bbs_txbuf_t *tx, const char *text)
{
    if (text == NULL) {
        return;
    }

    bbs_txbuf_append(tx, (const uint8_t *)text, strlen(text));
}

static void bbs_txbuf_append_prompt(bbs_txbuf_t *tx)
{
    if (tx == NULL || tx->bbs == NULL) {
        return;
    }

    bbs_txbuf_append_text(tx, bbs_get_prompt_text(tx->bbs));
    bbs_txbuf_append_text(tx, CR);
}

static void bbs_txbuf_init(bbs_txbuf_t *tx, bbs_t *bbs)
{
    if (tx == NULL) {
        return;
    }

    tx->bbs = bbs;
    tx->len = 0;
}

static void bbs_txbuf_flush(bbs_txbuf_t *tx)
{
    if (tx == NULL || tx->bbs == NULL) {
        return;
    }

    if (tx->len == 0) {
        return;
    }

    bbs_send_bytes(tx->bbs, tx->buf, tx->len);
    tx->len = 0;
}

static void bbs_txbuf_append(bbs_txbuf_t *tx, const uint8_t *data, size_t data_len)
{
    size_t offset = 0;

    if (tx == NULL || tx->bbs == NULL || data == NULL || data_len == 0) {
        return;
    }

    while (offset < data_len) {
        size_t room;
        size_t chunk;
        const size_t limit = AX25_MAX_INFO_LEN;

        if (tx->len >= limit) {
            bbs_txbuf_flush(tx);
        }

        room = limit - tx->len;
        if (room == 0) {
            return;
        }

        chunk = data_len - offset;
        if (chunk > room) {
            chunk = room;
        }

        memcpy(tx->buf + tx->len, data + offset, chunk);
        tx->len += chunk;
        offset += chunk;

        if (tx->len >= limit) {
            bbs_txbuf_flush(tx);
        }
    }
}

static void bbs_send_prompt(bbs_t *bbs)
{
    if (bbs == NULL) {
        return;
    }

    bbs_txbuf_t tx;
    bbs_txbuf_init(&tx, bbs);
    bbs_txbuf_append_text(&tx, bbs_get_prompt_text(bbs));
    bbs_txbuf_append_text(&tx, CR);
    bbs_txbuf_flush(&tx);
}

static bool is_readable_for_user(const bbs_t *bbs, const bbs_message_index_t *rec)
{
    if ((rec->flags & BBS_MSG_FLAG_BULLETIN) != 0) {
        return true;
    }
    if (bbs->is_sysop) {
        return true;
    }
    if ((rec->flags & BBS_MSG_FLAG_PRIVATE) != 0) {
        return strcmp(rec->to, bbs->remote_callsign) == 0;
    }
    return false;
}

void bbs_base_callsign(const char *src, char *out, size_t out_size)
{
    size_t j = 0;

    if (out_size == 0) {
        return;
    }

    for (size_t i = 0; src != NULL && src[i] != '\0' && src[i] != '-' && j < out_size - 1; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[j++] = c;
    }
    out[j] = '\0';
}

static void bbs_normalize_full_callsign(const char *src, char *out, size_t out_size)
{
    size_t j = 0;

    if (out_size == 0) {
        return;
    }

    for (size_t i = 0; src != NULL && src[i] != '\0' && j < out_size - 1; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[j++] = c;
    }
    out[j] = '\0';
}

bool bbs_is_valid_callsign(const char *str)
{
    char base[7];
    size_t i = 0;

    bbs_base_callsign(str, base, sizeof(base));

    while (base[i] >= 'A' && base[i] <= 'Z') {
        i++;
    }
    if (i < 1 || i > 2) {
        return false;
    }

    if (!(base[i] >= '0' && base[i] <= '9')) {
        return false;
    }
    i++;

    size_t suffix = 0;
    while (base[i] >= 'A' && base[i] <= 'Z') {
        suffix++;
        i++;
    }

    if (suffix < 1 || suffix > 3) {
        return false;
    }

    return base[i] == '\0';
}

static esp_err_t nvs_get_last_read(bbs_t *bbs, const char *base_call, uint32_t *last_id)
{
    char key[15];
    snprintf(key, sizeof(key), NVS_LAST_READ_KEY_FMT, base_call);
    return nvs_get_u32(bbs->nvs, key, last_id);
}

static esp_err_t nvs_set_last_read(bbs_t *bbs, const char *base_call, uint32_t last_id)
{
    char key[15];
    snprintf(key, sizeof(key), NVS_LAST_READ_KEY_FMT, base_call);
    esp_err_t err = nvs_set_u32(bbs->nvs, key, last_id);
    if (err == ESP_OK) {
        err = nvs_commit(bbs->nvs);
    }
    return err;
}

static uint32_t bbs_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool sysop_secret_is_empty(const bbs_t *bbs)
{
    return bbs == NULL || sysop_auth_secret_is_empty(&bbs->sysop_auth);
}

static void sysop_issue_challenge(bbs_t *bbs)
{
    if (bbs == NULL) {
        return;
    }

    char chal[64];
    uint32_t challenge_id = 0;
    esp_err_t err = sysop_auth_issue_challenge(&bbs->sysop_auth,
                                               bbs_now_ms(),
                                               chal,
                                               sizeof(chal),
                                               &challenge_id);
    if (err != ESP_OK) {
        bbs_send_text(bbs, "*** SYSOP challenge unavailable\r");
        bbs_send_prompt(bbs);
        return;
    }

    bbs->input_mode = BBS_INPUT_MODE_SYSOP_RESPONSE;
    bbs_send_text(bbs, chal);
    ESP_LOGI(TAG, "SYSOP challenge issued id=%lu remote=%s",
             (unsigned long)challenge_id,
             bbs->remote_callsign);
}

static void sysop_expire_authenticated_if_idle(bbs_t *bbs)
{
    if (bbs == NULL) {
        return;
    }

    if (sysop_auth_expire_authenticated_if_idle(&bbs->sysop_auth, bbs_now_ms())) {
        bbs->is_sysop = false;
        bbs_send_text(bbs, "*** SYSOP session expired\r");
        ESP_LOGI(TAG, "SYSOP session expired remote=%s", bbs->remote_callsign);
    }
}

static void sysop_send_lockout_message(bbs_t *bbs)
{
    if (bbs == NULL) {
        return;
    }

    unsigned long mins = sysop_auth_lockout_minutes(&bbs->sysop_auth);

    char msg[80];
    snprintf(msg, sizeof(msg), "*** SYSOP locked out for %lu minutes\r", mins);
    bbs_send_text(bbs, msg);
}

static int list_messages(bbs_t *bbs, int limit, bool mine_only)
{
    char line[192];
    bbs_txbuf_t tx;
    int shown = 0;

    if (bbs->storage->count == 0) {
        bbs_send_text(bbs, "*** No messages\r");
        return -1;
    }

    size_t start = 0;
    if (limit > 0 && (size_t)limit < bbs->storage->count) {
        start = bbs->storage->count - (size_t)limit;
    }

    bbs_txbuf_init(&tx, bbs);
    bbs_txbuf_append_text(&tx, "   # From            To              Flags Subject\r");

    for (size_t i = start; i < bbs->storage->count; i++) {
        const bbs_message_index_t *rec = &bbs->storage->records[i];
        if (mine_only) {
            if (strcmp(rec->from, bbs->remote_callsign) != 0 &&
                strcmp(rec->to, bbs->remote_callsign) != 0) {
                continue;
            }
        } else if (!is_readable_for_user(bbs, rec)) {
            continue;
        }

        snprintf(line, sizeof(line), "%4lu %-15s %-15s %c%c    %s\r",
                 (unsigned long)rec->id,
                 rec->from,
                 rec->to,
                 (rec->flags & BBS_MSG_FLAG_PRIVATE) ? 'P' : '-',
                 (rec->flags & BBS_MSG_FLAG_BULLETIN) ? 'B' : '-',
                 rec->subject);
        bbs_txbuf_append(&tx, (const uint8_t *)line, strlen(line));
        shown++;
    }

    if (shown == 0) {
        bbs_send_text(bbs, "*** No readable messages\r");
        return 0;
    }

    bbs_txbuf_flush(&tx);

    return shown;
}

static const char *cmd_read(bbs_t *bbs, uint32_t id)
{
    const bbs_message_index_t *rec = bbs_storage_find(bbs->storage, id);
    if (rec == NULL) {
        bbs_send_text(bbs, MESSAGE_NOT_FOUND_TEXT);
        return MESSAGE_NOT_FOUND;
    }

    if (!is_readable_for_user(bbs, rec)) {
        bbs_send_text(bbs, "*** Access denied\r");
        return "access denied";
    }

    size_t body_len = 0;
    if (bbs_storage_read_message(bbs->storage,
                                 id,
                                 bbs->read_body_buf,
                                 sizeof(bbs->read_body_buf),
                                 &body_len) != ESP_OK) {
        bbs_send_text(bbs, "*** Failed to read message\r");
        return "read failed";
    }

    char line[192];
    bbs_txbuf_t tx;

    bbs_txbuf_init(&tx, bbs);

    snprintf(line, sizeof(line), "Msg %lu From:%s To:%s Subj:%s\r",
             (unsigned long)rec->id, rec->from, rec->to, rec->subject);
    bbs_txbuf_append_text(&tx, line);
    bbs_txbuf_append_text(&tx, READ_SEPARATOR);
    bbs_txbuf_append(&tx, (const uint8_t *)bbs->read_body_buf, body_len);
    bbs_txbuf_append_text(&tx, READ_SEPARATOR);
    bbs_txbuf_flush(&tx);

    uint32_t last_id = 0;
    nvs_get_last_read(bbs, bbs->remote_callsign, &last_id);
    if (id > last_id) {
        nvs_set_last_read(bbs, bbs->remote_callsign, id);
    }

    return "message body sent";
}

static const char *cmd_kill(bbs_t *bbs, uint32_t id)
{
    const bbs_message_index_t *rec = bbs_storage_find(bbs->storage, id);
    if (rec == NULL) {
        bbs_send_text(bbs, MESSAGE_NOT_FOUND_TEXT);
        return MESSAGE_NOT_FOUND;
    }

    if (!bbs->is_sysop && strcmp(rec->from, bbs->remote_callsign) != 0) {
        bbs_send_text(bbs, "*** Kill denied\r");
        return "kill denied";
    }

    if (bbs_storage_delete_message(bbs->storage, id) == ESP_OK) {
        bbs_send_text(bbs, "*** Message deleted\r");
        return "message deleted";
    } else {
        bbs_send_text(bbs, "*** Delete failed\r");
        return "delete failed";
    }
}

static void finish_post(bbs_t *bbs)
{
    uint32_t id = 0;
    esp_err_t err = bbs_storage_store_message(bbs->storage,
                                              bbs->remote_callsign,
                                              bbs->pending_to,
                                              bbs->pending_subject,
                                              bbs->pending_body,
                                              bbs->pending_flags,
                                              &id);
    if (err == ESP_OK) {
        char line[80];
        snprintf(line, sizeof(line), "*** Stored as message %lu\r", (unsigned long)id);
        bbs_send_text(bbs, line);
    } else {
        bbs_send_text(bbs, "*** Store failed\r");
    }

    bbs->input_mode = BBS_INPUT_MODE_COMMAND;
    bbs->pending_body_len = 0;
    bbs->pending_body[0] = '\0';
    bbs_send_prompt(bbs);
}

static void bbs_enter_terminal_mode(bbs_t *bbs);
static void bbs_exit_terminal_mode(bbs_t *bbs);

static void handle_sysop_response(bbs_t *bbs, const char *line)
{
    if (bbs == NULL || line == NULL) {
        return;
    }

    if (equals_ignore_case_ascii(line, "SYSOP NEW")) {
        if (sysop_auth_is_awaiting_response(&bbs->sysop_auth)) {
            sysop_issue_challenge(bbs);
        } else {
            bbs_send_text(bbs, "*** No active SYSOP challenge\r");
            bbs_send_prompt(bbs);
        }
        return;
    }

    uint32_t now_ms = bbs_now_ms();
    bool enter_config_on_success = sysop_auth_take_deferred_config(&bbs->sysop_auth);
    bool enter_terminal_on_success = sysop_auth_take_deferred_console(&bbs->sysop_auth);
    sysop_auth_verify_result_t v = sysop_auth_submit_response(&bbs->sysop_auth, line, now_ms);
    bbs->input_mode = BBS_INPUT_MODE_COMMAND;

    if (v == SYSOP_AUTH_VERIFY_SUCCESS) {
        bbs->is_sysop = true;
        bbs_send_text(bbs, "*** SYSOP authenticated\r");
        if (enter_config_on_success && ax25_cfg_is_initialized()) {
            ax25_cfg_set_output(bbs_cfg_output_cb, bbs);
            bbs->input_mode = BBS_INPUT_MODE_CONFIG;
            bbs_send_text(bbs, "*** Entering config mode\r");
            bbs_send_text(bbs, "Commands: set get clear show show all save config commit revert reboot exit\r");
        } else if (enter_terminal_on_success && bbs->uart_is_terminal_mode && bbs->uart_phy) {
            bbs_enter_terminal_mode(bbs);
            return;
        }
        bbs_send_prompt(bbs);
        ESP_LOGI(TAG, "SYSOP auth success remote=%s", bbs->remote_callsign);
        return;
    }

    if (v == SYSOP_AUTH_VERIFY_LOCKED_OUT) {
        bbs->is_sysop = false;
        sysop_send_lockout_message(bbs);
        bbs_send_prompt(bbs);
        ESP_LOGW(TAG, "SYSOP lockout remote=%s failures=%u",
                 bbs->remote_callsign,
                 sysop_auth_failed_attempts(&bbs->sysop_auth));
        return;
    }

    if (v == SYSOP_AUTH_VERIFY_EXPIRED || v == SYSOP_AUTH_VERIFY_NO_ACTIVE) {
        bbs->is_sysop = false;
        bbs_send_text(bbs, "*** SYSOP challenge expired\r");
        bbs_send_prompt(bbs);
        ESP_LOGI(TAG, "SYSOP challenge expired remote=%s", bbs->remote_callsign);
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "*** SYSOP auth failed (%u/%u)\r",
             sysop_auth_failed_attempts(&bbs->sysop_auth),
             sysop_auth_max_attempts(&bbs->sysop_auth));
    bbs_send_text(bbs, msg);
    bbs_send_prompt(bbs);
    ESP_LOGI(TAG, "SYSOP auth failed remote=%s failures=%u",
             bbs->remote_callsign,
             sysop_auth_failed_attempts(&bbs->sysop_auth));
}

static void bbs_enter_config_mode(bbs_t *bbs)
{
    if (bbs == NULL) {
        return;
    }

    ax25_cfg_set_output(bbs_cfg_output_cb, bbs);
    bbs->input_mode = BBS_INPUT_MODE_CONFIG;
    bbs_send_text(bbs, "*** Entering config mode\r");
    bbs_send_text(bbs, "Commands: set get clear show show all save config commit revert reboot exit\r");
    bbs_send_prompt(bbs);
}

/* -------------------------------------------------------------------------
 * Terminal mode
 * ---------------------------------------------------------------------- */

static void terminal_rx_tap_cb(const uint8_t *data, size_t len, void *user_data)
{
    bbs_t *bbs = (bbs_t *)user_data;
    if (bbs == NULL || bbs->input_mode != BBS_INPUT_MODE_TERMINAL) {
        return;
    }

    /* Forward bytes verbatim to the connected station */
    bbs_send_bytes(bbs, data, len);
}

static void bbs_enter_terminal_mode(bbs_t *bbs)
{
    if (bbs == NULL) {
        return;
    }

    bbs->input_mode = BBS_INPUT_MODE_TERMINAL;
    ax25_phy_kiss_uart_set_rx_tap(terminal_rx_tap_cb, bbs, bbs->uart_phy);
    bbs_send_text(bbs, "*** Entering terminal mode (TNC terminal)\r");
    bbs_send_text(bbs, "Lines are forwarded to TNC as-is. /exit to quit, /reset to send KISS reset.\r");
    bbs_send_prompt(bbs);
}

static void bbs_exit_terminal_mode(bbs_t *bbs)
{
    if (bbs == NULL) {
        return;
    }

    ax25_phy_kiss_uart_clear_rx_tap(bbs->uart_phy);
    bbs->input_mode = BBS_INPUT_MODE_COMMAND;
    bbs_send_text(bbs, "*** Exiting terminal mode\r");
    bbs_send_prompt(bbs);
}

static void send_help_text(bbs_t *bbs)
{
    bbs_txbuf_t tx;

    bbs_txbuf_init(&tx, bbs);
    bbs_txbuf_append_text(&tx, "Commands:\r");
    bbs_txbuf_append_text(&tx, "  B           Disconnect\r");
    bbs_txbuf_append_text(&tx, "  CONFIG      Enter OTA configuration mode\r");
    bbs_txbuf_append_text(&tx, "  H           Show this help text\r");
    bbs_txbuf_append_text(&tx, "  I           Show BBS station information\r");
    bbs_txbuf_append_text(&tx, "  J           Show heard list\r");
    bbs_txbuf_append_text(&tx, "  K <n>       Delete message n (own or sysop)\r");
    bbs_txbuf_append_text(&tx, "  L           List readable messages\r");
    bbs_txbuf_append_text(&tx, "  LL [n]      List newest n readable messages\r");
    bbs_txbuf_append_text(&tx, "  LM          List only my sent/received messages\r");
    bbs_txbuf_append_text(&tx, "  R <n>       Read message n\r");
    bbs_txbuf_append_text(&tx, "  S <to>      Send message (auto private/bulletin)\r");
    bbs_txbuf_append_text(&tx, "  SB <topic>  Send bulletin message\r");
    bbs_txbuf_append_text(&tx, "  SP <call>   Send private message\r");
    bbs_txbuf_append_text(&tx, "  SYSOP       Start SYSOP challenge login\r");
    bbs_txbuf_append_text(&tx, "  SYSOP NEW   Refresh active SYSOP challenge\r");
    bbs_txbuf_append_text(&tx, "  TERM        Enter TNC terminal mode\r");
    bbs_txbuf_flush(&tx);
}

static void process_command(bbs_t *bbs, const char *line)
{
    char cmd[8] = {0};
    char arg[80] = {0};
    char detail[96];

    sscanf(line, "%7s %79[^\r]", cmd, arg);
    for (size_t i = 0; cmd[i] != '\0'; i++) {
        if (cmd[i] >= 'a' && cmd[i] <= 'z') {
            cmd[i] = (char)(cmd[i] - 'a' + 'A');
        }
    }

    if (cmd[0] == '\0') {
        bbs_log_command(EMPTY_CMD, "");
        bbs_send_prompt(bbs);
        bbs_log_response(EMPTY_CMD, "prompt only");
        return;
    }

    bbs_log_command(cmd, arg);

    sysop_expire_authenticated_if_idle(bbs);
    if (sysop_auth_is_authenticated(&bbs->sysop_auth)) {
        sysop_auth_note_activity(&bbs->sysop_auth, bbs_now_ms());
    }

    if (strcmp(cmd, CMD_HELP) == 0 || strcmp(cmd, CMD_HELP_ALIAS) == 0) {
        send_help_text(bbs);
        bbs_log_response(cmd, "help shown");
    } else if (strcmp(cmd, CMD_LIST) == 0) {
        int shown = list_messages(bbs, 0, false);
        if (shown < 0) {
            bbs_log_response(cmd, NO_MESSAGES);
        } else if (shown == 0) {
            bbs_log_response(cmd, NO_READABLE_MESSAGES);
        } else {
            snprintf(detail, sizeof(detail), "listed %d messages", shown);
            bbs_log_response(cmd, detail);
        }
    } else if (strcmp(cmd, CMD_LIST_LAST) == 0) {
        int n = atoi(arg);
        if (n <= 0) n = 10;
        int shown = list_messages(bbs, n, false);
        if (shown < 0) {
            bbs_log_response(cmd, NO_MESSAGES);
        } else if (shown == 0) {
            bbs_log_response(cmd, NO_READABLE_MESSAGES);
        } else {
            snprintf(detail, sizeof(detail), "listed %d messages (limit=%d)", shown, n);
            bbs_log_response(cmd, detail);
        }
    } else if (strcmp(cmd, CMD_LIST_MINE) == 0) {
        int shown = list_messages(bbs, 0, true);
        if (shown < 0) {
            bbs_log_response(cmd, NO_MESSAGES);
        } else if (shown == 0) {
            bbs_log_response(cmd, "no mine-only messages");
        } else {
            snprintf(detail, sizeof(detail), "listed %d mine-only messages", shown);
            bbs_log_response(cmd, detail);
        }
    } else if (strcmp(cmd, CMD_READ) == 0) {
        uint32_t id = (uint32_t)strtoul(arg, NULL, 10);
        if (id == 0) {
            bbs_send_text(bbs, "Usage: R <n>\r");
            bbs_log_response(cmd, USAGE_ERROR);
        } else {
            bbs_log_response(cmd, cmd_read(bbs, id));
        }
    } else if (strcmp(cmd, CMD_SEND) == 0 || strcmp(cmd, CMD_SP) == 0 || strcmp(cmd, CMD_SB) == 0) {
        char to[7] = {0};
        bbs_base_callsign(arg, to, sizeof(to));
        if (to[0] == '\0') {
            bbs_send_text(bbs, "Usage: S|SP|SB <call/topic>\r");
            bbs_send_prompt(bbs);
            bbs_log_response(cmd, USAGE_ERROR);
            return;
        }

        if (strcmp(cmd, CMD_SP) == 0 && !bbs_is_valid_callsign(arg)) {
            bbs_send_text(bbs, "*** SP requires valid callsign\r");
            bbs_send_prompt(bbs);
            bbs_log_response(cmd, "invalid callsign");
            return;
        }

        if (strcmp(cmd, CMD_SB) == 0) {
            bbs->pending_flags = BBS_MSG_FLAG_BULLETIN;
        } else if (strcmp(cmd, CMD_SP) == 0) {
            bbs->pending_flags = BBS_MSG_FLAG_PRIVATE;
        } else {
            bbs->pending_flags = bbs_is_valid_callsign(arg)
                                 ? BBS_MSG_FLAG_PRIVATE
                                 : BBS_MSG_FLAG_BULLETIN;
        }

        strlcpy(bbs->pending_to, to, sizeof(bbs->pending_to));
        bbs->pending_subject[0] = '\0';
        bbs->pending_body[0] = '\0';
        bbs->pending_body_len = 0;
        bbs->input_mode = BBS_INPUT_MODE_SUBJECT;
        bbs_send_text(bbs, "\rSubject: \r");
        snprintf(detail, sizeof(detail), "entering subject mode (to=%s)", bbs->pending_to);
        bbs_log_response(cmd, detail);
        return;
    } else if (strcmp(cmd, CMD_KILL) == 0) {
        uint32_t id = (uint32_t)strtoul(arg, NULL, 10);
        if (id == 0) {
            bbs_send_text(bbs, "Usage: K <n>\r");
            bbs_log_response(cmd, USAGE_ERROR);
        } else {
            bbs_log_response(cmd, cmd_kill(bbs, id));
        }
    } else if (strcmp(cmd, CMD_HEARD) == 0) {
        bbs_heard_entry_t tmp[BBS_HEARD_MAX];
        size_t n = bbs_heard_snapshot(bbs->heard, tmp, BBS_HEARD_MAX);
        uint32_t now_unix = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        bbs_txbuf_t tx;

        bbs_txbuf_init(&tx, bbs);
        bbs_txbuf_append_text(&tx, "Heard:\r  Callsign        Last heard\r");

        for (size_t i = 0; i < n; i++) {
            char line[64];
            char age[24];

            format_elapsed_since(tmp[i].last_heard_unix, age, sizeof(age));
            snprintf(line, sizeof(line), "  %-15s %s\r", tmp[i].callsign, age);
            bbs_txbuf_append_text(&tx, line);
        }

        if (n == 0) {
            bbs_txbuf_append_text(&tx, "  (none)\r");
        }

        bbs_txbuf_flush(&tx);

        snprintf(detail, sizeof(detail), "heard list entries=%u age_base=%lu",
                 (unsigned)n,
                 (unsigned long)now_unix);
        bbs_log_response(cmd, detail);
    } else if (strcmp(cmd, CMD_INFO) == 0) {
        char l[160];
        snprintf(l, sizeof(l), "BBS %s  Call:%s  Sysop:%s\r",
                 bbs->version_text, bbs->bbs_callsign, bbs->sysop_name);
        bbs_send_text(bbs, l);
        bbs_log_response(cmd, "station info returned");
    } else if (strcmp(cmd, CMD_CONFIG) == 0) {
        if (!ax25_cfg_is_initialized()) {
            bbs_send_text(bbs, "*** Config module not available\r");
            bbs_log_response(cmd, "module unavailable");
        } else if (!sysop_secret_is_empty(bbs) && !bbs->is_sysop) {
            if (sysop_auth_is_locked_out(&bbs->sysop_auth, bbs_now_ms())) {
                sysop_send_lockout_message(bbs);
                bbs_log_response(cmd, "locked out");
            } else {
                sysop_auth_set_deferred_config(&bbs->sysop_auth, true);
                sysop_issue_challenge(bbs);
                bbs_log_response(cmd, "config auth challenge issued");
                return;
            }
        } else {
            sysop_auth_set_deferred_config(&bbs->sysop_auth, false);
            if (sysop_secret_is_empty(bbs)) {
                bbs->is_sysop = true;
                sysop_auth_bypass_authenticate(&bbs->sysop_auth, bbs_now_ms());
            }
            bbs_enter_config_mode(bbs);
            bbs_log_response(cmd, "entered config mode");
            return;
        }
    } else if (strcmp(cmd, CMD_TERMINAL) == 0) {
        if (!bbs->uart_is_terminal_mode || bbs->uart_phy == NULL) {
            bbs_send_text(bbs, "*** Terminal mode requires uart.type=term\r");
            bbs_log_response(cmd, "uart.type not terminal");
        } else if (!sysop_secret_is_empty(bbs) && !bbs->is_sysop) {
            if (sysop_auth_is_locked_out(&bbs->sysop_auth, bbs_now_ms())) {
                sysop_send_lockout_message(bbs);
                bbs_log_response(cmd, "locked out");
            } else {
                sysop_auth_set_deferred_console(&bbs->sysop_auth, true);
                sysop_issue_challenge(bbs);
                bbs_log_response(cmd, "terminal auth challenge issued");
                return;
            }
        } else {
            sysop_auth_set_deferred_console(&bbs->sysop_auth, false);
            if (sysop_secret_is_empty(bbs)) {
                bbs->is_sysop = true;
                sysop_auth_bypass_authenticate(&bbs->sysop_auth, bbs_now_ms());
            }
            bbs_enter_terminal_mode(bbs);
            bbs_log_response(cmd, "entered terminal mode");
            return;
        }
    } else if (strcmp(cmd, CMD_BYE) == 0) {
        bbs_send_text(bbs, "*** Bye\r");
        bbs_log_response(cmd, "disconnecting");
        ax25_conn_shutdown(bbs->conn);
        return;
    } else if (strcmp(cmd, CMD_SYSOP) == 0) {
        sysop_auth_set_deferred_config(&bbs->sysop_auth, false);
        if (sysop_secret_is_empty(bbs)) {
            bbs->is_sysop = true;
            sysop_auth_bypass_authenticate(&bbs->sysop_auth, bbs_now_ms());
            bbs_send_text(bbs, "*** SYSOP authenticated\r");
            bbs_send_prompt(bbs);
            bbs_log_response(cmd, "empty secret bypass");
            return;
        }

        if (sysop_auth_is_locked_out(&bbs->sysop_auth, bbs_now_ms())) {
            sysop_send_lockout_message(bbs);
            bbs_send_prompt(bbs);
            bbs_log_response(cmd, "locked out");
            return;
        }

        if (bbs->is_sysop || sysop_auth_is_authenticated(&bbs->sysop_auth)) {
            bbs_send_text(bbs, "*** SYSOP already authenticated\r");
            bbs_send_prompt(bbs);
            bbs_log_response(cmd, "already authenticated");
            return;
        }

        if (equals_ignore_case_ascii(arg, "NEW") &&
            sysop_auth_is_awaiting_response(&bbs->sysop_auth)) {
            sysop_issue_challenge(bbs);
            bbs_log_response(cmd, "challenge refreshed");
            return;
        }

        if (sysop_auth_is_awaiting_response(&bbs->sysop_auth)) {
            bbs_send_text(bbs, "*** SYSOP challenge active (use SYSOP NEW to refresh)\r");
            bbs_send_prompt(bbs);
            bbs_log_response(cmd, "challenge already active");
            return;
        }

        sysop_issue_challenge(bbs);
        bbs_log_response(cmd, "challenge issued");
        return;
    } else {
        send_help_text(bbs);
        bbs_log_response(cmd, "unknown command help shown");
    }

    bbs_send_prompt(bbs);
}

static void bbs_process_input_line(bbs_t *bbs)
{
    bbs->line_buf[bbs->line_len] = '\0';

    if (bbs->input_mode == BBS_INPUT_MODE_SUBJECT) {
        strlcpy(bbs->pending_subject,
                bbs->line_buf[0] ? bbs->line_buf : "(no subject)",
                sizeof(bbs->pending_subject));
        bbs->input_mode = BBS_INPUT_MODE_BODY;
        char info[80];
        size_t free_bytes = 0;
        bbs_storage_free_bytes(bbs->storage, &free_bytes);
        snprintf(info, sizeof(info),
                 "Max message size: %u bytes, Total storage free: %u bytes\r",
                 (unsigned)(sizeof(bbs->pending_body) - 1),
                 (unsigned)free_bytes);
        bbs_send_text(bbs, info);
        bbs_send_text(bbs, "Enter body, use /EX to finish:\r");
    } else if (bbs->input_mode == BBS_INPUT_MODE_BODY) {
        if (equals_ignore_case_ascii(bbs->line_buf, "/EX")) {
            finish_post(bbs);
        } else {
            size_t remain = sizeof(bbs->pending_body) - 1 - bbs->pending_body_len;
            size_t copy = strlen(bbs->line_buf);
            if (remain <= 2) {
                bbs_send_text(bbs, "*** Body full, use /EX to finish\r");
            } else if (copy > remain - 2) {
                copy = remain - 2;
                memcpy(bbs->pending_body + bbs->pending_body_len, bbs->line_buf, copy);
                bbs->pending_body_len += copy;
                bbs->pending_body[bbs->pending_body_len++] = '\r';
                bbs->pending_body[bbs->pending_body_len] = '\0';
                bbs_send_text(bbs, "*** Body limit reached, use /EX to finish\r");
            } else {
                memcpy(bbs->pending_body + bbs->pending_body_len, bbs->line_buf, copy);
                bbs->pending_body_len += copy;
                bbs->pending_body[bbs->pending_body_len++] = '\r';
                bbs->pending_body[bbs->pending_body_len] = '\0';
            }
        }
    } else if (bbs->input_mode == BBS_INPUT_MODE_SYSOP_RESPONSE) {
        handle_sysop_response(bbs, bbs->line_buf);
    } else if (bbs->input_mode == BBS_INPUT_MODE_CONFIG) {
        bool exit_cfg = ax25_cfg_handle_command(bbs->line_buf);
        if (exit_cfg) {
            bbs->input_mode = BBS_INPUT_MODE_COMMAND;
            bbs_send_text(bbs, "*** Exiting config mode\r");
            bbs_send_prompt(bbs);
        } else {
            bbs_send_prompt(bbs);
        }
    } else if (bbs->input_mode == BBS_INPUT_MODE_TERMINAL) {
        /* /exit — return to command mode */
        if (equals_ignore_case_ascii(bbs->line_buf, "/EXIT")) {
            bbs->line_len = 0;
            bbs_exit_terminal_mode(bbs);
            return;
        }

        /* /reset — send KISS reset frame bytes C0 FF C0 */
        if (equals_ignore_case_ascii(bbs->line_buf, "/RESET")) {
            static const uint8_t kiss_reset[] = {0xC0, 0xFF, 0xC0};
            esp_err_t err = ax25_phy_kiss_uart_write_raw(kiss_reset, sizeof(kiss_reset), bbs->uart_phy);
            if (err == ESP_OK) {
                bbs_send_text(bbs, "*** KISS reset sent\r");
            } else {
                bbs_send_text(bbs, "*** KISS reset failed\r");
            }
            bbs_send_prompt(bbs);
            bbs->line_len = 0;
            return;
        }

        /* Forward line + CR to TNC */
        size_t line_len = strlen(bbs->line_buf);
        if (line_len > 0) {
            esp_err_t err = ax25_phy_kiss_uart_write_raw(
                (const uint8_t *)bbs->line_buf,
                line_len,
                bbs->uart_phy);
            if (err != ESP_OK) {
                bbs_send_text(bbs, "*** UART write failed\r");
                bbs_send_prompt(bbs);
                bbs->line_len = 0;
                return;
            }
        }
        static const uint8_t eol[] = {'\r'};
        esp_err_t err = ax25_phy_kiss_uart_write_raw(eol, sizeof(eol), bbs->uart_phy);
        if (err != ESP_OK) {
            bbs_send_text(bbs, "*** UART write failed\r");
            bbs_send_prompt(bbs);
        }
        /* No local prompt echo — TNC response will arrive via tap callback */
    } else {
        process_command(bbs, bbs->line_buf);
    }

    bbs->line_len = 0;
}

void bbs_send_banner(bbs_t *bbs)
{
    bbs_txbuf_t tx;

    bbs_txbuf_init(&tx, bbs);
    bbs_txbuf_append(&tx, (const uint8_t *)bbs->greeting_text, strlen(bbs->greeting_text));
    bbs_txbuf_append_text(&tx, CR);

    uint32_t last_read = 0;
    nvs_get_last_read(bbs, bbs->remote_callsign, &last_read);

    uint32_t total = 0;
    uint32_t unread = 0;

    for (size_t i = 0; i < bbs->storage->count; i++) {
        const bbs_message_index_t *rec = &bbs->storage->records[i];
        bool for_user = ((rec->flags & BBS_MSG_FLAG_BULLETIN) != 0) ||
                        (strcmp(rec->to, bbs->remote_callsign) == 0);
        if (!for_user) {
            continue;
        }
        total++;
        if (rec->id > last_read) {
            unread++;
        }
    }

    char line[128];
    if (total == 0) {
        snprintf(line, sizeof(line), "*** No messages for %s\r", bbs->remote_callsign);
    } else {
        snprintf(line, sizeof(line), "*** You have %lu messages (%lu new)\r",
                 (unsigned long)total, (unsigned long)unread);
    }

    bbs_txbuf_append(&tx, (const uint8_t *)line, strlen(line));
    bbs_txbuf_append_prompt(&tx);
    bbs_txbuf_flush(&tx);
}

bool bbs_has_unread_messages(bbs_t *bbs)
{
    if (bbs == NULL || !bbs->initialized || bbs->storage == NULL || bbs->remote_callsign[0] == '\0') {
        return false;
    }

    uint32_t last_read = 0;
    esp_err_t err = nvs_get_last_read(bbs, bbs->remote_callsign, &last_read);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }

    for (size_t i = 0; i < bbs->storage->count; i++) {
        const bbs_message_index_t *rec = &bbs->storage->records[i];
        bool for_user = ((rec->flags & BBS_MSG_FLAG_BULLETIN) != 0) ||
                        (strcmp(rec->to, bbs->remote_callsign) == 0);
        if (!for_user) {
            continue;
        }

        if (rec->id > last_read) {
            return true;
        }
    }

    return false;
}

esp_err_t bbs_init(bbs_t *bbs, const bbs_config_t *cfg, ax25_conn_t *conn)
{
    if (bbs == NULL || cfg == NULL || conn == NULL || cfg->storage == NULL || cfg->heard == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(bbs, 0, sizeof(*bbs));

    bbs->conn = conn;
    bbs->storage = cfg->storage;
    bbs->heard = cfg->heard;
    bbs->uart_phy = cfg->uart_phy;
    bbs->uart_is_terminal_mode = cfg->uart_is_terminal_mode;
    const sysop_auth_config_t auth_cfg = {
        .secret = cfg->sysop_secret,
        .challenge_timeout_ms = cfg->sysop_challenge_timeout_ms,
        .session_timeout_ms = cfg->sysop_session_timeout_ms,
        .lockout_ms = cfg->sysop_lockout_ms,
        .max_attempts = cfg->sysop_max_attempts,
        .secret_min_len = cfg->sysop_secret_min_len,
        .secret_max_len = cfg->sysop_secret_max_len,
    };

    if (sysop_auth_configure(&bbs->sysop_auth, &auth_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid bbs.sysop_secret: must be empty or printable ASCII with len %u..%u",
                 (unsigned)bbs->sysop_auth.secret_min_len,
                 (unsigned)bbs->sysop_auth.secret_max_len);
        return ESP_ERR_INVALID_ARG;
    }

    bbs_normalize_full_callsign(cfg->bbs_callsign ? cfg->bbs_callsign : "",
                                bbs->bbs_callsign,
                                sizeof(bbs->bbs_callsign));
    snprintf(bbs->greeting_text, sizeof(bbs->greeting_text), "%s",
             cfg->greeting_text ? cfg->greeting_text : CONFIG_BBS_GREETING_TEXT);
    snprintf(bbs->prompt_text, sizeof(bbs->prompt_text), "%s",
             cfg->prompt_text ? cfg->prompt_text : CONFIG_BBS_PROMPT_TEXT);
    snprintf(bbs->config_prompt_text, sizeof(bbs->config_prompt_text), "%s",
             cfg->config_prompt_text ? cfg->config_prompt_text : CONFIG_BBS_CONFIG_PROMPT_TEXT);
    snprintf(bbs->terminal_prompt_text, sizeof(bbs->terminal_prompt_text), "%s",
             cfg->terminal_prompt_text ? cfg->terminal_prompt_text : CONFIG_BBS_TERMINAL_PROMPT_TEXT);
    snprintf(bbs->sysop_name, sizeof(bbs->sysop_name), "%s",
             cfg->sysop_name ? cfg->sysop_name : CONFIG_BBS_SYSOP_NAME);
    snprintf(bbs->version_text, sizeof(bbs->version_text), "%s",
             cfg->version_text ? cfg->version_text : CONFIG_BBS_VERSION_TEXT);
    snprintf(bbs->nvs_namespace, sizeof(bbs->nvs_namespace), "%s",
             cfg->nvs_namespace ? cfg->nvs_namespace : "bbs");

    esp_err_t err = nvs_open(bbs->nvs_namespace, NVS_READWRITE, &bbs->nvs);
    if (err != ESP_OK) {
        return err;
    }

    bbs->input_mode = BBS_INPUT_MODE_COMMAND;
    bbs->initialized = true;

    ESP_LOGI(TAG, "BBS initialized as %s", bbs->bbs_callsign);
    return ESP_OK;
}

void bbs_deinit(bbs_t *bbs)
{
    if (bbs == NULL || !bbs->initialized) {
        return;
    }

    nvs_close(bbs->nvs);
    bbs->nvs = 0;
    bbs->initialized = false;
}

void bbs_on_connect(bbs_t *bbs, const ax25_address_t *remote_addr, bool is_local_initiated)
{
    (void)is_local_initiated;

    char call[16];
    ax25_address_to_string(remote_addr, call, sizeof(call));
    bbs_base_callsign(call, bbs->remote_callsign, sizeof(bbs->remote_callsign));

    bbs->connected = true;
    bbs->is_sysop = false;
    sysop_auth_reset_session(&bbs->sysop_auth);
    bbs->input_mode = BBS_INPUT_MODE_COMMAND;
    bbs->line_len = 0;
    bbs->suppress_lf_after_cr = false;

    bbs_send_banner(bbs);
}

void bbs_on_disconnect(bbs_t *bbs)
{
    bbs->connected = false;
    bbs->is_sysop = false;
    sysop_auth_reset_session(&bbs->sysop_auth);
    bbs->line_len = 0;
    bbs->suppress_lf_after_cr = false;
    if (bbs->input_mode == BBS_INPUT_MODE_TERMINAL && bbs->uart_phy != NULL) {
        ax25_phy_kiss_uart_clear_rx_tap(bbs->uart_phy);
    }
    bbs->input_mode = BBS_INPUT_MODE_COMMAND;
}

void bbs_on_data(bbs_t *bbs, const uint8_t *data, size_t len)
{
    if (bbs == NULL || data == NULL || len == 0 || !bbs->connected) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];

        if (bbs->input_mode == BBS_INPUT_MODE_BODY && (uint8_t)c == 0x1A) {
            finish_post(bbs);
            continue;
        }

        if (c == '\r') {
            bbs_process_input_line(bbs);
            bbs->suppress_lf_after_cr = true;
            continue;
        }

        if (c == '\n') {
            if (bbs->suppress_lf_after_cr) {
                bbs->suppress_lf_after_cr = false;
                continue;
            }

            bbs_process_input_line(bbs);
            continue;
        }

        bbs->suppress_lf_after_cr = false;

        if (bbs->line_len < sizeof(bbs->line_buf) - 1) {
            bbs->line_buf[bbs->line_len++] = c;
        }
    }
}
