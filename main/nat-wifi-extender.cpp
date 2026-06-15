#include "sdkconfig.h" // Mandatory first include for feature flags
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#if CONFIG_LWIP_IPV4_NAPT
#include "lwip/lwip_napt.h"
#endif

// 2. Extender Hotspot Settings (Downlink)
#define EXT_SSID "esp-wifi-extender"
#define EXT_PASS "12345678"
#define MAX_STA_CONN 4
#define MAX_RETRY 5

static const char *TAG = "wifi_repeater";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
static int s_retry_num = 0;

esp_netif_t *ap_netif = NULL;
esp_netif_t *sta_netif = NULL;
httpd_handle_t server = NULL;

char target_ssid[33] = {0};
char target_pass[64] = {0};
bool should_connect_new_wifi = false;

// --- HTML Webpage Template ---
const char html_page[] =
    "<!DOCTYPE html><html><head><meta name=\"viewport\" "
    "content=\"width=device-width, initial-scale=1\">"
    "<style>body{font-family:Arial; margin:20px; text-align:center;} "
    "select,input{padding:10px; width:80%%; margin:10px; "
    "font-size:16px;}</style>"
    "</head><body><h2>ESP32 Wi-Fi Setup</h2>"
    "<form action=\"/connect\" method=\"POST\">"
    "<label>Select Network:</label><br>"
    "<select name=\"ssid\">"
    "%s" // Dynamic Wi-Fi Scan options will inject here
    "</select><br>"
    "<label>Password:</label><br>"
    "<input type=\"password\" name=\"password\" placeholder=\"Enter "
    "Password\"><br>"
    "<input type=\"submit\" value=\"Connect\" "
    "style=\"background-color:#4CAF50; color:white; border:none;\">"
    "</form></body></html>";

// --- HTTP GET Handler: Serve Wi-Fi List ---
static esp_err_t root_get_handler(httpd_req_t *req) {
  // 1. Scan for nearby Wi-Fi Networks
  uint16_t number = 12;
  wifi_ap_record_t ap_info[12];
  uint16_t ap_count = 0;

  // Initialize and fix scan configuration
  wifi_scan_config_t scan_config{};
  scan_config.ssid = NULL;
  scan_config.bssid = NULL;
  scan_config.channel = 0;
  scan_config.show_hidden = false;
  scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_config.scan_time.active.min = 100;
  scan_config.scan_time.active.max = 250;

  // Start blocking scan (true means wait until done)
  esp_err_t res = esp_wifi_scan_start(&scan_config, true);
  if (res != ESP_OK) {
    ESP_LOGE(TAG, "Scan failed to start: %s", esp_err_to_name(res));
    return res;
  }

  // Fetch the actual record array and total count
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count > number)
    ap_count = number;
  ESP_LOGI(TAG, "Scan finished. Found %d access points.", ap_count);
  esp_wifi_scan_get_ap_records(&number, ap_info);

  // 2. Build the option list dynamically
  char *options_buf = static_cast<char *>(malloc(2048));
  if (!options_buf)
    return ESP_ERR_NO_MEM;
  options_buf[0] = '\0';

  for (int i = 0; i < ap_count; i++) {
    char item[128];
    snprintf(item, sizeof(item), "<option value=\"%s\">%s</option>",
             (char *)ap_info[i].ssid, (char *)ap_info[i].ssid);
    strcat(options_buf, item);
    ESP_LOGI(TAG, "Wifi option found: %s", item);
  }

  // 3. Render page
  char *response_buf = static_cast<char *>(malloc(3072));
  if (!response_buf) {
    free(options_buf);
    return ESP_ERR_NO_MEM;
  }
  snprintf(response_buf, 3072, html_page, options_buf);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_sendstr(req, response_buf);

  free(options_buf);
  free(response_buf);
  return ESP_OK;
}

// Helper to decode basic URL-encoded form data
void url_decode(char *dst, const char *src) {
  char a, b;
  while (*src) {
    if ((*src == '%') && ((a = src[1]) && (b = src[2])) &&
        (isxdigit(a) && isxdigit(b))) {
      if (a >= 'a')
        a -= 'a' - 'A';
      if (a >= 'A')
        a -= ('A' - 10);
      else
        a -= '0';
      if (b >= 'a')
        b -= 'a' - 'A';
      if (b >= 'A')
        b -= ('A' - 10);
      else
        b -= '0';
      *dst++ = 16 * a + b;
      src += 3;
    } else if (*src == '+') {
      *dst++ = ' ';
      src++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
}

// --- HTTP POST Handler: Process Credentials ---
static esp_err_t connect_post_handler(httpd_req_t *req) {
  char buf[256];
  int ret, remaining = req->content_len;

  if (remaining >= sizeof(buf)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too long");
    return ESP_FAIL;
  }

  ret = httpd_req_recv(req, buf, remaining);
  if (ret <= 0)
    return ESP_FAIL;
  buf[ret] = '\0';

  // Crude parsing for application/x-www-form-urlencoded (ssid=XXX&password=YYY)
  char raw_ssid[64] = {0};
  char raw_pass[64] = {0};

  char *ssid_ptr = strstr(buf, "ssid=");
  char *pass_ptr = strstr(buf, "password=");

  if (ssid_ptr && pass_ptr) {
    // Simple manual split parsing
    if (ssid_ptr < pass_ptr) {
      sscanf(ssid_ptr, "ssid=%[^&]&password=%s", raw_ssid, raw_pass);
    } else {
      sscanf(pass_ptr, "password=%[^&]&ssid=%s", raw_pass, raw_ssid);
    }

    url_decode(target_ssid, raw_ssid);
    url_decode(target_pass, raw_pass);

    ESP_LOGI(TAG, "Web Received SSID: %s", target_ssid);

    const char *resp = "<html><body><h2>Connecting... ESP32 is switching "
                       "networks.</h2></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, resp);

    should_connect_new_wifi = true;
    return ESP_OK;
  }

  httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form Parsing Failed");
  return ESP_FAIL;
}

// Start HTTP Server
void start_webserver(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.ctrl_port = 8000;

  httpd_uri_t root_uri{};
  root_uri.uri = "/";
  root_uri.method = HTTP_GET;
  root_uri.handler = root_get_handler;
  root_uri.user_ctx = NULL;

  httpd_uri_t connect_uri{};
  connect_uri.uri = "/connect";
  connect_uri.method = HTTP_POST;
  connect_uri.handler = connect_post_handler;
  connect_uri.user_ctx = NULL;

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &connect_uri);
    ESP_LOGI(TAG, "Webserver started on port 80");
  }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    if (strlen(target_ssid) > 0)
      esp_wifi_connect();
  }

  else if (event_base == WIFI_EVENT &&
           event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_num < MAX_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "STA: Retrying connection to home router...");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
  }

  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "STA: Connected! Assigned WAN IP: " IPSTR,
             IP2STR(&event->ip_info.ip));
    s_retry_num = 0;

    esp_netif_set_default_netif(sta_netif);
    ESP_LOGI(TAG, "STA set as default netif for routing");

#if CONFIG_LWIP_IPV4_NAPT
    esp_netif_ip_info_t ap_info{};
    esp_netif_get_ip_info(ap_netif, &ap_info);
    ip_napt_enable(ap_info.ip.addr, 1);
    ESP_LOGI(TAG, "NAT enabled on AP interface: " IPSTR, IP2STR(&ap_info.ip));
#else
    ESP_LOGE(TAG, "ERROR: NAT option is disabled in menuconfig!");
#endif

    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }

  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "AP: Extender Client " MACSTR " connected.",
             MAC2STR(event->mac));
  }

  else if (event_base == WIFI_EVENT &&
           event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "AP: Extender Client " MACSTR " disconnected.",
             MAC2STR(event->mac));
  }
}

void wifi_init_repeater(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  sta_netif = esp_netif_create_default_wifi_sta();
  ap_netif = esp_netif_create_default_wifi_ap();

  ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));

  esp_netif_ip_info_t ap_ip_info{};
  IP4_ADDR(&ap_ip_info.ip, 10, 0, 0, 1);
  IP4_ADDR(&ap_ip_info.gw, 10, 0, 0, 1);
  IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 0);
  ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip_info));

  dhcps_lease_t lease{};
  lease.enable = true;
  IP4_ADDR(&lease.start_ip, 10, 0, 0, 2);
  IP4_ADDR(&lease.end_ip, 10, 0, 0, 10);
  ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                         ESP_NETIF_REQUESTED_IP_ADDRESS, &lease,
                                         sizeof(lease)));

  uint8_t offer_dns = 1;
  esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                         ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns,
                         sizeof(offer_dns));

  esp_netif_dns_info_t dns_main_info{};
  dns_main_info.ip.type = IPADDR_TYPE_V4;
  dns_main_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
  ESP_ERROR_CHECK(
      esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_main_info));

  esp_netif_dns_info_t dns_backup_info{};
  dns_backup_info.ip.type = IPADDR_TYPE_V4;
  dns_backup_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
  ESP_ERROR_CHECK(
      esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_BACKUP, &dns_backup_info));

  ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

  wifi_config_t ap_config{};
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_config.ap.max_connection = MAX_STA_CONN;
  ap_config.ap.pmf_cfg.required = false;
  strcpy((char *)ap_config.ap.ssid, EXT_SSID);
  strcpy((char *)ap_config.ap.password, EXT_PASS);
  ap_config.ap.ssid_len = strlen(EXT_SSID);
  ap_config.ap.channel = 1;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

  ESP_ERROR_CHECK(esp_wifi_start());
  wifi_country_t country{
      .cc = "IN",
      .schan = 1,
      .nchan = 11,
      .max_tx_power = CONFIG_ESP32_PHY_MAX_WIFI_TX_POWER,
      .policy = WIFI_COUNTRY_POLICY_AUTO,
  };
  ESP_ERROR_CHECK(esp_wifi_set_country(&country));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  ESP_LOGI(TAG, "Extender setup completed.");

  start_webserver();
}

extern "C" {

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifi_init_repeater();

  while (1) {
    if (should_connect_new_wifi) {
      should_connect_new_wifi = false;
      s_retry_num = 0;

      wifi_config_t sta_config{};
      strcpy((char *)sta_config.sta.ssid, target_ssid);
      strcpy((char *)sta_config.sta.password, target_pass);

      ESP_LOGI(TAG, "Applying new credentials and connecting...");
      esp_wifi_disconnect();
      esp_wifi_set_config(WIFI_IF_STA, &sta_config);
      esp_wifi_connect();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
}