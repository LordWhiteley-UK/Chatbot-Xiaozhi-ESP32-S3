# xiaozhi-xiao-s3

Original ESP-IDF firmware for the Seeed **XIAO ESP32-S3** acting as an
AI voice chatbot client compatible with the **xiaozhi.me** backend —
implemented from the protocol specification documents (`docs/`), not from
the reference implementation's source.

- ESP-IDF **v5.3.3**, target `esp32s3`
- 8MB flash, 8MB **Octal PSRAM @ 80MHz**
- Console: **USB Serial/JTAG** (native USB, GPIO19/20)
- Audio: Opus 16 kHz mono, 60 ms frames (binary protocol **v1**, raw frames)
- Transport: **MQTT + UDP** (docs/mqtt-udp.md) — TLS MQTT control channel,
  AES-128-CTR encrypted Opus audio over UDP
- Wake word: local **"Computer"** keyword spotting (esp-sr WakeNet9, English)
- **Animated face display** on the SSD1309 OLED (sleeping, listening, talking)
- **External push button** support (volume + face toggle + barge-in)
- **Conversation mode** — follow-up questions without the wake word
- **Software volume control** — 5 levels via button
- WiFi: SoftAP + web-form provisioning on first boot, NVS storage
- MCP tools over the JSON-RPC 2.0 control channel

## Build & flash

**Beginner?** See [FLASHING.md](FLASHING.md) — a hand-held walkthrough
covering both the browser flasher (prebuilt binaries, no install) and
flashing locally with VS Code or the command line.

```bash
source ~/esp/esp-idf/export.sh        # once per terminal session
cd ~/xiaozhi-xiao-s3
idf.py build                          # menuconfig options live in sdkconfig.defaults
idf.py -p /dev/cu.usbmodem212101 flash monitor
```

> **When `sdkconfig.defaults` is changed** (e.g. after a `git pull`), the
> existing `sdkconfig` file keeps its old baked-in values. Delete it so
> the new defaults apply:
> ```bash
> rm -f sdkconfig sdkconfig.old && idf.py fullclean && idf.py build
> ```

`idf.py flash` also writes `build/srmodels/srmodels.bin` (the wake-word
model, ~284 KB) to the `model` partition at 0x290000 — the esp-sr build
hooks this in automatically. The board on this Mac enumerated as
`/dev/cu.usbmodem212101` (verify with `ls /dev/cu.usb*`). If the port
drops after a crash or during flashing: hold **BOOT** while plugging in
USB, tap **RESET**, release **BOOT** (ROM bootloader mode).

> If you have more than one ESP32 board plugged in, `idf.py flash` may
> target the wrong one. Unplug the other board, or use `-p /dev/cu.usbmodemXXXX`
> to specify the correct port explicitly.

## Button controls

Both the on-board **BOOT** button (GPIO0) and the optional **external
push button** (GPIO3 / D2 pad) share the same multi-press logic:

| Gesture | Action |
|---|---|
| **1 press** | Wake / barge-in (start listening, or stop the current answer) |
| **2 presses** | Volume up |
| **3 presses** | Volume down |
| **Long press** (>1 second) | Toggle face / text display mode |

Volume levels: 20%, 40%, 60%, 80%, 100% (default 40%). In text mode the
level is shown briefly on the OLED; in face mode it adjusts silently.

### Adding an external push button

Wire a momentary push button between the **D2 pad** (GPIO3) and **GND**.
No external resistor needed — the internal pull-up is enabled in code.
GPIO3 is the only unused broken-out pad on the XIAO ESP32-S3 with this
wiring (D0=mic, D1=amp, D3=amp, D4/D5=OLED, D6=amp, D7=mic, D10=mic).
To use a different pad, change `BOARD_BUTTON_EXT` in `main/board.h`.

## Animated face display

Long-press either button to toggle between the text status display and
the animated face. The face has four states:

| State | Appearance |
|---|---|
| **Sleeping** (idle, waiting for wake word) | Closed eyes, relaxed brows, gentle smile, floating "z z" marks |
| **Listening** (awake after wake word) | Wide elliptical eyes, raised angled eyebrows, "o" mouth, occasional blinks |
| **Thinking** (connecting to server) | Eyes open with pupils glancing side to side, asymmetric eyebrows |
| **Talking** (TTS playback) | Eyes open, mouth ellipse opens/closes in sync with real audio amplitude |

The mouth only moves when audio is actually playing — it tracks the peak
PCM amplitude of the I2S output, so it closes during pauses between words
and opens wider for louder speech.

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
   stands by on the OLED:
   - say **"Computer"** → ~1.2 s later (the word's tail is skipped) a
     listening round opens; speak your question
   - the server's VAD ends the round when you stop talking → the answer
     is spoken (state `Speaking`)
   - **Conversation mode**: after each answer the device listens again
     directly for **10 seconds** — ask follow-up questions without the
     wake word. After 10 s of silence it goes back to sleep.
   - press the button **once** while speaking = barge-in (stops the
     answer and starts listening); press in standby = start a round
     without the wake word
   - **double-press** = volume up, **triple-press** = volume down
   - **long-press** = toggle face / text display

## Layout

| File | Purpose |
|---|---|
| `main/board.h` | fixed pin map (mic SD=1 SCK=44 WS=9, amp DIN=2 LRC=4 BCLK=7, OLED SDA=5 SCL=6, ext button=3) |
| `main/Kconfig.projbuild` | OTA URL, opus params, display orientation (this panel is mounted rotated 180°) |
| `main/main.c` | app state machine: wake-word standby, listening rounds, conversation mode, TTS finishing, button multi-press, volume control |
| `main/session_mqtt.c` | **MQTT+UDP transport** (docs/mqtt-udp.md): TLS MQTT control, AES-128-CTR Opus over UDP, unconnected socket for downlink |
| `main/wake_word.c` | local "Computer" KWS via esp-sr wakenet, with echo muzzle period after re-arming |
| `main/opus_codec.c` | libopus (registry `78/esp-opus`) encode/decode |
| `main/audio.c` | I2S RX (mic, I2S0) and TX (amp, I2S1); ring-buffer playback, amplitude tracking for face, software volume scaling |
| `main/face_display.c` | animated face on SSD1309: elliptical eyes, angled eyebrows, amplitude-synced mouth, sleeping z's |
| `main/face_display.h` | face display public API |
| `main/display.c` | SSD1309 via esp_lcd SSD1306 panel driver + generated font (`font.h`: Monaco 12px, 15px line height) |
| `main/wifi_prov.c` | SoftAP + web form provisioning, NVS |
| `main/ota.c` | OTA activation / endpoint discovery |
| `main/mcp_server.c` | MCP tools: `self.get_device_status`, `self.reboot` |
| `partitions.csv` | custom table: factory 2.5MB @0x10000, `model` 2MB @0x290000 |
| `docs/` | the three spec docs this was written against |

## Key configuration (sdkconfig.defaults)

| Setting | Value | Reason |
|---|---|---|
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 4096 | Large allocs (task stacks, ring buffers) go to PSRAM |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | 65536 | Keep 64KB internal RAM for DMA/WiFi; push big allocs to PSRAM |
| `CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC` | y | TLS buffers can use PSRAM (internal RAM too fragmented mid-session) |
| `CONFIG_MBEDTLS_DYNAMIC_BUFFER` | y | Allocate TLS record buffers per-operation, not statically |
| `CONFIG_ESP_TLS_INSECURE` | y | MQTT broker root CA not in ESP cert bundle; skip verify (encryption intact) |
| `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` | y | Same — OTA HTTPS still uses full cert-bundle verification |
| `CONFIG_SR_WN_WN9_COMPUTER_TTS` | y | "Computer" wake word model (WakeNet9, English) |

## Session protocol notes (from docs/mqtt-udp.md, docs/websocket.md)

- Handshake: MQTT `hello` with `version:3, transport:"udp", language:"en"`,
  `features.mcp`, `audio_params {opus, 16000, 1, 60ms}`; server `hello` returns
  the session id, UDP endpoint and hex AES key+nonce.
- **AUTO listen mode**: the device sends `listen start` with `mode:"auto"` and
  the server's VAD detects end-of-speech. The device never sends `listen stop`
  (matching the reference firmware for non-AEC devices).
- **Unconnected UDP socket**: the socket uses `sendto()`/`recvfrom()` (not
  `connect()`), so downlink audio from any server media port is accepted. A
  connected socket silently drops datagrams whose source IP:port differs from
  the hello's advertised endpoint.
- **TTS finishing**: after `tts.stop`, UDP audio may still be in transit
  (MQTT/TCP can overtake UDP). The device keeps accepting audio, waits for
  3 s of ring-buffer silence, then enters conversation mode or standby.
- **Echo protection**: the wake word is disarmed at `tts.start` and re-armed
  1.5 s after playback ends, plus a 600 ms "muzzle" period that discards
  audio to prevent the device's own TTS echo from re-triggering "Computer".
- JSON `type` handled: `hello`, `stt`, `tts` (start/stop/sentence_start),
  `llm` (emotion), `mcp`, `system` (`reboot`), `alert`.
- Not implemented: binary protocol v2/v3 framing, server AEC/NR/AGC,
  glyph_push, `custom` messages.