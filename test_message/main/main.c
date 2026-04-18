// Copyright (C) 2026 Robert Ambrose N7GET
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "buffer.h"
#include "ax25_address.h"
#include "ax25_config.h"
#include "ax25_conn.h"
#include "ax25_frame.h"
#include "ax25_router.h"
#if CONFIG_TEST_MSG_KISS_TCP
#include "ax25_phy_kiss_tcp_client.h"
#include "ax25_wifi.h"
#elif CONFIG_TEST_MSG_KISS_UART
#include "ax25_phy_kiss_uart.h"
#else
#error "Either CONFIG_TEST_MSG_KISS_TCP or CONFIG_TEST_MSG_KISS_UART must be enabled"
#endif

#define TAG "TEST_MSG"
#define RX_CHUNK_MAX AX25_MAX_INFO_LEN
#define RX_QUEUE_LEN 32
#define RX_BUF_SIZE 768
#define TEST_SEND_RETRY_MAX 200
#define TEST_SEND_RETRY_DELAY_MS 10
#define PHY_CONNECT_POLL_MS 50
#define CAPTURE_MAX 6144
#define MSG_BODY_MAX CONFIG_TEST_MSG_MAX_BODY_SIZE
#define EVT_CONNECTED BIT0
#define EVT_DISCONNECTED BIT1
#define EVT_FINISHED BIT2

static const char *const PROMPT_TOKENS[] = {
    "BBS READY>", "BBS READY:", "BBS> "
};

typedef struct {
    size_t len;
    uint8_t data[RX_CHUNK_MAX];
} rx_chunk_t;

typedef struct {
    bool pass;
    const char *name;
    char detail[120];
} test_result_t;

typedef struct {
    uint32_t id;
    char body[MSG_BODY_MAX + 1];
    size_t body_len;
} msg_entry_t;

typedef struct {
    ax25_router_port_t app_port;
    ax25_conn_t conn;
    ax25_address_t remote_addr;
    EventGroupHandle_t events;
    QueueHandle_t rx_queue;
    TickType_t deadline;
    buffer_t *rx_buf;
    uint32_t tests_run;
    uint32_t tests_failed;
    msg_entry_t messages[CONFIG_TEST_MSG_MESSAGE_COUNT];
    size_t bbs_body_limit;
    char capture[CAPTURE_MAX];
    test_result_t last;
} app_ctx_t;

static app_ctx_t s_ctx;


// --- Connection, router, and buffer helpers (verbatim from test_bbs) ---
static void log_router_send_result(const char *path, esp_err_t err) {
    if (err == ESP_OK) return;
    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "%s: router queue full", path);
        return;
    }
    ESP_LOGW(TAG, "%s: router send failed: %s", path, esp_err_to_name(err));
}

static esp_err_t send_phy_frame_with_retry(const ax25_frame_t *frame, void *user_data) {
    esp_err_t err = ESP_ERR_INVALID_ARG;
    for (int attempt = 0; attempt < TEST_SEND_RETRY_MAX; attempt++) {
#if CONFIG_TEST_MSG_KISS_TCP
        err = ax25_phy_kiss_tcp_client_send(frame, (ax25_phy_kiss_tcp_client_t *)user_data);
#elif CONFIG_TEST_MSG_KISS_UART
        err = ax25_phy_kiss_uart_send(frame, (ax25_phy_kiss_uart_t *)user_data);
#endif
        if (err == ESP_OK) return ESP_OK;
        if (err != ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "PHY send failed: %s", esp_err_to_name(err));
            return err;
        }
        if (attempt == 0) ESP_LOGW(TAG, "PHY TX queue full, retrying");
        vTaskDelay(pdMS_TO_TICKS(TEST_SEND_RETRY_DELAY_MS));
    }
    ESP_LOGW(TAG, "PHY TX queue remained full after %d retries", TEST_SEND_RETRY_MAX);
    return err;
}

static esp_err_t send_conn_data_with_retry(ax25_conn_t *conn, const uint8_t *data, size_t len) {
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!conn || !data || len == 0) return ESP_ERR_INVALID_ARG;
    for (int attempt = 0; attempt < TEST_SEND_RETRY_MAX; attempt++) {
        err = ax25_conn_send_data(conn, data, len);
        if (err == ESP_OK) return ESP_OK;
        if (err == ESP_ERR_INVALID_STATE) return err;
        if (err != ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "AX.25 send failed: %s", esp_err_to_name(err));
            return err;
        }
        if (attempt == 0) ESP_LOGW(TAG, "AX.25 TX queue full, retrying");
        vTaskDelay(pdMS_TO_TICKS(TEST_SEND_RETRY_DELAY_MS));
    }
    ESP_LOGW(TAG, "AX.25 TX queue remained full after %d retries", TEST_SEND_RETRY_MAX);
    return err;
}

#if CONFIG_TEST_MSG_KISS_TCP
static bool tcp_phy_is_connected(ax25_phy_kiss_tcp_client_t *phy) {
    bool connected;
    if (!phy || !phy->send_mutex) return false;
    if (xSemaphoreTake(phy->send_mutex, pdMS_TO_TICKS(PHY_CONNECT_POLL_MS)) != pdTRUE) return false;
    connected = (phy->sock >= 0);
    xSemaphoreGive(phy->send_mutex);
    return connected;
}
static bool wait_for_tcp_phy_connected(ax25_phy_kiss_tcp_client_t *phy, uint32_t timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((TickType_t)(deadline - xTaskGetTickCount()) < 0x80000000u) {
        if (tcp_phy_is_connected(phy)) return true;
        vTaskDelay(pdMS_TO_TICKS(PHY_CONNECT_POLL_MS));
    }
    return tcp_phy_is_connected(phy);
}
#endif

#if CONFIG_TEST_MSG_KISS_UART
static esp_err_t configure_uart_keys_from_kconfig(void) {
    char value[16], err_msg[96] = {0};
    snprintf(value, sizeof(value), "%d", CONFIG_TEST_MSG_UART_NUMBER);
    if (ax25_cfg_set("uart.no", value, err_msg, sizeof(err_msg)) != ESP_OK) return ESP_FAIL;
    snprintf(value, sizeof(value), "%d", CONFIG_TEST_MSG_UART_BAUD);
    if (ax25_cfg_set("uart.baud", value, err_msg, sizeof(err_msg)) != ESP_OK) return ESP_FAIL;
    snprintf(value, sizeof(value), "%d", CONFIG_TEST_MSG_UART_TXD_PIN);
    if (ax25_cfg_set("uart.tx_pin", value, err_msg, sizeof(err_msg)) != ESP_OK) return ESP_FAIL;
    snprintf(value, sizeof(value), "%d", CONFIG_TEST_MSG_UART_RXD_PIN);
    if (ax25_cfg_set("uart.rx_pin", value, err_msg, sizeof(err_msg)) != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}
#endif

static void phy_frame_cb(const ax25_frame_t *frame, void *user_data) {
    esp_err_t err = ax25_router_send(frame, (ax25_router_port_t *)user_data);
    log_router_send_result("PHY RX", err);
}
static void phy_port_output_cb(const ax25_frame_t *frame, void *user_data) {
    send_phy_frame_with_retry(frame, user_data);
}
static void app_port_on_frame(const ax25_frame_t *frame, void *user_data) {
    ax25_conn_t *conn = (ax25_conn_t *)user_data;
    esp_err_t err = ax25_conn_on_frame(conn, frame);
    if (err != ESP_OK) ESP_LOGW(TAG, "ax25_conn_on_frame failed: %s", esp_err_to_name(err));
}
static void on_connect(ax25_address_t remote_addr, bool is_local_initiated, void *user_data) {
    char call[16] = {0};
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    ax25_address_to_string(&remote_addr, call, sizeof(call));
    ESP_LOGI(TAG, "Connected: remote=%s local_initiated=%d", call, (int)is_local_initiated);
    xEventGroupSetBits(ctx->events, EVT_CONNECTED);
}
static void on_disconnect(void *user_data) {
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    ESP_LOGW(TAG, "Disconnected");
    xEventGroupSetBits(ctx->events, EVT_DISCONNECTED);
}
static void on_error(const ax25_conn_error_t *error, void *user_data) {
    (void)user_data;
    ESP_LOGW(TAG, "AX.25 error: code=%d msg=%s", error->code, error->message ? error->message : "");
}
static void on_data(const uint8_t *data, size_t len, void *user_data) {
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    rx_chunk_t chunk;
    if (!data || len == 0 || !ctx->rx_queue) return;
    chunk.len = (len > RX_CHUNK_MAX) ? RX_CHUNK_MAX : len;
    memcpy(chunk.data, data, chunk.len);
    if (xQueueSend(ctx->rx_queue, &chunk, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full, dropping %u bytes", (unsigned)chunk.len);
    }
}
static void on_tx_frame(const ax25_frame_t *frame, void *user_data) {
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    esp_err_t err = ax25_router_send(frame, &ctx->app_port);
    log_router_send_result("AX.25 TX", err);
}
static int rx_fill_cb(void *ctx_p, uint8_t *buf, size_t max_len) {
    app_ctx_t *ctx = (app_ctx_t *)ctx_p;
    TickType_t now = xTaskGetTickCount();
    TickType_t wait = (ctx->deadline > now) ? (ctx->deadline - now) : 0;
    rx_chunk_t chunk;
    if (xQueueReceive(ctx->rx_queue, &chunk, wait) != pdTRUE) return 0;
    size_t n = (chunk.len < max_len) ? chunk.len : max_len;
    memcpy(buf, chunk.data, n);
    return (int)n;
}
static esp_err_t read_until_token(app_ctx_t *ctx, const char *token, uint32_t timeout_ms, char *capture, size_t capture_len) {
    size_t token_len = strlen(token), cap_pos = 0, win_len = 0;
    char window[96];
    if (token_len == 0 || token_len >= sizeof(window)) return ESP_ERR_INVALID_ARG;
    if (capture && capture_len > 0) capture[0] = '\0';
    ctx->deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (1) {
        uint8_t b = 0;
        int r = buffer_get_byte(ctx->rx_buf, &b);
        if (r == 0) return ESP_ERR_TIMEOUT;
        if (r < 0) return ESP_FAIL;
        if (capture && capture_len > 1 && cap_pos < capture_len - 1) {
            capture[cap_pos++] = (char)b;
            capture[cap_pos] = '\0';
        }
        if (win_len < token_len) window[win_len++] = (char)b;
        else {
            memmove(window, window + 1, token_len - 1);
            window[token_len - 1] = (char)b;
        }
        if (win_len == token_len && memcmp(window, token, token_len) == 0) return ESP_OK;
    }
}
static esp_err_t read_until_any_token(app_ctx_t *ctx, const char *const *tokens, size_t token_count, uint32_t timeout_ms, char *capture, size_t capture_len) {
    size_t cap_pos = 0, win_len = 0, max_token_len = 0;
    char window[96];
    if (!ctx || !tokens || token_count == 0) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < token_count; i++) {
        size_t len = strlen(tokens[i]);
        if (!tokens[i] || len == 0 || len >= sizeof(window)) return ESP_ERR_INVALID_ARG;
        if (len > max_token_len) max_token_len = len;
    }
    if (capture && capture_len > 0) capture[0] = '\0';
    ctx->deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (1) {
        uint8_t b = 0;
        int r = buffer_get_byte(ctx->rx_buf, &b);
        if (r == 0) return ESP_ERR_TIMEOUT;
        if (r < 0) return ESP_FAIL;
        if (capture && capture_len > 1 && cap_pos < capture_len - 1) {
            capture[cap_pos++] = (char)b;
            capture[cap_pos] = '\0';
        }
        if (win_len < max_token_len) window[win_len++] = (char)b;
        else {
            memmove(window, window + 1, max_token_len - 1);
            window[max_token_len - 1] = (char)b;
        }
        for (size_t i = 0; i < token_count; i++) {
            size_t token_len = strlen(tokens[i]);
            if (win_len >= token_len && memcmp(window + (win_len - token_len), tokens[i], token_len) == 0) return ESP_OK;
        }
    }
}
static esp_err_t read_until_command_prompt(app_ctx_t *ctx, uint32_t timeout_ms, char *capture, size_t capture_len) {
    return read_until_any_token(ctx, PROMPT_TOKENS, sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]), timeout_ms, capture, capture_len);
}
static bool capture_contains(const char *capture, const char *needle) {
    if (!capture || !needle) return false;
    return strstr(capture, needle) != NULL;
}
static void flush_rx(app_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->rx_queue) xQueueReset(ctx->rx_queue);
    if (ctx->rx_buf) buffer_flush(ctx->rx_buf);
}
static bool run_cmd_expect_prompt(app_ctx_t *ctx, const char *cmd, const char *expect, uint32_t timeout_ms, char *capture, size_t capture_len) {
    bool ok;
    if (!send_conn_data_with_retry(&ctx->conn, (const uint8_t *)cmd, strlen(cmd)) == ESP_OK) return false;
    if (read_until_command_prompt(ctx, timeout_ms, capture, capture_len) != ESP_OK) return false;
    if (!expect || !expect[0]) return true;
    ok = capture_contains(capture, expect);
    return ok;
}
static bool parse_stored_message_id(const char *capture, uint32_t *out_id) {
    const char *p = capture, *last = NULL;
    unsigned long id = 0;
    if (!capture || !out_id) return false;
    while ((p = strstr(p, "*** Stored as message ")) != NULL) { last = p; p++; }
    if (!last) return false;
    if (sscanf(last, "*** Stored as message %lu", &id) != 1) return false;
    *out_id = (uint32_t)id;
    return true;
}
static void record_result(app_ctx_t *ctx, const char *name, bool pass, const char *detail) {
    ctx->tests_run++;
    if (!pass) ctx->tests_failed++;
    ctx->last.pass = pass;
    ctx->last.name = name;
    snprintf(ctx->last.detail, sizeof(ctx->last.detail), "%s", detail ? detail : "");
    ESP_LOGI(TAG, "[%s] %s - %s", pass ? "PASS" : "FAIL", name, ctx->last.detail);
}

// --- New helpers for test_message ---
static size_t parse_body_limit(const char *capture) {
    const char *p = strstr(capture, "Max message size: ");
    if (!p) return 0;
    unsigned limit = 0;
    if (sscanf(p, "Max message size: %u", &limit) == 1) return limit;
    return 0;
}
static size_t effective_body_max(app_ctx_t *ctx) {
    size_t kconfig_max = MSG_BODY_MAX;
    if (ctx->bbs_body_limit > 0 && ctx->bbs_body_limit < kconfig_max) return ctx->bbs_body_limit;
    return kconfig_max;
}
static size_t generate_random_body(char *buf, size_t max_len) {
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    size_t min_len = 100;
    if (max_len < min_len) max_len = min_len;
    size_t len = min_len + (esp_random() % (max_len - min_len + 1));
    for (size_t i = 0; i < len; ++i) {
        buf[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    buf[len] = '\0';
    return len;
}

// --- Main test script ---
static void run_script(app_ctx_t *ctx) {
    char *capture = ctx->capture;
    char line[80];
    bool ok;
    size_t effective_max = 0;
    // Wait for the initial BBS banner before sending any commands
    if (read_until_command_prompt(ctx, CONFIG_TEST_MSG_STEP_TIMEOUT_MS, capture, CAPTURE_MAX) != ESP_OK) {
        record_result(ctx, "BBS banner", false, "timeout waiting for initial prompt");
        return;
    }
    record_result(ctx, "BBS banner", true, "initial prompt received");
    flush_rx(ctx);
    // Phase I: Create all messages
    for (int i = 0; i < CONFIG_TEST_MSG_MESSAGE_COUNT; ++i) {
        msg_entry_t *entry = &ctx->messages[i];
        // Send SB TEST, wait for subject prompt (not a command prompt - BBS is in subject mode)
        send_conn_data_with_retry(&ctx->conn, (const uint8_t *)"SB TEST\r", 8);
        ok = read_until_token(ctx, "Subject: ", CONFIG_TEST_MSG_STEP_TIMEOUT_MS, capture, CAPTURE_MAX) == ESP_OK;
        if (!ok) { record_result(ctx, "SB command", false, "failed to enter subject mode"); continue; }
        record_result(ctx, "SB command", true, "entered subject mode");
        flush_rx(ctx);
        // Send subject, wait for body prompt (also not a command prompt)
        snprintf(line, sizeof(line), "Test message %d\r", i + 1);
        send_conn_data_with_retry(&ctx->conn, (const uint8_t *)line, strlen(line));
        ok = read_until_token(ctx, "use /EX to finish:", CONFIG_TEST_MSG_STEP_TIMEOUT_MS, capture, CAPTURE_MAX) == ESP_OK;
        if (!ok) { record_result(ctx, "Subject entry", false, "body prompt missing"); continue; }
        if (i == 0) {
            ctx->bbs_body_limit = parse_body_limit(capture);
        }
        effective_max = effective_body_max(ctx);
        entry->body_len = generate_random_body(entry->body, effective_max);
        // Send body in 64-char lines
        for (size_t pos = 0; pos < entry->body_len; pos += 64) {
            size_t chunk = (entry->body_len - pos > 64) ? 64 : (entry->body_len - pos);
            memcpy(line, entry->body + pos, chunk);
            line[chunk] = '\r'; line[chunk + 1] = '\0';
            send_conn_data_with_retry(&ctx->conn, (const uint8_t *)line, chunk + 1);
        }
        ok = send_conn_data_with_retry(&ctx->conn, (const uint8_t *)"/EX\r", 4) == ESP_OK;
        if (ok) ok = read_until_command_prompt(ctx, CONFIG_TEST_MSG_STEP_TIMEOUT_MS, capture, CAPTURE_MAX) == ESP_OK;
        if (ok) ok = parse_stored_message_id(capture, &entry->id);
        record_result(ctx, "Post message", ok, ok ? "message stored" : "store confirmation missing");
    }
    // Phase II: Read and verify all
    for (int i = 0; i < CONFIG_TEST_MSG_MESSAGE_COUNT; ++i) {
        msg_entry_t *entry = &ctx->messages[i];
        snprintf(line, sizeof(line), "R %lu\r", (unsigned long)entry->id);
        ok = run_cmd_expect_prompt(ctx, line, NULL, CONFIG_TEST_MSG_STEP_TIMEOUT_MS * 2, capture, CAPTURE_MAX);
        // Verify all 64-char lines present
        bool all_found = true;
        for (size_t pos = 0; pos < entry->body_len; pos += 64) {
            size_t chunk = (entry->body_len - pos > 64) ? 64 : (entry->body_len - pos);
            memcpy(line, entry->body + pos, chunk);
            line[chunk] = '\0';
            if (!capture_contains(capture, line)) {
                all_found = false;
                break;
            }
        }
        record_result(ctx, "Read message", all_found, all_found ? "body verified" : "body mismatch");
    }
    // Phase III: Delete all
    for (int i = 0; i < CONFIG_TEST_MSG_MESSAGE_COUNT; ++i) {
        msg_entry_t *entry = &ctx->messages[i];
        snprintf(line, sizeof(line), "K %lu\r", (unsigned long)entry->id);
        ok = run_cmd_expect_prompt(ctx, line, "*** Message deleted", CONFIG_TEST_MSG_STEP_TIMEOUT_MS, capture, CAPTURE_MAX);
        record_result(ctx, "Delete message", ok, ok ? "message deleted" : "delete confirmation missing");
    }
    // Phase IV: Oversized message test
    // Post a message whose body exceeds the BBS limit.  The BBS should respond
    // with a body-full warning, still accept /EX, store the (truncated) message,
    // and we should be able to delete it afterwards.
    {
        uint32_t oversize_id = 0;
        size_t oversize_limit = (ctx->bbs_body_limit > 0) ? ctx->bbs_body_limit : MSG_BODY_MAX;
        // Build a body that is 128 bytes longer than the BBS limit
        size_t oversize_len = oversize_limit + 128;
        char *oversize_body = malloc(oversize_len + 1);
        if (oversize_body == NULL) {
            record_result(ctx, "Oversize post", false, "malloc failed for oversize body");
        } else {
            // Fill with a recognisable repeating pattern
            for (size_t i = 0; i < oversize_len; ++i) {
                oversize_body[i] = (char)('A' + (i % 26));
            }
            oversize_body[oversize_len] = '\0';

            // Start the post
            send_conn_data_with_retry(&ctx->conn, (const uint8_t *)"SB OVERSIZE\r", 12);
            ok = read_until_token(ctx, "Subject: ", CONFIG_TEST_MSG_STEP_TIMEOUT_MS, capture, CAPTURE_MAX) == ESP_OK;
            record_result(ctx, "Oversize SB", ok, ok ? "entered subject mode" : "failed to enter subject mode");

            if (ok) {
                flush_rx(ctx);
                send_conn_data_with_retry(&ctx->conn, (const uint8_t *)"Oversize test message\r", 22);
                ok = read_until_token(ctx, "use /EX to finish:", CONFIG_TEST_MSG_STEP_TIMEOUT_MS, capture, CAPTURE_MAX) == ESP_OK;
                record_result(ctx, "Oversize subject", ok, ok ? "body prompt received" : "body prompt missing");
            }

            if (ok) {
                flush_rx(ctx);
                // Send lines until we get a body-full/limit warning or exhaust the body
                bool got_limit_warning = false;
                for (size_t pos = 0; pos < oversize_len && !got_limit_warning; pos += 64) {
                    size_t chunk = (oversize_len - pos > 64) ? 64 : (oversize_len - pos);
                    memcpy(line, oversize_body + pos, chunk);
                    line[chunk] = '\r'; line[chunk + 1] = '\0';
                    send_conn_data_with_retry(&ctx->conn, (const uint8_t *)line, chunk + 1);
                    // After each line, check if the BBS has sent a body-full warning.
                    // Use a short timeout — if nothing arrives that matches, keep sending.
                    if (read_until_any_token(ctx,
                            (const char *const[]){"*** Body limit reached", "*** Body full"},
                            2,
                            300,
                            capture, CAPTURE_MAX) == ESP_OK) {
                        got_limit_warning = true;
                    }
                }
                record_result(ctx, "Oversize body warning", got_limit_warning,
                              got_limit_warning ? "BBS sent body-full warning" : "no body-full warning received");

                // Send /EX — the BBS should store the (truncated) message regardless
                send_conn_data_with_retry(&ctx->conn, (const uint8_t *)"/EX\r", 4);
                ok = read_until_command_prompt(ctx, CONFIG_TEST_MSG_STEP_TIMEOUT_MS, capture, CAPTURE_MAX) == ESP_OK;
                bool stored = ok && parse_stored_message_id(capture, &oversize_id);
                record_result(ctx, "Oversize post stored", stored,
                              stored ? "truncated message stored" : "store confirmation missing");

                // Clean up: delete the oversize message
                if (stored) {
                    snprintf(line, sizeof(line), "K %lu\r", (unsigned long)oversize_id);
                    ok = run_cmd_expect_prompt(ctx, line, "*** Message deleted",
                                              CONFIG_TEST_MSG_STEP_TIMEOUT_MS, capture, CAPTURE_MAX);
                    record_result(ctx, "Oversize delete", ok,
                                  ok ? "oversize message deleted" : "delete confirmation missing");
                }
            }
            free(oversize_body);
        }
    }
    // Disconnect
    send_conn_data_with_retry(&ctx->conn, (const uint8_t *)"B\r", 2);
    xEventGroupWaitBits(ctx->events, EVT_DISCONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(CONFIG_TEST_MSG_STEP_TIMEOUT_MS));
    record_result(ctx, "Disconnect", true, "session closed");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "BBS test complete: run=%lu failed=%lu passed=%lu", (unsigned long)ctx->tests_run, (unsigned long)ctx->tests_failed, (unsigned long)(ctx->tests_run - ctx->tests_failed));
    ESP_LOGI(TAG, "========================================");
}

// --- test_task and app_main (boilerplate, mirrors test_bbs) ---
static void test_task(void *arg) {
    app_ctx_t *ctx = (app_ctx_t *)arg;
    EventBits_t bits = xEventGroupWaitBits(ctx->events, EVT_CONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(CONFIG_TEST_MSG_CONNECT_TIMEOUT_MS));
    if ((bits & EVT_CONNECTED) == 0) {
        record_result(ctx, "AX.25 connect", false, "timeout waiting for connect callback");
        xEventGroupSetBits(ctx->events, EVT_FINISHED);
        vTaskDelete(NULL);
        return;
    }
    record_result(ctx, "AX.25 connect", true, "session established");
    run_script(ctx);
    xEventGroupSetBits(ctx->events, EVT_FINISHED);
    vTaskDelete(NULL);
}

void app_main(void) {
    static ax25_router_port_t phy_port;
    static TaskHandle_t task;
#if CONFIG_TEST_MSG_KISS_TCP
    static ax25_wifi_t wifi_ctx;
    static ax25_phy_kiss_tcp_client_t phy;
#elif CONFIG_TEST_MSG_KISS_UART
    static ax25_phy_kiss_uart_t phy;
#endif
    ax25_conn_callbacks_t cbs = {0};
    ax25_conn_config_t conn_cfg = AX25_CONN_CONFIG_DEFAULT();
    ax25_address_t local_addr, remote_addr;
    esp_err_t err;
    ESP_LOGI(TAG, "ESP-AX25 test_message starting");
    memset(&s_ctx, 0, sizeof(s_ctx));
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(ax25_cfg_init(NULL, 0));
#if CONFIG_TEST_MSG_KISS_TCP
    {
        char err_msg[96] = {0};
        ESP_ERROR_CHECK(ax25_cfg_set("wifi.sta.ssid", CONFIG_TEST_MSG_WIFI_SSID, err_msg, sizeof(err_msg)));
        ESP_ERROR_CHECK(ax25_cfg_set("wifi.sta.password", CONFIG_TEST_MSG_WIFI_PASSWORD, err_msg, sizeof(err_msg)));
        ESP_ERROR_CHECK(ax25_cfg_set("wifi.ap.ssid", CONFIG_TEST_MSG_WIFI_AP_SSID, err_msg, sizeof(err_msg)));
        ESP_ERROR_CHECK(ax25_cfg_set("wifi.ap.password", CONFIG_TEST_MSG_WIFI_AP_PASSWORD, err_msg, sizeof(err_msg)));
    }
    err = ax25_wifi_start(&wifi_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return;
    }
#endif
    if (ax25_address_from_string(CONFIG_TEST_MSG_LOCAL_CALLSIGN, &local_addr) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid local callsign: %s", CONFIG_TEST_MSG_LOCAL_CALLSIGN);
        return;
    }
    if (ax25_address_from_string(CONFIG_TEST_MSG_REMOTE_CALLSIGN, &remote_addr) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid remote callsign: %s", CONFIG_TEST_MSG_REMOTE_CALLSIGN);
        return;
    }
    s_ctx.remote_addr = remote_addr;
    err = ax25_router_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Router init failed: %s", esp_err_to_name(err));
        return;
    }
    s_ctx.events = xEventGroupCreate();
    s_ctx.rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_chunk_t));
    if (!s_ctx.events || !s_ctx.rx_queue) {
        ESP_LOGE(TAG, "Failed to create event group or RX queue");
        return;
    }
    s_ctx.rx_buf = buffer_create(RX_BUF_SIZE, rx_fill_cb, &s_ctx);
    if (!s_ctx.rx_buf) {
        ESP_LOGE(TAG, "buffer_create failed");
        return;
    }
    s_ctx.app_port.destination = local_addr;
    s_ctx.app_port.mode = AX25_PORT_STATIC;
    s_ctx.app_port.on_tx_frame = app_port_on_frame;
    s_ctx.app_port.user_data = &s_ctx.conn;
    err = ax25_router_register_port(&s_ctx.app_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register app port: %s", esp_err_to_name(err));
        return;
    }
    phy_port.mode = AX25_PORT_DEFAULT;
    phy_port.on_tx_frame = phy_port_output_cb;
    phy_port.user_data = &phy;
    err = ax25_router_register_port(&phy_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PHY port: %s", esp_err_to_name(err));
        return;
    }
#if CONFIG_TEST_MSG_KISS_TCP
    const ax25_phy_kiss_tcp_client_config_t phy_cfg = {
        .host = CONFIG_TEST_MSG_REMOTE_HOST,
        .port = CONFIG_TEST_MSG_REMOTE_TCP_PORT,
        .on_rx_frame = phy_frame_cb,
        .user_data = &phy_port,
        .connect_timeout_ms = 6000,
        .reconnect_delay_ms = 2000,
        .rx_task_stack_size = 4096,
        .rx_task_priority = 5,
    };
    err = ax25_phy_kiss_tcp_client_init(&phy_cfg, &phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCP KISS client init failed: %s", esp_err_to_name(err));
        return;
    }
#elif CONFIG_TEST_MSG_KISS_UART
    ESP_ERROR_CHECK(configure_uart_keys_from_kconfig());
    err = ax25_phy_kiss_uart_init(phy_frame_cb, &phy_port, &phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART KISS init failed: %s", esp_err_to_name(err));
        return;
    }
#endif
    cbs.on_connect = on_connect;
    cbs.on_disconnect = on_disconnect;
    cbs.on_error = on_error;
    cbs.on_data = on_data;
    cbs.on_tx_frame = on_tx_frame;
    err = ax25_conn_init(&s_ctx.conn, &local_addr, &cbs, &s_ctx, &conn_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ax25_conn_init failed: %s", esp_err_to_name(err));
        return;
    }
#if CONFIG_TEST_MSG_KISS_TCP
    ESP_LOGI(TAG, "Waiting for KISS TCP transport to connect");
    if (!wait_for_tcp_phy_connected(&phy, phy_cfg.connect_timeout_ms + phy_cfg.reconnect_delay_ms)) {
        ESP_LOGE(TAG, "Timed out waiting for KISS TCP transport to connect");
        return;
    }
#endif
    err = ax25_conn_connect(&s_ctx.conn, &remote_addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ax25_conn_connect failed: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(test_task, "test_msg_task", 9216, &s_ctx, 5, &task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create test task");
        return;
    }
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_ctx.events, EVT_FINISHED, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if ((bits & EVT_FINISHED) != 0) {
            ESP_LOGI(TAG, "test_message complete, idling");
            break;
        }
    }
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Idle: tests_run=%lu tests_failed=%lu", (unsigned long)s_ctx.tests_run, (unsigned long)s_ctx.tests_failed);
    }
}
