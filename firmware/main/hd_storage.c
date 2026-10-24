/*
 * [Input] P4 MJPEG/H.264 transaction chunks (see pet_p4_raw_assets.h) plus the
 *         SPI-NAND wear-leveled block device from the Mosaico BSP.
 * [Output] A LittleFS volume at /nand plus the raw-appearance slot backend:
 *          append-only pack files, commit-last 4 KiB catalog headers, atomic
 *          header swap and direct pread-style reads.
 * [Pos] ESP-Mosaico storage backend replacing HachimoDock's dedicated raw
 *       flash partitions. Same wire semantics, no custom partition table, so
 *       code-only updates stay app-update compatible.
 * [Sync] Widget files (pet_p4_miniapp.c) and serial asset files
 *       (pet_p4_protocol.c) also live under /nand/pet.
 */
#include "hd_storage.h"
#include "pet_p4_raw_assets.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_blockdev.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "spi_nand_flash.h"

#include "bsp/esp_mosaico.h"

#define PET_P4_RAW_SLOT_COUNT 2
#define PET_P4_RAW_HEADER_BYTES 4096U
#define PET_P4_RAW_HEADER_VERSION 1U
#define PET_P4_RAW_HEADER_ENTRY_OFFSET 88U
#define PET_P4_RAW_HEADER_ENTRY_BYTES 24U
#define PET_P4_RAW_MAX_FILES 32U
#define PET_P4_RAW_PACK_ID_BYTES 64U

/* Per-slot payload budget on the ~121.7 MB NAND volume. The original board
 * used 10 MB raw partitions per slot; the H.264 packs re-encoded for the
 * 480x480 canvas land well below this cap. */
#define HD_STORAGE_SLOT_BUDGET_BYTES (40ULL * 1024ULL * 1024ULL)

static const unsigned char PET_P4_RAW_MAGIC[8] = {
  'P', '4', 'R', 'A', 'W', '0', '1', '\0'
};

static const char *TAG = "hd_storage";
static const char *k_slot_paths[PET_P4_RAW_SLOT_COUNT] = {
  "/nand/pet/raw0",
  "/nand/pet/raw1",
};

typedef struct {
  unsigned long long path_hash;
  uint32_t offset;
  uint32_t size;
  unsigned long long checksum;
} pet_p4_raw_entry_t;

typedef struct {
  bool prepared;
  uint32_t cursor;
  unsigned int entry_count;
  pet_p4_raw_entry_t entries[PET_P4_RAW_MAX_FILES];
  bool file_active;
  char file_path[128];
  uint32_t file_offset;
  unsigned long long file_size;
  unsigned long long file_checksum;
  uint32_t next_chunk_index;
  bool last_chunk_valid;
  uint32_t last_chunk_index;
  uint32_t last_chunk_size;
  unsigned long long last_chunk_checksum;
  FILE *pack_file;
} pet_p4_raw_transfer_t;

typedef struct {
  bool attempted;
  bool valid;
  char pack_id[PET_P4_RAW_PACK_ID_BYTES + 1U];
  unsigned int entry_count;
  pet_p4_raw_entry_t entries[PET_P4_RAW_MAX_FILES];
} pet_p4_raw_cache_t;

static bool s_mounted;
static pet_p4_raw_transfer_t g_transfer;
static pet_p4_raw_cache_t g_caches[PET_P4_RAW_SLOT_COUNT];

/* ---- little helpers (byte order, hashing, errors) ---- */

static void set_error(char *error, size_t error_size, const char *message) {
  if (!error || error_size == 0) return;
  snprintf(error, error_size, "%s", message ? message : "raw appearance storage failed");
}

static unsigned long long fnv1a64_update(
  unsigned long long hash,
  const unsigned char *bytes,
  size_t len
) {
  for (size_t i = 0; i < len; i += 1) {
    hash ^= (unsigned long long) bytes[i];
    hash *= 0x00000100000001b3ULL;
  }
  return hash;
}

static unsigned long long path_hash(const char *logical_path) {
  return fnv1a64_update(
    0xcbf29ce484222325ULL,
    (const unsigned char *) logical_path,
    strlen(logical_path)
  );
}

static void put_u32(unsigned char *out, uint32_t value) {
  out[0] = (unsigned char) (value & 0xffU);
  out[1] = (unsigned char) ((value >> 8U) & 0xffU);
  out[2] = (unsigned char) ((value >> 16U) & 0xffU);
  out[3] = (unsigned char) ((value >> 24U) & 0xffU);
}

static uint32_t get_u32(const unsigned char *in) {
  return ((uint32_t) in[0])
       | ((uint32_t) in[1] << 8U)
       | ((uint32_t) in[2] << 16U)
       | ((uint32_t) in[3] << 24U);
}

static void put_u64(unsigned char *out, unsigned long long value) {
  for (unsigned int i = 0; i < 8U; i += 1U) {
    out[i] = (unsigned char) ((value >> (i * 8U)) & 0xffULL);
  }
}

static unsigned long long get_u64(const unsigned char *in) {
  unsigned long long value = 0;
  for (unsigned int i = 0; i < 8U; i += 1U) {
    value |= (unsigned long long) in[i] << (i * 8U);
  }
  return value;
}

static bool pack_id_is_valid(const char *pack_id) {
  if (!pack_id || strlen(pack_id) != PET_P4_RAW_PACK_ID_BYTES) return false;
  for (size_t i = 0; i < PET_P4_RAW_PACK_ID_BYTES; i += 1) {
    char ch = pack_id[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  }
  return true;
}

static void invalidate_cache(int slot) {
  if (slot < 0 || slot >= PET_P4_RAW_SLOT_COUNT) return;
  memset(&g_caches[slot], 0, sizeof(g_caches[slot]));
}

static bool parse_chunk_index(const char *text, uint32_t *out) {
  char *end = NULL;
  unsigned long value;
  if (!text || !text[0] || !out) return false;
  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || !end || *end != '\0' || value > UINT32_MAX) return false;
  *out = (uint32_t) value;
  return true;
}

static bool slot_in_range(int slot) {
  return slot >= 0 && slot < PET_P4_RAW_SLOT_COUNT;
}

static void slot_pack_path(int slot, char *out, size_t out_size) {
  snprintf(out, out_size, "%s/pack.bin", k_slot_paths[slot]);
}

static void slot_header_path(int slot, char *out, size_t out_size) {
  snprintf(out, out_size, "%s/header.bin", k_slot_paths[slot]);
}

static void make_dirs_for(const char *file_path) {
  char dir[128];
  snprintf(dir, sizeof(dir), "%s", file_path);
  char *slash = strrchr(dir, '/');
  if (!slash) return;
  *slash = '\0';
  mkdir(dir, 0775);
}

/* ---- mount ---- */

#define HD_NAND_HOST_ID      BSP_NAND_SPI_HOST
#define HD_NAND_DMA_CHAN     SPI_DMA_CH_AUTO
#define HD_NAND_CLOCK_HZ     (40 * 1000 * 1000)
#define HD_NAND_MAX_TRANSFER (4 * 1024)
#define HD_NAND_QUEUE_SIZE   10
#define HD_NAND_SPI_FLAGS    SPI_DEVICE_HALFDUPLEX
#define HD_NAND_IO_MODE      SPI_NAND_IO_MODE_QIO

#define HD_NAND_CMD_GET_FEATURE 0x0F
#define HD_NAND_CMD_SET_FEATURE 0x1F
#define HD_NAND_CMD_READ_ID     0x9F
#define HD_NAND_FEATURE_CONFIG  0xB0
#define HD_NAND_CONFIG_QE_BIT   BIT(0)

#define HD_NAND_MFR_ALLIANCE   0x52
#define HD_NAND_MFR_WINBOND    0xEF
#define HD_NAND_MFR_GIGADEVICE 0xC8
#define HD_NAND_MFR_MICRON     0x2C
#define HD_NAND_MFR_ZETTA      0xBA
#define HD_NAND_MFR_XTX        0x0B
#define HD_NAND_MFR_FM         0xA1
#define HD_NAND_MFR_MACRONIX   0xC2

static bool hd_nand_mfr_supported(uint8_t m)
{
    switch (m) {
        case HD_NAND_MFR_ALLIANCE:
        case HD_NAND_MFR_WINBOND:
        case HD_NAND_MFR_GIGADEVICE:
        case HD_NAND_MFR_MICRON:
        case HD_NAND_MFR_ZETTA:
        case HD_NAND_MFR_XTX:
        case HD_NAND_MFR_FM:
        case HD_NAND_MFR_MACRONIX:
            return true;
        default:
            return false;
    }
}

static esp_err_t hd_release_control_pin(gpio_num_t pin)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_set_level(pin, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "preload NAND GPIO%d", pin);
    ret = gpio_config(&cfg);
    ESP_RETURN_ON_ERROR(ret, TAG, "config NAND GPIO%d", pin);
    ret = gpio_set_level(pin, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "release NAND GPIO%d", pin);
    return ESP_OK;
}

static esp_err_t hd_nand_read_u8(spi_device_handle_t spi, uint8_t cmd,
                                 uint8_t addr, uint8_t *value)
{
    spi_transaction_ext_t t = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_CMD |
                     SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_RXDATA,
            .rxlength = 8,
            .addr = addr,
            .cmd = cmd,
        },
        .address_bits = 8,
        .command_bits = 8,
        .dummy_bits = 0,
    };
    ESP_RETURN_ON_ERROR(spi_device_transmit(spi, (spi_transaction_t *)&t), TAG,
                        "NAND bootstrap cmd 0x%02x", cmd);
    *value = t.base.rx_data[0];
    return ESP_OK;
}

static esp_err_t hd_nand_write_u8(spi_device_handle_t spi, uint8_t cmd,
                                  uint8_t addr, uint8_t value)
{
    spi_transaction_ext_t t = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_CMD |
                     SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_TXDATA,
            .length = 8,
            .addr = addr,
            .cmd = cmd,
        },
        .address_bits = 8,
        .command_bits = 8,
        .dummy_bits = 0,
    };
    t.base.tx_data[0] = value;
    return spi_device_transmit(spi, (spi_transaction_t *)&t);
}

/* Single-wire READ-ID, then set QE before switching to QIO. Ported from the
 * verified perler_studio / factory-firmware mount sequence. */
static esp_err_t hd_nand_quad_bootstrap(spi_device_handle_t spi)
{
    uint8_t mfr = 0;
    ESP_RETURN_ON_ERROR(hd_nand_read_u8(spi, HD_NAND_CMD_READ_ID, 0, &mfr), TAG,
                        "NAND READ-ID failed");
    ESP_RETURN_ON_FALSE(hd_nand_mfr_supported(mfr), ESP_ERR_INVALID_RESPONSE, TAG,
                        "unsupported NAND manufacturer 0x%02x", mfr);
    if (mfr == HD_NAND_MFR_WINBOND || mfr == HD_NAND_MFR_MICRON) {
        return ESP_OK;
    }
    uint8_t config = 0;
    ESP_RETURN_ON_ERROR(
        hd_nand_read_u8(spi, HD_NAND_CMD_GET_FEATURE, HD_NAND_FEATURE_CONFIG, &config),
        TAG, "NAND read config failed");
    if ((config & HD_NAND_CONFIG_QE_BIT) == 0) {
        ESP_RETURN_ON_ERROR(
            hd_nand_write_u8(spi, HD_NAND_CMD_SET_FEATURE, HD_NAND_FEATURE_CONFIG,
                             config | HD_NAND_CONFIG_QE_BIT),
            TAG, "NAND set QE failed");
    }
    return ESP_OK;
}

esp_err_t hd_storage_mount(void) {
  if (s_mounted) return ESP_OK;

  spi_device_handle_t spi = NULL;
  esp_blockdev_handle_t wl_bdl = NULL;
  bool bus_up = false;

  ESP_RETURN_ON_ERROR(hd_release_control_pin(BSP_NAND_HOLD), TAG, "release HOLD");
  ESP_RETURN_ON_ERROR(hd_release_control_pin(BSP_NAND_WP), TAG, "release WP");

  const spi_device_interface_config_t dev_config = {
      .clock_speed_hz = HD_NAND_CLOCK_HZ,
      .mode = 0,
      .spics_io_num = BSP_NAND_CS,
      .queue_size = HD_NAND_QUEUE_SIZE,
      .flags = HD_NAND_SPI_FLAGS,
  };

  /* bootstrap bus (single wire) for READ-ID + QE */
  const spi_bus_config_t boot_bus = {
      .mosi_io_num = BSP_NAND_D,
      .miso_io_num = BSP_NAND_Q,
      .sclk_io_num = BSP_NAND_CLK,
      .quadhd_io_num = GPIO_NUM_NC,
      .quadwp_io_num = GPIO_NUM_NC,
      .max_transfer_sz = HD_NAND_MAX_TRANSFER,
  };
  esp_err_t ret = spi_bus_initialize(HD_NAND_HOST_ID, &boot_bus, HD_NAND_DMA_CHAN);
  ESP_RETURN_ON_ERROR(ret, TAG, "bootstrap bus: %s", esp_err_to_name(ret));
  bus_up = true;
  ret = spi_bus_add_device(HD_NAND_HOST_ID, &dev_config, &spi);
  ESP_GOTO_ON_ERROR(ret, fail, TAG, "bootstrap device: %s", esp_err_to_name(ret));
  ret = hd_nand_quad_bootstrap(spi);
  ESP_GOTO_ON_ERROR(ret, fail, TAG, "QIO bootstrap: %s", esp_err_to_name(ret));
  ESP_RETURN_ON_ERROR(spi_bus_remove_device(spi), TAG, "remove bootstrap device");
  spi = NULL;
  ESP_RETURN_ON_ERROR(spi_bus_free(HD_NAND_HOST_ID), TAG, "free bootstrap bus");
  bus_up = false;

  /* real QIO bus */
  const spi_bus_config_t bus = {
      .mosi_io_num = BSP_NAND_D,
      .miso_io_num = BSP_NAND_Q,
      .sclk_io_num = BSP_NAND_CLK,
      .quadhd_io_num = BSP_NAND_HOLD,
      .quadwp_io_num = BSP_NAND_WP,
      .max_transfer_sz = HD_NAND_MAX_TRANSFER,
  };
  ret = spi_bus_initialize(HD_NAND_HOST_ID, &bus, HD_NAND_DMA_CHAN);
  ESP_RETURN_ON_ERROR(ret, TAG, "NAND bus: %s", esp_err_to_name(ret));
  bus_up = true;
  ret = spi_bus_add_device(HD_NAND_HOST_ID, &dev_config, &spi);
  ESP_GOTO_ON_ERROR(ret, fail, TAG, "NAND device: %s", esp_err_to_name(ret));

  spi_nand_flash_config_t nand_config = {
      .device_handle = spi,
      .gc_factor = 0,
      .io_mode = HD_NAND_IO_MODE,
      .flags = HD_NAND_SPI_FLAGS,
  };
  ret = spi_nand_flash_init_with_layers(&nand_config, &wl_bdl);
  ESP_GOTO_ON_ERROR(ret, fail, TAG, "NAND WL-BDL: %s", esp_err_to_name(ret));

  const esp_vfs_littlefs_conf_t mount_config = {
      .base_path = "/nand",
      .blockdev = wl_bdl,
      .format_if_mount_failed = true,
      .read_only = false,
      .dont_mount = false,
      .grow_on_mount = false,
  };
  ret = esp_vfs_littlefs_register(&mount_config);
  ESP_GOTO_ON_ERROR(ret, fail, TAG, "littlefs mount: %s", esp_err_to_name(ret));

  mkdir("/nand/pet", 0775);
  s_mounted = true;
  ESP_LOGI(TAG, "/nand pet storage mounted");
  return ESP_OK;

fail:
  if (spi) (void)spi_bus_remove_device(spi);
  if (wl_bdl) (void)wl_bdl->ops->release(wl_bdl);
  if (bus_up) (void)spi_bus_free(HD_NAND_HOST_ID);
  return ret;
}

bool hd_storage_ready(void) { return s_mounted; }

/* ---- pet_p4_raw_assets.h backend ---- */

void pet_p4_raw_assets_reset_transfer(void) {
  if (g_transfer.pack_file) {
    fclose(g_transfer.pack_file);
    g_transfer.pack_file = NULL;
  }
  memset(&g_transfer, 0, sizeof(g_transfer));
}

bool pet_p4_raw_assets_available(void) {
  return s_mounted && pet_p4_raw_assets_slot_available(PET_P4_RAW_APPEARANCE_SLOT);
}

unsigned long long pet_p4_raw_assets_capacity_bytes(void) {
  return pet_p4_raw_assets_slot_capacity_bytes(PET_P4_RAW_APPEARANCE_SLOT);
}

bool pet_p4_raw_assets_slot_available(int slot) {
  return s_mounted && slot_in_range(slot);
}

unsigned long long pet_p4_raw_assets_slot_capacity_bytes(int slot) {
  if (!slot_in_range(slot)) return 0;
  return HD_STORAGE_SLOT_BUDGET_BYTES;
}

bool pet_p4_raw_assets_supports_path(const char *logical_path) {
  static const char mjpeg_suffix[] = ".mjpg";
  static const char h264_suffix[] = ".h264";
  size_t path_len;
  if (!logical_path) return false;
  path_len = strlen(logical_path);
  return (path_len > sizeof(mjpeg_suffix) - 1U
          && strcmp(
            logical_path + path_len - (sizeof(mjpeg_suffix) - 1U),
            mjpeg_suffix
          ) == 0)
      || (path_len > sizeof(h264_suffix) - 1U
          && strcmp(
            logical_path + path_len - (sizeof(h264_suffix) - 1U),
            h264_suffix
          ) == 0);
}

bool pet_p4_raw_assets_prepare(
  unsigned long long total_bytes,
  char *error,
  size_t error_size
) {
  char path[96];
  if (!s_mounted) {
    set_error(error, error_size, "appearance storage not mounted");
    return false;
  }
  unsigned long long requested =
    total_bytes > 0 ? total_bytes : HD_STORAGE_SLOT_BUDGET_BYTES;
  if (requested > HD_STORAGE_SLOT_BUDGET_BYTES) {
    set_error(error, error_size, "appearance pack exceeds raw partition capacity");
    return false;
  }
  pet_p4_raw_assets_reset_transfer();
  const int slot = PET_P4_RAW_APPEARANCE_SLOT;
  invalidate_cache(slot);
  slot_pack_path(slot, path, sizeof(path));
  make_dirs_for(path);
  remove(path);
  char header[96];
  slot_header_path(slot, header, sizeof(header));
  remove(header);
  g_transfer.pack_file = fopen(path, "wb");
  if (!g_transfer.pack_file) {
    set_error(error, error_size, "appearance pack file open failed");
    return false;
  }
  setvbuf(g_transfer.pack_file, NULL, _IOFBF, 16384);
  g_transfer.prepared = true;
  return true;
}

bool pet_p4_raw_assets_write_chunk(
  const char *logical_path,
  const char *index,
  const unsigned char *data,
  size_t data_len,
  char *error,
  size_t error_size
) {
  uint32_t chunk_index;
  if (!g_transfer.prepared || !g_transfer.pack_file) {
    set_error(error, error_size, "raw appearance transfer not prepared");
    return false;
  }
  if (!logical_path || !index || !data || data_len == 0) {
    set_error(error, error_size, "raw appearance chunk payload invalid");
    return false;
  }
  if (!parse_chunk_index(index, &chunk_index)) {
    set_error(error, error_size, "raw appearance chunk index invalid");
    return false;
  }
  if (g_transfer.file_active && strcmp(g_transfer.file_path, logical_path) != 0) {
    set_error(error, error_size, "raw appearance previous file not finished");
    return false;
  }
  unsigned long long checksum = fnv1a64_update(
    0xcbf29ce484222325ULL, data, data_len);
  if (g_transfer.last_chunk_valid && chunk_index == g_transfer.last_chunk_index
      && data_len == g_transfer.last_chunk_size
      && checksum == g_transfer.last_chunk_checksum) {
    /* Idempotent retry of the last accepted chunk. */
    return true;
  }
  if (chunk_index != g_transfer.next_chunk_index) {
    set_error(error, error_size, "raw appearance chunk sequence mismatch");
    return false;
  }
  if (!g_transfer.file_active) {
    if (g_transfer.entry_count >= PET_P4_RAW_MAX_FILES) {
      set_error(error, error_size, "raw appearance file count exceeded");
      return false;
    }
    snprintf(
      g_transfer.file_path, sizeof(g_transfer.file_path), "%s", logical_path);
    g_transfer.file_offset = g_transfer.cursor;
    g_transfer.file_size = 0;
    g_transfer.file_checksum = 0xcbf29ce484222325ULL;
    g_transfer.next_chunk_index = 0;
    g_transfer.file_active = true;
  }
  if (fwrite(data, 1, data_len, g_transfer.pack_file) != data_len) {
    set_error(error, error_size, "raw appearance chunk write failed");
    pet_p4_raw_assets_reset_transfer();
    return false;
  }
  g_transfer.cursor += (uint32_t) data_len;
  g_transfer.file_size += data_len;
  g_transfer.file_checksum = fnv1a64_update(
    g_transfer.file_checksum, data, data_len);
  g_transfer.last_chunk_valid = true;
  g_transfer.last_chunk_index = chunk_index;
  g_transfer.last_chunk_size = (uint32_t) data_len;
  g_transfer.last_chunk_checksum = checksum;
  g_transfer.next_chunk_index = chunk_index + 1U;
  return true;
}

bool pet_p4_raw_assets_finish_file(
  const char *logical_path,
  unsigned long long expected_size,
  const char *expected_checksum,
  char *error,
  size_t error_size
) {
  char actual[32];
  pet_p4_raw_entry_t *entry;
  if (!g_transfer.prepared || !g_transfer.file_active
      || strcmp(g_transfer.file_path, logical_path != NULL ? logical_path : "") != 0) {
    set_error(error, error_size, "raw appearance finish state is invalid");
    return false;
  }
  if (fflush(g_transfer.pack_file) != 0) {
    set_error(error, error_size, "raw appearance flush failed");
    return false;
  }
  snprintf(actual, sizeof(actual), "%016llx", g_transfer.file_checksum);
  if (g_transfer.file_size != expected_size
      || (expected_checksum != NULL && expected_checksum[0] != '\0'
          && strcmp(expected_checksum, actual) != 0)) {
    snprintf(
      error,
      error_size,
      "checksum mismatch expected_size=%llu actual_size=%llu expected=%s actual=%s",
      expected_size,
      g_transfer.file_size,
      expected_checksum ? expected_checksum : "",
      actual
    );
    return false;
  }
  entry = &g_transfer.entries[g_transfer.entry_count++];
  entry->path_hash = path_hash(logical_path);
  entry->offset = g_transfer.file_offset;
  entry->size = (uint32_t) g_transfer.file_size;
  entry->checksum = g_transfer.file_checksum;
  g_transfer.file_active = false;
  g_transfer.file_path[0] = '\0';
  g_transfer.file_offset = 0;
  g_transfer.file_size = 0;
  g_transfer.file_checksum = 0xcbf29ce484222325ULL;
  g_transfer.next_chunk_index = 0;
  g_transfer.last_chunk_valid = false;
  g_transfer.last_chunk_index = 0;
  g_transfer.last_chunk_size = 0;
  g_transfer.last_chunk_checksum = 0;
  return true;
}

bool pet_p4_raw_assets_commit(
  const char *pack_id,
  char *error,
  size_t error_size
) {
  unsigned char *header;
  char path[96];
  char tmp[112];
  FILE *file;
  if (!g_transfer.prepared || g_transfer.file_active
      || g_transfer.entry_count == 0 || !pack_id_is_valid(pack_id)) {
    set_error(error, error_size, "raw appearance commit state is invalid");
    return false;
  }
  if (fclose(g_transfer.pack_file) != 0) {
    g_transfer.pack_file = NULL;
    set_error(error, error_size, "raw appearance pack close failed");
    return false;
  }
  g_transfer.pack_file = NULL;

  header = (unsigned char *) calloc(1, PET_P4_RAW_HEADER_BYTES);
  if (!header) {
    set_error(error, error_size, "raw appearance header allocation failed");
    return false;
  }
  memcpy(header, PET_P4_RAW_MAGIC, sizeof(PET_P4_RAW_MAGIC));
  put_u32(header + 8U, PET_P4_RAW_HEADER_VERSION);
  put_u32(header + 12U, g_transfer.entry_count);
  put_u32(header + 16U, g_transfer.cursor);
  put_u32(header + 20U, 0U);
  memcpy(header + 24U, pack_id, PET_P4_RAW_PACK_ID_BYTES);
  for (unsigned int i = 0; i < g_transfer.entry_count; i += 1U) {
    unsigned char *encoded = header + PET_P4_RAW_HEADER_ENTRY_OFFSET
                           + i * PET_P4_RAW_HEADER_ENTRY_BYTES;
    put_u64(encoded, g_transfer.entries[i].path_hash);
    put_u32(encoded + 8U, g_transfer.entries[i].offset);
    put_u32(encoded + 12U, g_transfer.entries[i].size);
    put_u64(encoded + 16U, g_transfer.entries[i].checksum);
  }
  slot_header_path(PET_P4_RAW_APPEARANCE_SLOT, path, sizeof(path));
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  remove(tmp);
  file = fopen(tmp, "wb");
  if (!file) {
    free(header);
    set_error(error, error_size, "raw appearance header open failed");
    return false;
  }
  size_t written = fwrite(header, 1, PET_P4_RAW_HEADER_BYTES, file);
  free(header);
  if (written != PET_P4_RAW_HEADER_BYTES || fclose(file) != 0) {
    remove(tmp);
    set_error(error, error_size, "raw appearance header commit failed");
    return false;
  }
  /* Commit-last: the catalog header becomes visible only after the payload
   * is fully durable, mirroring the raw-flash original. */
  if (rename(tmp, path) != 0) {
    remove(tmp);
    set_error(error, error_size, "raw appearance header swap failed");
    return false;
  }
  pet_p4_raw_assets_reset_transfer();
  invalidate_cache(PET_P4_RAW_APPEARANCE_SLOT);
  return true;
}

bool pet_p4_raw_assets_invalidate(char *error, size_t error_size) {
  char path[96];
  bool ok = true;
  for (int slot = 0; slot < PET_P4_RAW_SLOT_COUNT; slot += 1) {
    slot_pack_path(slot, path, sizeof(path));
    if (remove(path) != 0 && errno != ENOENT) ok = false;
    slot_header_path(slot, path, sizeof(path));
    if (remove(path) != 0 && errno != ENOENT) ok = false;
    invalidate_cache(slot);
  }
  if (!ok) set_error(error, error_size, "raw appearance invalidate failed");
  return ok;
}

static bool load_cache(int slot) {
  pet_p4_raw_cache_t *cache;
  unsigned char *header;
  uint32_t count;
  uint32_t data_end;
  char path[96];
  FILE *file;
  if (!slot_in_range(slot) || !s_mounted) return false;
  cache = &g_caches[slot];
  if (cache->attempted) return cache->valid;
  cache->attempted = true;
  slot_header_path(slot, path, sizeof(path));
  file = fopen(path, "rb");
  if (!file) return false;
  header = (unsigned char *) malloc(PET_P4_RAW_HEADER_BYTES);
  if (!header) {
    fclose(file);
    return false;
  }
  bool parsed = fread(header, 1, PET_P4_RAW_HEADER_BYTES, file)
    == PET_P4_RAW_HEADER_BYTES;
  fclose(file);
  if (!parsed
      || memcmp(header, PET_P4_RAW_MAGIC, sizeof(PET_P4_RAW_MAGIC)) != 0
      || get_u32(header + 8U) != PET_P4_RAW_HEADER_VERSION) {
    free(header);
    return false;
  }
  count = get_u32(header + 12U);
  data_end = get_u32(header + 16U);
  memcpy(cache->pack_id, header + 24U, PET_P4_RAW_PACK_ID_BYTES);
  cache->pack_id[PET_P4_RAW_PACK_ID_BYTES] = '\0';
  if (count == 0 || count > PET_P4_RAW_MAX_FILES
      || !pack_id_is_valid(cache->pack_id)) {
    free(header);
    return false;
  }
  for (unsigned int i = 0; i < count; i += 1) {
    const unsigned char *encoded = header + PET_P4_RAW_HEADER_ENTRY_OFFSET
                                 + i * PET_P4_RAW_HEADER_ENTRY_BYTES;
    pet_p4_raw_entry_t *entry = &cache->entries[i];
    entry->path_hash = get_u64(encoded);
    entry->offset = get_u32(encoded + 8U);
    entry->size = get_u32(encoded + 12U);
    entry->checksum = get_u64(encoded + 16U);
    if (entry->offset < PET_P4_RAW_HEADER_BYTES || entry->size == 0
        || (unsigned long long) entry->offset + entry->size > data_end) {
      free(header);
      memset(cache, 0, sizeof(*cache));
      cache->attempted = true;
      return false;
    }
  }
  free(header);
  cache->entry_count = count;
  cache->valid = true;
  return true;
}

bool pet_p4_raw_assets_committed_pack_id(char *out, size_t out_size) {
  return pet_p4_raw_assets_committed_pack_id_for_slot(
    PET_P4_RAW_APPEARANCE_SLOT,
    out,
    out_size
  );
}

bool pet_p4_raw_assets_committed_pack_id_for_slot(
  int slot,
  char *out,
  size_t out_size
) {
  if (!out || out_size < PET_P4_RAW_PACK_ID_BYTES + 1U || !load_cache(slot)) return false;
  snprintf(out, out_size, "%s", g_caches[slot].pack_id);
  return true;
}

bool pet_p4_raw_assets_read_all(
  const char *logical_path,
  unsigned char *out,
  size_t expected_size
) {
  return pet_p4_raw_assets_read_all_from_slot(
    PET_P4_RAW_APPEARANCE_SLOT,
    logical_path,
    out,
    expected_size
  );
}

bool pet_p4_raw_assets_read_all_from_slot(
  int slot,
  const char *logical_path,
  unsigned char *out,
  size_t expected_size
) {
  pet_p4_raw_cache_t *cache;
  unsigned long long hash;
  char path[96];
  FILE *file;
  size_t read_total = 0;
  if (!slot_in_range(slot)
      || !logical_path || !out || expected_size == 0
      || !load_cache(slot)) {
    return false;
  }
  cache = &g_caches[slot];
  hash = path_hash(logical_path);
  for (unsigned int i = 0; i < cache->entry_count; i += 1U) {
    const pet_p4_raw_entry_t *entry = &cache->entries[i];
    if (entry->path_hash != hash || entry->size != expected_size) continue;
    slot_pack_path(slot, path, sizeof(path));
    file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, (long) entry->offset, SEEK_SET) == 0) {
      read_total = fread(out, 1, expected_size, file);
    }
    fclose(file);
    return read_total == expected_size;
  }
  return false;
}
