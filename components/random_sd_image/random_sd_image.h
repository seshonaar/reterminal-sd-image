#pragma once

#include <cstdio>
#include <cstdint>
#include <string>

#include "esphome/components/display/display.h"
#include "esphome/core/component.h"

#include "../spi_sd_storage/spi_sd_storage.h"

namespace esphome {

namespace random_sd_image {

class RandomSdImage : public Component {
 public:
  void set_storage(spi_sd_storage::SpiSdStorage *storage) { this->storage_ = storage; }
  void set_directory(const std::string &directory) { this->directory_ = directory; }

  void block_prefetch_for(uint32_t duration_ms);
  void ensure_prefetch();
  void request_refresh();
  bool is_ready() const { return this->committed_ready_; }
  bool is_loading() const { return this->bmp_loading_; }
  bool has_pending_frame() const { return this->bmp_loaded_; }
  void commit_refresh();
  bool draw_committed(display::Display &display);
  void loop() override;
  void setup() override;
  void dump_config() override;

 protected:
  bool start_next_image_load_();
  bool read_selected_bmp_info_();
  bool begin_load_selected_bmp_();
  bool continue_load_selected_bmp_();
  uint16_t quantize_pixel_(uint16_t rgb565, int x);
  void reset_buffer_();
  void release_buffer_(uint8_t *&buffer, size_t &buffer_size, int16_t *&err_curr, int16_t *&err_next);
  size_t get_row_stride_() const;

  spi_sd_storage::SpiSdStorage *storage_{nullptr};
  std::string directory_;
  std::string selected_path_;
  std::string committed_path_;
  FILE *bmp_file_{nullptr};
  uint8_t *pending_buffer_{nullptr};
  size_t pending_buffer_size_{0};
  int16_t *error_curr_{nullptr};
  int16_t *error_next_{nullptr};
  uint8_t *committed_buffer_{nullptr};
  size_t committed_buffer_size_{0};
  int32_t abs_height_{0};
  bool bmp_valid_{false};
  bool bmp_loaded_{false};
  bool bmp_loading_{false};
  bool committed_ready_{false};
  bool prefetch_blocked_{false};
  int32_t width_{0};
  int32_t height_{0};
  uint16_t bits_per_pixel_{0};
  uint32_t pixel_offset_{0};
  uint32_t dib_header_size_{0};
  int32_t load_row_{0};
};

}  // namespace random_sd_image
}  // namespace esphome
