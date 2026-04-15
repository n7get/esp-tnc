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

#ifndef SYSOP_AUTH_H
#define SYSOP_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifndef CONFIG_BBS_SYSOP_CHALLENGE_TIMEOUT_SEC
#define CONFIG_BBS_SYSOP_CHALLENGE_TIMEOUT_SEC 300
#endif

#ifndef CONFIG_BBS_SYSOP_SESSION_TIMEOUT_SEC
#define CONFIG_BBS_SYSOP_SESSION_TIMEOUT_SEC 600
#endif

#ifndef CONFIG_BBS_SYSOP_LOCKOUT_SEC
#define CONFIG_BBS_SYSOP_LOCKOUT_SEC 900
#endif

#ifndef CONFIG_BBS_SYSOP_MAX_ATTEMPTS
#define CONFIG_BBS_SYSOP_MAX_ATTEMPTS 3
#endif

#ifndef CONFIG_BBS_SYSOP_SECRET_MIN_LEN
#define CONFIG_BBS_SYSOP_SECRET_MIN_LEN 16
#endif

#ifndef CONFIG_BBS_SYSOP_SECRET_MAX_LEN
#define CONFIG_BBS_SYSOP_SECRET_MAX_LEN 256
#endif

#define BBS_SYSOP_CHALLENGE_INDEX_COUNT 4
#define BBS_SYSOP_RESPONSE_LEN 6

typedef enum {
    BBS_SYSOP_AUTH_IDLE = 0,
    BBS_SYSOP_AUTH_AWAITING_RESPONSE,
    BBS_SYSOP_AUTH_AUTHENTICATED,
    BBS_SYSOP_AUTH_LOCKED_OUT,
} bbs_sysop_auth_mode_t;

typedef struct {
    const char *secret;
    uint32_t challenge_timeout_ms;
    uint32_t session_timeout_ms;
    uint32_t lockout_ms;
    uint8_t max_attempts;
    uint16_t secret_min_len;
    uint16_t secret_max_len;
} sysop_auth_config_t;

typedef struct {
    bbs_sysop_auth_mode_t mode;
    char secret[CONFIG_BBS_SYSOP_SECRET_MAX_LEN + 1];
    uint16_t challenge_indices[BBS_SYSOP_CHALLENGE_INDEX_COUNT];
    uint32_t challenge_id_next;
    uint32_t active_challenge_id;
    uint32_t challenge_issued_ms;
    uint32_t challenge_expires_ms;
    uint32_t lockout_until_ms;
    uint32_t last_activity_ms;
    uint32_t challenge_timeout_ms;
    uint32_t session_timeout_ms;
    uint32_t lockout_ms;
    uint16_t secret_min_len;
    uint16_t secret_max_len;
    uint8_t failed_attempts;
    uint8_t max_attempts;
    bool deferred_config_entry;
    bool deferred_console_entry;
} sysop_auth_state_t;

typedef enum {
    SYSOP_AUTH_VERIFY_SUCCESS = 0,
    SYSOP_AUTH_VERIFY_FAILED,
    SYSOP_AUTH_VERIFY_LOCKED_OUT,
    SYSOP_AUTH_VERIFY_EXPIRED,
    SYSOP_AUTH_VERIFY_NO_ACTIVE,
} sysop_auth_verify_result_t;

esp_err_t sysop_auth_configure(sysop_auth_state_t *state, const sysop_auth_config_t *cfg);
void sysop_auth_reset_session(sysop_auth_state_t *state);
bool sysop_auth_secret_is_empty(const sysop_auth_state_t *state);
bool sysop_auth_is_locked_out(sysop_auth_state_t *state, uint32_t now_ms);
bool sysop_auth_is_authenticated(const sysop_auth_state_t *state);
bool sysop_auth_is_awaiting_response(const sysop_auth_state_t *state);
void sysop_auth_note_activity(sysop_auth_state_t *state, uint32_t now_ms);
bool sysop_auth_expire_authenticated_if_idle(sysop_auth_state_t *state, uint32_t now_ms);
void sysop_auth_bypass_authenticate(sysop_auth_state_t *state, uint32_t now_ms);
unsigned sysop_auth_lockout_minutes(const sysop_auth_state_t *state);
uint8_t sysop_auth_failed_attempts(const sysop_auth_state_t *state);
uint8_t sysop_auth_max_attempts(const sysop_auth_state_t *state);
void sysop_auth_set_deferred_config(sysop_auth_state_t *state, bool pending);
bool sysop_auth_take_deferred_config(sysop_auth_state_t *state);
void sysop_auth_set_deferred_console(sysop_auth_state_t *state, bool pending);
bool sysop_auth_take_deferred_console(sysop_auth_state_t *state);

esp_err_t sysop_auth_issue_challenge(sysop_auth_state_t *state,
                                     uint32_t now_ms,
                                     char *out_line,
                                     size_t out_line_len,
                                     uint32_t *out_challenge_id);

sysop_auth_verify_result_t sysop_auth_submit_response(sysop_auth_state_t *state,
                                                      const char *response,
                                                      uint32_t now_ms);

#endif
