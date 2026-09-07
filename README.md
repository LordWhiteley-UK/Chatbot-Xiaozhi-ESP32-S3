# xiaozhi-xiao-s3

Original ESP-IDF firmware for the Seeed **XIAO ESP32-S3** acting as an
AI voice chatbot client compatible with the **xiaozhi.me** backend —
implemented from the protocol specification documents (`docs/`), not from
the reference implementation's source.

- ESP-IDF **v6.0.2**, target `esp32s3`
- 8MB flash, 8MB **Octal PSRAM @ 80MHz**
- Console: **USB Serial/JTAG** (native USB, GPIO19/20)
- Audio: Opus 16 kHz mono, 60 ms frames (binary protocol **v1**, raw frames)
- Transport: **MQTT + UDP** (docs/mqtt-udp.md) — the WS gateway on
  api.tenclass.net currently closes every upgrade with code 1005, so the
  WebSocket backend exists but the device uses MQTT+UDP automatically
- Wake word: local **"Computer"** keyword spotting (esp-sr WakeNet9, English)
- WiFi: SoftAP + web-form provisioning on first boot, NVS storage
- MCP tools over the JSON-RPC 2.0 control channel

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

`idf.py flash` also writes `build/srmodels/srmodels.bin` (the wake-word
model, ~284 KB) to the `model` partition at 0x290000 — the esp-sr build
hooks this in automatically. The board on this Mac enumerated as
`/dev/cu.usbmodem212101` (verify with `ls /dev/cu.usb*`). If the port
drops after a crash or during flashing: hold **BOOT** while plugging in
USB, tap **RESET**, release **BOOT** (ROM bootloader mode).

## First boot flow

1. **WiFi provisioning** — device opens SoftAP `Xiaozhi-fd42cd` (name is
   MAC-derived); join it (no password) and open `http://192.168.4.1`, enter
   SSID/password. Device saves to NVS and reboots. Wrong stored credentials →
   back to provisioning mode automatically.
2. **Activation** — firmware POSTs identity to `CONFIG_OTA_URL`
   (default `https://api.tenclass.net/xiaozhi/ota/`). If the device is not
   yet bound, the response's activation code is shown large on the OLED —
   enter it at xiaozhi.me to bind. Firmware polls OTA until the transport
   credentials appear.
3. **Talk to it** — the MQTT session opens automatically and the device
   stands by on the OLED (`Ready — Say "Computer"`):
   - say **"Computer"** → ~1.2 s later (the word's tail is skipped) a
     listening round opens (state `Listening`); speak your question
   - the on-device VAD ends the round when you stop talking → `Thinking`
     on the OLED → the answer is spoken (state `Speaking`)
   - follow-ups: after each answer the device listens again directly for
     up to 30 s of activity — only after it lapses do you need the wake
     word again
   - press the on-board **BOOT** button while speaking = `abort`
     (barge-in) and straight back into listening; press in standby =
     start a round without the wake word

## Layout

| File | Purpose |
|---|---|
| `main/board.h` | fixed pin map (mic SD=1 SCK=44 WS=9, amp DIN=2 LRC=4 BCLK=7, OLED SDA=5 SCL=6) |
| `main/Kconfig.projbuild` | OTA URL, opus params, display orientation (this panel is mounted rotated 180°: both flip axes on) |
| `main/main.c` | app state machine: wake-word standby, listening rounds, auto-reconnect |
| `main/session.c` | transport dispatch + shared JSON protocol handling |
| `main/session_mqtt.c` | **MQTT+UDP transport** (docs/mqtt-udp.md): TLS MQTT control, AES-128-CTR Opus over UDP |
| `main/wake_word.c` | local "Computer" KWS via esp-sr wakenet, fed from the mic task |
| `main/opus_codec.c` | libopus (registry `78/esp-opus`) encode/decode |
| `main/audio.c` | I2S RX (mic, I2S0) and TX (amp, I2S1) — independent controllers; mic runs continuously for KWS |
| `main/display.c` | SSD1309 via esp_lcd SSD1306 panel driver + generated font (`font.h`: Monaco 12px, FreeType hinted monochrome, 15px line height — 4 lines on screen) |
| `main/wifi_prov.c` | SoftAP + web form provisioning, NVS |
| `main/ota.c` | OTA activation / endpoint discovery |
| `main/mcp_server.c` | MCP tools: `self.get_device_status`, `self.reboot` |
| `partitions.csv` | custom table: factory 2.5MB @0x10000, `model` 2MB @0x290000 |
| `docs/` | the three spec docs this was written against |

## Assumptions / interpretations to verify (flagged per project brief)

1. **OTA response schema is not covered by the three protocol docs.** Parsing
   in `ota.c` is deliberately lenient: `websocket.url`, `websocket.token`,
   `activation.code`, `activation.message`, `firmware.version` are looked up
   by name wherever they appear in the response JSON. Confirm the real
   response shape of your OTA endpoint.
2. **Opus uplink bitrate** is not specified in the docs — set to 24 kbps
   VBR (`CONFIG_OPUS_BITRATE` in menuconfig).
3. **Wake word** — the spec docs only describe a server-side `小智` gate;
   the local "Computer" engine (`CONFIG_SR_WN_WN9_COMPUTER_TTS`) matches the
   official English firmware build and was requested explicitly.
4. **MQTT+UDP details the docs don't pin down** (verified empirically against
   api.tenclass.net): broker port 8883 when the OTA endpoint carries no
   port; the server→device topic is `devices/p2p/<mac-with-underscores>`
   when OTA's `subscribe_topic` is literally `"null"`; the 16-byte audio
   header doubles as the AES-CTR IV. Conversation rounds use manual listen
   mode (`listen start`/`stop` per websocket.md §6) — the earlier
   `state:"detect"` arming made the server run its own wake-word pipeline
   on the uplink (every round transcribed as `小智`). In manual mode the
   server finalizes ASR only on `listen stop`, so end-of-speech is
   detected on-device by esp-sr's WebRTC VAD (see `audio.c`).
5. `Activation-Version: 1` request header on the OTA call is an
   interpretation; remove it if the endpoint rejects or ignores it.
6. Downlink audio is decoded at the rate announced in the server hello
   (16k or 24k per the docs) and the I2S TX clock is reconfigured to match;
   MAX98357A accepts 8–96 kHz.
7. MCP support is advertised (`features.mcp: true`) with a minimal tool set.

## Session protocol notes (from docs/mqtt-udp.md, docs/websocket.md)

- Handshake: MQTT `hello` with `version:3, transport:"udp"`, `features.mcp`,
  `audio_params {opus, 16000, 1, 60ms}`; server `hello` returns the
  session id, UDP endpoint and hex AES key+nonce. (The legacy WebSocket
  backend in `session.c` speaks the docs/websocket.md handshake with the
  same dispatch layer.)
- JSON `type` handled: `hello`, `stt`, `tts` (start/stop/sentence_start),
  `llm` (emotion), `mcp`, `system` (`reboot`), `alert`; malformed JSON
  missing `type` is logged and ignored.
- Downlink UDP packets arriving while a listening round is armed are
  dropped (half-duplex; same rule as websocket.md §4.2.9 binary frames).
- Known backend quirk: with each round the server re-registers our MCP
  tools and logs `Duplicate tool names ... / tools reached limit: 32` —
  an alert, not an error; rounds complete normally.
- Not implemented (optional per spec): binary protocol v2/v3 framing,
  server AEC/NR/AGC, glyph_push, `custom` messages. Wake-word `detect`
  events are not sent up (the wakenet model runs locally instead).