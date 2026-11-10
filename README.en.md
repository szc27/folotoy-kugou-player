# Kugou Music Player

[中文](README.md)

A portable Kugou Music player built on the [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) (ESP32-C3). The device talks to Kugou's APIs **directly** — no relay service involved — supporting Wi-Fi provisioning, QR-code login, playlist browsing, and online playback.

> This project is a derivative of FoloToy AI Passport and keeps its MIT license (see [LICENSE](LICENSE)).

## Features

| Feature | Description |
| --- | --- |
| Wi-Fi provisioning | Scans networks on first boot and offers a three-button on-screen keyboard; credentials live in NVS, never in firmware |
| QR-code login | Renders a Kugou login QR code on screen; session persists across reboots |
| Playlist browsing | Lists playlists (e.g. "My Favorites") and tracks with pagination |
| Online playback | Fetches the mp3 URL via Kugou's signed `/v5/url` endpoint, decodes with Helix MP3, outputs over ES8311 |

## Hardware

- FoloToy AI Passport board (ESP32-C3, 8 MB Flash)
- 240×320 LCD, ES8311 audio, on-board buttons

## Layout

```
.
├── main/                 # App sources (kugou_*.c/h: Kugou protocol + UI)
│   ├── kugou_api.c       # Kugou API client (register_dev / login / v5url)
│   ├── kugou_sign.c      # Kugou signing (md5 / register / android signature)
│   ├── kugou_proto.c     # JSON protocol parsing
│   ├── kugou_model.c     # Player state machine (navigation / playback)
│   ├── kugou_catalog.c   # Playlist & track catalog
│   ├── kugou_stream.c    # Audio streaming + Helix MP3 decoding
│   ├── kugou_ui.c        # LVGL UI
│   ├── kugou_wifi.c      # Wi-Fi provisioning
│   └── kugou_keyboard.c  # Three-button on-screen keyboard
├── components/bsp/       # Board support (display / audio / buttons / battery / I2C)
├── assets/fonts/         # CJK fonts (source font + generated artifacts)
├── tests/                # Host unit tests (signing / state machine / proto / keyboard)
├── tools/                # Font generation & debugging utilities
├── release/              # v1.0.0 artifacts (firmware bin + release notes)
└── sdkconfig.defaults    # Key configs (LVGL memory / console pins / Wi-Fi / mbedTLS)
```

## Build

Requires ESP-IDF **v5.5.3** (other versions untested).

```bash
# First build: select the target chip (generates sdkconfig)
idf.py set-target esp32c3

# Build (component dependencies are fetched automatically)
idf.py build
```

The artifact is `build/kugou-player.bin` (or use `idf.py merge-bin`).

### Optional config (menuconfig)

The `Kugou Music Player` menu provides:

- `Wi-Fi SSID / Passphrase`: preset credentials (leave empty for scan + on-screen keyboard)
- `QR login page URL`: login QR template (defaults to Kugou's official H5 page)

## Flash

```bash
idf.py -p <PORT> flash
```

Or flash the prebuilt firmware `release/v1.0.0/kugou-player-v1.0.0.bin` at offset `0x0`.

## Run tests

Unit tests for the core logic need no ESP-IDF / LVGL and compile with a host gcc:

```bash
gcc tests/test_kugou_sign.c     main/kugou_sign.c     -o /tmp/test_sign && /tmp/test_sign
gcc tests/test_kugou_model.c    main/kugou_model.c    main/kugou_catalog.c -o /tmp/test_model && /tmp/test_model
gcc tests/test_kugou_proto.c    main/kugou_proto.c    -o /tmp/test_proto && /tmp/test_proto
gcc tests/test_kugou_keyboard.c main/kugou_keyboard.c -o /tmp/test_kbd  && /tmp/test_kbd
```

## Dev-only login bypass

`main/CMakeLists.txt` keeps a `KG_DEV_DEBUG_LOGIN` compile flag for skip-login injection during development. **Keep it disabled for real flashing / release builds** so the firmware contains no account credentials.

## License

[MIT](LICENSE), © FoloToy and original authors. Kugou API usage here is for learning/research only — please comply with Kugou's terms of service and applicable laws.
