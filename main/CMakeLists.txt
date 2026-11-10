# 酷狗音乐播放器应用（feature/kugou-player），设备端直连酷狗，无中转服务。
#
# 只编译 kg_*.c 与通用中文字库 app_fonts.c。历史应用（米家 mijia_*、iPhone 通知
# app_*/main.c、上游 demo_*/ui_pixel*、旧版合成旋律 kugou_audio）已清理。
#
# chmorgan/esp-libhelix-mp3 由 main/idf_component.yml 声明，组件管理器自动
# 提供 mp3dec.h 与链接库；网络配置见 main/Kconfig.projbuild。
idf_component_register(
    SRCS "kugou_main.c"
         "kugou_model.c"
         "kugou_catalog.c"
         "kugou_proto.c"
         "kugou_sign.c"
         "kugou_keyboard.c"
         "kugou_wifi.c"
         "kugou_api.c"
         "kugou_stream.c"
         "kugou_ui.c"
         "app_fonts.c"
         "../assets/fonts/app_font_16.c"
         "../assets/fonts/app_font_20.c"
         "../assets/fonts/app_font_20_ext.c"
         "../assets/fonts/app_font_coverage.c"
    INCLUDE_DIRS "." "../assets/fonts"
    REQUIRES bsp esp_timer nvs_flash esp_wifi esp_netif esp_event
             esp_http_client esp-tls mbedtls json esp_hw_support lwip
)

# 联调免扫码登录开关（仅联调使用）：
#   取消下面这行的注释即可定义 KG_DEV_DEBUG_LOGIN，刷固件清空 NVS 后会用
#   kugou_api.c 里的 userid/token 自动注入登录态，省去反复扫码。
#   ⚠️ 真实刷机 / 正式发布构建务必保持注释状态，确保固件不含真实账号信息。
# target_compile_definitions(${COMPONENT_LIB} PRIVATE KG_DEV_DEBUG_LOGIN)
