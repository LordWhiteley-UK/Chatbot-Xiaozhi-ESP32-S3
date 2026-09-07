# xiaozhi-xiao-s3

Original ESP-IDF firmware for the Seeed **XIAO ESP32-S3** acting as an
AI voice chatbot client compatible with the **xiaozhi.me** backend —
implemented from the protocol specification documents (`docs/`), not from
the reference implementation's source.

- ESP-IDF **v6.0.2**, target `esp32s3`
- 8MB flash, 8MB **Octal PSRAM @ 80MHz**
- Console: **USB Serial/JTAG** (native USB, GPIO19/20)
- Audio: Opus 16 kHz mono, 60 ms frames (binary protocol **v1**, raw frames)
- WiFi: SoftAP + web-form provisioning on first boot, NVS storage
- MCP tools over the WebSocket JSON-RPC 2.0 channel (stretch goal, included)

## Build & flash

**Beginner?** See [FLASHING.md](FLASHING.md) — a hand-held walkthrough
covering both the browser flasher (prebuilt binaries in `flash/`, no
install) and flashing locally with VS Code or the command line.

```bash
. ~/esp/esp-idf-v6.0.2/export.sh      # once per terminal session
cd ~/xiaozhi-xiao-s3
idf.py build                          # menuconfig options live in sdkconfig.defaults
idf.py -p /dev/cu.usbmodem212101 flash monitor
```

The board on this Mac enumerated as `/dev/cu.usbmodem212101` (verify with
`ls /dev/cu.usb*`). If the port drops after a crash or during flashing:
hold **BOOT** while plugging in USB, tap **RESET**, release **BOOT** (ROM
bootloader mode).

## First boot flow

1. **WiFi provisioning** — device opens SoftAP `Xiaozhi-fd42cd` (name is
   MAC-derived); join it (no password) and open `http://192.168.4.1`, enter
   SSID/password. Device saves to NVS and reboots. Wrong stored credentials →
   back to provisioning mode automatically.
2. **Activation** — firmware POSTs identity to `CONFIG_OTA_URL`
   (default `https://api.tenclass.net/xiaozhi/ota/`). If the device is not
   yet bound, the response's activation code is shown large on the OLED —
   enter it at xiaozhi.me to bind. Firmware polls OTA until the WebSocket
   URL appears.
3. **Push to talk** — the on-board **BOOT button** is the only trigger (this
   wiring has no external button and there is no wake-word engine):
   - hold = connect + send `listen start` + stream mic Opus
   - release = `listen stop`; TTS then plays through the amp
   - press while speaking = `abort` (barge-in) and back to listening

## Layout

| File | Purpose |
|---|---|
| `main/board.h` | fixed pin map (mic SD=1 SCK=44 WS=9, amp DIN=2 LRC=4 BCLK=7, OLED SDA=5 SCL=6) |
| `main/Kconfig.projbuild` | OTA URL, opus params, display orientation (this panel is mounted rotated 180°: both flip axes on) |
| `main/main.c` | app state machine (manual mode per websocket.md §6.4), BOOT button |
| `main/session.c` | WebSocket protocol: hello handshake, headers, JSON dispatch, binary frames |
| `main/opus_codec.c` | libopus (registry `78/esp-opus`) encode/decode |
| `main/audio.c` | I2S RX (mic, I2S0) and TX (amp, I2S1) — independent controllers |
| `main/display.c` | SSD1309 via esp_lcd SSD1306 panel driver + generated font (`font.h`: Monaco 12px, FreeType hinted monochrome, 15px line height — 4 lines on screen) |
| `main/wifi_prov.c` | SoftAP + web form provisioning, NVS |
| `main/ota.c` | OTA activation / endpoint discovery |
| `main/mcp_server.c` | MCP tools: `self.get_device_status`, `self.reboot` |
| `docs/` | the three spec docs this was written against |

## Assumptions / interpretations to verify (flagged per project brief)

1. **OTA response schema is not covered by the three protocol docs.** Parsing
   in `ota.c` is deliberately lenient: `websocket.url`, `websocket.token`,
   `activation.code`, `activation.message`, `firmware.version` are looked up
   by name wherever they appear in the response JSON. Confirm the real
   response shape of your OTA endpoint.
2. **Opus uplink bitrate** is not specified in the docs — set to 24 kbps
   VBR (`CONFIG_OPUS_BITRATE` in menuconfig).
3. **Push-to-talk trigger** uses the on-board BOOT button (GPIO0) because the
   external wiring includes no button and there is no wake-word engine.
4. `Activation-Version: 1` request header on the OTA call is an
   interpretation; remove it if the endpoint rejects or ignores it.
5. Downlink audio is decoded at the rate announced in the server hello
   (16k or 24k per the docs) and the I2S TX clock is reconfigured to match;
   MAX98357A accepts 8–96 kHz.
6. MCP support is advertised (`features.mcp: true`) with a minimal tool set.

## Session protocol notes (from docs/websocket.md)

- Handshake: `hello` with `audio_params {opus, 16000, 1, 60ms}`; waits for
  server `hello` (transport `websocket`), stores `session_id`, honors the
  server's announced `sample_rate` for the decoder.
- Headers on connect: `Authorization: Bearer <token>`, `Protocol-Version: 1`,
  `Device-Id: <MAC>`, `Client-Id: <NVS-persisted UUIDv4>`.
- JSON `type` handled: `hello`, `stt`, `tts` (start/stop/sentence_start),
  `llm` (emotion), `mcp`, `system` (`reboot`), `alert`; malformed JSON
  missing `type` is logged and ignored.
- Binary downlink frames arriving while listening are dropped (spec §4.2.9).
- Not implemented (optional per spec): binary protocol v2/v3 framing,
  server AEC/NR/AGC, wake-word `detect`, glyph_push, `custom` messages.