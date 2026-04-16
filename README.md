# esphome-lvgl-screenshot
 
## ESPHome LVGL 截图工具组件
- Fork自https://github.com/dcgrove/esphome-lvgl-screenshot
- 修复了原分支在P4上的部分界面截图输出花屏问题
- 针对esphome lvgl 9 适配，不支持旧版lvgl 8
- 全部为 AI Coding
## 使用要求
- 仅在ESP32-P4连接MIPI_DSI显示器上测试通过
- esphome ver >= 2026.4.0
- LVGL 100% buffer_size
- 帧缓冲拷贝占用PSRAM 2~3MB
- ## 为节省系统资源，建议只在调试或展示界面时使用此组件

## yaml配置

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/gasment/esphome-lvgl-screenshot
      ref: main
    components: [lvgl_screenshot]
    refresh: 24h

lvgl_screenshot:
  port: 8080
```

## 获取截图

访问P4的 http 端点
```
http://<device-ip>:8080/screenshot
```

