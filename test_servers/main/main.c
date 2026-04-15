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
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "ax25_address.h"
#include "ax25_agwpe.h"
#include "ax25_agwpe_client.h"
#include "ax25_config.h"
#include "ax25_conn.h"
#include "ax25_frame.h"
#include "ax25_phy_kiss_tcp_client.h"
#include "ax25_router.h"
#include "ax25_types.h"
#include "ax25_wifi.h"

#ifndef CONFIG_TEST_SERVER_ENABLE_KISS_CLIENT
#define CONFIG_TEST_SERVER_ENABLE_KISS_CLIENT 0
#endif

#ifndef CONFIG_TEST_SERVER_ENABLE_AGWPE_CLIENT
#define CONFIG_TEST_SERVER_ENABLE_AGWPE_CLIENT 0
#endif

#ifndef CONFIG_TEST_SERVER_KISS_PORT
#define CONFIG_TEST_SERVER_KISS_PORT 0
#endif

#ifndef CONFIG_TEST_SERVER_AGWPE_PORT
#define CONFIG_TEST_SERVER_AGWPE_PORT 0
#endif

static const char *TAG = "TEST_SERVER";

#define RX_QUEUE_LEN 32

#define SESSION_CONNECTED_BIT BIT0
#define SESSION_DISCONNECTED_BIT BIT1
#define SESSION_ABORT_BIT BIT2

#define HEARD_CMD "j\r"
#define BYE_CMD   "b\r"

typedef esp_err_t (*bbs_send_fn_t)(void *ctx, const uint8_t *data, size_t len);

typedef enum {
    TEST_SERVER_TRANSPORT_KISS = 0,
    TEST_SERVER_TRANSPORT_AGWPE,
} test_server_transport_t;

typedef struct {
    size_t len;
    uint8_t data[AX25_MAX_INFO_LEN];
} rx_chunk_t;

typedef struct {
    QueueHandle_t queue;
    TickType_t deadline;
    rx_chunk_t current;
    size_t offset;
    bool chunk_valid;
    EventGroupHandle_t abort_events;
    EventBits_t abort_bits;
    volatile esp_err_t *abort_err;
} stream_reader_t;

typedef struct {
    EventGroupHandle_t events;
    stream_reader_t reader;
    ax25_conn_t conn;
    ax25_router_port_t app_port;
    ax25_router_port_t phy_port;
    ax25_phy_kiss_tcp_client_t phy;
    bool router_ready;
    bool app_port_registered;
    bool phy_port_registered;
    bool conn_ready;
    bool phy_ready;
    volatile esp_err_t wait_abort_err;
} kiss_session_t;

typedef struct {
    EventGroupHandle_t events;
    stream_reader_t reader;
    ax25_agwpe_client_t client;
    const char *local_call;
    const char *remote_call;
    uint8_t port;
    bool client_ready;
    volatile esp_err_t wait_abort_err;
} agwpe_session_t;

typedef struct {
    uint8_t port;
    const char *local_call;
    const char *remote_call;
    ax25_agwpe_client_t *client;
} agwpe_send_ctx_t;

static const char *transport_to_string(test_server_transport_t transport)
{
    return transport == TEST_SERVER_TRANSPORT_KISS ? "KISS/TCP" : "AGWPE/TCP";
}

static esp_err_t configure_wifi_keys_from_kconfig(void)
{
    char err_msg[96] = {0};

    if (ax25_cfg_set("wifi.sta.ssid", CONFIG_TEST_SERVER_WIFI_SSID,
                     err_msg, sizeof(err_msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set wifi.sta.ssid: %s", err_msg);
        return ESP_FAIL;
    }
    if (ax25_cfg_set("wifi.sta.password", CONFIG_TEST_SERVER_WIFI_PASSWORD,
                     err_msg, sizeof(err_msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set wifi.sta.password: %s", err_msg);
        return ESP_FAIL;
    }
    if (ax25_cfg_set("wifi.ap.ssid", CONFIG_TEST_SERVER_WIFI_AP_SSID,
                     err_msg, sizeof(err_msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set wifi.ap.ssid: %s", err_msg);
        return ESP_FAIL;
    }
    if (ax25_cfg_set("wifi.ap.password", CONFIG_TEST_SERVER_WIFI_AP_PASSWORD,
                     err_msg, sizeof(err_msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set wifi.ap.password: %s", err_msg);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t enqueue_rx_data(QueueHandle_t queue, const uint8_t *data, size_t len)
{
    if (queue == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    while (offset < len) {
        rx_chunk_t chunk = {0};
        chunk.len = len - offset;
        if (chunk.len > sizeof(chunk.data)) {
            chunk.len = sizeof(chunk.data);
        }
        memcpy(chunk.data, data + offset, chunk.len);
        if (xQueueSend(queue, &chunk, 0) != pdTRUE) {
            return ESP_ERR_NO_MEM;
        }
        offset += chunk.len;
    }

    return ESP_OK;
}

static esp_err_t stream_reader_abort_error(const stream_reader_t *reader)
{
    if (reader != NULL && reader->abort_err != NULL && *reader->abort_err != ESP_OK) {
        return *reader->abort_err;
    }

    return ESP_ERR_INVALID_STATE;
}

static bool stream_reader_is_aborted(const stream_reader_t *reader)
{
    if (reader == NULL || reader->abort_events == NULL || reader->abort_bits == 0) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(reader->abort_events);
    return (bits & reader->abort_bits) != 0;
}

static int stream_read_byte(stream_reader_t *reader, uint8_t *out)
{
    if (!reader->chunk_valid || reader->offset >= reader->current.len) {
        while (1) {
            TickType_t now = xTaskGetTickCount();
            if (reader->deadline <= now) {
                return 0;
            }
            if (stream_reader_is_aborted(reader)) {
                return -2;
            }
            if (reader->queue == NULL) {
                return -1;
            }

            TickType_t remaining = reader->deadline - now;
            TickType_t wait_ticks = pdMS_TO_TICKS(100);
            if (wait_ticks == 0) {
                wait_ticks = 1;
            }
            if (remaining < wait_ticks) {
                wait_ticks = remaining;
            }

            if (xQueueReceive(reader->queue, &reader->current, wait_ticks) == pdTRUE) {
                reader->offset = 0;
                reader->chunk_valid = true;
                break;
            }
        }
    }

    if (reader->offset >= reader->current.len) {
        return 0;
    }

    *out = reader->current.data[reader->offset++];
    if (reader->offset >= reader->current.len) {
        reader->chunk_valid = false;
    }

    return 1;
}

static esp_err_t stream_readline(stream_reader_t *reader, char *out, size_t out_len)
{
    size_t pos = 0;

    while (1) {
        uint8_t byte = 0;
        int rc = stream_read_byte(reader, &byte);
        if (rc == 0) {
            return ESP_ERR_TIMEOUT;
        }
        if (rc == -2) {
            return stream_reader_abort_error(reader);
        }
        if (rc < 0) {
            return ESP_FAIL;
        }
        if (byte == '\r' || byte == '\n') {
            if (pos == 0) {
                continue;
            }
            out[pos] = '\0';
            return ESP_OK;
        }
        if (pos < out_len - 1) {
            out[pos++] = (char)byte;
        }
    }
}

static esp_err_t wait_for_prompt(stream_reader_t *reader, const char *prompt, uint32_t timeout_ms)
{
    char line[160];
    size_t prompt_len = strlen(prompt);
    ESP_LOGI(TAG, "Waiting for prompt '%s'", prompt);

    reader->deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (1) {
        esp_err_t err = stream_readline(reader, line, sizeof(line));
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "BBS: %s", line);
        if (strncmp(line, prompt, prompt_len) == 0) {
            return ESP_OK;
        }
    }
}

static esp_err_t send_command_and_wait_for_prompt(stream_reader_t *reader,
                                                  bbs_send_fn_t send_fn,
                                                  void *send_ctx,
                                                  const char *command,
                                                  const char *step_name,
                                                  uint32_t timeout_ms)
{
    esp_err_t err = send_fn(send_ctx, (const uint8_t *)command, strlen(command));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send %s command: %s", step_name, esp_err_to_name(err));
        return err;
    }

    err = wait_for_prompt(reader, CONFIG_TEST_SERVER_PROMPT_PREFIX, timeout_ms);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Timed out waiting for prompt after %s command", step_name);
        } else {
            ESP_LOGE(TAG, "Session ended while waiting for prompt after %s command: %s",
                     step_name,
                     esp_err_to_name(err));
        }
        return err;
    }

    return ESP_OK;
}

static esp_err_t run_bbs_exercises(stream_reader_t *reader,
                                   bbs_send_fn_t send_fn,
                                   void *send_ctx,
                                   uint32_t timeout_ms)
{
#if CONFIG_TEST_SERVER_EXERCISE_HEARD_LIST
    esp_err_t err = send_command_and_wait_for_prompt(reader,
                                                     send_fn,
                                                     send_ctx,
                                                     HEARD_CMD,
                                                     "heard-list",
                                                     timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
#else
    (void)reader;
    (void)send_fn;
    (void)send_ctx;
    (void)timeout_ms;
#endif

    return ESP_OK;
}

static esp_err_t wait_for_kiss_socket(ax25_phy_kiss_tcp_client_t *phy, uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (phy->sock >= 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_for_agwpe_socket(ax25_agwpe_client_t *client, uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (client->sock >= 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_ERR_TIMEOUT;
}

static void kiss_phy_input_cb(const ax25_frame_t *frame, void *user_data)
{
    ax25_router_send(frame, (ax25_router_port_t *)user_data);
}

static void kiss_phy_output_cb(const ax25_frame_t *frame, void *user_data)
{
    ax25_phy_kiss_tcp_client_send(frame, (ax25_phy_kiss_tcp_client_t *)user_data);
}

static void kiss_app_port_on_frame(const ax25_frame_t *frame, void *user_data)
{
    ax25_conn_on_frame((ax25_conn_t *)user_data, frame);
}

static void kiss_conn_on_connect(ax25_address_t remote_addr, bool is_local_initiated, void *user_data)
{
    kiss_session_t *session = (kiss_session_t *)user_data;
    (void)remote_addr;
    (void)is_local_initiated;
    session->wait_abort_err = ESP_OK;
    xEventGroupClearBits(session->events, SESSION_DISCONNECTED_BIT | SESSION_ABORT_BIT);
    xEventGroupSetBits(session->events, SESSION_CONNECTED_BIT);
}

static void kiss_conn_on_disconnect(void *user_data)
{
    kiss_session_t *session = (kiss_session_t *)user_data;
    if (session->wait_abort_err == ESP_OK) {
        session->wait_abort_err = ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(session->events, SESSION_DISCONNECTED_BIT | SESSION_ABORT_BIT);
}

static void kiss_conn_on_error(const ax25_conn_error_t *error, void *user_data)
{
    kiss_session_t *session = (kiss_session_t *)user_data;
    ESP_LOGE(TAG, "KISS session AX.25 error %d: %s",
             error->code,
             error->message != NULL ? error->message : "");
    if (session != NULL && session->wait_abort_err == ESP_OK) {
        session->wait_abort_err = ESP_FAIL;
        xEventGroupSetBits(session->events, SESSION_ABORT_BIT);
    }
}

static void kiss_conn_on_data(const uint8_t *data, size_t len, void *user_data)
{
    kiss_session_t *session = (kiss_session_t *)user_data;
    if (enqueue_rx_data(session->reader.queue, data, len) != ESP_OK) {
        ESP_LOGW(TAG, "KISS receive queue full, dropping %u bytes", (unsigned)len);
    }
}

static void kiss_conn_on_frame(const ax25_frame_t *frame, void *user_data)
{
    kiss_session_t *session = (kiss_session_t *)user_data;
    ax25_router_send(frame, &session->app_port);
}

static esp_err_t kiss_send_connected_command(void *ctx, const uint8_t *data, size_t len)
{
    return ax25_conn_send_data((ax25_conn_t *)ctx, data, len);
}

static void cleanup_kiss_session(kiss_session_t *session)
{
    if (session->conn_ready) {
        ax25_conn_deinit(&session->conn);
        session->conn_ready = false;
    }
    if (session->phy_ready) {
        ax25_phy_kiss_tcp_client_deinit(&session->phy);
        session->phy_ready = false;
    }
    if (session->phy_port_registered) {
        ax25_router_remove_port(&session->phy_port);
        session->phy_port_registered = false;
    }
    if (session->app_port_registered) {
        ax25_router_remove_port(&session->app_port);
        session->app_port_registered = false;
    }
    if (session->router_ready) {
        ax25_router_deinit();
        session->router_ready = false;
    }
    if (session->reader.queue != NULL) {
        vQueueDelete(session->reader.queue);
        session->reader.queue = NULL;
    }
    if (session->events != NULL) {
        vEventGroupDelete(session->events);
        session->events = NULL;
    }
}

static esp_err_t run_kiss_session(const ax25_address_t *local_addr,
                                  const ax25_address_t *remote_addr,
                                  uint32_t session_index)
{
    kiss_session_t *session = calloc(1, sizeof(*session));
    esp_err_t err = ESP_OK;

    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Session %u using KISS-over-TCP", (unsigned)(session_index + 1));

    session->events = xEventGroupCreate();
    session->reader.queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_chunk_t));
    if (session->events == NULL || session->reader.queue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    session->wait_abort_err = ESP_OK;
    session->reader.abort_events = session->events;
    session->reader.abort_bits = SESSION_ABORT_BIT;
    session->reader.abort_err = &session->wait_abort_err;

    err = ax25_router_init();
    if (err != ESP_OK) {
        goto cleanup;
    }
    session->router_ready = true;

    session->app_port.destination = *local_addr;
    session->app_port.mode = AX25_PORT_STATIC;
    session->app_port.on_tx_frame = kiss_app_port_on_frame;
    session->app_port.user_data = &session->conn;
    err = ax25_router_register_port(&session->app_port);
    if (err != ESP_OK) {
        goto cleanup;
    }
    session->app_port_registered = true;

    ax25_phy_kiss_tcp_client_config_t phy_config = {
        .host = CONFIG_TEST_SERVER_TARGET_HOST,
        .port = CONFIG_TEST_SERVER_KISS_PORT,
        .on_rx_frame = kiss_phy_input_cb,
        .user_data = &session->phy_port,
        .connect_timeout_ms = CONFIG_TEST_SERVER_CONNECT_TIMEOUT_MS,
        .reconnect_delay_ms = 1000,
    };
    err = ax25_phy_kiss_tcp_client_init(&phy_config, &session->phy);
    if (err != ESP_OK) {
        goto cleanup;
    }
    session->phy_ready = true;

    session->phy_port.mode = AX25_PORT_DEFAULT;
    session->phy_port.on_tx_frame = kiss_phy_output_cb;
    session->phy_port.user_data = &session->phy;
    err = ax25_router_register_port(&session->phy_port);
    if (err != ESP_OK) {
        goto cleanup;
    }
    session->phy_port_registered = true;

    ax25_conn_callbacks_t callbacks = {
        .on_connect = kiss_conn_on_connect,
        .on_disconnect = kiss_conn_on_disconnect,
        .on_error = kiss_conn_on_error,
        .on_data = kiss_conn_on_data,
        .on_tx_frame = kiss_conn_on_frame,
    };
    err = ax25_conn_init(&session->conn, local_addr, &callbacks, session, NULL);
    if (err != ESP_OK) {
        goto cleanup;
    }
    session->conn_ready = true;

    err = wait_for_kiss_socket(&session->phy, CONFIG_TEST_SERVER_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Timed out waiting for KISS TCP connection");
        goto cleanup;
    }

    err = ax25_conn_connect(&session->conn, remote_addr);
    if (err != ESP_OK) {
        goto cleanup;
    }

    EventBits_t bits = xEventGroupWaitBits(session->events,
                                           SESSION_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(CONFIG_TEST_SERVER_CONNECT_TIMEOUT_MS));
    if ((bits & SESSION_CONNECTED_BIT) == 0) {
        err = ESP_ERR_TIMEOUT;
        ESP_LOGE(TAG, "Timed out waiting for AX.25 KISS session connect");
        goto cleanup;
    }

    err = wait_for_prompt(&session->reader,
                          CONFIG_TEST_SERVER_PROMPT_PREFIX,
                          CONFIG_TEST_SERVER_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Timed out waiting for first prompt over KISS");
        } else {
            ESP_LOGE(TAG, "KISS session ended while waiting for first prompt: %s",
                     esp_err_to_name(err));
        }
        goto shutdown;
    }

    err = run_bbs_exercises(&session->reader,
                            kiss_send_connected_command,
                            &session->conn,
                            CONFIG_TEST_SERVER_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        goto shutdown;
    }

    err = ax25_conn_send_data(&session->conn, (const uint8_t *)BYE_CMD, sizeof(BYE_CMD) - 1);
    if (err != ESP_OK) {
        goto shutdown;
    }

shutdown:
    ax25_conn_shutdown(&session->conn);
    xEventGroupWaitBits(session->events,
                        SESSION_DISCONNECTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(CONFIG_TEST_SERVER_CONNECT_TIMEOUT_MS));

cleanup:
    cleanup_kiss_session(session);
    free(session);
    return err;
}

static void log_agwpe_info(const agwpe_frame_t *frame)
{
    if (frame->header.data_len == 0) {
        return;
    }

    char text[AGWPE_MAX_DATA_LEN + 1];
    size_t copy_len = frame->header.data_len;
    if (copy_len > AGWPE_MAX_DATA_LEN) {
        copy_len = AGWPE_MAX_DATA_LEN;
    }
    memcpy(text, frame->data, copy_len);
    text[copy_len] = '\0';
    ESP_LOGI(TAG, "AGWPE: %s", text);
}

static void agwpe_client_on_frame(const agwpe_frame_t *frame, void *user_data)
{
    agwpe_session_t *session = (agwpe_session_t *)user_data;

    switch (frame->header.data_kind) {
        case 'C':
            log_agwpe_info(frame);
            session->wait_abort_err = ESP_OK;
            xEventGroupClearBits(session->events, SESSION_DISCONNECTED_BIT | SESSION_ABORT_BIT);
            xEventGroupSetBits(session->events, SESSION_CONNECTED_BIT);
            break;
        case 'D':
            if (frame->header.data_len > 0 &&
                enqueue_rx_data(session->reader.queue,
                                frame->data,
                                frame->header.data_len) != ESP_OK) {
                ESP_LOGW(TAG, "AGWPE receive queue full, dropping %u bytes",
                         (unsigned)frame->header.data_len);
            }
            break;
        case 'd':
            log_agwpe_info(frame);
            if (session->wait_abort_err == ESP_OK) {
                session->wait_abort_err = ESP_ERR_INVALID_STATE;
            }
            xEventGroupSetBits(session->events, SESSION_DISCONNECTED_BIT | SESSION_ABORT_BIT);
            break;
        default:
            ESP_LOGD(TAG, "AGWPE frame kind %c len=%u",
                     frame->header.data_kind,
                     (unsigned)frame->header.data_len);
            break;
    }
}

static esp_err_t agwpe_send_connected_command(void *ctx, const uint8_t *data, size_t len)
{
    agwpe_send_ctx_t *send_ctx = (agwpe_send_ctx_t *)ctx;
    return ax25_agwpe_client_send_connected(send_ctx->port,
                                            send_ctx->local_call,
                                            send_ctx->remote_call,
                                            data,
                                            len,
                                            AX25_PID_TEXT,
                                            send_ctx->client);
}

static void cleanup_agwpe_session(agwpe_session_t *session)
{
    if (session->client_ready) {
        ax25_agwpe_client_deinit(&session->client);
        session->client_ready = false;
    }
    if (session->reader.queue != NULL) {
        vQueueDelete(session->reader.queue);
        session->reader.queue = NULL;
    }
    if (session->events != NULL) {
        vEventGroupDelete(session->events);
        session->events = NULL;
    }
}

static esp_err_t run_agwpe_session(const char *local_call,
                                   const char *remote_call,
                                   uint32_t session_index)
{
    agwpe_session_t *session = calloc(1, sizeof(*session));
    agwpe_send_ctx_t send_ctx = {0};
    esp_err_t err = ESP_OK;

    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }

    session->local_call = local_call;
    session->remote_call = remote_call;
    session->port = 0;

    ESP_LOGI(TAG, "Session %u using AGWPE-over-TCP", (unsigned)(session_index + 1));

    session->events = xEventGroupCreate();
    session->reader.queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_chunk_t));
    if (session->events == NULL || session->reader.queue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    session->wait_abort_err = ESP_OK;
    session->reader.abort_events = session->events;
    session->reader.abort_bits = SESSION_ABORT_BIT;
    session->reader.abort_err = &session->wait_abort_err;

    ax25_agwpe_client_config_t config = {
        .host = CONFIG_TEST_SERVER_TARGET_HOST,
        .port = CONFIG_TEST_SERVER_AGWPE_PORT,
        .on_rx_frame = agwpe_client_on_frame,
        .user_data = session,
        .connect_timeout_ms = CONFIG_TEST_SERVER_CONNECT_TIMEOUT_MS,
        .reconnect_delay_ms = 1000,
    };
    err = ax25_agwpe_client_init(&config, &session->client);
    if (err != ESP_OK) {
        goto cleanup;
    }
    session->client_ready = true;

    err = wait_for_agwpe_socket(&session->client, CONFIG_TEST_SERVER_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Timed out waiting for AGWPE TCP connection");
        goto cleanup;
    }

    err = ax25_agwpe_client_register_callsign(session->port, local_call, &session->client);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = ax25_agwpe_client_connect(session->port, local_call, remote_call, &session->client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    EventBits_t bits = xEventGroupWaitBits(session->events,
                                           SESSION_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(CONFIG_TEST_SERVER_CONNECT_TIMEOUT_MS));
    if ((bits & SESSION_CONNECTED_BIT) == 0) {
        err = ESP_ERR_TIMEOUT;
        ESP_LOGE(TAG, "Timed out waiting for AGWPE connected event");
        goto cleanup;
    }

    err = wait_for_prompt(&session->reader,
                          CONFIG_TEST_SERVER_PROMPT_PREFIX,
                          CONFIG_TEST_SERVER_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Timed out waiting for first prompt over AGWPE");
        } else {
            ESP_LOGE(TAG, "AGWPE session ended while waiting for first prompt: %s",
                     esp_err_to_name(err));
        }
        goto shutdown;
    }

    send_ctx.port = session->port;
    send_ctx.local_call = local_call;
    send_ctx.remote_call = remote_call;
    send_ctx.client = &session->client;

    err = run_bbs_exercises(&session->reader,
                            agwpe_send_connected_command,
                            &send_ctx,
                            CONFIG_TEST_SERVER_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        goto shutdown;
    }

    err = ax25_agwpe_client_send_connected(session->port,
                                           local_call,
                                           remote_call,
                                           (const uint8_t *)BYE_CMD,
                                           sizeof(BYE_CMD) - 1,
                                           AX25_PID_TEXT,
                                           &session->client);
    if (err != ESP_OK) {
        goto shutdown;
    }

shutdown:
    EventBits_t disconnect_bits = xEventGroupWaitBits(session->events,
                                                      SESSION_DISCONNECTED_BIT,
                                                      pdFALSE,
                                                      pdTRUE,
                                                      pdMS_TO_TICKS(500));
    if ((disconnect_bits & SESSION_DISCONNECTED_BIT) == 0) {
        ax25_agwpe_client_disconnect(session->port, local_call, remote_call, &session->client);
    }
    xEventGroupWaitBits(session->events,
                        SESSION_DISCONNECTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(CONFIG_TEST_SERVER_CONNECT_TIMEOUT_MS));

cleanup:
    cleanup_agwpe_session(session);
    free(session);
    return err;
}

static test_server_transport_t select_transport(uint32_t session_index)
{
#if CONFIG_TEST_SERVER_ENABLE_KISS_CLIENT && CONFIG_TEST_SERVER_ENABLE_AGWPE_CLIENT
    return (session_index % 2u == 0u) ? TEST_SERVER_TRANSPORT_KISS : TEST_SERVER_TRANSPORT_AGWPE;
#elif CONFIG_TEST_SERVER_ENABLE_KISS_CLIENT
    (void)session_index;
    return TEST_SERVER_TRANSPORT_KISS;
#else
    (void)session_index;
    return TEST_SERVER_TRANSPORT_AGWPE;
#endif
}

void app_main(void)
{
    esp_err_t err;
    ax25_address_t local_addr;
    ax25_address_t remote_addr;

    ESP_LOGI(TAG, "AX.25 test_server");

    if (!CONFIG_TEST_SERVER_ENABLE_KISS_CLIENT && !CONFIG_TEST_SERVER_ENABLE_AGWPE_CLIENT) {
        ESP_LOGE(TAG, "Both KISS and AGWPE clients are disabled. Enable at least one transport.");
        return;
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(ax25_cfg_init(NULL, 0));
    ESP_ERROR_CHECK(configure_wifi_keys_from_kconfig());

    ax25_wifi_t wifi = {0};
    err = ax25_wifi_start(&wifi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        return;
    }

    if (ax25_address_from_string(CONFIG_TEST_SERVER_LOCAL_CALLSIGN, &local_addr) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid local callsign: %s", CONFIG_TEST_SERVER_LOCAL_CALLSIGN);
        return;
    }
    if (ax25_address_from_string(CONFIG_TEST_SERVER_REMOTE_CALLSIGN, &remote_addr) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid remote callsign: %s", CONFIG_TEST_SERVER_REMOTE_CALLSIGN);
        return;
    }

    for (uint32_t session_index = 0;
         session_index < (uint32_t)CONFIG_TEST_SERVER_SESSION_COUNT;
         ++session_index) {
        test_server_transport_t transport = select_transport(session_index);
        ESP_LOGI(TAG, "Starting session %u/%u via %s",
                 (unsigned)(session_index + 1),
                 (unsigned)CONFIG_TEST_SERVER_SESSION_COUNT,
                 transport_to_string(transport));

        if (transport == TEST_SERVER_TRANSPORT_KISS) {
            err = run_kiss_session(&local_addr, &remote_addr, session_index);
        } else {
            err = run_agwpe_session(CONFIG_TEST_SERVER_LOCAL_CALLSIGN,
                                    CONFIG_TEST_SERVER_REMOTE_CALLSIGN,
                                    session_index);
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Session %u failed over %s: %s",
                     (unsigned)(session_index + 1),
                     transport_to_string(transport),
                     esp_err_to_name(err));
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "All test_server sessions completed successfully");
}