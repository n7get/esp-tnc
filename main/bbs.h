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

#ifndef BBS_H
#define BBS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "nvs.h"
#include "ax25_conn.h"
#include "ax25_address.h"
#include "ax25_phy_kiss_uart.h"
#include "bbs_storage.h"
#include "bbs_heard.h"
#include "sysop_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_BBS_GREETING_TEXT
#define CONFIG_BBS_GREETING_TEXT "Welcome to ESP-TNC BBS"
#endif

#ifndef CONFIG_BBS_SYSOP_NAME
#define CONFIG_BBS_SYSOP_NAME "SYSOP"
#endif

#ifndef CONFIG_BBS_VERSION_TEXT
#define CONFIG_BBS_VERSION_TEXT "ESP-TNC BBS 0.1"
#endif

#ifndef CONFIG_BBS_PROMPT_TEXT
#define CONFIG_BBS_PROMPT_TEXT "BBS READY>"
#endif

#ifndef CONFIG_BBS_CONFIG_PROMPT_TEXT
#define CONFIG_BBS_CONFIG_PROMPT_TEXT "CONFIG READY>"
#endif

#ifndef CONFIG_BBS_TERMINAL_PROMPT_TEXT
#define CONFIG_BBS_TERMINAL_PROMPT_TEXT "TERM READY>"
#endif

#ifndef CONFIG_BBS_MAX_BODY_LEN
#define CONFIG_BBS_MAX_BODY_LEN 1024
#endif

typedef struct {
    const char *bbs_callsign;
    const char *greeting_text;
    const char *prompt_text;
    const char *config_prompt_text;
    const char *terminal_prompt_text;
    const char *sysop_name;
    const char *version_text;
    const char *nvs_namespace;
    const char *sysop_secret;
    uint32_t sysop_challenge_timeout_ms;
    uint32_t sysop_session_timeout_ms;
    uint32_t sysop_lockout_ms;
    uint8_t sysop_max_attempts;
    uint16_t sysop_secret_min_len;
    uint16_t sysop_secret_max_len;
    bbs_storage_t *storage;
    bbs_heard_t *heard;
    ax25_phy_kiss_uart_t *uart_phy;
    bool uart_is_terminal_mode;
} bbs_config_t;

typedef enum {
    BBS_INPUT_MODE_COMMAND = 0,
    BBS_INPUT_MODE_SUBJECT,
    BBS_INPUT_MODE_BODY,
    BBS_INPUT_MODE_SYSOP_RESPONSE,
    BBS_INPUT_MODE_CONFIG,
    BBS_INPUT_MODE_TERMINAL,
} bbs_input_mode_t;

typedef struct {
    bool initialized;
    bool connected;
    bool is_sysop;
    bbs_input_mode_t input_mode;

    ax25_conn_t *conn;
    bbs_storage_t *storage;
    bbs_heard_t *heard;

    char bbs_callsign[16];
    char remote_callsign[7];
    char greeting_text[96];
    char prompt_text[32];
    char config_prompt_text[32];
    char terminal_prompt_text[32];
    char sysop_name[32];
    char version_text[32];
    char nvs_namespace[16];

    char line_buf[256];
    size_t line_len;
    bool suppress_lf_after_cr;

    char pending_to[7];
    uint8_t pending_flags;
    char pending_subject[BBS_SUBJECT_LEN + 1];
    char pending_body[CONFIG_BBS_MAX_BODY_LEN + 1];
    size_t pending_body_len;

    /* Scratch buffer used by read/display paths to avoid large task-stack locals. */
    char read_body_buf[CONFIG_BBS_MAX_BODY_LEN + 64];

    sysop_auth_state_t sysop_auth;
    nvs_handle_t nvs;

    ax25_phy_kiss_uart_t *uart_phy;
    bool uart_is_terminal_mode;
} bbs_t;

esp_err_t bbs_init(bbs_t *bbs, const bbs_config_t *cfg, ax25_conn_t *conn);
void bbs_deinit(bbs_t *bbs);

void bbs_on_connect(bbs_t *bbs, const ax25_address_t *remote_addr, bool is_local_initiated);
void bbs_on_disconnect(bbs_t *bbs);
void bbs_on_data(bbs_t *bbs, const uint8_t *data, size_t len);

void bbs_send_banner(bbs_t *bbs);
bool bbs_has_unread_messages(bbs_t *bbs);
bool bbs_is_valid_callsign(const char *str);
void bbs_base_callsign(const char *src, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
