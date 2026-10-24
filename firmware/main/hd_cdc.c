/*
 * [Input] JSON Lines / raw-chunk bytes from the PC pet client.
 * [Output] A High-Speed USB CDC-ACM port owned by the application, with
 *          bounded blocking writes and an RX task feeding the shared
 *          transport-byte consumer in main.c.
 * [Pos] ESP-Mosaico transport for the HachimoDock wire protocol. The
 *       application claims the OTG controller for this link; ESP-Iris keeps
 *       its TCP transport for mosaico.py operations (sdkconfig.defaults).
 * [Sync] VID/PID also live in the patched PC client's port filter.
 */
#include "hd_cdc.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#define HD_CDC_VID 0x303A
#define HD_CDC_PID 0x4AC1
#define HD_CDC_RX_CHUNK_BYTES 512
#define HD_CDC_RX_TASK_STACK_BYTES 8192
#define HD_CDC_WRITE_TIMEOUT_TICKS pdMS_TO_TICKS(100)

static const char *TAG = "hd_cdc";

static hd_cdc_rx_fn s_rx_fn;
static void *s_rx_ctx;
static SemaphoreHandle_t s_write_mutex;
static volatile bool s_host_open;
static char s_serial[33];

static const char s_langid[] = {0x09, 0x04};
static const char *s_string_descriptors[] = {
    s_langid,
    "HachimoDock Mosaico",
    "HachimoPet Link",
    NULL,
    "pet-usb-jsonl-v3",
};

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = HD_CDC_VID,
    .idProduct = HD_CDC_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static void cdc_rx_callback(int itf, cdcacm_event_t *event) {
  (void) itf;
  (void) event;
}

static void cdc_line_state_callback(int itf, cdcacm_event_t *event) {
  (void) itf;
  s_host_open = event->line_state_changed_data.dtr;
}

static void usb_device_event_callback(tinyusb_event_t *event, void *arg) {
  (void) arg;
  if (event->id == TINYUSB_EVENT_DETACHED) {
    s_host_open = false;
  }
}

static void cdc_rx_task(void *arg) {
  (void) arg;
  uint8_t buf[HD_CDC_RX_CHUNK_BYTES];
  while (true) {
    size_t available = 0;
    esp_err_t err = tinyusb_cdcacm_read(
      TINYUSB_CDC_ACM_0, buf, sizeof(buf), &available);
    if (err == ESP_OK && available > 0 && s_rx_fn) {
      s_rx_fn(buf, available, s_rx_ctx);
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

esp_err_t hd_cdc_init(hd_cdc_rx_fn rx_fn, void *rx_ctx) {
  ESP_RETURN_ON_FALSE(rx_fn != NULL, ESP_ERR_INVALID_ARG, TAG, "rx handler");

  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
    snprintf(s_serial, sizeof(s_serial), "HACHIMO000000");
  } else {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(mac); ++i) {
      s_serial[i * 2] = hex[mac[i] >> 4];
      s_serial[i * 2 + 1] = hex[mac[i] & 0x0f];
    }
    s_serial[12] = '\0';
  }
  s_string_descriptors[3] = s_serial;

  s_write_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(s_write_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex");
  s_rx_fn = rx_fn;
  s_rx_ctx = rx_ctx;

  tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_device_event_callback, NULL);
  config.descriptor.device = &s_device_descriptor;
  config.descriptor.string = s_string_descriptors;
  config.descriptor.string_count =
    sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0]);
  esp_err_t err = tinyusb_driver_install(&config);
  ESP_RETURN_ON_ERROR(err, TAG, "tinyusb driver");

  const tinyusb_config_cdcacm_t cdc = {
    .cdc_port = TINYUSB_CDC_ACM_0,
    .callback_rx = cdc_rx_callback,
    .callback_line_state_changed = cdc_line_state_callback,
  };
  err = tinyusb_cdcacm_init(&cdc);
  ESP_RETURN_ON_ERROR(err, TAG, "cdc acm");

  BaseType_t created = xTaskCreate(
    cdc_rx_task, "hd_cdc_rx", HD_CDC_RX_TASK_STACK_BYTES, NULL, 10, NULL);
  ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "rx task");
  ESP_LOGI(TAG, "pet link CDC ready vid=0x%04x pid=0x%04x", HD_CDC_VID, HD_CDC_PID);
  return ESP_OK;
}

bool hd_cdc_host_open(void) { return s_host_open; }

void hd_cdc_write_all(const char *data, size_t len) {
  if (!data || !len) return;
  if (s_write_mutex) xSemaphoreTake(s_write_mutex, portMAX_DELAY);
  size_t written = 0;
  while (written < len) {
    size_t queued = tinyusb_cdcacm_write_queue(
      TINYUSB_CDC_ACM_0, (const uint8_t *) (data + written), len - written);
    if (queued == 0) break;
    written += queued;
    if (tinyusb_cdcacm_write_flush(
          TINYUSB_CDC_ACM_0, HD_CDC_WRITE_TIMEOUT_TICKS) != ESP_OK) {
      break;
    }
  }
  (void) tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, HD_CDC_WRITE_TIMEOUT_TICKS);
  if (s_write_mutex) xSemaphoreGive(s_write_mutex);
}
