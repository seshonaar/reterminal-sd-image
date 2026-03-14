#include "spi_sd_storage.h"

#include <dirent.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "esphome/core/log.h"

namespace esphome {
namespace spi_sd_storage {

static const char *const TAG = "spi_sd_storage";
static const char *const MOUNT_POINT = "/sdcard";

static bool has_bmp_extension(const std::string &name) {
  if (name.size() < 4)
    return false;
  std::string lower;
  lower.reserve(name.size());
  for (char c : name)
    lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
  const size_t len = lower.size();
  return len >= 4 && lower.compare(len - 4, 4, ".bmp") == 0;
}

void SpiSdStorage::configure_output_pin_(int pin) {
  if (pin < 0)
    return;
  gpio_config_t cfg{};
  cfg.pin_bit_mask = 1ULL << pin;
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
}

void SpiSdStorage::configure_input_pin_(int pin) {
  if (pin < 0)
    return;
  gpio_config_t cfg{};
  cfg.pin_bit_mask = 1ULL << pin;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
}

void SpiSdStorage::scan_directory_() {
  this->image_paths_.clear();
  const std::string images_dir = std::string(MOUNT_POINT) + "/images";
  DIR *dir = opendir(images_dir.c_str());
  if (dir == nullptr) {
    ESP_LOGW(TAG, "Could not open %s", images_dir.c_str());
    return;
  }

  while (auto *entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name == "." || name == "..")
      continue;
    if (!has_bmp_extension(name))
      continue;
    this->image_paths_.push_back(images_dir + "/" + name);
  }
  closedir(dir);
}

void SpiSdStorage::probe_known_file_() {
  const char *path = "/sdcard/images/us.bmp";
  FILE *fp = fopen(path, "rb");
  if (fp == nullptr) {
    ESP_LOGE(TAG, "Known file probe failed to open: %s", path);
    return;
  }

  uint8_t header[16];
  size_t bytes_read = fread(header, 1, sizeof(header), fp);
  fclose(fp);

  ESP_LOGI(TAG, "Known file probe opened: %s", path);
  ESP_LOGI(TAG, "Known file probe read %u bytes", static_cast<unsigned>(bytes_read));
  if (bytes_read >= 2) {
    ESP_LOGI(TAG, "Known file probe header: %02X %02X", header[0], header[1]);
  }
}

std::string SpiSdStorage::get_random_image_path() const {
  if (this->image_paths_.empty())
    return "";
  size_t index = esp_random() % this->image_paths_.size();
  return this->image_paths_[index];
}

void SpiSdStorage::setup() {
  ESP_LOGI(TAG, "Initializing SPI SD storage");

  this->configure_output_pin_(this->enable_pin_);
  this->configure_input_pin_(this->detect_pin_);

  if (this->enable_pin_ >= 0)
    gpio_set_level(static_cast<gpio_num_t>(this->enable_pin_), 1);

  if (this->detect_pin_ >= 0) {
    int detect = gpio_get_level(static_cast<gpio_num_t>(this->detect_pin_));
    ESP_LOGI(TAG, "Card detect pin reads: %d", detect);
  }

  vTaskDelay(pdMS_TO_TICKS(100));

  spi_bus_config_t bus_cfg{};
  bus_cfg.mosi_io_num = this->mosi_pin_;
  bus_cfg.miso_io_num = this->miso_pin_;
  bus_cfg.sclk_io_num = this->clk_pin_;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 4096;

  constexpr spi_host_device_t host_slot = SPI2_HOST;
  esp_err_t err = spi_bus_initialize(host_slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  vTaskDelay(pdMS_TO_TICKS(50));

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = host_slot;
  host.max_freq_khz = 400;

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.host_id = host_slot;
  slot_config.gpio_cs = static_cast<gpio_num_t>(this->cs_pin_);

  esp_vfs_fat_sdmmc_mount_config_t mount_config{};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 8;
  mount_config.allocation_unit_size = 16 * 1024;

  err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_fat_sdspi_mount failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  this->mounted_ = true;
  this->probe_known_file_();
  this->scan_directory_();
  ESP_LOGI(TAG, "Mounted SD card at %s", MOUNT_POINT);
  ESP_LOGI(TAG, "Found %u BMP files in /images", static_cast<unsigned>(this->image_paths_.size()));
  if (!this->image_paths_.empty()) {
    ESP_LOGI(TAG, "Random sample: %s", this->get_random_image_path().c_str());
  }
}

void SpiSdStorage::dump_config() {
  ESP_LOGCONFIG(TAG, "SPI SD Storage");
  ESP_LOGCONFIG(TAG, "  CLK pin: %d", this->clk_pin_);
  ESP_LOGCONFIG(TAG, "  MISO pin: %d", this->miso_pin_);
  ESP_LOGCONFIG(TAG, "  MOSI pin: %d", this->mosi_pin_);
  ESP_LOGCONFIG(TAG, "  CS pin: %d", this->cs_pin_);
  ESP_LOGCONFIG(TAG, "  Enable pin: %d", this->enable_pin_);
  ESP_LOGCONFIG(TAG, "  Detect pin: %d", this->detect_pin_);
  ESP_LOGCONFIG(TAG, "  Mounted: %s", this->mounted_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  BMP files found: %u", static_cast<unsigned>(this->image_paths_.size()));
}

}  // namespace spi_sd_storage
}  // namespace esphome
