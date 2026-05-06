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

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "ax25_address.h"
#include "ax25_agwpe.h"
#include "ax25_agwpe_server.h"
#include "ax25_conn.h"
#include "ax25_frame.h"
#include "ax25_kiss_tcp_server.h"
#include "ax25_log_tcp_server.h"
#include "ax25_monitor_tcp_server.h"
#include "ax25_phy_kiss_uart.h"
#include "ax25_phy_tcp_server.h"
#include "ax25_beacon.h"
#include "ax25_config.h"
#include "ax25_digipeater.h"
#include "ax25_router.h"
#include "ax25_wifi.h"

#include "bbs.h"
#include "bbs_heard.h"
#include "bbs_storage.h"

static const char *TAG = "BBS";

#ifndef CONFIG_TNC_PWR_LED_GPIO
#define CONFIG_TNC_PWR_LED_GPIO 11
#endif

#ifndef CONFIG_TNC_BBS_LED_GPIO
#define CONFIG_TNC_BBS_LED_GPIO 3
#endif

#define STR_HELPER(_x) #_x
#define STR(_x) STR_HELPER(_x)

#define AX25_UART_SEND_RETRY_MAX 20
#define AX25_UART_SEND_RETRY_DELAY_MS 10
#define AGWPE_CLEANUP_QUEUE_LEN 4
#define AGWPE_CLEANUP_TASK_STACK_BYTES 4096

typedef struct {
    uint32_t internal_free;
    uint32_t internal_largest;
    uint32_t spiram_free;
    uint32_t spiram_largest;
} heap_snapshot_t;

static heap_snapshot_t heap_snapshot_capture(void)
{
    heap_snapshot_t snapshot = {
        .internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .spiram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };

    return snapshot;
}

static void log_heap_snapshot_delta(const char *label,
                                    const heap_snapshot_t *before,
                                    const heap_snapshot_t *after)
{
    if (label == NULL || before == NULL || after == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "%s heap: internal free %lu -> %lu (%+ld), largest %lu -> %lu (%+ld)",
             label,
             (unsigned long)before->internal_free,
             (unsigned long)after->internal_free,
             (long)after->internal_free - (long)before->internal_free,
             (unsigned long)before->internal_largest,
             (unsigned long)after->internal_largest,
             (long)after->internal_largest - (long)before->internal_largest);
    ESP_LOGI(TAG,
             "%s heap: PSRAM free %lu -> %lu (%+ld), largest %lu -> %lu (%+ld)",
             label,
             (unsigned long)before->spiram_free,
             (unsigned long)after->spiram_free,
             (long)after->spiram_free - (long)before->spiram_free,
             (unsigned long)before->spiram_largest,
             (unsigned long)after->spiram_largest,
             (long)after->spiram_largest - (long)before->spiram_largest);
}

static void log_heap_snapshot(const char *label, const heap_snapshot_t *snapshot)
{
    if (label == NULL || snapshot == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "%s heap now: internal free %lu, largest %lu",
             label,
             (unsigned long)snapshot->internal_free,
             (unsigned long)snapshot->internal_largest);
    ESP_LOGI(TAG,
             "%s heap now: PSRAM free %lu, largest %lu",
             label,
             (unsigned long)snapshot->spiram_free,
             (unsigned long)snapshot->spiram_largest);
}

static const ax25_cfg_param_t s_bbs_local_cfg_schema[] = {
    {
        .parameter = "bbs.callsign",
        .nvs_key = "bbs_callsign",
        .default_value = "N0CALL-1",
        .range = "",
        .type = AX25_CFG_TYPE_STRING,
        .hide = false,
    },
    {
        .parameter = "bbs.sysop_secret",
        .nvs_key = "bbs_sysop__ef90",
        .default_value = "",
        .range = "",
        .type = AX25_CFG_TYPE_STRING,
        .hide = true,
    },
    {
        .parameter = "uart.enable",
        .nvs_key = "uart_enable",
        .default_value = "1",
        .range = "0,1",
        .type = AX25_CFG_TYPE_BOOL,
        .hide = false,
    },
    {
        .parameter = "uart.type",
        .nvs_key = "uart_type",
        .default_value = "term",
        .range = "kiss term",
        .type = AX25_CFG_TYPE_ENUM,
        .hide = false,
    },
    {
        .parameter = "led.pwr_pin",
        .nvs_key = "led_pwr_pin",
        .default_value = STR(CONFIG_TNC_PWR_LED_GPIO),
        .range = "0,48",
        .type = AX25_CFG_TYPE_INT,
        .hide = false,
    },
    {
        .parameter = "led.bbs_pin",
        .nvs_key = "led_bbs_gpio",
        .default_value = STR(CONFIG_TNC_BBS_LED_GPIO),
        .range = "0,48",
        .type = AX25_CFG_TYPE_INT,
        .hide = false,
    },
};

#define BBS_EVT_QUEUE_LEN 16
#define BBS_WORKER_STACK_BYTES 6144

static bool bbs_sysop_secret_is_valid(const char *secret)
{
    if (secret == NULL) {
        return false;
    }

    size_t len = strlen(secret);
    if (len == 0) {
        return true;
    }

    if (len < CONFIG_BBS_SYSOP_SECRET_MIN_LEN || len > CONFIG_BBS_SYSOP_SECRET_MAX_LEN) {
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

typedef enum {
    BBS_EVT_CONNECT = 1,
    BBS_EVT_DISCONNECT,
    BBS_EVT_DATA,
} bbs_evt_type_t;

typedef struct {
    bbs_evt_type_t type;
    ax25_address_t remote_addr;
    bool is_local_initiated;
    size_t len;
    uint8_t data[AX25_MAX_INFO_LEN];
} bbs_evt_t;

typedef struct {
    ax25_conn_t conn;
    ax25_router_port_t app_port;
    ax25_address_t local_addr;
    bbs_storage_t storage;
    bbs_heard_t heard;
    bbs_t bbs;
    QueueHandle_t evt_queue;
    StaticQueue_t evt_queue_buf;
    uint8_t *evt_queue_storage;
    TaskHandle_t bbs_task;
    StaticTask_t bbs_task_tcb;
    StackType_t bbs_task_stack[BBS_WORKER_STACK_BYTES / sizeof(StackType_t)];
} bbs_app_ctx_t;

static bbs_app_ctx_t s_app;

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_ON,
    LED_MODE_BLINK,
} led_mode_t;

typedef struct {
    bool initialized;
    bool blink_phase_on;
    int pwr_pin;
    int bbs_pin;
    led_mode_t pwr_mode;
    led_mode_t bbs_mode;
} app_leds_t;

static app_leds_t s_leds;

static bool led_gpio_is_valid(int gpio_num)
{
    // GPIO 0 is a reserved/strapping pin, treat as disabled
    if (gpio_num <= 0 || gpio_num >= GPIO_NUM_MAX) {
        return false;
    }

    return GPIO_IS_VALID_OUTPUT_GPIO(gpio_num);
}

static int led_resolve_gpio(const char *parameter, int fallback, const char *label)
{
    int gpio_num = ax25_cfg_get_int(parameter);

    if (led_gpio_is_valid(gpio_num)) {
        return gpio_num;
    }

    if (led_gpio_is_valid(fallback)) {
        ESP_LOGW(TAG,
                 "%s GPIO %d from %s invalid, using menuconfig fallback %d",
                 label,
                 gpio_num,
                 parameter,
                 fallback);
        return fallback;
    }

    ESP_LOGW(TAG,
             "%s GPIO invalid in runtime (%d) and fallback (%d); disabling",
             label,
             gpio_num,
             fallback);
    return -1;
}

static int led_mode_level(led_mode_t mode)
{
    switch (mode) {
        case LED_MODE_ON:
            return 1;
        case LED_MODE_BLINK:
            return s_leds.blink_phase_on ? 1 : 0;
        case LED_MODE_OFF:
        default:
            return 0;
    }
}

static void led_apply_outputs(void)
{
    if (!s_leds.initialized) {
        return;
    }

    if (led_gpio_is_valid(s_leds.pwr_pin)) {
        gpio_set_level((gpio_num_t)s_leds.pwr_pin, led_mode_level(s_leds.pwr_mode));
    }

    if (led_gpio_is_valid(s_leds.bbs_pin)) {
        gpio_set_level((gpio_num_t)s_leds.bbs_pin, led_mode_level(s_leds.bbs_mode));
    }
}

static void led_refresh_pwr_mode(void)
{
    s_leds.pwr_mode = ax25_cfg_is_safety_timer_active() ? LED_MODE_BLINK : LED_MODE_ON;
}

static void led_refresh_bbs_mode(const bbs_app_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->bbs.connected) {
        s_leds.bbs_mode = LED_MODE_ON;
        return;
    }

    s_leds.bbs_mode = bbs_has_unread_messages((bbs_t *)&ctx->bbs) ? LED_MODE_BLINK : LED_MODE_OFF;
}

static void led_tick(const bbs_app_ctx_t *ctx)
{
    if (!s_leds.initialized) {
        return;
    }

    s_leds.blink_phase_on = !s_leds.blink_phase_on;
    led_refresh_pwr_mode();
    led_refresh_bbs_mode(ctx);
    led_apply_outputs();
}

static void led_init_from_config(void)
{
    memset(&s_leds, 0, sizeof(s_leds));

    s_leds.pwr_pin = led_resolve_gpio("led.pwr_pin", CONFIG_TNC_PWR_LED_GPIO, "PWR LED");
    s_leds.bbs_pin = led_resolve_gpio("led.bbs_pin", CONFIG_TNC_BBS_LED_GPIO, "BBS LED");

    uint64_t pin_mask = 0;
    if (led_gpio_is_valid(s_leds.pwr_pin)) {
        pin_mask |= (1ULL << s_leds.pwr_pin);
    }
    if (led_gpio_is_valid(s_leds.bbs_pin)) {
        pin_mask |= (1ULL << s_leds.bbs_pin);
    }

    if (pin_mask == 0) {
        return;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return;
    }

    s_leds.blink_phase_on = false;
    s_leds.pwr_mode = LED_MODE_ON;
    s_leds.bbs_mode = LED_MODE_OFF;
    s_leds.initialized = true;
    led_apply_outputs();
}

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

static void log_transport_send_result(const char *path, esp_err_t err)
{
    if (err == ESP_OK) {
        return;
    }

    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "%s: transport queue full", path);
        return;
    }

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "%s: transport not ready", path);
        return;
    }

    ESP_LOGW(TAG, "%s: transport send failed: %s", path, esp_err_to_name(err));
}

static esp_err_t send_uart_frame_with_retry(const ax25_frame_t *frame,
                                            ax25_phy_kiss_uart_t *uart_phy,
                                            const char *path)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (frame == NULL || uart_phy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int attempt = 0; attempt < AX25_UART_SEND_RETRY_MAX; attempt++) {
        err = ax25_phy_kiss_uart_send(frame, uart_phy);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (err != ESP_ERR_NO_MEM) {
            log_transport_send_result(path, err);
            return err;
        }

        if (attempt == 0) {
            ESP_LOGW(TAG, "%s: UART TX queue full, retrying", path);
        }
        vTaskDelay(pdMS_TO_TICKS(AX25_UART_SEND_RETRY_DELAY_MS));
    }

    ESP_LOGW(TAG, "%s: UART TX queue remained full after %d retries",
             path,
             AX25_UART_SEND_RETRY_MAX);
    return err;
}

static void bbs_worker_task(void *arg)
{
    bbs_app_ctx_t *ctx = (bbs_app_ctx_t *)arg;
    bbs_evt_t evt;

    for (;;) {
        if (xQueueReceive(ctx->evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (evt.type) {
            case BBS_EVT_CONNECT:
                bbs_on_connect(&ctx->bbs, &evt.remote_addr, evt.is_local_initiated);
                break;
            case BBS_EVT_DISCONNECT:
                bbs_on_disconnect(&ctx->bbs);
                break;
            case BBS_EVT_DATA:
                bbs_on_data(&ctx->bbs, evt.data, evt.len);
                break;
            default:
                break;
        }

        led_refresh_bbs_mode(ctx);
        led_apply_outputs();
    }
}

static void uart_on_frame(const ax25_frame_t *frame, void *user_data)
{
    esp_err_t err = ax25_router_send(frame, (ax25_router_port_t *)user_data);
    log_router_send_result("UART RX", err);
}

static void uart_noop_frame_cb(const ax25_frame_t *frame, void *user_data)
{
    (void)frame;
    (void)user_data;
}

#if CONFIG_TNC_ENABLE_DIGIPEATER_SERVICE
static esp_err_t digipeater_tx_cb(const ax25_frame_t *frame, void *user_data)
{
    return send_uart_frame_with_retry(frame,
                                      (ax25_phy_kiss_uart_t *)user_data,
                                      "Digipeater relay");
}
#endif

typedef struct {
    ax25_router_port_t port;
    bool in_use;
} kiss_port_slot_t;

static size_t s_kiss_max_clients;
static kiss_port_slot_t *s_kiss_pool;
static StaticSemaphore_t s_kiss_pool_mutex_buf;
static SemaphoreHandle_t s_kiss_pool_mutex;

static void *alloc_runtime_pool_prefer_psram(size_t count, size_t elem_size)
{
#if CONFIG_SPIRAM
    void *ptr = heap_caps_calloc(count, elem_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != NULL) {
        return ptr;
    }
#endif

    return calloc(count, elem_size);
}

static void kiss_port_frame_cb(const ax25_frame_t *frame, void *user_data)
{
    esp_err_t err = ax25_kiss_tcp_server_conn_send(
        (ax25_kiss_tcp_server_conn_t *)user_data,
        frame);
    log_transport_send_result("KISS TCP client", err);
}

static void kiss_on_connected(ax25_kiss_tcp_server_conn_t *conn)
{
    xSemaphoreTake(s_kiss_pool_mutex, portMAX_DELAY);

    kiss_port_slot_t *slot = NULL;
    for (size_t i = 0; i < s_kiss_max_clients; i++) {
        if (!s_kiss_pool[i].in_use) {
            slot = &s_kiss_pool[i];
            slot->in_use = true;
            break;
        }
    }

    xSemaphoreGive(s_kiss_pool_mutex);

    if (slot == NULL) {
        ESP_LOGE(TAG, "KISS connect: no free router slot");
        return;
    }

    memset(&slot->port, 0, sizeof(slot->port));
    slot->port.mode = AX25_PORT_DYNAMIC;
    slot->port.on_tx_frame = kiss_port_frame_cb;
    slot->port.user_data = conn;

    esp_err_t err = ax25_router_register_port(&slot->port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "KISS connect: register port failed (%s)", esp_err_to_name(err));
        xSemaphoreTake(s_kiss_pool_mutex, portMAX_DELAY);
        slot->in_use = false;
        xSemaphoreGive(s_kiss_pool_mutex);
        return;
    }

    ax25_kiss_tcp_server_conn_set_user_data(conn, &slot->port);
    ESP_LOGI(TAG, "KISS TCP client connected");
}

static void kiss_on_disconnected(ax25_kiss_tcp_server_conn_t *conn)
{
    ax25_router_port_t *port =
        (ax25_router_port_t *)ax25_kiss_tcp_server_conn_get_user_data(conn);
    if (port == NULL) {
        return;
    }

    ax25_router_remove_port(port);

    kiss_port_slot_t *slot = (kiss_port_slot_t *)port;
    xSemaphoreTake(s_kiss_pool_mutex, portMAX_DELAY);
    slot->in_use = false;
    xSemaphoreGive(s_kiss_pool_mutex);

    ESP_LOGI(TAG, "KISS TCP client disconnected");
}

static void kiss_on_frame(ax25_kiss_tcp_server_conn_t *conn, const ax25_frame_t *frame)
{
    ax25_router_port_t *port =
        (ax25_router_port_t *)ax25_kiss_tcp_server_conn_get_user_data(conn);
    if (port != NULL) {
        esp_err_t err = ax25_router_send(frame, port);
        log_router_send_result("KISS TCP RX", err);
    }
}

typedef struct {
    bool in_use;
    ax25_phy_tcp_server_conn_t *conn;
    agwpe_decoder_t decoder;
    ax25_agwpe_server_t *client;
    bool cleanup_pending;
} agwpe_client_slot_t;

typedef struct {
    agwpe_client_slot_t *slot;
    ax25_agwpe_server_t *client;
} agwpe_cleanup_req_t;

static agwpe_client_slot_t *s_agwpe_clients;
static size_t s_agwpe_max_clients;
static StaticSemaphore_t s_agwpe_clients_mutex_buf;
static SemaphoreHandle_t s_agwpe_clients_mutex;
static StaticQueue_t s_agwpe_cleanup_queue_buf;
static uint8_t s_agwpe_cleanup_queue_storage[AGWPE_CLEANUP_QUEUE_LEN * sizeof(agwpe_cleanup_req_t)];
static QueueHandle_t s_agwpe_cleanup_queue;
static StaticTask_t s_agwpe_cleanup_task_tcb;
static StackType_t s_agwpe_cleanup_task_stack[AGWPE_CLEANUP_TASK_STACK_BYTES / sizeof(StackType_t)];

static void free_agwpe_client_slot(agwpe_client_slot_t *slot);

static void uart_port_frame_cb(const ax25_frame_t *frame, void *user_data)
{
    send_uart_frame_with_retry(frame,
                               (ax25_phy_kiss_uart_t *)user_data,
                               "UART default port");

    // Notify all AGWPE clients of locally transmitted UI frames (for monitor mode)
    if (frame && frame->type == AX25_FRAME_UI) {
        xSemaphoreTake(s_agwpe_clients_mutex, portMAX_DELAY);
        for (size_t i = 0; i < s_agwpe_max_clients; i++) {
            agwpe_client_slot_t *slot = &s_agwpe_clients[i];
            if (slot->in_use && slot->client) {
                ax25_agwpe_server_client_ax25_out(slot->client, frame);
            }
        }
        xSemaphoreGive(s_agwpe_clients_mutex);
    }
}

static void agwpe_cleanup_task(void *arg)
{
    (void)arg;

    agwpe_cleanup_req_t req;
    for (;;) {
        if (xQueueReceive(s_agwpe_cleanup_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (req.client != NULL) {
            ax25_agwpe_server_remove_client(req.client);
        }

        if (req.slot != NULL) {
            req.slot->client = NULL;
            req.slot->conn = NULL;
            req.slot->cleanup_pending = false;
            free_agwpe_client_slot(req.slot);
        }

        ESP_LOGI(TAG, "AGWPE client disconnected");
    }
}

static agwpe_client_slot_t *alloc_agwpe_client_slot(void)
{
    agwpe_client_slot_t *slot = NULL;

    xSemaphoreTake(s_agwpe_clients_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_agwpe_max_clients; i++) {
        if (!s_agwpe_clients[i].in_use) {
            slot = &s_agwpe_clients[i];
            memset(slot, 0, sizeof(*slot));
            slot->in_use = true;
            break;
        }
    }
    xSemaphoreGive(s_agwpe_clients_mutex);

    return slot;
}

static void free_agwpe_client_slot(agwpe_client_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }

    xSemaphoreTake(s_agwpe_clients_mutex, portMAX_DELAY);
    memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(s_agwpe_clients_mutex);
}

static void on_agwpe_frame_out(const agwpe_frame_t *frame, void *user_data)
{
    agwpe_client_slot_t *slot = (agwpe_client_slot_t *)user_data;
    if (slot == NULL || slot->conn == NULL) {
        return;
    }

    uint8_t out[AGWPE_MAX_FRAME_SIZE];
    size_t n = agwpe_frame_encode(frame, out, sizeof(out));
    if (n == 0) {
        ESP_LOGW(TAG, "AGWPE encode failed for kind '%c'", frame->header.data_kind);
        return;
    }

    esp_err_t err = ax25_phy_tcp_server_conn_send(slot->conn, out, n);
    log_transport_send_result("AGWPE TCP client", err);
}

static void on_agwpe_frame_in(const agwpe_frame_t *frame, void *user_data)
{
    agwpe_client_slot_t *slot = (agwpe_client_slot_t *)user_data;
    if (slot == NULL || slot->client == NULL) {
        return;
    }

    esp_err_t err = ax25_agwpe_server_client_agwpe_in(slot->client, frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AGWPE client in failed: %s", esp_err_to_name(err));
    }
}

static void agwpe_on_connected(ax25_phy_tcp_server_conn_t *conn)
{
    agwpe_client_slot_t *slot = alloc_agwpe_client_slot();
    if (slot == NULL) {
        ESP_LOGE(TAG, "No free AGWPE client slot");
        return;
    }

    slot->conn = conn;
    agwpe_decoder_init(&slot->decoder, on_agwpe_frame_in, slot);

    ax25_agwpe_server_config_t cfg = {
        .on_agwpe_frame = on_agwpe_frame_out,
        .user_data = slot,
        .port = 0,
        .port_description = "AX25 BBS UART",
        .tx_queue_depth = 0,
        .task_stack_size = 0,
        .task_priority = 0,
    };

    esp_err_t err = ax25_agwpe_server_add_client(&cfg, &slot->client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ax25_agwpe_server_add_client failed: %s", esp_err_to_name(err));
        free_agwpe_client_slot(slot);
        return;
    }
    ax25_phy_tcp_server_conn_set_user_data(conn, slot);
    ESP_LOGI(TAG, "AGWPE client connected");
}

static void agwpe_on_disconnected(ax25_phy_tcp_server_conn_t *conn)
{
    agwpe_client_slot_t *slot =
        (agwpe_client_slot_t *)ax25_phy_tcp_server_conn_get_user_data(conn);
    if (slot == NULL) {
        return;
    }

    ax25_phy_tcp_server_conn_set_user_data(conn, NULL);
    slot->conn = NULL;

    if (slot->client == NULL) {
        free_agwpe_client_slot(slot);
        ESP_LOGI(TAG, "AGWPE client disconnected");
        return;
    }

    if (slot->cleanup_pending) {
        ESP_LOGW(TAG, "AGWPE client cleanup already pending");
        return;
    }

    if (s_agwpe_cleanup_queue == NULL) {
        ESP_LOGE(TAG, "AGWPE cleanup queue unavailable; leaving client slot allocated");
        return;
    }

    agwpe_cleanup_req_t req = {
        .slot = slot,
        .client = slot->client,
    };
    slot->cleanup_pending = true;
    if (xQueueSend(s_agwpe_cleanup_queue, &req, pdMS_TO_TICKS(1000)) != pdTRUE) {
        slot->cleanup_pending = false;
        ESP_LOGE(TAG, "Failed to enqueue AGWPE cleanup request");
        return;
    }

    ESP_LOGI(TAG, "AGWPE client cleanup scheduled");
}

static void agwpe_on_data(ax25_phy_tcp_server_conn_t *conn,
                          const uint8_t *data,
                          size_t len)
{
    agwpe_client_slot_t *slot =
        (agwpe_client_slot_t *)ax25_phy_tcp_server_conn_get_user_data(conn);
    if (slot == NULL || slot->client == NULL) {
        return;
    }

    agwpe_decoder_process_bytes(&slot->decoder, data, len);
}

static void app_port_on_frame(const ax25_frame_t *frame, void *user_data)
{
    ax25_conn_t *conn = (ax25_conn_t *)user_data;
    char src[16] = {0};
    char dst[16] = {0};
    ax25_address_to_string(&frame->source, src, sizeof(src));
    ax25_address_to_string(&frame->destination, dst, sizeof(dst));
    ESP_LOGI(TAG, "BBS app_port RX frame: %s -> %s type=%d ctrl=0x%02X pid=0x%02X len=%u",
             src, dst, (int)frame->type, frame->control, frame->pid,
             (unsigned)frame->payload_len);

    esp_err_t err = ax25_conn_on_frame(conn, frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BBS app_port: ax25_conn_on_frame returned %s", esp_err_to_name(err));
    }
}

static void conn_on_connect(ax25_address_t remote_addr, bool is_local_initiated, void *user_data)
{
    bbs_app_ctx_t *ctx = (bbs_app_ctx_t *)user_data;
    char remote[16] = {0};
    ax25_address_to_string(&remote_addr, remote, sizeof(remote));
    ESP_LOGI(TAG, "BBS conn connected: remote=%s local_initiated=%d",
             remote, (int)is_local_initiated);
    bbs_evt_t evt = {
        .type = BBS_EVT_CONNECT,
        .remote_addr = remote_addr,
        .is_local_initiated = is_local_initiated,
        .len = 0,
    };
    if (xQueueSend(ctx->evt_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "BBS event queue full: dropping CONNECT event");
    }
}

static void conn_on_disconnect(void *user_data)
{
    bbs_app_ctx_t *ctx = (bbs_app_ctx_t *)user_data;
    ESP_LOGI(TAG, "BBS conn disconnected");
    bbs_evt_t evt = {
        .type = BBS_EVT_DISCONNECT,
        .len = 0,
    };
    if (xQueueSend(ctx->evt_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "BBS event queue full: dropping DISCONNECT event");
    }
}

static void conn_on_data(const uint8_t *data, size_t len, void *user_data)
{
    bbs_app_ctx_t *ctx = (bbs_app_ctx_t *)user_data;
    ESP_LOGI(TAG, "BBS conn data RX len=%u", (unsigned)len);

    bbs_evt_t evt = {
        .type = BBS_EVT_DATA,
    };
    evt.len = len > AX25_MAX_INFO_LEN ? AX25_MAX_INFO_LEN : len;
    memcpy(evt.data, data, evt.len);

    if (xQueueSend(ctx->evt_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "BBS event queue full: dropping DATA event");
    }
}

static void conn_on_frame(const ax25_frame_t *frame, void *user_data)
{
    bbs_app_ctx_t *ctx = (bbs_app_ctx_t *)user_data;
    char src[16] = {0};
    char dst[16] = {0};
    ax25_address_to_string(&frame->source, src, sizeof(src));
    ax25_address_to_string(&frame->destination, dst, sizeof(dst));
    ESP_LOGI(TAG, "BBS conn TX frame: %s -> %s type=%d ctrl=0x%02X pid=0x%02X len=%u",
             src, dst, (int)frame->type, frame->control, frame->pid,
             (unsigned)frame->payload_len);
    esp_err_t err = ax25_router_send(frame, &ctx->app_port);
    log_router_send_result("BBS conn TX", err);
}

static void conn_on_error(const ax25_conn_error_t *error, void *user_data)
{
    (void)user_data;
    ESP_LOGW(TAG, "ax25_conn error %d: %s",
             error->code,
             error->message ? error->message : "");
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-TNC BBS");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Initialise config first so all subsequent init can read NVS-backed values. */
    ESP_ERROR_CHECK(ax25_cfg_init(s_bbs_local_cfg_schema,
                                  sizeof(s_bbs_local_cfg_schema) /
                                      sizeof(s_bbs_local_cfg_schema[0])));

    /* Read all schema-backed values from ax25_config, including schema defaults. */
    char bbs_callsign[16];
    ax25_cfg_get_str("bbs.callsign", bbs_callsign, sizeof(bbs_callsign));

    led_init_from_config();
    led_refresh_pwr_mode();
    led_apply_outputs();

    s_kiss_max_clients = (size_t)ax25_cfg_get_int_global("net.kiss.max_conns", 4);
    if (s_kiss_max_clients < 1) {
        s_kiss_max_clients = 1;
    }

    s_agwpe_max_clients = (size_t)ax25_cfg_get_int_global("net.agwpe.max_clients", 4);
    if (s_agwpe_max_clients < 1) {
        s_agwpe_max_clients = 1;
    }

    s_kiss_pool = (kiss_port_slot_t *)alloc_runtime_pool_prefer_psram(s_kiss_max_clients,
                                                                       sizeof(kiss_port_slot_t));
    s_agwpe_clients = (agwpe_client_slot_t *)alloc_runtime_pool_prefer_psram(s_agwpe_max_clients,
                                                                              sizeof(agwpe_client_slot_t));
    if (s_kiss_pool == NULL || s_agwpe_clients == NULL) {
        ESP_LOGE(TAG,
                 "Failed to allocate runtime connection pools (kiss=%d agwpe=%d)",
                 (int)s_kiss_max_clients,
                 (int)s_agwpe_max_clients);
        free(s_kiss_pool);
        free(s_agwpe_clients);
        s_kiss_pool = NULL;
        s_agwpe_clients = NULL;
        return;
    }

    char sysop_secret[CONFIG_BBS_SYSOP_SECRET_MAX_LEN + 1];
    ax25_cfg_get_str("bbs.sysop_secret", sysop_secret, sizeof(sysop_secret));
    if (!bbs_sysop_secret_is_valid(sysop_secret)) {
        ESP_LOGW(TAG,
                 "Invalid bbs.sysop_secret (len=%u). Falling back to empty secret for startup",
                 (unsigned)strlen(sysop_secret));
        sysop_secret[0] = '\0';
    }

    static ax25_wifi_t wifi_ctx;
    heap_snapshot_t wifi_heap_before = heap_snapshot_capture();
    err = ax25_wifi_start(&wifi_ctx);
    heap_snapshot_t wifi_heap_after = heap_snapshot_capture();
    log_heap_snapshot_delta("WiFi startup", &wifi_heap_before, &wifi_heap_after);
    log_heap_snapshot("After WiFi startup", &wifi_heap_after);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return;
    }

    /* Log TCP server must come after ax25_wifi_start() because that call
     * runs esp_netif_init() which starts the lwIP TCPIP thread.  Creating
     * a socket before the thread exists triggers "Invalid mbox" panic. */
#if CONFIG_TNC_ENABLE_LOG_TCP_SERVER
    static ax25_log_tcp_server_t log_srv;
    ESP_ERROR_CHECK(ax25_log_tcp_server_init(&log_srv));
#endif

    ESP_ERROR_CHECK(ax25_router_init());
    ESP_ERROR_CHECK(ax25_agwpe_server_manager_init());
#if CONFIG_TNC_ENABLE_BEACON_SERVICE
    ESP_ERROR_CHECK(ax25_beacon_init());
#endif

    static ax25_phy_kiss_uart_t uart_phy;
    static ax25_router_port_t uart_port;

    bool uart_enable = ax25_cfg_get_bool("uart.enable");

    char uart_type[16] = {0};
    ax25_cfg_get_str("uart.type", uart_type, sizeof(uart_type));
    bool uart_terminal_mode = (strcmp(uart_type, "term") == 0);

    if (uart_enable) {
        if (uart_terminal_mode) {
            /* Terminal mode: init UART PHY with a no-op frame callback.
             * The KISS decoder is bypassed while the console RX tap is active. */
            ESP_ERROR_CHECK(ax25_phy_kiss_uart_init(uart_noop_frame_cb, NULL, &uart_phy));
        } else {
            uart_port.mode = AX25_PORT_DEFAULT;
            uart_port.on_tx_frame = uart_port_frame_cb;
            uart_port.user_data = &uart_phy;
            ESP_ERROR_CHECK(ax25_router_register_port(&uart_port));

            ESP_ERROR_CHECK(ax25_phy_kiss_uart_init(uart_on_frame, &uart_port, &uart_phy));
        }
    }

    char digi_callsign[AX25_MAX_CALLSIGN_LEN + 5] = {0};
    ax25_cfg_get_str("digi.callsign", digi_callsign, sizeof(digi_callsign));
    if (digi_callsign[0] != '\0') {
#if CONFIG_TNC_ENABLE_DIGIPEATER_SERVICE
        if (!uart_enable) {
            ESP_LOGW(TAG, "digi.callsign is set but uart.enable=0; digipeater not started");
        } else {
            const ax25_digipeater_config_t digi_cfg = {
                .on_transmit = digipeater_tx_cb,
                .user_data = &uart_phy,
            };
            ESP_ERROR_CHECK(ax25_digipeater_init(&digi_cfg));
        }
#endif
    }

    s_kiss_pool_mutex = xSemaphoreCreateMutexStatic(&s_kiss_pool_mutex_buf);
    s_agwpe_clients_mutex = xSemaphoreCreateMutexStatic(&s_agwpe_clients_mutex_buf);
    s_agwpe_cleanup_queue = xQueueCreateStatic(AGWPE_CLEANUP_QUEUE_LEN,
                                               sizeof(agwpe_cleanup_req_t),
                                               s_agwpe_cleanup_queue_storage,
                                               &s_agwpe_cleanup_queue_buf);
    TaskHandle_t agwpe_cleanup_task_handle = xTaskCreateStatic(agwpe_cleanup_task,
                                                               "agwpe_cleanup",
                                                               AGWPE_CLEANUP_TASK_STACK_BYTES / sizeof(StackType_t),
                                                               NULL,
                                                               5,
                                                               s_agwpe_cleanup_task_stack,
                                                               &s_agwpe_cleanup_task_tcb);
    if (s_kiss_pool_mutex == NULL || s_agwpe_clients_mutex == NULL ||
        s_agwpe_cleanup_queue == NULL || agwpe_cleanup_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create static mutexes");
        return;
    }

    static ax25_kiss_tcp_server_t kiss_srv;
    ESP_ERROR_CHECK(ax25_kiss_tcp_server_init(kiss_on_connected,
                                              kiss_on_disconnected,
                                              kiss_on_frame,
                                              NULL,
                                              &kiss_srv));

#if CONFIG_TNC_ENABLE_MONITOR_TCP_SERVER
    static ax25_monitor_tcp_server_t monitor_srv;
    ESP_ERROR_CHECK(ax25_monitor_tcp_server_init(NULL, NULL, NULL, &monitor_srv));
#endif

    static ax25_phy_tcp_server_t agwpe_srv;
    ESP_ERROR_CHECK(ax25_phy_tcp_server_init("net.agwpe.port",
                                             agwpe_on_connected,
                                             agwpe_on_disconnected,
                                             agwpe_on_data,
                                             NULL,
                                             &agwpe_srv));

    heap_snapshot_t core_services_heap = heap_snapshot_capture();
    log_heap_snapshot_delta("Core services startup", &wifi_heap_after, &core_services_heap);
    log_heap_snapshot("After core services startup", &core_services_heap);

    err = bbs_storage_init(&s_app.storage, CONFIG_BBS_PARTITION_LABEL, CONFIG_BBS_MAX_MESSAGES);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bbs_storage_init failed: %s", esp_err_to_name(err));
        return;
    }

    err = bbs_heard_init(&s_app.heard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bbs_heard_init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Load heard list from persistent storage (if exists) */
    err = bbs_heard_load(&s_app.heard);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bbs_heard_load: %s (continuing with empty list)", esp_err_to_name(err));
    }

    /* Set the BBS callsign filter to prevent the BBS from hearing itself */
    bbs_heard_set_filter_callsign(&s_app.heard, bbs_callsign);

    if (ax25_address_from_string(bbs_callsign, &s_app.local_addr) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid BBS callsign: %s", bbs_callsign);
        return;
    }

    memset(&s_app.app_port, 0, sizeof(s_app.app_port));
    s_app.app_port.mode = AX25_PORT_STATIC;
    s_app.app_port.destination = s_app.local_addr;
    s_app.app_port.on_tx_frame = app_port_on_frame;
    s_app.app_port.user_data = &s_app.conn;
    ESP_ERROR_CHECK(ax25_router_register_port(&s_app.app_port));

    ax25_conn_callbacks_t conn_cb = {
        .on_connect = conn_on_connect,
        .on_disconnect = conn_on_disconnect,
        .on_error = conn_on_error,
        .on_data = conn_on_data,
        .on_tx_frame = conn_on_frame,
    };

    ax25_conn_config_t conn_cfg = AX25_CONN_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(ax25_conn_init(&s_app.conn, &s_app.local_addr, &conn_cb, &s_app, &conn_cfg));

    bbs_config_t bcfg = {
        .bbs_callsign = bbs_callsign,
        .greeting_text = CONFIG_BBS_GREETING_TEXT,
        .prompt_text = CONFIG_BBS_PROMPT_TEXT,
        .config_prompt_text = CONFIG_BBS_CONFIG_PROMPT_TEXT,
        .terminal_prompt_text = CONFIG_BBS_TERMINAL_PROMPT_TEXT,
        .sysop_name = CONFIG_BBS_SYSOP_NAME,
        .version_text = CONFIG_BBS_VERSION_TEXT,
        .nvs_namespace = "bbs",
        .sysop_secret = sysop_secret,
        .sysop_challenge_timeout_ms = CONFIG_BBS_SYSOP_CHALLENGE_TIMEOUT_SEC * 1000U,
        .sysop_session_timeout_ms = CONFIG_BBS_SYSOP_SESSION_TIMEOUT_SEC * 1000U,
        .sysop_lockout_ms = CONFIG_BBS_SYSOP_LOCKOUT_SEC * 1000U,
        .sysop_max_attempts = CONFIG_BBS_SYSOP_MAX_ATTEMPTS,
        .sysop_secret_min_len = CONFIG_BBS_SYSOP_SECRET_MIN_LEN,
        .sysop_secret_max_len = CONFIG_BBS_SYSOP_SECRET_MAX_LEN,
        .storage = &s_app.storage,
        .heard = &s_app.heard,
        .uart_phy = uart_enable ? &uart_phy : NULL,
        .uart_is_terminal_mode = uart_enable && uart_terminal_mode,
    };

    ESP_ERROR_CHECK(bbs_init(&s_app.bbs, &bcfg, &s_app.conn));
    led_refresh_bbs_mode(&s_app);
    led_apply_outputs();

    s_app.evt_queue_storage = alloc_runtime_pool_prefer_psram(BBS_EVT_QUEUE_LEN,
                                                              sizeof(bbs_evt_t));
    if (s_app.evt_queue_storage == NULL) {
        ESP_LOGE(TAG, "Failed to allocate BBS event queue storage");
        return;
    }

    s_app.evt_queue = xQueueCreateStatic(BBS_EVT_QUEUE_LEN,
                                         sizeof(bbs_evt_t),
                                         s_app.evt_queue_storage,
                                         &s_app.evt_queue_buf);
    if (s_app.evt_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create BBS event queue");
        free(s_app.evt_queue_storage);
        s_app.evt_queue_storage = NULL;
        return;
    }

    s_app.bbs_task = xTaskCreateStatic(bbs_worker_task,
                                       "bbs_worker",
                                       BBS_WORKER_STACK_BYTES / sizeof(StackType_t),
                                       &s_app,
                                       5,
                                       s_app.bbs_task_stack,
                                       &s_app.bbs_task_tcb);
    if (s_app.bbs_task == NULL) {
        ESP_LOGE(TAG, "Failed to create BBS worker task");
        s_app.evt_queue = NULL;
        free(s_app.evt_queue_storage);
        s_app.evt_queue_storage = NULL;
        return;
    }

    heap_snapshot_t app_ready_heap = heap_snapshot_capture();
    log_heap_snapshot_delta("App services startup", &core_services_heap, &app_ready_heap);
    log_heap_snapshot_delta("Total post-WiFi startup", &wifi_heap_after, &app_ready_heap);
    log_heap_snapshot("After app services startup", &app_ready_heap);

    ax25_router_log_port_summary("BBS startup baseline");

    ESP_LOGI(TAG, "BBS ready: local=%s", bbs_callsign);

    bool stats_initialized = false;
    uint32_t prev_sent = 0;
    uint32_t prev_dropped = 0;
    uint32_t prev_sram_total = 0;
    uint32_t prev_sram_free_now = 0;
    uint32_t prev_sram_free_min = 0;
    uint32_t prev_psram_total = 0;
    uint32_t prev_psram_free_now = 0;
    uint32_t prev_psram_free_min = 0;
    uint32_t housekeeping_ticks = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        led_tick(&s_app);

        housekeeping_ticks++;
        if (housekeeping_ticks < 30) {
            continue;
        }
        housekeeping_ticks = 0;

        uint32_t sent = uart_port.frames_sent;
        uint32_t dropped = uart_port.frames_dropped;
        uint32_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t sram_free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t sram_free_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        uint32_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint32_t psram_free_now = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint32_t psram_free_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (!stats_initialized || sent != prev_sent || dropped != prev_dropped) {
            ESP_LOGI(TAG, "UART port: sent=%lu dropped=%lu",
                     (unsigned long)sent,
                     (unsigned long)dropped);
        }

        if (!stats_initialized ||
            sram_total != prev_sram_total ||
            sram_free_now != prev_sram_free_now ||
            sram_free_min != prev_sram_free_min ||
            psram_total != prev_psram_total ||
            psram_free_now != prev_psram_free_now ||
            psram_free_min != prev_psram_free_min) {
            ESP_LOGI(TAG, "SRAM:  %lu used now, %lu peak used, %lu free of %lu total",
                     (unsigned long)(sram_total - sram_free_now),
                     (unsigned long)(sram_total - sram_free_min),
                     (unsigned long)sram_free_now,
                     (unsigned long)sram_total);
            ESP_LOGI(TAG, "PSRAM: %lu used now, %lu peak used, %lu free of %lu total",
                     (unsigned long)(psram_total - psram_free_now),
                     (unsigned long)(psram_total - psram_free_min),
                     (unsigned long)psram_free_now,
                     (unsigned long)psram_total);
        }

        /* Persist heard list if dirty or rate limit triggered (at most every 30 min). */
        bbs_heard_persist(&s_app.heard, false);

        prev_sent = sent;
        prev_dropped = dropped;
        prev_sram_total = sram_total;
        prev_sram_free_now = sram_free_now;
        prev_sram_free_min = sram_free_min;
        prev_psram_total = psram_total;
        prev_psram_free_now = psram_free_now;
        prev_psram_free_min = psram_free_min;
        stats_initialized = true;
    }
}
