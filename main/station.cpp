#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

#include "lwip/err.h"
#include "lwip/sys.h"

// Configuration
#define WIFI_SSID "Darling in the Franxx"
#define WIFI_PASS "Comperswift@123"
#define MAXIMUM_RETRY 5

// Event group bits to track connection status
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi_station";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

// Event handler for Wi-Fi and IP events
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < MAXIMUM_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG, "connect to the AP fail");
    return;
  }
  
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void wifi_init_sta(void) {
  s_wifi_event_group = xEventGroupCreate();

  // 1. Initialize the underlying TCP/IP stack
  ESP_ERROR_CHECK(esp_netif_init());

  // 2. Create the default event loop
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  // 3. Initialize Wi-Fi with default configuration
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // 4. Register Event Handlers
  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

  // 5. Configure Wi-Fi Credentials
  wifi_config_t wifi_config = {};
  strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
  strcpy((char *)wifi_config.sta.password, WIFI_PASS);
  wifi_config.sta.threshold.authmode =
      WIFI_AUTH_WPA2_PSK; // Minimum security level

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  // 6. Start the Wi-Fi hardware
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");

  // Waiting until either the connection is established or failed
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  // 7. Process the connection results
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }
}

extern "C" void app_main(void) {
  // Initialize Non-Volatile Storage (NVS) - Wi-Fi driver needs it to store
  // configs
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_LOGI(TAG, "ESP_IDF ESP32 Wi-Fi STA Mode Initializing...");
  wifi_init_sta();
}