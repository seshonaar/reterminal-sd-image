#include "random_sd_image.h"

#include <cstring>
#include <stdio.h>

#include <algorithm>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esphome/core/log.h"

namespace esphome {
namespace random_sd_image {

static const char *const TAG = "random_sd_image";

static uint16_t read_le16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t read_le32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

static uint16_t to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint8_t rgb565_r(uint16_t value) { return static_cast<uint8_t>(((value >> 11) & 0x1F) * 255 / 31); }
static uint8_t rgb565_g(uint16_t value) { return static_cast<uint8_t>(((value >> 5) & 0x3F) * 255 / 63); }
static uint8_t rgb565_b(uint16_t value) { return static_cast<uint8_t>((value & 0x1F) * 255 / 31); }

struct PaletteColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint16_t rgb565;
};

static const PaletteColor SPECTRA6_PALETTE[] = {
    {255, 255, 255, to_rgb565(255, 255, 255)},
    {0, 0, 0, to_rgb565(0, 0, 0)},
    {213, 53, 49, to_rgb565(213, 53, 49)},
    {243, 193, 59, to_rgb565(243, 193, 59)},
    {35, 93, 178, to_rgb565(35, 93, 178)},
    {41, 144, 74, to_rgb565(41, 144, 74)},
};

size_t RandomSdImage::get_row_stride_() const {
  return static_cast<size_t>(((this->width_ * 2) + 3) & ~3);
}

void RandomSdImage::release_buffer_(uint8_t *&buffer, size_t &buffer_size, int16_t *&err_curr, int16_t *&err_next) {
  if (buffer != nullptr) {
    heap_caps_free(buffer);
    buffer = nullptr;
  }
  if (err_curr != nullptr) {
    heap_caps_free(err_curr);
    err_curr = nullptr;
  }
  if (err_next != nullptr) {
    heap_caps_free(err_next);
    err_next = nullptr;
  }
  buffer_size = 0;
}

void RandomSdImage::reset_buffer_() {
  if (this->bmp_file_ != nullptr) {
    fclose(this->bmp_file_);
    this->bmp_file_ = nullptr;
  }
  this->release_buffer_(this->pending_buffer_, this->pending_buffer_size_, this->error_curr_, this->error_next_);
  this->bmp_loaded_ = false;
  this->bmp_loading_ = false;
  this->load_row_ = 0;
}

bool RandomSdImage::start_next_image_load_() {
  if (this->prefetch_blocked_) {
    ESP_LOGI(TAG, "Prefetch start skipped because display cooldown is active");
    return false;
  }
  if (this->storage_ == nullptr || !this->storage_->is_mounted()) {
    ESP_LOGW(TAG, "Cannot load image because storage is not mounted");
    return false;
  }
  if (this->bmp_loading_) {
    return false;
  }

  const auto &paths = this->storage_->get_image_paths();
  if (paths.empty()) {
    ESP_LOGW(TAG, "No BMP files found in %s", this->directory_.c_str());
    return false;
  }

  this->selected_path_.clear();
  if (paths.size() == 1) {
    this->selected_path_ = paths.front();
  } else {
    for (size_t attempt = 0; attempt < 8; attempt++) {
      std::string candidate = this->storage_->get_random_image_path();
      if (!candidate.empty() && candidate != this->committed_path_) {
        this->selected_path_ = candidate;
        break;
      }
    }
    if (this->selected_path_.empty()) {
      for (const auto &path : paths) {
        if (path != this->committed_path_) {
          this->selected_path_ = path;
          break;
        }
      }
    }
  }

  if (this->selected_path_.empty()) {
    ESP_LOGW(TAG, "Could not choose a BMP path for prefetch");
    return false;
  }

  ESP_LOGI(TAG, "Preparing next image: %s", this->selected_path_.c_str());
  if (!this->read_selected_bmp_info_()) {
    return false;
  }

  ESP_LOGI(TAG, "BMP info: %ldx%ld, %u bpp, pixel offset %u, DIB header %u",
           static_cast<long>(this->width_), static_cast<long>(this->height_), this->bits_per_pixel_,
           static_cast<unsigned>(this->pixel_offset_), static_cast<unsigned>(this->dib_header_size_));
  return this->begin_load_selected_bmp_();
}

void RandomSdImage::block_prefetch_for(uint32_t duration_ms) {
  this->prefetch_blocked_ = true;
  this->cancel_timeout("prefetch_unblock");
  ESP_LOGI(TAG, "Blocking image prefetch for %u ms while display refresh owns SPI", static_cast<unsigned>(duration_ms));
  this->set_timeout("prefetch_unblock", duration_ms, [this]() {
    this->prefetch_blocked_ = false;
    ESP_LOGI(TAG, "Display cooldown finished; prefetch may resume");
    this->ensure_prefetch();
  });
}

void RandomSdImage::ensure_prefetch() {
  if (!this->prefetch_enabled_) {
    return;
  }
  if (this->prefetch_blocked_) {
    return;
  }
  if (this->bmp_loading_) {
    ESP_LOGI(TAG, "Prefetch already in progress: %s", this->selected_path_.c_str());
    return;
  }
  if (this->bmp_loaded_ && this->pending_buffer_ != nullptr) {
    ESP_LOGI(TAG, "Prefetch already ready: %s", this->selected_path_.c_str());
    return;
  }
  if (this->start_next_image_load_()) {
    ESP_LOGI(TAG, "Explicitly started image prefetch");
  }
}

uint16_t RandomSdImage::quantize_pixel_(uint16_t rgb565, int x) {
  int r = rgb565_r(rgb565);
  int g = rgb565_g(rgb565);
  int b = rgb565_b(rgb565);

  if (this->error_curr_ != nullptr) {
    const int idx = x * 3;
    r = std::clamp(r + this->error_curr_[idx + 0], 0, 255);
    g = std::clamp(g + this->error_curr_[idx + 1], 0, 255);
    b = std::clamp(b + this->error_curr_[idx + 2], 0, 255);
  }

  const PaletteColor *best = &SPECTRA6_PALETTE[0];
  int best_distance = INT32_MAX;
  for (const auto &color : SPECTRA6_PALETTE) {
    const int dr = r - color.r;
    const int dg = g - color.g;
    const int db = b - color.b;
    const int distance = dr * dr + dg * dg + db * db;
    if (distance < best_distance) {
      best_distance = distance;
      best = &color;
    }
  }

  const int err_r = r - best->r;
  const int err_g = g - best->g;
  const int err_b = b - best->b;

  if (this->error_curr_ != nullptr) {
    const int idx = x * 3;
    if (x + 1 < this->width_) {
      this->error_curr_[idx + 3] += err_r * 7 / 16;
      this->error_curr_[idx + 4] += err_g * 7 / 16;
      this->error_curr_[idx + 5] += err_b * 7 / 16;
    }
    if (this->error_next_ != nullptr) {
      if (x > 0) {
        this->error_next_[idx - 3] += err_r * 3 / 16;
        this->error_next_[idx - 2] += err_g * 3 / 16;
        this->error_next_[idx - 1] += err_b * 3 / 16;
      }
      this->error_next_[idx + 0] += err_r * 5 / 16;
      this->error_next_[idx + 1] += err_g * 5 / 16;
      this->error_next_[idx + 2] += err_b * 5 / 16;
      if (x + 1 < this->width_) {
        this->error_next_[idx + 3] += err_r * 1 / 16;
        this->error_next_[idx + 4] += err_g * 1 / 16;
        this->error_next_[idx + 5] += err_b * 1 / 16;
      }
    }
  }

  return best->rgb565;
}

bool RandomSdImage::read_selected_bmp_info_() {
  this->bmp_valid_ = false;
  this->bmp_loaded_ = false;
  this->width_ = 0;
  this->height_ = 0;
  this->abs_height_ = 0;
  this->bits_per_pixel_ = 0;
  this->pixel_offset_ = 0;
  this->dib_header_size_ = 0;

  if (this->selected_path_.empty()) {
    return false;
  }

  FILE *fp = fopen(this->selected_path_.c_str(), "rb");
  if (fp == nullptr) {
    ESP_LOGE(TAG, "Failed to open selected BMP: %s", this->selected_path_.c_str());
    return false;
  }

  uint8_t header[54];
  const size_t bytes_read = fread(header, 1, sizeof(header), fp);
  fclose(fp);

  if (bytes_read < sizeof(header)) {
    ESP_LOGE(TAG, "BMP header too short: %u bytes", static_cast<unsigned>(bytes_read));
    return false;
  }

  if (header[0] != 'B' || header[1] != 'M') {
    ESP_LOGE(TAG, "Selected file is not a BMP: %s", this->selected_path_.c_str());
    return false;
  }

  this->pixel_offset_ = read_le32(&header[10]);
  this->dib_header_size_ = read_le32(&header[14]);
  this->width_ = static_cast<int32_t>(read_le32(&header[18]));
  this->height_ = static_cast<int32_t>(read_le32(&header[22]));
  this->abs_height_ = this->height_ > 0 ? this->height_ : -this->height_;
  const uint16_t planes = read_le16(&header[26]);
  this->bits_per_pixel_ = read_le16(&header[28]);
  const uint32_t compression = read_le32(&header[30]);

  if (planes != 1) {
    ESP_LOGE(TAG, "Unsupported BMP planes: %u", planes);
    return false;
  }

  if (this->bits_per_pixel_ != 16) {
    ESP_LOGE(TAG, "Unsupported BMP bpp: %u", this->bits_per_pixel_);
    return false;
  }

  if (compression != 0 && compression != 3) {
    ESP_LOGE(TAG, "Unsupported BMP compression: %u", static_cast<unsigned>(compression));
    return false;
  }

  this->bmp_valid_ = true;
  return true;
}

bool RandomSdImage::begin_load_selected_bmp_() {
  this->reset_buffer_();

  if (!this->bmp_valid_) {
    ESP_LOGW(TAG, "Skipping BMP load because header is invalid");
    return false;
  }

  const size_t frame_stride = static_cast<size_t>(this->width_) * 2;
  this->pending_buffer_size_ = frame_stride * static_cast<size_t>(this->abs_height_);
  this->pending_buffer_ = static_cast<uint8_t *>(heap_caps_malloc(this->pending_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->pending_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM for BMP", static_cast<unsigned>(this->pending_buffer_size_));
    return false;
  }

  const size_t error_bytes = static_cast<size_t>(this->width_) * 3 * sizeof(int16_t);
  this->error_curr_ = static_cast<int16_t *>(heap_caps_calloc(static_cast<size_t>(this->width_) * 3, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->error_next_ = static_cast<int16_t *>(heap_caps_calloc(static_cast<size_t>(this->width_) * 3, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->error_curr_ == nullptr || this->error_next_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes for dithering buffers", static_cast<unsigned>(error_bytes * 2));
    this->reset_buffer_();
    return false;
  }

  this->bmp_file_ = fopen(this->selected_path_.c_str(), "rb");
  if (this->bmp_file_ == nullptr) {
    ESP_LOGE(TAG, "Failed to open selected BMP for staged loading: %s", this->selected_path_.c_str());
    this->reset_buffer_();
    return false;
  }

  if (fseek(this->bmp_file_, static_cast<long>(this->pixel_offset_), SEEK_SET) != 0) {
    ESP_LOGE(TAG, "Failed to seek to BMP pixel data");
    this->reset_buffer_();
    return false;
  }

  this->bmp_loading_ = true;
  this->load_row_ = 0;
  ESP_LOGI(TAG, "Beginning staged BMP load into PSRAM: %s (%u bytes)", this->selected_path_.c_str(),
           static_cast<unsigned>(this->pending_buffer_size_));
  return true;
}

bool RandomSdImage::continue_load_selected_bmp_() {
  if (!this->bmp_loading_ || this->pending_buffer_ == nullptr || this->bmp_file_ == nullptr) {
    return this->bmp_loaded_;
  }

  const size_t frame_stride = static_cast<size_t>(this->width_) * 2;
  const size_t row_stride = this->get_row_stride_();

  uint8_t row_buffer[1604];
  const bool bottom_up = this->height_ > 0;

  const int32_t rows_per_pass = 24;
  const int32_t end_row = std::min(this->abs_height_, this->load_row_ + rows_per_pass);
  for (int32_t src_row = this->load_row_; src_row < end_row; src_row++) {
    const size_t bytes_read = fread(row_buffer, 1, row_stride, this->bmp_file_);
    if (bytes_read < row_stride) {
      ESP_LOGE(TAG, "Short read while loading BMP row %ld", static_cast<long>(src_row));
      this->reset_buffer_();
      return false;
    }

    const int32_t dst_row = bottom_up ? (this->abs_height_ - 1 - src_row) : src_row;
    auto *dst = reinterpret_cast<uint16_t *>(this->pending_buffer_ + static_cast<size_t>(dst_row) * frame_stride);
    auto *src = reinterpret_cast<uint16_t *>(row_buffer);
    for (int x = 0; x < this->width_; x++) {
      dst[x] = this->quantize_pixel_(src[x], x);
    }
    std::swap(this->error_curr_, this->error_next_);
    memset(this->error_next_, 0, static_cast<size_t>(this->width_) * 3 * sizeof(int16_t));
  }

  this->load_row_ = end_row;
  if (this->load_row_ >= this->abs_height_) {
    this->bmp_loaded_ = true;
    this->bmp_loading_ = false;
    fclose(this->bmp_file_);
    this->bmp_file_ = nullptr;
    ESP_LOGI(TAG, "Loaded BMP into PSRAM: %s (%u bytes)", this->selected_path_.c_str(),
             static_cast<unsigned>(this->pending_buffer_size_));
  }
  return this->bmp_loaded_;
}

void RandomSdImage::request_refresh() {
  if (this->bmp_loaded_ && this->pending_buffer_ != nullptr) {
    ESP_LOGI(TAG, "Using prefetched image for refresh: %s", this->selected_path_.c_str());
    return;
  }

  if (this->bmp_loading_) {
    ESP_LOGI(TAG, "Refresh requested while prefetch is loading; will use it when ready: %s", this->selected_path_.c_str());
    return;
  }

  if (this->start_next_image_load_()) {
    ESP_LOGI(TAG, "Refresh requested with no cached image; loading a new one now");
  }
}

void RandomSdImage::commit_refresh() {
  if (!this->bmp_loaded_ || this->pending_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Ignoring image commit because no pending frame is ready");
    return;
  }
  int16_t *no_err_curr = nullptr;
  int16_t *no_err_next = nullptr;
  this->release_buffer_(this->committed_buffer_, this->committed_buffer_size_, no_err_curr, no_err_next);
  this->committed_buffer_ = this->pending_buffer_;
  this->committed_buffer_size_ = this->pending_buffer_size_;
  this->pending_buffer_ = nullptr;
  this->pending_buffer_size_ = 0;
  this->committed_ready_ = true;
  this->bmp_loaded_ = false;
  this->committed_path_ = this->selected_path_;
  ESP_LOGI(TAG, "Committed image frame: %s", this->selected_path_.c_str());
}

bool RandomSdImage::draw_committed(display::Display &display) {
  if (!this->committed_ready_ || this->committed_buffer_ == nullptr) {
    return false;
  }
  display.draw_pixels_at(0, 0, this->width_, this->abs_height_, this->committed_buffer_, display::COLOR_ORDER_RGB,
                         display::COLOR_BITNESS_565, false);
  return true;
}

void RandomSdImage::loop() {
  if (this->bmp_loading_) {
    this->continue_load_selected_bmp_();
    return;
  }

  if (this->prefetch_enabled_ && !this->bmp_loaded_ && this->pending_buffer_ == nullptr) {
    this->ensure_prefetch();
  }
}

void RandomSdImage::setup() {
  ESP_LOGI(TAG, "Random SD image setup");
  if (this->storage_ == nullptr || !this->storage_->is_mounted()) {
    ESP_LOGW(TAG, "Storage is not mounted yet");
  }
}

void RandomSdImage::dump_config() {
  ESP_LOGCONFIG(TAG, "Random SD Image");
  ESP_LOGCONFIG(TAG, "  Directory: %s", this->directory_.c_str());
  ESP_LOGCONFIG(TAG, "  Storage attached: %s", this->storage_ != nullptr ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Selected path: %s", this->selected_path_.empty() ? "<none>" : this->selected_path_.c_str());
  ESP_LOGCONFIG(TAG, "  BMP header valid: %s", this->bmp_valid_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  BMP loaded: %s", this->bmp_loaded_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Committed frame ready: %s", this->committed_ready_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Prefetch enabled: %s", this->prefetch_enabled_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Prefetch blocked: %s", this->prefetch_blocked_ ? "yes" : "no");
  if (this->bmp_valid_) {
    ESP_LOGCONFIG(TAG, "  Width: %ld", static_cast<long>(this->width_));
    ESP_LOGCONFIG(TAG, "  Height: %ld", static_cast<long>(this->height_));
    ESP_LOGCONFIG(TAG, "  Bits per pixel: %u", this->bits_per_pixel_);
    ESP_LOGCONFIG(TAG, "  Pixel offset: %u", static_cast<unsigned>(this->pixel_offset_));
  }
}

}  // namespace random_sd_image
}  // namespace esphome
