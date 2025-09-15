#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mesh.h"

/*
 * ESP-MESH example with send and receive tasks.
 * - Beginner-friendly, heavily commented.
 * - Built for ESP-IDF v5.5 (API: esp_mesh_send / esp_mesh_recv).
 *
 * What this file provides:
 * 1) Mesh initialization (NVS, netif, wifi, mesh).
 * 2) A recv_task() that blocks on esp_mesh_recv() and prints incoming messages.
 * 3) A send_task() that periodically sends a short text payload to the root.
 *
 * Safety notes:
 * - recv_task allocates a receive buffer and reuses it. It must poll the RX queue
 *   to avoid running out of memory on the device.
 * - We use small periodic messages to avoid exceeding MTU. If you send large
 *   payloads, split them or increase mesh MTU (advanced).
 */

static const char *TAG = "MESH";

/* ----------------------- Configuration ----------------------- */
static const uint8_t MESH_ID[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
#define ROUTER_SSID     "Pi"
#define ROUTER_PASS     "12345678"

#define MESH_AP_PASSWORD    "meshpassword"   // password children use to join mesh-AP
#define MESH_MAX_CONN       6                // allow up to 6 children per node

#define RECV_BUFFER_LEN     1500             // safe upper bound for mesh packet (bytes)
#define SEND_INTERVAL_MS    5000             // send every 5 seconds

/* ----------------------- Forward declarations ----------------------- */
static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void init_nvs(void);
static void init_netif_eventloop(void);
static void init_wifi(void);
static void init_mesh(void);

/* ----------------------- Event handler ----------------------- */
static void mesh_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    switch (event_id) {
    case MESH_EVENT_STARTED:
        ESP_LOGI(TAG, "MESH_EVENT_STARTED");
        break;

    case MESH_EVENT_PARENT_CONNECTED: {
        int layer = esp_mesh_get_layer();
        ESP_LOGI(TAG, "MESH_EVENT_PARENT_CONNECTED. Layer: %d", layer);
        break;
    }

    case MESH_EVENT_LAYER_CHANGE:
        ESP_LOGI(TAG, "MESH_EVENT_LAYER_CHANGE -> Layer now: %d", esp_mesh_get_layer());
        break;

    case MESH_EVENT_ROOT_ADDRESS:
        ESP_LOGI(TAG, "MESH_EVENT_ROOT_ADDRESS (root BSSID available)");
        break;

    case MESH_EVENT_CHILD_CONNECTED:
        ESP_LOGI(TAG, "MESH_EVENT_CHILD_CONNECTED (a child associated to me)");
        break;

    case MESH_EVENT_CHILD_DISCONNECTED:
        ESP_LOGI(TAG, "MESH_EVENT_CHILD_DISCONNECTED (a child left)");
        break;

    default:
        ESP_LOGD(TAG, "Unhandled mesh event ID: %ld", event_id);
        break;
    }
}

/* ----------------------- Init helpers ----------------------- */
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_LOGI(TAG, "NVS initialized");
}

static void init_netif_eventloop(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Network interface + event loop ready");
}

static void init_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); // mesh needs AP+STA
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi started in AP+STA mode");
}

/* ----------------------- Runtime tasks ----------------------- */

/*
 * recv_task
 * - Allocates a receive buffer once and reuses it.
 * - Calls esp_mesh_recv() with portMAX_DELAY to block until data arrives.
 * - Prints source MAC and message payload.
 *
 * Why this design:
 * - Receiving in a dedicated task avoids blocking main or other logic.
 * - Using a single reusable buffer avoids repeated malloc/free overhead.
 */
static void recv_task(void *arg)
{
    mesh_addr_t from;
    mesh_data_t data;
    int recv_flag = 0;

    data.data = (uint8_t *)malloc(RECV_BUFFER_LEN);
    if (!data.data) {
        ESP_LOGE(TAG, "recv_task: malloc failed");
        vTaskDelete(NULL);
        return;
    }
    data.size = RECV_BUFFER_LEN; // input: buffer capacity

    while (1) {
        data.size = RECV_BUFFER_LEN; // reset available buffer size before each recv
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &recv_flag, NULL, 0);
        if (err == ESP_OK) {
            // data.size now contains actual payload length
            ESP_LOGI(TAG, "RECV from " MACSTR " size=%d flag=0x%X",
                     MAC2STR(from.addr), data.size, recv_flag);

            // Print payload safely. Payload may be binary. We print up to data.size
            int len = data.size;
            if (len > 0) {
                // print printable subset for debugging
                int printable = (len < 200) ? len : 200;
                ESP_LOGI(TAG, "Payload: %.*s", printable, (char *)data.data);
            }
        } else if (err == ESP_ERR_MESH_TIMEOUT) {
            // not expected because we use portMAX_DELAY. But handle gracefully.
            ESP_LOGW(TAG, "esp_mesh_recv timeout");
        } else {
            ESP_LOGE(TAG, "esp_mesh_recv failed: %s", esp_err_to_name(err));
            // small delay before retrying to avoid tight loop on fatal error
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // never reached, but good habit
    free(data.data);
    vTaskDelete(NULL);
}

/*
 * send_task
 * - Prepares a small text message that contains this node's MAC and layer.
 * - Sends to the mesh root (destination NULL). If this node is root it may
 *   broadcast or log locally.
 * - Runs periodically.
 *
 * Why no malloc for TX payload?
 * - We use a stack buffer and trust esp_mesh_send to copy/enqueue data. This
 *   matches examples from Espressif. If you need zero-copy or work with huge
 *   payloads, use the mesh_opt_t options (advanced).
 */
static void send_task(void *arg)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA); // station MAC, unique per device

    char tx_buf[256];
    mesh_data_t data;

    while (1) {
        int layer = esp_mesh_get_layer();
        int is_root = esp_mesh_is_root();

        int len = snprintf(tx_buf, sizeof(tx_buf),
                           "MSG from %02X:%02X:%02X:%02X:%02X:%02X layer=%d root=%d\n",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], layer, is_root);
        if (len < 0) len = 0;
        if (len >= (int)sizeof(tx_buf)) len = sizeof(tx_buf) - 1;

        data.data = (uint8_t *)tx_buf;
        data.size = (uint16_t)len;
        data.proto = MESH_PROTO_BIN; // treat payload as raw bytes
        data.tos  = MESH_TOS_P2P;    // type-of-service: P2P is fine for example

        // Send to root (to == NULL) so the message is routed upstream.
        esp_err_t err = esp_mesh_send(NULL, &data, 0, NULL, 0);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SENT len=%d layer=%d", data.size, layer);
        } else {
            ESP_LOGW(TAG, "esp_mesh_send failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL_MS));
    }

    vTaskDelete(NULL);
}

/* ----------------------- Mesh init and task startup ----------------------- */
static void init_mesh(void)
{
    ESP_ERROR_CHECK(esp_mesh_init());

    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                               &mesh_event_handler, NULL));

    mesh_cfg_t mesh_cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&mesh_cfg.mesh_id, MESH_ID, sizeof(MESH_ID));

    mesh_cfg.channel = 0; // auto channel (root will follow router if configured)

    // Router credentials - root will use this to connect to upstream router
    mesh_cfg.router.ssid_len = strlen(ROUTER_SSID);
    memcpy(&mesh_cfg.router.ssid, ROUTER_SSID, mesh_cfg.router.ssid_len);
    memcpy(&mesh_cfg.router.password, ROUTER_PASS, strlen(ROUTER_PASS));

    // Mesh AP config - children use these credentials to join this node if it
    // acts as an AP for its children. Must be >0 or esp_mesh_set_config will error.
    mesh_cfg.mesh_ap.max_connection = MESH_MAX_CONN;
    strncpy((char *)mesh_cfg.mesh_ap.password, MESH_AP_PASSWORD,
            sizeof(mesh_cfg.mesh_ap.password));

    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_cfg));

    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_LOGI(TAG, "Mesh started");

    // Create the receive task. This task must run while mesh is up.
    xTaskCreate(recv_task, "mesh_recv", 4096, NULL, 5, NULL);

    // Create the send task. Lower priority to not block recv.
    xTaskCreate(send_task, "mesh_send", 4096, NULL, 4, NULL);
}

/* ----------------------- Main ----------------------- */
void app_main(void)
{
    // Minimal log filtering: suppress noisy components and keep mesh logs.
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("MESH", ESP_LOG_INFO);

    init_nvs();
    init_netif_eventloop();
    init_wifi();

    init_mesh();

    // app_main returns here and FreeRTOS runs tasks
}
