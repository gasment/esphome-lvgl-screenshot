#include "lvgl_screenshot.h"

#ifdef USE_ESP_IDF

#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include <algorithm>

namespace esphome {
namespace lvgl_screenshot {

static const char *const TAG = "lvgl_screenshot";

LvglScreenshot *LvglScreenshot::instance_ = nullptr;

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void LvglScreenshot::setup() {
  instance_ = this;

  // Create semaphores for HTTP handler <-> main-loop synchronisation
  this->capture_requested_ = xSemaphoreCreateBinary();
  this->capture_done_ = xSemaphoreCreateBinary();

  if (!this->capture_requested_ || !this->capture_done_) {
    ESP_LOGE(TAG, "Failed to create semaphores");
    this->mark_failed();
    return;
  }

  // Determine display dimensions from the default LVGL display
  lv_disp_t *disp = lv_disp_get_default();
  if (!disp) {
    ESP_LOGE(TAG, "No LVGL display found - is lvgl: initialised before this component?");
    this->mark_failed();
    return;
  }

  uint32_t width = (uint32_t) lv_disp_get_hor_res(disp);
  uint32_t height = (uint32_t) lv_disp_get_ver_res(disp);

  // BMP rows must be padded to a 4-byte boundary.
  // For 800 px: 800*3 = 2400 bytes, already aligned — but keep the formula generic.
  uint32_t row_size = ((width * 3u + 3u) / 4u) * 4u;
  this->bmp_size_ = 54u + row_size * height;

  this->bmp_buf_ = (uint8_t *) heap_caps_malloc(this->bmp_size_, MALLOC_CAP_SPIRAM);
  if (!this->bmp_buf_) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes for BMP buffer in PSRAM", this->bmp_size_);
    this->mark_failed();
    return;
  }

  this->start_server_();
  ESP_LOGI(TAG, "LVGL screenshot server started — http://<device-ip>:%u/screenshot", this->port_);
}

// ---------------------------------------------------------------------------
// start_server_()  –  spin up esp_http_server on the configured port
// ---------------------------------------------------------------------------
void LvglScreenshot::start_server_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = this->port_;
  cfg.stack_size = 8192;
  // Use a unique ctrl_port so it doesn't clash with any other httpd instance
  cfg.ctrl_port = (uint16_t) (this->port_ + 1u);

  if (httpd_start(&this->server_, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server on port %u", this->port_);
    this->server_ = nullptr;
    return;
  }

  httpd_uri_t uri = {
      .uri = "/screenshot",
      .method = HTTP_GET,
      .handler = LvglScreenshot::handle_screenshot_,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(this->server_, &uri);
}

// ---------------------------------------------------------------------------
// loop()  –  called from the ESPHome main task; safe to touch LVGL here
// ---------------------------------------------------------------------------
void LvglScreenshot::loop() {
  // Non-blocking check: did the HTTP handler signal a capture request?
  if (xSemaphoreTake(this->capture_requested_, 0) == pdTRUE) {
    this->do_capture_();
    xSemaphoreGive(this->capture_done_);
  }
}

// ---------------------------------------------------------------------------
// do_capture_()  –  convert the live LVGL framebuffer to a 24-bit BMP
// ---------------------------------------------------------------------------
void LvglScreenshot::do_capture_() {
  lv_disp_t *disp = lv_disp_get_default();
  if (!disp || !disp->driver || !disp->driver->draw_buf ||
      !disp->driver->draw_buf->buf_act) {
    ESP_LOGE(TAG, "LVGL framebuffer not available");
    // Write a zero-length marker so the handler doesn't hang
    this->bmp_size_ = 0;
    return;
  }

  auto *lvgl_buf = (lv_color_t *) disp->driver->draw_buf->buf_act;
  uint32_t width = (uint32_t) lv_disp_get_hor_res(disp);
  uint32_t height = (uint32_t) lv_disp_get_ver_res(disp);
  uint32_t row_size = ((width * 3u + 3u) / 4u) * 4u;
  uint32_t file_size = 54u + row_size * height;

  // ------------------------------------------------------------------
  // BMP File Header (14 bytes)
  // ------------------------------------------------------------------
  uint8_t *p = this->bmp_buf_;

  // Signature
  p[0] = 'B';
  p[1] = 'M';
  // File size (little-endian)
  p[2] = (uint8_t)(file_size);
  p[3] = (uint8_t)(file_size >> 8);
  p[4] = (uint8_t)(file_size >> 16);
  p[5] = (uint8_t)(file_size >> 24);
  // Reserved
  p[6] = 0; p[7] = 0; p[8] = 0; p[9] = 0;
  // Pixel data offset = 54
  p[10] = 54; p[11] = 0; p[12] = 0; p[13] = 0;

  // ------------------------------------------------------------------
  // DIB Header – BITMAPINFOHEADER (40 bytes)
  // ------------------------------------------------------------------
  // Header size
  p[14] = 40; p[15] = 0; p[16] = 0; p[17] = 0;
  // Width
  p[18] = (uint8_t)(width);
  p[19] = (uint8_t)(width >> 8);
  p[20] = (uint8_t)(width >> 16);
  p[21] = (uint8_t)(width >> 24);
  // Height – negative = top-down rows (matches LVGL buffer layout)
  int32_t neg_h = -(int32_t) height;
  p[22] = (uint8_t)(neg_h);
  p[23] = (uint8_t)((uint32_t) neg_h >> 8);
  p[24] = (uint8_t)((uint32_t) neg_h >> 16);
  p[25] = (uint8_t)((uint32_t) neg_h >> 24);
  // Colour planes = 1
  p[26] = 1; p[27] = 0;
  // Bits per pixel = 24
  p[28] = 24; p[29] = 0;
  // Compression = BI_RGB (none)
  p[30] = 0; p[31] = 0; p[32] = 0; p[33] = 0;
  // Image size (0 is valid for BI_RGB)
  p[34] = 0; p[35] = 0; p[36] = 0; p[37] = 0;
  // Horizontal pixels/metre (~96 DPI)
  p[38] = 0x13; p[39] = 0x0B; p[40] = 0; p[41] = 0;
  // Vertical pixels/metre
  p[42] = 0x13; p[43] = 0x0B; p[44] = 0; p[45] = 0;
  // Colours in table = 0 (use max)
  p[46] = 0; p[47] = 0; p[48] = 0; p[49] = 0;
  // Important colours = 0 (all)
  p[50] = 0; p[51] = 0; p[52] = 0; p[53] = 0;

  // ------------------------------------------------------------------
  // Pixel data  –  RGB565 → BGR888, row-by-row
  // ------------------------------------------------------------------
  uint8_t *pixel_data = this->bmp_buf_ + 54;

  for (uint32_t y = 0; y < height; y++) {
    uint8_t *row = pixel_data + y * row_size;
    for (uint32_t x = 0; x < width; x++) {
      lv_color_t c = lvgl_buf[y * width + x];

      // ESPHome builds LVGL with LV_COLOR_16_SWAP=1, so the green channel
      // is split across green_h (bits 2:0 of low byte) and green_l (bits 15:13).
      // Red and blue are contiguous 5-bit fields regardless of swap setting.
      uint8_t r5 = c.ch.red;
      uint8_t g6 = (uint8_t)((c.ch.green_h << 3) | c.ch.green_l);
      uint8_t b5 = c.ch.blue;

      // Scale 5-bit → 8-bit and 6-bit → 8-bit by replicating the MSBs into the LSBs
      uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
      uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
      uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));

      // BMP stores pixels as BGR
      row[x * 3 + 0] = b;
      row[x * 3 + 1] = g;
      row[x * 3 + 2] = r;
    }

    // Zero-fill any row padding bytes
    for (uint32_t pad = width * 3u; pad < row_size; pad++) {
      row[pad] = 0;
    }
  }

  this->bmp_size_ = file_size;
  ESP_LOGD(TAG, "Captured %ux%u screenshot (%u bytes)", width, height, file_size);
}

// ---------------------------------------------------------------------------
// handle_screenshot_()  –  runs in esp_http_server's task, NOT the main loop
// ---------------------------------------------------------------------------
esp_err_t LvglScreenshot::handle_screenshot_(httpd_req_t *req) {
  LvglScreenshot *self = instance_;
  if (!self || !self->bmp_buf_) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Component not ready");
    return ESP_FAIL;
  }

  // Only one capture at a time
  if (self->in_progress_) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Capture in progress, try again");
    return ESP_OK;
  }
  self->in_progress_ = true;

  // Ask the main loop to do the capture
  xSemaphoreGive(self->capture_requested_);

  // Wait up to 3 s for the main loop to finish (it runs at ~60 Hz so ~16 ms max wait)
  if (xSemaphoreTake(self->capture_done_, pdMS_TO_TICKS(3000)) != pdTRUE) {
    self->in_progress_ = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture timed out");
    return ESP_FAIL;
  }

  if (self->bmp_size_ == 0) {
    self->in_progress_ = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Framebuffer unavailable");
    return ESP_FAIL;
  }

  // Send headers
  httpd_resp_set_type(req, "image/bmp");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screenshot.bmp\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");

  // Stream the BMP in 4 KB chunks to avoid large single-shot writes
  const size_t CHUNK = 4096;
  size_t sent = 0;
  esp_err_t ret = ESP_OK;
  while (sent < self->bmp_size_) {
    size_t chunk_len = std::min(CHUNK, self->bmp_size_ - sent);
    ret = httpd_resp_send_chunk(req, (const char *) self->bmp_buf_ + sent, (ssize_t) chunk_len);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Failed to send chunk at offset %u", sent);
      break;
    }
    sent += chunk_len;
  }

  // Terminate chunked transfer
  httpd_resp_send_chunk(req, nullptr, 0);

  self->in_progress_ = false;
  return ret;
}

}  // namespace lvgl_screenshot
}  // namespace esphome

#endif  // USE_ESP_IDF
