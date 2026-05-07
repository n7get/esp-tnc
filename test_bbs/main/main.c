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

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "buffer.h"

#include "ax25_address.h"
#include "ax25_config.h"
#include "ax25_conn.h"
#include "ax25_frame.h"
#include "ax25_router.h"

#if CONFIG_TEST_BBS_KISS_TCP
#include "ax25_phy_kiss_tcp_client.h"
#include "ax25_wifi.h"
#elif CONFIG_TEST_BBS_KISS_UART
#include "ax25_phy_kiss_uart.h"
#else
#error "Either CONFIG_TEST_BBS_KISS_TCP or CONFIG_TEST_BBS_KISS_UART must be enabled"
#endif

static const char *TAG = "TEST_BBS";
static const char *ALT_LOCAL_CALLSIGN = "N9ALT-1";

#define RX_CHUNK_MAX AX25_MAX_INFO_LEN
#define RX_QUEUE_LEN 32
#define RX_BUF_SIZE 768
#define TEST_SEND_RETRY_MAX 200
#define TEST_SEND_RETRY_DELAY_MS 10
#define PHY_CONNECT_POLL_MS 50
#define SYSOP_CHALLENGE_INDEX_COUNT 4
#define SYSOP_RESPONSE_LEN 6

#define EVT_CONNECTED BIT0
#define EVT_DISCONNECTED BIT1
#define EVT_FINISHED BIT2

#define CAPTURE_MAX 4096

static const char *const PROMPT_TOKENS[] = {
    "BBS READY>",
    "BBS READY:",
    "BBS> "
};

static const char *const CONFIG_PROMPT_TOKENS[] = {
    "CONFIG READY>"
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
    ax25_router_port_t app_port;
    ax25_conn_t conn;
    ax25_address_t primary_local_addr;
    ax25_address_t alt_local_addr;
    ax25_address_t remote_addr;
#if CONFIG_TEST_BBS_KISS_TCP
    ax25_phy_kiss_tcp_client_t *tcp_phy;
    ax25_phy_kiss_tcp_client_config_t tcp_phy_cfg;
#endif

    EventGroupHandle_t events;
    QueueHandle_t rx_queue;

    TickType_t deadline;
    buffer_t *rx_buf;

    uint32_t tests_run;
    uint32_t tests_failed;
    uint32_t posted_message_id;
    uint32_t created_ids[16];
    size_t created_count;
    char run_subject_tag[32];
    char capture[CAPTURE_MAX];

    test_result_t last;
} app_ctx_t;

static app_ctx_t s_ctx;

static void log_router_send_result(const char *path, esp_err_t err)
{
    if (err == ESP_OK) {
        return;
    }

    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "%s: router queue full", path);
        return;
    }

    ESP_LOGW(TAG, "%s: router send failed: %s", path, esp_err_to_name(err));
}

static esp_err_t send_phy_frame_with_retry(const ax25_frame_t *frame, void *user_data)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;

    for (int attempt = 0; attempt < TEST_SEND_RETRY_MAX; attempt++) {
#if CONFIG_TEST_BBS_KISS_TCP
        err = ax25_phy_kiss_tcp_client_send(frame, (ax25_phy_kiss_tcp_client_t *)user_data);
#elif CONFIG_TEST_BBS_KISS_UART
        err = ax25_phy_kiss_uart_send(frame, (ax25_phy_kiss_uart_t *)user_data);
#endif
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (err != ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "PHY send failed: %s", esp_err_to_name(err));
            return err;
        }

        if (attempt == 0) {
            ESP_LOGW(TAG, "PHY TX queue full, retrying");
        }
        vTaskDelay(pdMS_TO_TICKS(TEST_SEND_RETRY_DELAY_MS));
    }

    ESP_LOGW(TAG, "PHY TX queue remained full after %d retries", TEST_SEND_RETRY_MAX);
    return err;
}

static esp_err_t send_conn_data_with_retry(ax25_conn_t *conn,
                                           const uint8_t *data,
                                           size_t len)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (conn == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int attempt = 0; attempt < TEST_SEND_RETRY_MAX; attempt++) {
        err = ax25_conn_send_data(conn, data, len);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (err == ESP_ERR_INVALID_STATE) {
            return err;
        }

        if (err != ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "AX.25 send failed: %s", esp_err_to_name(err));
            return err;
        }

        if (attempt == 0) {
            ESP_LOGW(TAG, "AX.25 TX queue full, retrying");
        }
        vTaskDelay(pdMS_TO_TICKS(TEST_SEND_RETRY_DELAY_MS));
    }

    ESP_LOGW(TAG, "AX.25 TX queue remained full after %d retries", TEST_SEND_RETRY_MAX);
    return err;
}

#if CONFIG_TEST_BBS_KISS_TCP
static bool tcp_phy_is_connected(ax25_phy_kiss_tcp_client_t *phy)
{
    bool connected;

    if (phy == NULL || phy->send_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(phy->send_mutex, pdMS_TO_TICKS(PHY_CONNECT_POLL_MS)) != pdTRUE) {
        return false;
    }

    connected = (phy->sock >= 0);
    xSemaphoreGive(phy->send_mutex);
    return connected;
}

static bool wait_for_tcp_phy_connected(ax25_phy_kiss_tcp_client_t *phy, uint32_t timeout_ms)
{
    TickType_t deadline;

    if (phy == NULL) {
        return false;
    }

    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((TickType_t)(deadline - xTaskGetTickCount()) < 0x80000000u) {
        if (tcp_phy_is_connected(phy)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(PHY_CONNECT_POLL_MS));
    }

    return tcp_phy_is_connected(phy);
}
#endif

#if CONFIG_TEST_BBS_KISS_UART
static esp_err_t configure_uart_keys_from_kconfig(void)
{
    char value[16];
    char err_msg[96] = {0};

    snprintf(value, sizeof(value), "%d", CONFIG_TEST_BBS_UART_NUMBER);
    if (ax25_cfg_set("uart.no", value, err_msg, sizeof(err_msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set uart.no: %s", err_msg);
        return ESP_FAIL;
    }

    snprintf(value, sizeof(value), "%d", CONFIG_TEST_BBS_UART_BAUD);
    if (ax25_cfg_set("uart.baud", value, err_msg, sizeof(err_msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set uart.baud: %s", err_msg);
        return ESP_FAIL;
    }

    snprintf(value, sizeof(value), "%d", CONFIG_TEST_BBS_UART_TXD_PIN);
    if (ax25_cfg_set("uart.tx_pin", value, err_msg, sizeof(err_msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set uart.tx_pin: %s", err_msg);
        return ESP_FAIL;
    }

    snprintf(value, sizeof(value), "%d", CONFIG_TEST_BBS_UART_RXD_PIN);
    if (ax25_cfg_set("uart.rx_pin", value, err_msg, sizeof(err_msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set uart.rx_pin: %s", err_msg);
        return ESP_FAIL;
    }

    return ESP_OK;
}
#endif

static void test_task(void *arg);
static bool run_cmd_expect_prompt(app_ctx_t *ctx,
                                  const char *cmd,
                                  const char *expect,
                                  uint32_t timeout_ms,
                                  char *capture,
                                  size_t capture_len);
static bool reconnect_session(app_ctx_t *ctx, uint32_t timeout_ms);
static bool reconnect_as_local(app_ctx_t *ctx,
                               const ax25_address_t *local_addr,
                               uint32_t timeout_ms);
static bool parse_banner_counts(const char *capture,
                                uint32_t *out_total,
                                uint32_t *out_new_count);
static bool reconnect_and_capture_banner(app_ctx_t *ctx,
                                         uint32_t timeout_ms,
                                         char *capture,
                                         size_t capture_len,
                                         uint32_t *out_total,
                                         uint32_t *out_new_count);
static void on_tx_frame(const ax25_frame_t *frame, void *user_data);

static void phy_frame_cb(const ax25_frame_t *frame, void *user_data)
{
    esp_err_t err = ax25_router_send(frame, (ax25_router_port_t *)user_data);
    log_router_send_result("PHY RX", err);
}

static void phy_port_output_cb(const ax25_frame_t *frame, void *user_data)
{
    send_phy_frame_with_retry(frame, user_data);
}

static void app_port_on_frame(const ax25_frame_t *frame, void *user_data)
{
    ax25_conn_t *conn = (ax25_conn_t *)user_data;
    esp_err_t err = ax25_conn_on_frame(conn, frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ax25_conn_on_frame failed: %s", esp_err_to_name(err));
    }
}

static void on_connect(ax25_address_t remote_addr, bool is_local_initiated, void *user_data)
{
    char call[16] = {0};
    app_ctx_t *ctx = (app_ctx_t *)user_data;

    ax25_address_to_string(&remote_addr, call, sizeof(call));
    ESP_LOGI(TAG, "Connected: remote=%s local_initiated=%d", call, (int)is_local_initiated);

    xEventGroupSetBits(ctx->events, EVT_CONNECTED);
}

static void on_disconnect(void *user_data)
{
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    ESP_LOGW(TAG, "Disconnected");
    xEventGroupSetBits(ctx->events, EVT_DISCONNECTED);
}

static void on_error(const ax25_conn_error_t *error, void *user_data)
{
    (void)user_data;
    ESP_LOGW(TAG, "AX.25 error: code=%d msg=%s", error->code, error->message ? error->message : "");
}

static void on_data(const uint8_t *data, size_t len, void *user_data)
{
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    rx_chunk_t chunk;

    if (data == NULL || len == 0 || ctx->rx_queue == NULL) {
        return;
    }

    chunk.len = (len > RX_CHUNK_MAX) ? RX_CHUNK_MAX : len;
    memcpy(chunk.data, data, chunk.len);

    if (xQueueSend(ctx->rx_queue, &chunk, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full, dropping %u bytes", (unsigned)chunk.len);
    }
}

static void setup_conn_callbacks(ax25_conn_callbacks_t *cbs)
{
    if (cbs == NULL) {
        return;
    }

    memset(cbs, 0, sizeof(*cbs));
    cbs->on_connect = on_connect;
    cbs->on_disconnect = on_disconnect;
    cbs->on_error = on_error;
    cbs->on_data = on_data;
    cbs->on_tx_frame = on_tx_frame;
}

static void on_tx_frame(const ax25_frame_t *frame, void *user_data)
{
    app_ctx_t *ctx = (app_ctx_t *)user_data;
    esp_err_t err = ax25_router_send(frame, &ctx->app_port);
    log_router_send_result("AX.25 TX", err);
}

static int rx_fill_cb(void *ctx_p, uint8_t *buf, size_t max_len)
{
    app_ctx_t *ctx = (app_ctx_t *)ctx_p;
    TickType_t now = xTaskGetTickCount();
    TickType_t wait = (ctx->deadline > now) ? (ctx->deadline - now) : 0;

    rx_chunk_t chunk;
    if (xQueueReceive(ctx->rx_queue, &chunk, wait) != pdTRUE) {
        return 0;
    }

    size_t n = (chunk.len < max_len) ? chunk.len : max_len;
    memcpy(buf, chunk.data, n);
    return (int)n;
}

static esp_err_t read_until_token(app_ctx_t *ctx,
                                  const char *token,
                                  uint32_t timeout_ms,
                                  char *capture,
                                  size_t capture_len)
{
    size_t token_len = strlen(token);
    size_t cap_pos = 0;
    size_t win_len = 0;
    char window[96];

    if (token_len == 0 || token_len >= sizeof(window)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (capture && capture_len > 0) {
        capture[0] = '\0';
    }

    ctx->deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (1) {
        uint8_t b = 0;
        int r = buffer_get_byte(ctx->rx_buf, &b);

        if (r == 0) {
            return ESP_ERR_TIMEOUT;
        }
        if (r < 0) {
            return ESP_FAIL;
        }

        if (capture && capture_len > 1) {
            if (cap_pos < capture_len - 1) {
                capture[cap_pos++] = (char)b;
                capture[cap_pos] = '\0';
            }
        }

        if (win_len < token_len) {
            window[win_len++] = (char)b;
        } else {
            memmove(window, window + 1, token_len - 1);
            window[token_len - 1] = (char)b;
        }

        if (win_len == token_len && memcmp(window, token, token_len) == 0) {
            return ESP_OK;
        }
    }
}

static esp_err_t read_until_any_token(app_ctx_t *ctx,
                                      const char *const *tokens,
                                      size_t token_count,
                                      uint32_t timeout_ms,
                                      char *capture,
                                      size_t capture_len)
{
    size_t cap_pos = 0;
    size_t win_len = 0;
    size_t max_token_len = 0;
    char window[96];

    if (ctx == NULL || tokens == NULL || token_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < token_count; i++) {
        size_t len;

        if (tokens[i] == NULL) {
            return ESP_ERR_INVALID_ARG;
        }

        len = strlen(tokens[i]);
        if (len == 0 || len >= sizeof(window)) {
            return ESP_ERR_INVALID_ARG;
        }

        if (len > max_token_len) {
            max_token_len = len;
        }
    }

    if (capture && capture_len > 0) {
        capture[0] = '\0';
    }

    ctx->deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (1) {
        uint8_t b = 0;
        int r = buffer_get_byte(ctx->rx_buf, &b);

        if (r == 0) {
            return ESP_ERR_TIMEOUT;
        }
        if (r < 0) {
            return ESP_FAIL;
        }

        if (capture && capture_len > 1) {
            if (cap_pos < capture_len - 1) {
                capture[cap_pos++] = (char)b;
                capture[cap_pos] = '\0';
            }
        }

        if (win_len < max_token_len) {
            window[win_len++] = (char)b;
        } else {
            memmove(window, window + 1, max_token_len - 1);
            window[max_token_len - 1] = (char)b;
        }

        for (size_t i = 0; i < token_count; i++) {
            size_t token_len = strlen(tokens[i]);
            if (win_len >= token_len && memcmp(window + (win_len - token_len), tokens[i], token_len) == 0) {
                return ESP_OK;
            }
        }
    }
}

static esp_err_t read_until_command_prompt(app_ctx_t *ctx,
                                           uint32_t timeout_ms,
                                           char *capture,
                                           size_t capture_len)
{
    return read_until_any_token(ctx,
                                PROMPT_TOKENS,
                                sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]),
                                timeout_ms,
                                capture,
                                capture_len);
}

static esp_err_t read_until_config_prompt(app_ctx_t *ctx,
                                          uint32_t timeout_ms,
                                          char *capture,
                                          size_t capture_len)
{
    return read_until_any_token(ctx,
                                CONFIG_PROMPT_TOKENS,
                                sizeof(CONFIG_PROMPT_TOKENS) / sizeof(CONFIG_PROMPT_TOKENS[0]),
                                timeout_ms,
                                capture,
                                capture_len);
}

static bool capture_contains(const char *capture, const char *needle)
{
    if (capture == NULL || needle == NULL) {
        return false;
    }
    return strstr(capture, needle) != NULL;
}

static bool parse_sysop_challenge(const char *capture,
                                  uint16_t out_indices[SYSOP_CHALLENGE_INDEX_COUNT])
{
    const char *chal;
    unsigned i1;
    unsigned i2;
    unsigned i3;
    unsigned i4;

    if (capture == NULL || out_indices == NULL) {
        return false;
    }

    chal = strstr(capture, "CHAL ");
    if (chal == NULL) {
        return false;
    }

    if (sscanf(chal, "CHAL %u %u %u %u", &i1, &i2, &i3, &i4) != 4) {
        return false;
    }

    out_indices[0] = (uint16_t)i1;
    out_indices[1] = (uint16_t)i2;
    out_indices[2] = (uint16_t)i3;
    out_indices[3] = (uint16_t)i4;
    return true;
}

static bool build_sysop_response(const char *secret,
                                 const uint16_t indices[SYSOP_CHALLENGE_INDEX_COUNT],
                                 char out_response[SYSOP_RESPONSE_LEN + 1])
{
    size_t secret_len;
    char required[SYSOP_CHALLENGE_INDEX_COUNT];

    if (secret == NULL || indices == NULL || out_response == NULL) {
        return false;
    }

    secret_len = strlen(secret);
    if (secret_len < SYSOP_CHALLENGE_INDEX_COUNT) {
        return false;
    }

    for (size_t i = 0; i < SYSOP_CHALLENGE_INDEX_COUNT; i++) {
        uint16_t idx = indices[i];

        if (idx == 0 || idx > secret_len) {
            return false;
        }

        required[i] = secret[idx - 1];
    }

    for (size_t i = 0; i < SYSOP_RESPONSE_LEN; i++) {
        out_response[i] = required[i % SYSOP_CHALLENGE_INDEX_COUNT];
    }
    out_response[SYSOP_RESPONSE_LEN] = '\0';
    return true;
}

static void flush_rx(app_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->rx_queue != NULL) {
        xQueueReset(ctx->rx_queue);
    }

    if (ctx->rx_buf != NULL) {
        buffer_flush(ctx->rx_buf);
    }
}

static bool reconnect_session(app_ctx_t *ctx, uint32_t timeout_ms)
{
    EventBits_t bits;
    esp_err_t err;

    if (ctx == NULL || ctx->events == NULL) {
        return false;
    }

    flush_rx(ctx);
    xEventGroupClearBits(ctx->events, EVT_CONNECTED | EVT_DISCONNECTED);

    err = ax25_conn_shutdown(&ctx->conn);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reconnect shutdown failed: %s", esp_err_to_name(err));
        return false;
    }

    bits = xEventGroupWaitBits(ctx->events,
                               EVT_DISCONNECTED,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & EVT_DISCONNECTED) == 0) {
        ESP_LOGW(TAG, "Reconnect timed out waiting for disconnect");
        return false;
    }

    flush_rx(ctx);
    xEventGroupClearBits(ctx->events, EVT_CONNECTED);

    err = ax25_conn_connect(&ctx->conn, &ctx->remote_addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reconnect connect failed: %s", esp_err_to_name(err));
        return false;
    }

    bits = xEventGroupWaitBits(ctx->events,
                               EVT_CONNECTED,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & EVT_CONNECTED) == 0) {
        ESP_LOGW(TAG, "Reconnect timed out waiting for connect");
        return false;
    }

    return true;
}

static bool reconnect_as_local(app_ctx_t *ctx,
                               const ax25_address_t *local_addr,
                               uint32_t timeout_ms)
{
    EventBits_t bits;
    esp_err_t err;
    ax25_conn_callbacks_t cbs;
    ax25_conn_config_t conn_cfg = AX25_CONN_CONFIG_DEFAULT();

    if (ctx == NULL || ctx->events == NULL || local_addr == NULL) {
        return false;
    }

    flush_rx(ctx);
    xEventGroupClearBits(ctx->events, EVT_CONNECTED | EVT_DISCONNECTED);

    err = ax25_conn_shutdown(&ctx->conn);
    if (err == ESP_OK) {
        bits = xEventGroupWaitBits(ctx->events,
                                   EVT_DISCONNECTED,
                                   pdTRUE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(timeout_ms));
        if ((bits & EVT_DISCONNECTED) == 0) {
            ESP_LOGW(TAG, "Reconnect-as-local timed out waiting for disconnect");
            return false;
        }
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Reconnect-as-local shutdown failed: %s", esp_err_to_name(err));
        return false;
    }

    ax25_conn_deinit(&ctx->conn);
    memset(&ctx->conn, 0, sizeof(ctx->conn));

#if CONFIG_TEST_BBS_KISS_TCP
    // The server-side dynamic router binding keys off the transport session.
    // Reconnect the KISS TCP transport so replies for the new local callsign
    // are routed back to this client.
    if (ctx->tcp_phy != NULL) {
        ax25_phy_kiss_tcp_client_deinit(ctx->tcp_phy);
        err = ax25_phy_kiss_tcp_client_init(&ctx->tcp_phy_cfg, ctx->tcp_phy);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Reconnect-as-local transport init failed: %s", esp_err_to_name(err));
            return false;
        }

        if (!wait_for_tcp_phy_connected(ctx->tcp_phy,
                                        ctx->tcp_phy_cfg.connect_timeout_ms +
                                        ctx->tcp_phy_cfg.reconnect_delay_ms)) {
            ESP_LOGW(TAG, "Reconnect-as-local timed out waiting for TCP transport");
            return false;
        }
    }
#endif

    // Update the app_port destination to match the new local callsign
    // so frames destined to the new callsign are routed to this connection
    err = ax25_router_remove_port(&ctx->app_port);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Reconnect-as-local remove port failed: %s", esp_err_to_name(err));
        return false;
    }

    ctx->app_port.destination = *local_addr;
    ctx->app_port.mode = AX25_PORT_STATIC;
    ctx->app_port.on_tx_frame = app_port_on_frame;
    ctx->app_port.user_data = &ctx->conn;

    err = ax25_router_register_port(&ctx->app_port);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reconnect-as-local register port failed: %s", esp_err_to_name(err));
        return false;
    }

    setup_conn_callbacks(&cbs);
    err = ax25_conn_init(&ctx->conn, local_addr, &cbs, ctx, &conn_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reconnect-as-local conn init failed: %s", esp_err_to_name(err));
        return false;
    }

    flush_rx(ctx);
    xEventGroupClearBits(ctx->events, EVT_CONNECTED);

    err = ax25_conn_connect(&ctx->conn, &ctx->remote_addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reconnect-as-local connect failed: %s", esp_err_to_name(err));
        return false;
    }

    bits = xEventGroupWaitBits(ctx->events,
                               EVT_CONNECTED,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & EVT_CONNECTED) == 0) {
        ESP_LOGW(TAG, "Reconnect-as-local timed out waiting for connect");
        return false;
    }

    return true;
}

static bool parse_banner_counts(const char *capture,
                                uint32_t *out_total,
                                uint32_t *out_new_count)
{
    const char *summary;
    unsigned long total = 0;
    unsigned long new_count = 0;

    if (capture == NULL || out_total == NULL || out_new_count == NULL) {
        return false;
    }

    summary = strstr(capture, "*** No messages for ");
    if (summary != NULL) {
        *out_total = 0;
        *out_new_count = 0;
        return true;
    }

    summary = strstr(capture, "*** You have ");
    if (summary == NULL) {
        return false;
    }

    if (sscanf(summary, "*** You have %lu messages (%lu new)", &total, &new_count) != 2) {
        return false;
    }

    *out_total = (uint32_t)total;
    *out_new_count = (uint32_t)new_count;
    return true;
}

static bool reconnect_and_capture_banner(app_ctx_t *ctx,
                                         uint32_t timeout_ms,
                                         char *capture,
                                         size_t capture_len,
                                         uint32_t *out_total,
                                         uint32_t *out_new_count)
{
    if (!reconnect_session(ctx, timeout_ms)) {
        return false;
    }

    if (read_until_any_token(ctx,
                             PROMPT_TOKENS,
                             sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]),
                             CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                             capture,
                             capture_len) != ESP_OK) {
        return false;
    }

    return parse_banner_counts(capture, out_total, out_new_count);
}

static bool parse_stored_message_id(const char *capture, uint32_t *out_id)
{
    const char *p = capture;
    const char *last = NULL;
    unsigned long id = 0;

    if (capture == NULL || out_id == NULL) {
        return false;
    }

    while ((p = strstr(p, "*** Stored as message ")) != NULL) {
        last = p;
        p++;
    }

    if (last == NULL) {
        return false;
    }

    if (sscanf(last, "*** Stored as message %lu", &id) != 1) {
        return false;
    }

    *out_id = (uint32_t)id;
    return true;
}

static bool id_already_tracked(const app_ctx_t *ctx, uint32_t id)
{
    size_t i;

    if (id == 0) {
        return true;
    }

    for (i = 0; i < ctx->created_count; i++) {
        if (ctx->created_ids[i] == id) {
            return true;
        }
    }

    return false;
}

static void track_created_id(app_ctx_t *ctx, uint32_t id)
{
    if (id_already_tracked(ctx, id)) {
        return;
    }

    if (ctx->created_count >= (sizeof(ctx->created_ids) / sizeof(ctx->created_ids[0]))) {
        ESP_LOGW(TAG, "Created-ID tracker full, dropping id=%lu", (unsigned long)id);
        return;
    }

    ctx->created_ids[ctx->created_count++] = id;
}

static bool line_contains_token(const char *line_start, const char *line_end, const char *token)
{
    size_t token_len;
    const char *p;

    if (line_start == NULL || line_end == NULL || token == NULL || line_end < line_start) {
        return false;
    }

    token_len = strlen(token);
    if (token_len == 0 || (size_t)(line_end - line_start) < token_len) {
        return false;
    }

    for (p = line_start; (size_t)(line_end - p) >= token_len; p++) {
        if (memcmp(p, token, token_len) == 0) {
            return true;
        }
    }

    return false;
}

static size_t collect_subject_ids(const char *capture,
                                  const char *subject_tag,
                                  uint32_t *ids,
                                  size_t ids_cap)
{
    const char *p = capture;
    size_t count = 0;

    if (capture == NULL || subject_tag == NULL || ids == NULL || ids_cap == 0) {
        return 0;
    }

    while (*p != '\0') {
        const char *line_start = p;
        const char *line_end = strchr(p, '\r');
        if (line_end == NULL) { line_end = strchr(p, '\n'); }
        const char *q;
        unsigned long id;
        char *endptr = NULL;

        if (line_end == NULL) {
            line_end = p + strlen(p);
        }

        q = line_start;
        while (q < line_end && (*q == ' ' || *q == '\t')) {
            q++;
        }

        id = strtoul(q, &endptr, 10);
        if (endptr != q && id > 0 && line_contains_token(line_start, line_end, subject_tag)) {
            size_t i;
            bool seen = false;

            for (i = 0; i < count; i++) {
                if (ids[i] == (uint32_t)id) {
                    seen = true;
                    break;
                }
            }

            if (!seen && count < ids_cap) {
                ids[count++] = (uint32_t)id;
            }
        }

        if (*line_end == '\0') {
            break;
        }

        p = line_end + 1;
        if (*p == '\n') { p++; }  /* skip LF after CR if present */
    }

    return count;
}

static size_t collect_run_messages_from_list(app_ctx_t *ctx, char *capture, size_t capture_len)
{
    uint32_t ids[16];
    size_t found = 0;
    size_t i;

    if (!run_cmd_expect_prompt(ctx,
                               "LL 200\r",
                               NULL,
                               CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                               capture,
                               capture_len)) {
        return 0;
    }

    found = collect_subject_ids(capture,
                                ctx->run_subject_tag,
                                ids,
                                sizeof(ids) / sizeof(ids[0]));

    for (i = 0; i < found; i++) {
        track_created_id(ctx, ids[i]);
    }

    return found;
}

static uint32_t latest_tracked_id(const app_ctx_t *ctx)
{
    uint32_t latest = 0;
    size_t i;

    for (i = 0; i < ctx->created_count; i++) {
        if (ctx->created_ids[i] > latest) {
            latest = ctx->created_ids[i];
        }
    }

    return latest;
}

static bool resolve_posted_message_id(app_ctx_t *ctx, char *capture, size_t capture_len)
{
    uint32_t latest;

    collect_run_messages_from_list(ctx, capture, capture_len);
    latest = latest_tracked_id(ctx);
    if (latest == 0) {
        return false;
    }

    ctx->posted_message_id = latest;
    return true;
}

static bool cleanup_created_messages(app_ctx_t *ctx, char *capture, size_t capture_len)
{
    bool all_deleted = true;
    size_t i;
    char line[32];

    if (collect_run_messages_from_list(ctx, capture, capture_len) == 0 && ctx->created_count == 0) {
        return true;
    }

    for (i = 0; i < ctx->created_count; i++) {
        snprintf(line, sizeof(line), "K %lu\r", (unsigned long)ctx->created_ids[i]);
        if (!run_cmd_expect_prompt(ctx,
                                   line,
                                   NULL,
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   capture_len)) {
            all_deleted = false;
            continue;
        }

        if (!capture_contains(capture, "*** Message deleted") &&
            !capture_contains(capture, "*** Message not found")) {
            all_deleted = false;
        }
    }

    if (collect_run_messages_from_list(ctx, capture, capture_len) > 0) {
        all_deleted = false;
    }

    return all_deleted;
}

static void record_result(app_ctx_t *ctx, const char *name, bool pass, const char *detail)
{
    ctx->tests_run++;
    if (!pass) {
        ctx->tests_failed++;
    }

    ctx->last.pass = pass;
    ctx->last.name = name;
    snprintf(ctx->last.detail, sizeof(ctx->last.detail), "%s", detail ? detail : "");

    ESP_LOGI(TAG, "[%s] %s - %s", pass ? "PASS" : "FAIL", name, ctx->last.detail);
}

static bool send_text(ax25_conn_t *conn, const char *text)
{
    if (text == NULL) {
        return false;
    }

    return send_conn_data_with_retry(conn,
                                     (const uint8_t *)text,
                                     strlen(text)) == ESP_OK;
}

static bool run_cmd_expect_prompt(app_ctx_t *ctx,
                                  const char *cmd,
                                  const char *expect,
                                  uint32_t timeout_ms,
                                  char *capture,
                                  size_t capture_len)
{
    bool ok;

    if (!send_text(&ctx->conn, cmd)) {
        return false;
    }

    if (read_until_command_prompt(ctx,
                                  timeout_ms,
                                  capture,
                                  capture_len) != ESP_OK) {
        return false;
    }

    if (expect == NULL || expect[0] == '\0') {
        return true;
    }

    ok = capture_contains(capture, expect);
    return ok;
}

static bool run_cmd_expect_config_prompt(app_ctx_t *ctx,
                                         const char *cmd,
                                         const char *expect,
                                         uint32_t timeout_ms,
                                         char *capture,
                                         size_t capture_len)
{
    bool ok;

    if (!send_text(&ctx->conn, cmd)) {
        return false;
    }

    if (read_until_config_prompt(ctx,
                                 timeout_ms,
                                 capture,
                                 capture_len) != ESP_OK) {
        return false;
    }

    if (expect == NULL || expect[0] == '\0') {
        return true;
    }

    ok = capture_contains(capture, expect);
    return ok;
}

static void run_config_mode_tests(app_ctx_t *ctx, char *capture, size_t capture_len)
{
    uint16_t indices[SYSOP_CHALLENGE_INDEX_COUNT];
    char response[SYSOP_RESPONSE_LEN + 2];
    char detail[120];
    bool auth_expected = (CONFIG_TEST_BBS_SYSOP_SECRET[0] != '\0');
    bool in_config_mode = false;
    bool ok;

    flush_rx(ctx);

    ok = send_text(&ctx->conn, "CONFIG\r");
    if (ok) {
        ok = (read_until_token(ctx,
                               "\r",
                               CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                               capture,
                               capture_len) == ESP_OK);
    }

    if (!ok) {
        record_result(ctx,
                      auth_expected ? "CONFIG auth entry" : "CONFIG no-auth entry",
                      false,
                      "timeout waiting for CONFIG response");
        goto recover;
    }

    if (parse_sysop_challenge(capture, indices)) {
        if (!auth_expected) {
            record_result(ctx,
                          "CONFIG no-auth entry",
                          true,
                          "challenge returned; set TEST_BBS_SYSOP_SECRET to fully exercise CONFIG auth flow");
            goto recover;
        }

        ok = build_sysop_response(CONFIG_TEST_BBS_SYSOP_SECRET, indices, response);
        if (!ok) {
            record_result(ctx,
                          "CONFIG auth entry",
                          false,
                          "unable to build SYSOP response from TEST_BBS_SYSOP_SECRET");
            goto recover;
        }

        response[SYSOP_RESPONSE_LEN] = '\r';
        response[SYSOP_RESPONSE_LEN + 1] = '\0';

        ok = send_text(&ctx->conn, response);
        if (ok) {
            ok = (read_until_config_prompt(ctx,
                                           CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                           capture,
                                           capture_len) == ESP_OK);
        }

        in_config_mode = ok && capture_contains(capture, "*** Entering config mode");
        snprintf(detail,
                 sizeof(detail),
                 ok ? "challenge answered and config prompt received"
                    : "challenge response rejected or config prompt missing");
        record_result(ctx,
                      "CONFIG auth entry",
                      ok && capture_contains(capture, "*** SYSOP authenticated") && in_config_mode,
                      detail);

        if (!in_config_mode) {
            goto recover;
        }
    } else if (capture_contains(capture, "*** Entering config mode")) {
        if (auth_expected) {
            record_result(ctx,
                          "CONFIG auth entry",
                          false,
                          "entered config mode without challenge while TEST_BBS_SYSOP_SECRET is set");
            if (read_until_config_prompt(ctx,
                                         CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                         capture,
                                         capture_len) == ESP_OK) {
                in_config_mode = true;
            }
            goto recover;
        }

        ok = (read_until_config_prompt(ctx,
                                       CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                       capture,
                                       capture_len) == ESP_OK);
        in_config_mode = ok;
        record_result(ctx,
                      "CONFIG no-auth entry",
                      ok,
                      ok ? "entered config mode without SYSOP challenge"
                         : "config prompt missing after entry banner");

        if (!in_config_mode) {
            goto recover;
        }
    } else {
        snprintf(detail, sizeof(detail), "unexpected CONFIG response: %.80s", capture);
        record_result(ctx,
                      auth_expected ? "CONFIG auth entry" : "CONFIG no-auth entry",
                      false,
                      detail);
        goto recover;
    }

    ok = run_cmd_expect_config_prompt(ctx,
                                      "get bbs.callsign\r",
                                      "bbs.callsign=",
                                      CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                      capture,
                                      capture_len);
    ok = ok && capture_contains(capture, CONFIG_TEST_BBS_REMOTE_CALLSIGN);
    record_result(ctx,
                  "CONFIG get bbs.callsign",
                  ok,
                  ok ? "read-only config query succeeded" : "missing bbs.callsign output");

    ok = send_text(&ctx->conn, "exit\r");
    if (ok) {
        ok = (read_until_command_prompt(ctx,
                                        CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                        capture,
                                        capture_len) == ESP_OK);
    }
    ok = ok && capture_contains(capture, "*** Exiting config mode");
    record_result(ctx,
                  "CONFIG exit",
                  ok,
                  ok ? "returned to BBS prompt" : "failed to leave config mode");
    return;

recover:
    if (in_config_mode && send_text(&ctx->conn, "exit\r")) {
        (void)read_until_command_prompt(ctx,
                                        CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                        capture,
                                        capture_len);
        return;
    }

    if (reconnect_session(ctx, CONFIG_TEST_BBS_CONNECT_TIMEOUT_MS)) {
        (void)read_until_command_prompt(ctx,
                                        CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                        capture,
                                        capture_len);
    }
}

static void run_script(app_ctx_t *ctx)
{
    char *capture = ctx->capture;
    char line[64];
    char detail[96];
    char primary_call[16];
    char private_subject[48];
    char recipient_subject[48];
    bool ok;
    bool switched_to_alt = false;
    bool recovered = false;
    esp_err_t banner_err;
    uint32_t banner_total = 0;
    uint32_t banner_new = 0;
    uint32_t banner_after_post_total = 0;
    uint32_t banner_after_post_new = 0;
    uint32_t banner_after_read_total = 0;
    uint32_t banner_after_read_new = 0;
    uint32_t banner_after_delete_total = 0;
    uint32_t banner_after_delete_new = 0;
    uint32_t private_message_id = 0;
    uint32_t recipient_message_id = 0;

    snprintf(ctx->run_subject_tag,
             sizeof(ctx->run_subject_tag),
             "AUTOTEST-%lu",
             (unsigned long)(esp_timer_get_time() / 1000ULL));
    snprintf(private_subject,
             sizeof(private_subject),
             "%s-PRIV",
             ctx->run_subject_tag);
    snprintf(recipient_subject,
             sizeof(recipient_subject),
             "%s-TO-ME",
             ctx->run_subject_tag);
    ax25_address_to_string(&ctx->primary_local_addr, primary_call, sizeof(primary_call));

    banner_err = read_until_any_token(ctx,
                                      PROMPT_TOKENS,
                                      sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]),
                                      CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                      capture,
                                      CAPTURE_MAX);
    if (banner_err != ESP_OK) {
        recovered = reconnect_session(ctx, CONFIG_TEST_BBS_CONNECT_TIMEOUT_MS);
        if (recovered) {
            banner_err = read_until_any_token(ctx,
                                              PROMPT_TOKENS,
                                              sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]),
                                              CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                              capture,
                                              CAPTURE_MAX);
        }
    }

    if (banner_err == ESP_OK) {
        ok = parse_banner_counts(capture, &banner_total, &banner_new);
        snprintf(detail,
                 sizeof(detail),
                 ok ? "parsed total=%lu new=%lu" : "unable to parse banner summary",
                 (unsigned long)banner_total,
                 (unsigned long)banner_new);
        record_result(ctx, "Banner unread summary", ok, detail);

        ok = capture_contains(capture, "Welcome") || capture_contains(capture, "BBS");
        record_result(ctx,
                      "Banner and prompt",
                      ok,
                      ok ? (recovered ? "received after reconnect recovery" : "received")
                         : "missing banner text");
    } else {
        record_result(ctx,
                      "Banner and prompt",
                      false,
                      recovered ? "timeout waiting for prompt after reconnect recovery"
                                : "timeout waiting for first prompt");
    }

    ok = run_cmd_expect_prompt(ctx,
                               "I\r",
                               "Call:",
                               CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                               capture,
                               CAPTURE_MAX);
    record_result(ctx, "I command", ok, ok ? "info returned" : "missing info output");

    ok = run_cmd_expect_prompt(ctx,
                               "J\r",
                               "Heard:",
                               CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                               capture,
                               CAPTURE_MAX);
    record_result(ctx, "J command", ok, ok ? "heard list returned" : "heard output missing");

    run_config_mode_tests(ctx, capture, CAPTURE_MAX);

    ok = run_cmd_expect_prompt(ctx,
                               "LL 5\r",
                               "# From",
                               CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                               capture,
                               CAPTURE_MAX);
    if (!ok) {
        ok = capture_contains(capture, "*** No messages") ||
             capture_contains(capture, "*** No readable messages");
    }
    record_result(ctx, "LL command", ok, ok ? "list output valid" : "list output invalid");

    ok = send_text(&ctx->conn, "SB TEST\r");
    if (ok) {
        ok = (read_until_token(ctx,
                               "Subject: ",
                               CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                               capture,
                               CAPTURE_MAX) == ESP_OK);
    }
    record_result(ctx, "SB command", ok, ok ? "entered subject mode" : "failed to enter subject mode");

    if (ok) {
        snprintf(line, sizeof(line), "%s\r", ctx->run_subject_tag);
        ok = send_text(&ctx->conn, line);
        if (ok) {
            ok = (read_until_token(ctx,
                                   "Enter body, use /EX to finish:\r",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX) == ESP_OK);
        }
        record_result(ctx, "Subject entry", ok, ok ? "body prompt received" : "body prompt missing");
    }

    if (ok) {
        ok = send_text(&ctx->conn, "line one from test_bbs\r");
        if (ok) {
            ok = send_text(&ctx->conn, "/EX\r");
        }
        if (ok) {
            ok = (read_until_any_token(ctx,
                                       PROMPT_TOKENS,
                                       sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]),
                                       CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                       capture,
                                       CAPTURE_MAX) == ESP_OK);
        }
        if (ok) {
            uint32_t parsed_id = 0;

            if (parse_stored_message_id(capture, &parsed_id)) {
                track_created_id(ctx, parsed_id);
                ctx->posted_message_id = parsed_id;
            }

            /* Confirm using LL so we use the newest ID for this run tag. */
            ok = resolve_posted_message_id(ctx, capture, CAPTURE_MAX);
        }
        record_result(ctx,
                      "Post + /EX",
                      ok,
                      ok ? "message stored" : "store confirmation missing");
    }

    if (ctx->posted_message_id != 0) {
          ok = reconnect_and_capture_banner(ctx,
                                                        CONFIG_TEST_BBS_CONNECT_TIMEOUT_MS,
                                                        capture,
                                                        CAPTURE_MAX,
                                                        &banner_after_post_total,
                                                        &banner_after_post_new);
          ok = ok &&
                 banner_after_post_total >= (banner_total + 1U) &&
                 banner_after_post_new <= banner_after_post_total &&
                 (banner_after_post_new == 0U ||
                  banner_after_post_new == (banner_new + 1U));
          snprintf(detail,
                      sizeof(detail),
                      ok ? "total=%lu new=%lu (persisted last-read may keep new=0)"
                          : "unexpected post banner total=%lu new=%lu",
                      (unsigned long)banner_after_post_total,
                      (unsigned long)banner_after_post_new);
        record_result(ctx, "Banner after post", ok, detail);

        ok = run_cmd_expect_prompt(ctx,
                                   "L\r",
                                   ctx->run_subject_tag,
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        record_result(ctx, "L command", ok, ok ? "posted message listed" : "posted message missing from list");

        snprintf(line, sizeof(line), "R %lu\r", (unsigned long)ctx->posted_message_id);
        ok = run_cmd_expect_prompt(ctx,
                                   line,
                                   "line one from test_bbs",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS + 10000,
                                   capture,
                                   CAPTURE_MAX);
        if (!ok && capture_contains(capture, "*** Message not found")) {
            /* Re-resolve by run tag and retry once in case the parsed ID was stale. */
            if (resolve_posted_message_id(ctx, capture, CAPTURE_MAX)) {
                snprintf(line, sizeof(line), "R %lu\r", (unsigned long)ctx->posted_message_id);
                ok = run_cmd_expect_prompt(ctx,
                                           line,
                                           "line one from test_bbs",
                                           CONFIG_TEST_BBS_STEP_TIMEOUT_MS + 10000,
                                           capture,
                                           CAPTURE_MAX);
            }
        }
        record_result(ctx, "R command", ok, ok ? "message body read" : "failed to read stored message");

        ok = reconnect_and_capture_banner(ctx,
                                          CONFIG_TEST_BBS_CONNECT_TIMEOUT_MS,
                                          capture,
                                          CAPTURE_MAX,
                                          &banner_after_read_total,
                                          &banner_after_read_new);
        ok = ok &&
             banner_after_read_total == banner_after_post_total &&
             banner_after_read_new <= banner_after_post_new;
        snprintf(detail,
             sizeof(detail),
                 ok ? "total=%lu new=%lu" : "unexpected read banner total=%lu new=%lu",
             (unsigned long)banner_after_read_total,
             (unsigned long)banner_after_read_new);
        record_result(ctx, "Banner after read", ok, detail);

        snprintf(line, sizeof(line), "K %lu\r", (unsigned long)ctx->posted_message_id);
        ok = run_cmd_expect_prompt(ctx,
                                   line,
                                   "*** Message deleted",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        record_result(ctx, "K command", ok, ok ? "message deleted" : "delete confirmation missing");

        snprintf(line, sizeof(line), "R %lu\r", (unsigned long)ctx->posted_message_id);
        ok = run_cmd_expect_prompt(ctx,
                                   line,
                                   "*** Message not found",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        record_result(ctx, "R after K", ok, ok ? "not-found confirmed" : "unexpected read result after delete");

        ok = reconnect_and_capture_banner(ctx,
                                          CONFIG_TEST_BBS_CONNECT_TIMEOUT_MS,
                                          capture,
                                          CAPTURE_MAX,
                                          &banner_after_delete_total,
                                          &banner_after_delete_new);
        ok = ok && banner_after_delete_total + 1U == banner_after_post_total && banner_after_delete_new == 0;
        snprintf(detail,
                 sizeof(detail),
                 ok ? "total=%lu new=%lu" : "unexpected delete banner total=%lu new=%lu",
                 (unsigned long)banner_after_delete_total,
                 (unsigned long)banner_after_delete_new);
        record_result(ctx, "Banner after delete", ok, detail);
    }

    ok = send_text(&ctx->conn, "SP N1PVT-1\r");
    if (ok) {
        ok = (read_until_token(ctx,
                               "Subject: ",
                               CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                               capture,
                               CAPTURE_MAX) == ESP_OK);
    }
    record_result(ctx, "SP private command", ok, ok ? "entered private subject mode" : "failed to enter private subject mode");

    if (ok) {
        snprintf(line, sizeof(line), "%s\r", private_subject);
        ok = send_text(&ctx->conn, line);
        if (ok) {
            ok = (read_until_token(ctx,
                                   "Enter body, use /EX to finish:\r",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX) == ESP_OK);
        }
        record_result(ctx,
                      "Private subject entry",
                      ok,
                      ok ? "private body prompt received" : "private body prompt missing");
    }

    if (ok) {
        ok = send_text(&ctx->conn, "private line from test_bbs\r");
        if (ok) {
            ok = send_text(&ctx->conn, "/EX\r");
        }
        if (ok) {
            ok = (read_until_any_token(ctx,
                                       PROMPT_TOKENS,
                                       sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]),
                                       CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                       capture,
                                       CAPTURE_MAX) == ESP_OK);
        }
        if (ok) {
            ok = parse_stored_message_id(capture, &private_message_id);
            if (ok) {
                track_created_id(ctx, private_message_id);
            }
        }
        record_result(ctx,
                      "Private post + /EX",
                      ok,
                      ok ? "private message stored" : "private store confirmation missing");
    }

    if (private_message_id != 0) {
        ok = run_cmd_expect_prompt(ctx,
                                   "L\r",
                                   NULL,
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        ok = ok && capture_contains(capture, private_subject);
        record_result(ctx,
                      "Private visible in L",
                      ok,
                      ok ? "L includes sent private message" : "L missing sent private message");

        ok = run_cmd_expect_prompt(ctx,
                                   "LM\r",
                                   NULL,
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        ok = ok && capture_contains(capture, private_subject);
        record_result(ctx,
                      "Private visible in LM",
                      ok,
                      ok ? "mine-only list includes private post" : "mine-only list missing private post");

        ok = run_cmd_expect_prompt(ctx,
                       "LL 5\r",
                       NULL,
                       CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                       capture,
                       CAPTURE_MAX);
        ok = ok && capture_contains(capture, private_subject);
        record_result(ctx,
                  "Private visible in LL",
                  ok,
                  ok ? "newest list includes private post" : "newest list missing private post");

        snprintf(line, sizeof(line), "R %lu\r", (unsigned long)private_message_id);
        ok = run_cmd_expect_prompt(ctx,
                                   line,
                                   "private line from test_bbs",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        record_result(ctx,
                      "Private sender read",
                      ok,
                      ok ? "sender can read own private message" : "sender could not read own private message");

        snprintf(line, sizeof(line), "K %lu\r", (unsigned long)private_message_id);
        ok = run_cmd_expect_prompt(ctx,
                                   line,
                                   "*** Message deleted",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        record_result(ctx,
                      "Private delete",
                      ok,
                      ok ? "sender deleted private post" : "private delete confirmation missing");
    }

    ok = reconnect_as_local(ctx, &ctx->alt_local_addr, CONFIG_TEST_BBS_CONNECT_TIMEOUT_MS);
    switched_to_alt = ok;
    if (ok) {
        ok = (read_until_any_token(ctx,
                                   PROMPT_TOKENS,
                                   sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]),
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX) == ESP_OK);
    }
    record_result(ctx,
                  "Recipient-K alt session",
                  ok,
                  ok ? "connected as alternate callsign" : "failed to switch to alternate callsign");

    if (ok) {
        snprintf(line, sizeof(line), "SP %s\r", primary_call);
        ok = send_text(&ctx->conn, line);
        if (ok) {
            ok = (read_until_token(ctx,
                                   "Subject: ",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX) == ESP_OK);
        }
        record_result(ctx,
                      "Recipient-K setup SP",
                      ok,
                      ok ? "alt sender entered private subject mode" : "failed to enter private mode for recipient regression");
    }

    if (ok) {
        snprintf(line, sizeof(line), "%s\r", recipient_subject);
        ok = send_text(&ctx->conn, line);
        if (ok) {
            ok = (read_until_token(ctx,
                                   "Enter body, use /EX to finish:\r",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX) == ESP_OK);
        }
    }

    if (ok) {
        ok = send_text(&ctx->conn, "recipient regression payload\r");
        if (ok) {
            ok = send_text(&ctx->conn, "/EX\r");
        }
        if (ok) {
            ok = (read_until_any_token(ctx,
                                       PROMPT_TOKENS,
                                       sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]),
                                       CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                       capture,
                                       CAPTURE_MAX) == ESP_OK);
        }
        if (ok) {
            ok = parse_stored_message_id(capture, &recipient_message_id);
            if (ok) {
                track_created_id(ctx, recipient_message_id);
            }
        }
        record_result(ctx,
                      "Recipient-K setup post",
                      ok,
                      ok ? "message to primary user stored" : "failed to store recipient regression message");
    }

    if (switched_to_alt) {
        ok = reconnect_as_local(ctx, &ctx->primary_local_addr, CONFIG_TEST_BBS_CONNECT_TIMEOUT_MS);
        if (ok) {
            ok = (read_until_any_token(ctx,
                                       PROMPT_TOKENS,
                                       sizeof(PROMPT_TOKENS) / sizeof(PROMPT_TOKENS[0]),
                                       CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                       capture,
                                       CAPTURE_MAX) == ESP_OK);
        }
        record_result(ctx,
                      "Recipient-K return primary",
                      ok,
                      ok ? "returned to primary callsign session" : "failed to return to primary session");
    }

    if (recipient_message_id != 0) {
        ok = run_cmd_expect_prompt(ctx,
                                   "L\r",
                                   NULL,
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        ok = ok && capture_contains(capture, recipient_subject);
        record_result(ctx,
                      "Recipient visible in L",
                      ok,
                      ok ? "L includes message addressed to current callsign"
                         : "L missing message addressed to current callsign");

          snprintf(line, sizeof(line), "R %lu\r", (unsigned long)recipient_message_id);
          ok = run_cmd_expect_prompt(ctx,
                                              line,
                                              "recipient regression payload",
                                              CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                              capture,
                                              CAPTURE_MAX);
          record_result(ctx,
                             "Recipient read regression",
                             ok,
                             ok ? "recipient can read addressed message"
                                 : "recipient could not read addressed message");

        snprintf(line, sizeof(line), "K %lu\r", (unsigned long)recipient_message_id);
        ok = run_cmd_expect_prompt(ctx,
                                   line,
                                   "*** Message deleted",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        record_result(ctx,
                      "Recipient-K regression",
                      ok,
                      ok ? "recipient successfully deleted addressed message"
                         : "recipient could not delete addressed message");

        snprintf(line, sizeof(line), "R %lu\r", (unsigned long)recipient_message_id);
        ok = run_cmd_expect_prompt(ctx,
                                   line,
                                   "*** Message not found",
                                   CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                                   capture,
                                   CAPTURE_MAX);
        record_result(ctx,
                      "Recipient-K verify removed",
                      ok,
                      ok ? "message no longer present" : "message still present after recipient delete");
    }

    ok = cleanup_created_messages(ctx, capture, CAPTURE_MAX);
    record_result(ctx,
                  "Cleanup created messages",
                  ok,
                  ok ? "no run-tagged messages left" : "some run-tagged messages remain");

    ok = run_cmd_expect_prompt(ctx,
                               "SP BAD*\r",
                               "*** SP requires valid callsign",
                               CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                               capture,
                               CAPTURE_MAX);
    record_result(ctx, "SP validation", ok, ok ? "invalid callsign rejected" : "invalid callsign accepted");

    ok = run_cmd_expect_prompt(ctx,
                               "XYZ\r",
                               NULL,
                               CONFIG_TEST_BBS_STEP_TIMEOUT_MS,
                               capture,
                               CAPTURE_MAX);
    ok = ok &&
         (capture_contains(capture, "*** Unknown command") ||
          capture_contains(capture, "Commands:"));
    record_result(ctx, "Unknown command help", ok, ok ? "help shown" : "help not shown");

    xEventGroupClearBits(ctx->events, EVT_DISCONNECTED);
    ok = send_text(&ctx->conn, "B\r");
    if (ok) {
        EventBits_t bits = xEventGroupWaitBits(ctx->events,
                                               EVT_DISCONNECTED,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(CONFIG_TEST_BBS_STEP_TIMEOUT_MS));
        ok = ((bits & EVT_DISCONNECTED) != 0);
    }
    record_result(ctx, "B disconnect", ok, ok ? "remote disconnected" : "disconnect timeout");

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "BBS tests complete: run=%lu failed=%lu passed=%lu",
             (unsigned long)ctx->tests_run,
             (unsigned long)ctx->tests_failed,
             (unsigned long)(ctx->tests_run - ctx->tests_failed));
    ESP_LOGI(TAG, "========================================");
}

static void test_task(void *arg)
{
    app_ctx_t *ctx = (app_ctx_t *)arg;
    EventBits_t bits;

    bits = xEventGroupWaitBits(ctx->events,
                               EVT_CONNECTED,
                               pdFALSE,
                               pdFALSE,
                               pdMS_TO_TICKS(CONFIG_TEST_BBS_CONNECT_TIMEOUT_MS));

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

void app_main(void)
{
    static ax25_router_port_t phy_port;
    static TaskHandle_t task;
#if CONFIG_TEST_BBS_KISS_TCP
    static ax25_wifi_t wifi_ctx;
    static ax25_phy_kiss_tcp_client_t phy;
#elif CONFIG_TEST_BBS_KISS_UART
    static ax25_phy_kiss_uart_t phy;
#endif
    ax25_conn_callbacks_t cbs = {0};
    ax25_conn_config_t conn_cfg = AX25_CONN_CONFIG_DEFAULT();
    ax25_address_t local_addr;
    ax25_address_t alt_local_addr;
    ax25_address_t remote_addr;
    esp_err_t err;

    ESP_LOGI(TAG, "ESP-AX25 test_bbs starting");

    memset(&s_ctx, 0, sizeof(s_ctx));

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(ax25_cfg_init(NULL, 0));

#if CONFIG_TEST_BBS_KISS_TCP
    {
        char err_msg[96] = {0};
        ESP_ERROR_CHECK(ax25_cfg_set("wifi.sta.ssid", CONFIG_TEST_BBS_WIFI_SSID,
                                     err_msg, sizeof(err_msg)));
        ESP_ERROR_CHECK(ax25_cfg_set("wifi.sta.password", CONFIG_TEST_BBS_WIFI_PASSWORD,
                                     err_msg, sizeof(err_msg)));
        ESP_ERROR_CHECK(ax25_cfg_set("wifi.ap.ssid", CONFIG_TEST_BBS_WIFI_AP_SSID,
                                     err_msg, sizeof(err_msg)));
        ESP_ERROR_CHECK(ax25_cfg_set("wifi.ap.password", CONFIG_TEST_BBS_WIFI_AP_PASSWORD,
                                     err_msg, sizeof(err_msg)));
    }

    err = ax25_wifi_start(&wifi_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return;
    }
#endif

    if (ax25_address_from_string(CONFIG_TEST_BBS_LOCAL_CALLSIGN, &local_addr) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid local callsign: %s", CONFIG_TEST_BBS_LOCAL_CALLSIGN);
        return;
    }
    if (ax25_address_from_string(CONFIG_TEST_BBS_REMOTE_CALLSIGN, &remote_addr) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid remote callsign: %s", CONFIG_TEST_BBS_REMOTE_CALLSIGN);
        return;
    }
    if (ax25_address_from_string(ALT_LOCAL_CALLSIGN, &alt_local_addr) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid alternate local callsign: %s", ALT_LOCAL_CALLSIGN);
        return;
    }
    s_ctx.primary_local_addr = local_addr;
    s_ctx.alt_local_addr = alt_local_addr;
    s_ctx.remote_addr = remote_addr;

    err = ax25_router_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Router init failed: %s", esp_err_to_name(err));
        return;
    }

    s_ctx.events = xEventGroupCreate();
    s_ctx.rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_chunk_t));
    if (s_ctx.events == NULL || s_ctx.rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event group or RX queue");
        return;
    }

    s_ctx.rx_buf = buffer_create(RX_BUF_SIZE, rx_fill_cb, &s_ctx);
    if (s_ctx.rx_buf == NULL) {
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

#if CONFIG_TEST_BBS_KISS_TCP
    const ax25_phy_kiss_tcp_client_config_t phy_cfg = {
        .host = CONFIG_TEST_BBS_REMOTE_HOST,
        .port = CONFIG_TEST_BBS_REMOTE_TCP_PORT,
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

    s_ctx.tcp_phy = &phy;
    s_ctx.tcp_phy_cfg = phy_cfg;
#elif CONFIG_TEST_BBS_KISS_UART
    ESP_ERROR_CHECK(configure_uart_keys_from_kconfig());
    err = ax25_phy_kiss_uart_init(phy_frame_cb, &phy_port, &phy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART KISS init failed: %s", esp_err_to_name(err));
        return;
    }
#endif

    setup_conn_callbacks(&cbs);

    err = ax25_conn_init(&s_ctx.conn, &local_addr, &cbs, &s_ctx, &conn_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ax25_conn_init failed: %s", esp_err_to_name(err));
        return;
    }

#if CONFIG_TEST_BBS_KISS_TCP
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

    if (xTaskCreate(test_task, "test_bbs_task", 9216, &s_ctx, 5, &task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create test task");
        return;
    }

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_ctx.events,
                                               EVT_FINISHED,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(1000));
        if ((bits & EVT_FINISHED) != 0) {
            ESP_LOGI(TAG, "test_bbs complete, idling");
            break;
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Idle: tests_run=%lu tests_failed=%lu",
                 (unsigned long)s_ctx.tests_run,
                 (unsigned long)s_ctx.tests_failed);
    }
}
