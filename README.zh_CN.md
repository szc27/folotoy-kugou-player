# 酷狗音乐随身播放器（Kugou Music Player）

基于 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport)（ESP32-C3）开发的酷狗音乐随身播放器。设备端**直连酷狗接口**，不经过任何中转服务，即可完成配网、扫码登录、歌单浏览与在线播放。

> 本项目是 FoloToy AI Passport 的派生作品，遵循原项目的 MIT 许可（见 [LICENSE](LICENSE)）。

## 功能特性

| 功能 | 说明 |
| --- | --- |
| WiFi 配网 | 首次开机扫描周边网络，用三键屏幕键盘输入密码；凭证存入 NVS，不进固件 |
| 扫码登录 | 屏幕显示酷狗登录二维码，手机扫码授权；登录态持久化，重启免登录 |
| 歌单浏览 | 展示「我喜欢的音乐」等歌单与曲目，支持分页翻页 |
| 在线播放 | 通过酷狗 `/v5/url` 签名接口换取 mp3 直链，Helix MP3 解码后经 ES8311 输出 |

## 硬件要求

- FoloToy AI Passport 开发板（ESP32-C3，8 MB Flash）
- 240×320 LCD、ES8311 音频、实体按键（板载）

## 目录结构

```
.
├── main/                 # 应用源码（kugou_*.c/h 直连酷狗协议与 UI）
│   ├── kugou_api.c       # 酷狗 API 客户端（register_dev / 登录 / v5url）
│   ├── kugou_sign.c      # 酷狗签名算法（md5 / register 签名 / android signature）
│   ├── kugou_proto.c     # JSON 协议解析
│   ├── kugou_model.c     # 播放器状态机（页面导航 / 播放控制）
│   ├── kugou_catalog.c   # 歌单与曲目目录
│   ├── kugou_stream.c    # 音频流拉取 + Helix MP3 解码
│   ├── kugou_ui.c        # LVGL 界面
│   ├── kugou_wifi.c      # WiFi 配网
│   └── kugou_keyboard.c  # 三键屏幕键盘
├── components/bsp/       # 板级支持包（显示 / 音频 / 按键 / 电池 / I2C）
├── assets/fonts/         # 中文字库（含 Noto 源字体与生成脚本产物）
├── tests/                # 主机单元测试（签名 / 状态机 / 协议 / 键盘）
├── tools/                # 字体生成与调试工具
├── release/              # v1.0.0 交付物（固件 bin + 发布说明）
└── sdkconfig.defaults    # 关键配置（LVGL 内存 / 控制台引脚 / WiFi / mbedTLS）
```

## 编译

需要 ESP-IDF **v5.5.3**（其他版本未验证）。

```bash
# 首次构建：设置目标芯片（生成 sdkconfig）
idf.py set-target esp32c3

# 编译（依赖组件会自动从 Espressif 组件仓库下载）
idf.py build
```

产物位于 `build/kugou-player.bin`（或合并镜像 `idf.py merge-bin`）。

### 可选配置（menuconfig）

`Kugou Music Player` 菜单提供：

- `Wi-Fi SSID / Passphrase`：预置 WiFi 凭证（留空则开机扫描 + 屏幕键盘输入）
- `QR login page URL`：扫码登录页模板（默认酷狗官方 H5 页面）

## 刷机

```bash
idf.py -p <PORT> flash
```

或直接刷写 `release/v1.0.0/` 中的预编译固件 `kugou-player-v1.0.0.bin`（写入偏移 `0x0`）。

## 运行测试

酷狗核心逻辑的单元测试不依赖 ESP-IDF / LVGL，可用本机 gcc 直接编译运行：

```bash
# 签名测试
gcc tests/test_kugou_sign.c main/kugou_sign.c -o /tmp/test_sign && /tmp/test_sign
# 状态机测试
gcc tests/test_kugou_model.c main/kugou_model.c main/kugou_catalog.c -o /tmp/test_model && /tmp/test_model
# 协议测试
gcc tests/test_kugou_proto.c main/kugou_proto.c -o /tmp/test_proto && /tmp/test_proto
# 键盘测试
gcc tests/test_kugou_keyboard.c main/kugou_keyboard.c -o /tmp/test_kbd && /tmp/test_kbd
```

## 关于免扫码登录（仅联调）

`main/CMakeLists.txt` 末尾保留了 `KG_DEV_DEBUG_LOGIN` 编译宏，用于联调时免扫码注入登录态。**真实刷机 / 正式发布构建务必保持该宏关闭**，确保固件中不含任何账号信息。

## 许可

[MIT](LICENSE)，版权归 FoloToy 及原作者所有。本项目对酷狗接口的使用仅供学习研究，请遵守酷狗音乐的服务条款与相关法律法规。
