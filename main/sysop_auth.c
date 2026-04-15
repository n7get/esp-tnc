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

#include "sysop_auth.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"

static bool sysop_secret_is_valid(const sysop_auth_state_t *state, const char *secret)
{
    if (state == NULL || secret == NULL) {
        return false;
    }

    size_t len = strlen(secret);
    if (len == 0) {
        return true;
    }
    if (len < state->secret_min_len || len > state->secret_max_len) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)secret[i];
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

static void sysop_clear_challenge(sysop_auth_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->active_challenge_id = 0;
    state->challenge_issued_ms = 0;
    state->challenge_expires_ms = 0;
    memset(state->challenge_indices, 0, sizeof(state->challenge_indices));
    if (state->mode == BBS_SYSOP_AUTH_AWAITING_RESPONSE) {
        state->mode = BBS_SYSOP_AUTH_IDLE;
    }
}

static bool sysop_generate_challenge_indices(size_t secret_len, uint16_t out_indices[4])
{
    if (out_indices == NULL || secret_len < BBS_SYSOP_CHALLENGE_INDEX_COUNT) {
        return false;
    }

    for (size_t i = 0; i < BBS_SYSOP_CHALLENGE_INDEX_COUNT; i++) {
        uint16_t idx = 0;
        bool unique = false;

        while (!unique) {
            idx = (uint16_t)((esp_random() % secret_len) + 1);
            unique = true;
            for (size_t j = 0; j < i; j++) {
                if (out_indices[j] == idx) {
                    unique = false;
                    break;
                }
            }
        }

        out_indices[i] = idx;
    }

    return true;
}

static bool sysop_verify_response(const sysop_auth_state_t *state, const char *response)
{
    if (state == NULL || response == NULL || state->active_challenge_id == 0) {
        return false;
    }

    size_t r_len = strlen(response);
    if (r_len != BBS_SYSOP_RESPONSE_LEN) {
        return false;
    }

    char required[BBS_SYSOP_CHALLENGE_INDEX_COUNT];
    bool matched[BBS_SYSOP_CHALLENGE_INDEX_COUNT] = {false, false, false, false};
    bool used[BBS_SYSOP_RESPONSE_LEN] = {false, false, false, false, false, false};

    for (size_t i = 0; i < BBS_SYSOP_CHALLENGE_INDEX_COUNT; i++) {
        uint16_t idx = state->challenge_indices[i];
        if (idx == 0) {
            return false;
        }
        required[i] = state->secret[idx - 1];
    }

    for (size_t i = 0; i < BBS_SYSOP_CHALLENGE_INDEX_COUNT; i++) {
        int found = -1;
        for (size_t j = 0; j < BBS_SYSOP_RESPONSE_LEN; j++) {
            if (found < 0 && !used[j] && response[j] == required[i]) {
                found = (int)j;
            }
        }
        if (found >= 0) {
            used[found] = true;
            matched[i] = true;
        }
    }

    bool ok = true;
    for (size_t i = 0; i < BBS_SYSOP_CHALLENGE_INDEX_COUNT; i++) {
        ok = ok && matched[i];
    }
    return ok;
}

esp_err_t sysop_auth_configure(sysop_auth_state_t *state, const sysop_auth_config_t *cfg)
{
    if (state == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));

    state->challenge_timeout_ms = cfg->challenge_timeout_ms;
    state->session_timeout_ms = cfg->session_timeout_ms;
    state->lockout_ms = cfg->lockout_ms;
    state->max_attempts = cfg->max_attempts;
    state->secret_min_len = cfg->secret_min_len;
    state->secret_max_len = cfg->secret_max_len;

    if (state->challenge_timeout_ms == 0) {
        state->challenge_timeout_ms = CONFIG_BBS_SYSOP_CHALLENGE_TIMEOUT_SEC * 1000U;
    }
    if (state->session_timeout_ms == 0) {
        state->session_timeout_ms = CONFIG_BBS_SYSOP_SESSION_TIMEOUT_SEC * 1000U;
    }
    if (state->lockout_ms == 0) {
        state->lockout_ms = CONFIG_BBS_SYSOP_LOCKOUT_SEC * 1000U;
    }
    if (state->max_attempts == 0) {
        state->max_attempts = CONFIG_BBS_SYSOP_MAX_ATTEMPTS;
    }
    if (state->secret_min_len == 0) {
        state->secret_min_len = CONFIG_BBS_SYSOP_SECRET_MIN_LEN;
    }
    if (state->secret_max_len == 0) {
        state->secret_max_len = CONFIG_BBS_SYSOP_SECRET_MAX_LEN;
    }
    if (state->secret_max_len > CONFIG_BBS_SYSOP_SECRET_MAX_LEN) {
        state->secret_max_len = CONFIG_BBS_SYSOP_SECRET_MAX_LEN;
    }

    strlcpy(state->secret,
            cfg->secret != NULL ? cfg->secret : "",
            sizeof(state->secret));

    if (!sysop_secret_is_valid(state, state->secret)) {
        return ESP_ERR_INVALID_ARG;
    }

    state->mode = BBS_SYSOP_AUTH_IDLE;
    return ESP_OK;
}

void sysop_auth_reset_session(sysop_auth_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->mode = BBS_SYSOP_AUTH_IDLE;
    state->failed_attempts = 0;
    state->lockout_until_ms = 0;
    state->last_activity_ms = 0;
    state->deferred_config_entry = false;
    state->deferred_console_entry = false;
    sysop_clear_challenge(state);
}

bool sysop_auth_secret_is_empty(const sysop_auth_state_t *state)
{
    return state == NULL || state->secret[0] == '\0';
}

bool sysop_auth_is_locked_out(sysop_auth_state_t *state, uint32_t now_ms)
{
    if (state == NULL || state->mode != BBS_SYSOP_AUTH_LOCKED_OUT) {
        return false;
    }

    if (now_ms >= state->lockout_until_ms) {
        state->mode = BBS_SYSOP_AUTH_IDLE;
        state->failed_attempts = 0;
        state->lockout_until_ms = 0;
        return false;
    }

    return true;
}

bool sysop_auth_is_authenticated(const sysop_auth_state_t *state)
{
    return state != NULL && state->mode == BBS_SYSOP_AUTH_AUTHENTICATED;
}

bool sysop_auth_is_awaiting_response(const sysop_auth_state_t *state)
{
    return state != NULL && state->mode == BBS_SYSOP_AUTH_AWAITING_RESPONSE;
}

void sysop_auth_bypass_authenticate(sysop_auth_state_t *state, uint32_t now_ms)
{
    if (state == NULL) {
        return;
    }
    state->failed_attempts = 0;
    state->mode = BBS_SYSOP_AUTH_AUTHENTICATED;
    state->last_activity_ms = now_ms;
}

unsigned sysop_auth_lockout_minutes(const sysop_auth_state_t *state)
{
    if (state == NULL || state->lockout_ms == 0) {
        return 1;
    }
    unsigned long mins = (state->lockout_ms + 59999U) / 60000U;
    return (unsigned)(mins == 0 ? 1 : mins);
}

uint8_t sysop_auth_failed_attempts(const sysop_auth_state_t *state)
{
    return state != NULL ? state->failed_attempts : 0;
}

uint8_t sysop_auth_max_attempts(const sysop_auth_state_t *state)
{
    return state != NULL ? state->max_attempts : 0;
}

void sysop_auth_set_deferred_config(sysop_auth_state_t *state, bool pending)
{
    if (state != NULL) {
        state->deferred_config_entry = pending;
    }
}

bool sysop_auth_take_deferred_config(sysop_auth_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    bool v = state->deferred_config_entry;
    state->deferred_config_entry = false;
    return v;
}

void sysop_auth_set_deferred_console(sysop_auth_state_t *state, bool pending)
{
    if (state != NULL) {
        state->deferred_console_entry = pending;
    }
}

bool sysop_auth_take_deferred_console(sysop_auth_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    bool v = state->deferred_console_entry;
    state->deferred_console_entry = false;
    return v;
}

void sysop_auth_note_activity(sysop_auth_state_t *state, uint32_t now_ms)
{
    if (state == NULL || state->mode != BBS_SYSOP_AUTH_AUTHENTICATED) {
        return;
    }

    state->last_activity_ms = now_ms;
}

bool sysop_auth_expire_authenticated_if_idle(sysop_auth_state_t *state, uint32_t now_ms)
{
    if (state == NULL || state->mode != BBS_SYSOP_AUTH_AUTHENTICATED) {
        return false;
    }

    if ((now_ms - state->last_activity_ms) > state->session_timeout_ms) {
        state->mode = BBS_SYSOP_AUTH_IDLE;
        state->last_activity_ms = 0;
        return true;
    }

    return false;
}

esp_err_t sysop_auth_issue_challenge(sysop_auth_state_t *state,
                                     uint32_t now_ms,
                                     char *out_line,
                                     size_t out_line_len,
                                     uint32_t *out_challenge_id)
{
    if (state == NULL || out_line == NULL || out_line_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t secret_len = strlen(state->secret);
    if (secret_len < BBS_SYSOP_CHALLENGE_INDEX_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!sysop_generate_challenge_indices(secret_len, state->challenge_indices)) {
        return ESP_FAIL;
    }

    state->challenge_id_next++;
    if (state->challenge_id_next == 0) {
        state->challenge_id_next = 1;
    }

    state->active_challenge_id = state->challenge_id_next;
    state->challenge_issued_ms = now_ms;
    state->challenge_expires_ms = now_ms + state->challenge_timeout_ms;
    state->mode = BBS_SYSOP_AUTH_AWAITING_RESPONSE;

    snprintf(out_line, out_line_len, "CHAL %u %u %u %u\r",
             (unsigned)state->challenge_indices[0],
             (unsigned)state->challenge_indices[1],
             (unsigned)state->challenge_indices[2],
             (unsigned)state->challenge_indices[3]);

    if (out_challenge_id != NULL) {
        *out_challenge_id = state->active_challenge_id;
    }

    return ESP_OK;
}

sysop_auth_verify_result_t sysop_auth_submit_response(sysop_auth_state_t *state,
                                                      const char *response,
                                                      uint32_t now_ms)
{
    if (state == NULL || response == NULL) {
        return SYSOP_AUTH_VERIFY_NO_ACTIVE;
    }

    if (state->active_challenge_id == 0 || state->mode != BBS_SYSOP_AUTH_AWAITING_RESPONSE) {
        return SYSOP_AUTH_VERIFY_NO_ACTIVE;
    }

    if (now_ms > state->challenge_expires_ms) {
        sysop_clear_challenge(state);
        return SYSOP_AUTH_VERIFY_EXPIRED;
    }

    bool ok = sysop_verify_response(state, response);
    sysop_clear_challenge(state);

    if (ok) {
        state->failed_attempts = 0;
        state->mode = BBS_SYSOP_AUTH_AUTHENTICATED;
        state->last_activity_ms = now_ms;
        return SYSOP_AUTH_VERIFY_SUCCESS;
    }

    state->failed_attempts++;
    if (state->failed_attempts >= state->max_attempts) {
        state->mode = BBS_SYSOP_AUTH_LOCKED_OUT;
        state->lockout_until_ms = now_ms + state->lockout_ms;
        return SYSOP_AUTH_VERIFY_LOCKED_OUT;
    }

    state->mode = BBS_SYSOP_AUTH_IDLE;
    return SYSOP_AUTH_VERIFY_FAILED;
}
