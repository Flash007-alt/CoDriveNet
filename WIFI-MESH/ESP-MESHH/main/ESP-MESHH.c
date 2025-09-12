#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mesh.h"

static const char *TAG = "MESH";

/* ----------------------- Mesh Config ----------------------- */
// Define a unique Mesh ID (must be exactly 6 bytes).
static const uint8_t MESH_ID[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

// Router credentials (fallback if not provided from NVS or user input)
#define ROUTER_SSID     "Pi"
#define ROUTER_PASS     "12345678"

/* ----------------------- Event Handler ----------------------- */
static void mesh_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    switch (event_id) {
    case MESH_EVENT_STARTED:
        ESP_LOGI(TAG, "Mesh started");
        break;

    case MESH_EVENT_PARENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to parent. Layer: %d", esp_mesh_get_layer());
        break;

    case MESH_EVENT_LAYER_CHANGE:
        ESP_LOGI(TAG, "Layer changed to %d", esp_mesh_get_layer());
        break;

    case MESH_EVENT_ROOT_ADDRESS:
        ESP_LOGI(TAG, "Root address received");
        break;

    case MESH_EVENT_CHILD_CONNECTED:
        ESP_LOGI(TAG, "A child node connected to me");
        break;

    default:
        ESP_LOGI(TAG, "Unhandled mesh event ID: %ld", event_id);
        break;
    }
}

/* ----------------------- Init Blocks (Lego pieces) ----------------------- */

// NVS Init Block
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());   // Erase NVS if corrupted/outdated
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_LOGI(TAG, "NVS initialized");
}

// TCP/IP + Event Loop Block
static void init_netif_eventloop(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Network interface + event loop ready");
}

// Wi-Fi Init Block
static void init_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH)); // Store creds in flash
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));       // Mesh requires both AP+STA
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi started in AP+STA mode");
}

// Mesh Init Block
static void init_mesh(void)
{
    // 1. Init mesh subsystem
    ESP_ERROR_CHECK(esp_mesh_init());

    // 2. Register mesh event handler
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &mesh_event_handler,
                                               NULL));

    // 3. Configure mesh parameters
    mesh_cfg_t mesh_cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&mesh_cfg.mesh_id, MESH_ID, sizeof(MESH_ID));

    mesh_cfg.channel = 0; // Auto channel

    // Router (uplink) credentials
    mesh_cfg.router.ssid_len = strlen(ROUTER_SSID);
    memcpy(&mesh_cfg.router.ssid, ROUTER_SSID, mesh_cfg.router.ssid_len);
    memcpy(&mesh_cfg.router.password, ROUTER_PASS, strlen(ROUTER_PASS));

    // Mesh AP config (children allowed)
    mesh_cfg.mesh_ap.max_connection = 6;
    strncpy((char *)mesh_cfg.mesh_ap.password, "meshpassword",
            sizeof(mesh_cfg.mesh_ap.password));

    // 4. Apply mesh config
    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_cfg));

    // 5. Start mesh
    ESP_ERROR_CHECK(esp_mesh_start());

    ESP_LOGI(TAG, "Mesh initialized and started");
}

/* ----------------------- Main ----------------------- */
void app_main(void)
{
    init_nvs();                // 1. NVS
    init_netif_eventloop();    // 2. TCP/IP + Event loop
    init_wifi();               // 3. Wi-Fi AP+STA
    init_mesh();               // 4. Mesh
}
