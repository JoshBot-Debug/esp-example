#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

// AP Configuration
#define ESP_WIFI_SSID "ESP32-AP"
#define ESP_WIFI_PASS "12345678" // Must be at least 8 characters
#define ESP_WIFI_CHANNEL 1       // Wi-Fi channel (1-13)
#define MAX_STA_CONN 4           // Max clients allowed to connect

static const char *TAG = "wifi_ap";

// Event handler to track when devices connect or disconnect from the ESP32
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    return;
  }

  if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
    return;
  }
}

void wifi_init_softap(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Creates the network interface structure for an Access Point
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // Register handlers for connection events
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

  // Configure Access Point settings
  wifi_config_t wifi_config = {};
  strcpy((char *)wifi_config.ap.ssid, ESP_WIFI_SSID);
  strcpy((char *)wifi_config.ap.password, ESP_WIFI_PASS);
  wifi_config.ap.ssid_len = strlen(ESP_WIFI_SSID);
  wifi_config.ap.channel = ESP_WIFI_CHANNEL;
  wifi_config.ap.max_connection = MAX_STA_CONN;
  wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

  // If password string is empty, fall back to open network configuration
  if (strlen(ESP_WIFI_PASS) == 0) {
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s", ESP_WIFI_SSID,
           ESP_WIFI_PASS);
}

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_LOGI(TAG, "Starting ESP32 Access Point...");
  wifi_init_softap();
}