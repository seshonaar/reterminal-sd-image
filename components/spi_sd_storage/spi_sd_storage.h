#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"

#include "sdmmc_cmd.h"

namespace esphome {
namespace spi_sd_storage {

class SpiSdStorage : public Component {
 public:
  void set_clk_pin(int pin) { this->clk_pin_ = pin; }
  void set_miso_pin(int pin) { this->miso_pin_ = pin; }
  void set_mosi_pin(int pin) { this->mosi_pin_ = pin; }
  void set_cs_pin(int pin) { this->cs_pin_ = pin; }
  void set_enable_pin(int pin) { this->enable_pin_ = pin; }
  void set_detect_pin(int pin) { this->detect_pin_ = pin; }

  void setup() override;
  void dump_config() override;

  bool is_mounted() const { return this->mounted_; }
  const std::vector<std::string> &get_image_paths() const { return this->image_paths_; }
  std::string get_random_image_path() const;

 protected:
  void probe_known_file_();
  void configure_output_pin_(int pin);
  void configure_input_pin_(int pin);
  void scan_directory_();

  int clk_pin_{-1};
  int miso_pin_{-1};
  int mosi_pin_{-1};
  int cs_pin_{-1};
  int enable_pin_{-1};
  int detect_pin_{-1};
  bool mounted_{false};
  sdmmc_card_t *card_{nullptr};
  std::vector<std::string> image_paths_;
};

}  // namespace spi_sd_storage
}  // namespace esphome
