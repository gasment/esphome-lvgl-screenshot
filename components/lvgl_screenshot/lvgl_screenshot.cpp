#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "lvgl_screenshot.h"

#ifdef USE_ESP_IDF

#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <cstring>

// LVGL v9 私有头 — 访问 flush_cb / color_format / buf_act
#include "src/display/lv_display_private.h"
#include "src/draw/lv_draw_buf_private.h"

namespace esphome {
namespace lvgl_screenshot {

static const char *const TAG = "lvgl_screenshot";
LvglScreenshot *LvglScreenshot::instance_ = nullptr;

// -----------------------------------------------------------------
//  JPEG 编码回调
// -----------------------------------------------------------------
struct JpegWriteCtx {
  uint8_t *buf;
  size_t capacity;
  size_t size;
};

void LvglScreenshot::jpeg_write_cb_(void *ctx, void *data, int size) {
  auto *c = static_cast<JpegWriteCtx *>(ctx);
  if (size <= 0 || !data) return;
  size_t copy = std::min(static_cast<size_t>(size), c->capacity - c->size);
  std::memcpy(c->buf + c->size, data, copy);
  c->size += copy;
}

// -----------------------------------------------------------------
//  setup()
// -----------------------------------------------------------------
void LvglScreenshot::setup() {
  instance_ = this;

  capture_requested_ = xSemaphoreCreateBinary();
  capture_done_      = xSemaphoreCreateBinary();
  if (!capture_requested_ || !capture_done_) {
    ESP_LOGE(TAG, "Semaphore creation failed");
    mark_failed();
    return;
  }

  lv_disp_t *disp = lv_disp_get_default();
  if (!disp) {
    ESP_LOGE(TAG, "No LVGL display");
    mark_failed();
    return;
  }

  width_  = (uint32_t) lv_disp_get_hor_res(disp);
  height_ = (uint32_t) lv_disp_get_ver_res(disp);

  size_t rgb_size = width_ * height_ * 3u;
  rgb_buf_ = (uint8_t *) heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rgb_buf_) {
    ESP_LOGE(TAG, "RGB buf alloc failed (%u B)", (unsigned) rgb_size);
    mark_failed();
    return;
  }

  jpeg_capacity_ = rgb_size * 6 / 10;
  jpeg_buf_ = (uint8_t *) heap_caps_malloc(jpeg_capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!jpeg_buf_) {
    ESP_LOGE(TAG, "JPEG buf alloc failed (%u B)", (unsigned) jpeg_capacity_);
    mark_failed();
    return;
  }

  jpeg_size_ = 0;
  start_server_();
  ESP_LOGI(TAG, "Ready — http://<ip>:%u/screenshot (%ux%u)", port_, width_, height_);
}

// -----------------------------------------------------------------
//  start_server_()
// -----------------------------------------------------------------
void LvglScreenshot::start_server_() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port_;
  cfg.stack_size  = 8192;
  cfg.ctrl_port   = (uint16_t)(port_ + 1);

  if (httpd_start(&server_, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server failed on port %u", port_);
    server_ = nullptr;
    return;
  }

  httpd_uri_t uri = {
    .uri      = "/screenshot",
    .method   = HTTP_GET,
    .handler  = handle_screenshot_,
    .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server_, &uri);
}

// -----------------------------------------------------------------
//  loop() — LVGL 主线程
// -----------------------------------------------------------------
void LvglScreenshot::loop() {
  if (xSemaphoreTake(capture_requested_, 0) == pdTRUE) {
    do_capture_();
    xSemaphoreGive(capture_done_);
  }
}

// =================================================================
//  capture_flush_cb_() — 替代的 flush 回调
//
//  在 LVGL 完成整帧渲染后被调用，px_map 指向已渲染好的
//  完整帧数据。我们在此拷贝像素，然后放行给原始 flush_cb。
// =================================================================
void LvglScreenshot::capture_flush_cb_(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  auto *self = instance_;

  int32_t area_w = lv_area_get_width(area);
  int32_t area_h = lv_area_get_height(area);
  int32_t x_off  = area->x1;
  int32_t y_off  = area->y1;

  // 获取源缓冲区的 stride（每行字节数，含对齐填充）
  uint32_t src_stride = 0;
  if (disp->buf_act) {
    src_stride = disp->buf_act->header.stride;
  }

  lv_color_format_t cf = disp->color_format;
  uint32_t dst_w = self->width_;

  // 根据颜色格式转换到 RGB888
  for (int32_t y = 0; y < area_h; y++) {
    int32_t dst_y = y_off + y;
    if (dst_y < 0 || dst_y >= (int32_t) self->height_) continue;

    uint8_t *dst_row = self->rgb_buf_ + dst_y * dst_w * 3;

    for (int32_t x = 0; x < area_w; x++) {
      int32_t dst_x = x_off + x;
      if (dst_x < 0 || dst_x >= (int32_t) self->width_) continue;

      uint8_t *out = dst_row + dst_x * 3;

      if (cf == LV_COLOR_FORMAT_RGB565) {
        uint8_t *src_row = px_map + y * src_stride;
        uint16_t raw = ((uint16_t *) src_row)[x];
        uint8_t r5 = (raw >> 11) & 0x1F;
        uint8_t g6 = (raw >> 5)  & 0x3F;
        uint8_t b5 =  raw        & 0x1F;
        out[0] = (r5 << 3) | (r5 >> 2);
        out[1] = (g6 << 2) | (g6 >> 4);
        out[2] = (b5 << 3) | (b5 >> 2);

      } else if (cf == LV_COLOR_FORMAT_RGB888) {
        uint8_t *src_row = px_map + y * src_stride;
        // LVGL v9 RGB888 内存顺序: B G R
        out[0] = src_row[x * 3 + 2]; // R
        out[1] = src_row[x * 3 + 1]; // G
        out[2] = src_row[x * 3 + 0]; // B

      } else if (cf == LV_COLOR_FORMAT_XRGB8888 || cf == LV_COLOR_FORMAT_ARGB8888) {
        uint8_t *src_row = px_map + y * src_stride;
        // 内存顺序: B G R A/X (小端)
        out[0] = src_row[x * 4 + 2]; // R
        out[1] = src_row[x * 4 + 1]; // G
        out[2] = src_row[x * 4 + 0]; // B

      } else {
        // 未知格式，填灰色
        out[0] = out[1] = out[2] = 128;
      }
    }
  }

  self->flush_captured_ = true;

  // ★ 放行：调用原始 flush 回调，让显示驱动正常工作
  if (self->orig_flush_cb_) {
    self->orig_flush_cb_(disp, area, px_map);
  } else {
    lv_display_flush_ready(disp);
  }
}

// =================================================================
//  do_capture_() — 核心截图逻辑
//
//  步骤:
//    1. 保存原始 flush_cb
//    2. 替换为 capture_flush_cb_
//    3. 强制全屏失效 + 立即刷新
//    4. capture_flush_cb_ 中拷贝像素
//    5. 恢复原始 flush_cb
//    6. 编码 JPEG
// =================================================================
void LvglScreenshot::do_capture_() {
  lv_disp_t *disp = lv_disp_get_default();
  if (!disp) {
    ESP_LOGE(TAG, "No display");
    jpeg_size_ = 0;
    return;
  }

  lv_obj_t *scr = lv_scr_act();
  if (!scr) {
    ESP_LOGE(TAG, "No active screen");
    jpeg_size_ = 0;
    return;
  }

  // 清零 RGB 缓冲区（防止部分刷新时有残留）
  std::memset(rgb_buf_, 0, width_ * height_ * 3u);

  // ---- 1. 保存原始 flush 回调 ----
  orig_flush_cb_  = disp->flush_cb;
  flush_captured_ = false;

  // ---- 2. 替换为我们的拦截回调 ----
  disp->flush_cb = capture_flush_cb_;

  // ---- 3. 强制全屏失效 + 立即重绘 ----
  lv_obj_invalidate(scr);
  lv_refr_now(disp);

  // ---- 4. 恢复原始 flush 回调 ----
  disp->flush_cb  = orig_flush_cb_;
  orig_flush_cb_  = nullptr;

  // ---- 5. 检查是否成功截获 ----
  if (!flush_captured_) {
    ESP_LOGE(TAG, "Flush callback was not invoked — capture failed");
    jpeg_size_ = 0;
    return;
  }

  // ---- 6. RGB888 → JPEG ----
  JpegWriteCtx ctx{jpeg_buf_, jpeg_capacity_, 0};
  int ok = stbi_write_jpg_to_func(jpeg_write_cb_, &ctx,
                                  (int) width_, (int) height_,
                                  3, rgb_buf_, 80);
  if (!ok || ctx.size == 0) {
    ESP_LOGE(TAG, "JPEG encode failed");
    jpeg_size_ = 0;
    return;
  }

  jpeg_size_ = ctx.size;
  ESP_LOGD(TAG, "Snapshot %ux%u → %u bytes JPEG", width_, height_, (unsigned) jpeg_size_);
}

// -----------------------------------------------------------------
//  handle_screenshot_() — httpd 线程
// -----------------------------------------------------------------
esp_err_t LvglScreenshot::handle_screenshot_(httpd_req_t *req) {
  auto *self = instance_;
  if (!self || !self->jpeg_buf_) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not ready");
    return ESP_FAIL;
  }
  if (self->in_progress_) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Busy");
    return ESP_OK;
  }
  self->in_progress_ = true;

  xSemaphoreGive(self->capture_requested_);
  if (xSemaphoreTake(self->capture_done_, pdMS_TO_TICKS(5000)) != pdTRUE) {
    self->in_progress_ = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Timeout");
    return ESP_FAIL;
  }
  if (self->jpeg_size_ == 0) {
    self->in_progress_ = false;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture failed");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  constexpr size_t CHUNK = 4096;
  size_t sent = 0;
  esp_err_t ret = ESP_OK;
  while (sent < self->jpeg_size_) {
    size_t len = std::min(CHUNK, self->jpeg_size_ - sent);
    ret = httpd_resp_send_chunk(req, (const char *)(self->jpeg_buf_ + sent), (ssize_t) len);
    if (ret != ESP_OK) break;
    sent += len;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  self->in_progress_ = false;
  return ret;
}

}  // namespace lvgl_screenshot
}  // namespace esphome

#endif
