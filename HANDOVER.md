# HANDOVER — xiaozhi-xiao-s3 voice chatbot (state as of 2026-09-07)

This document is a complete handover for another AI (or engineer) taking
over debugging of this project. It explains what was built, what is
verified working, what is broken, and everything that was already tried.

---

## 1. Project

Original ESP-IDF firmware for a **Seeed XIAO ESP32-S3** acting as an AI
voice-chat client for the **xiaozhi.me** backend. Written from the three
protocol spec documents in `docs/` (`websocket.md`, `mqtt-udp.md`,
`mcp-protocol.md`) — **NOT** from the reference implementation's source.

**HARD CONSTRAINT (verbatim from the project brief):**
> Do not view, clone, copy, or closely paraphrase source code from
> `https://github.com/78/xiaozhi-esp32` (or any fork/mirror of it). Do not
> fetch its `.cc`/`.h`/`.py` implementation files.

If the spec docs don't cover a behavior, the rule was to ask rather than
infer from the reference repo. Several empirical findings below came from
probing the live backend instead.

- Repo: `https://github.com/LordWhiteley-UK/Chatbot-Xiaozhi-ESP32-S3`
  (all work pushed, commit `a5b0e42` + later local commits)
- Device MAC: `E0:72:A1:FD:42:CC`, bound to the user's xiaozhi.me account
- ESP-IDF **v6.0.2** at `~/esp/esp-idf-v6.0.2/export.sh` (macOS)
- Serial port: `/dev/cu.usbmodem212101` (USB Serial/JTAG; drops after
  crashes — hold BOOT while plugging in to recover)
- **Known shell gotcha:** the Bash tool's cwd persists and can sit in
  `/private/tmp`, making `idf.py` fail with "CMakeLists.txt not found".
  ALWAYS prefix: `cd /Users/jps/xiaozhi-xiao-s3 && idf.py ...`

## 2. Hardware (fixed pin table — do not "fix" these)

| Function | Part | Pins |
|---|---|---|
| Mic | INMP441 (I2S0 RX, 32-bit stereo, left slot) | SD=GPIO1, SCK=GPIO44, WS=GPIO9 |
| Amp | MAX98357A (I2S1 TX) | DIN=GPIO2, LRC=GPIO4, BCLK=GPIO7 |
| OLED | SSD1309 via esp_lcd SSD1306 driver (rotated 180°) | SDA=GPIO5, SCL=GPIO6 |
| USB | untouchable | GPIO19/20 |
| Button | on-board BOOT (GPIO0) | optional trigger / barge-in |

8MB flash, 8MB Octal PSRAM @80MHz. Console must be USB Serial/JTAG
(GPIO43/44 are taken by the mic). Partition table `partitions.csv`:
factory 2.5MB @0x10000, `model` 2MB @0x290000 (esp-sr wake-word model).

## 3. Architecture / files

```
main/main.c           app state machine: wake-word standby -> listening
                      rounds (manual mode) -> speaking -> conversation
                      window (30s auto-relisten) -> standby. Round arming
                      is delayed 1.2s after wake (see problem A).
main/session.c        transport dispatch + shared JSON protocol handling
main/session_mqtt.c   MQTT+UDP transport (the working one). TLS MQTT via
                      espressif/mqtt; AES-128-CTR Opus audio over UDP.
main/wake_word.c      local "Computer" KWS, esp-sr wn9_computer_tts,
                      fed every 960-sample mic frame from the mic task
main/audio.c          I2S RX/TX; mic runs continuously (KWS always fed);
                      uplink encode gated by s_mic_running + 1.2s warmup
main/wifi_prov.c      SoftAP provisioning (+/scan endpoint; fixed)
main/ota.c            OTA activation (api.tenclass.net/xiaozhi/ota/)
main/mcp_server.c     device MCP tools (get_device_status, reboot)
partitions.csv        custom table incl. `model` partition
```

## 4. Protocol findings (verified empirically against api.tenclass.net)

The WebSocket gateway (`wss://api.tenclass.net/xiaozhi/v1/`) is **dead** —
it CLOSEs every upgrade with code 1005 for every client. Do not chase it.
Sessions work via **MQTT+UDP** (docs/mqtt-udp.md):

- Broker `mqtts://api.tenclass.net:8883`, crt_bundle verification.
- Client id = OTA `mqtt.client_id` (includes device MAC + UUID).
- Server→device topic: `devices/p2p/<mac-with-colons-becoming-underscores>`
  (e.g. `e0_72_a1_fd_42_cc`). OTA's `subscribe_topic` is literally "null".
- Hello: `{"type":"hello","version":3,"transport":"udp","features":{"mcp":true},
  "audio_params":{"format":"opus","sample_rate":16000,"channels":1,
  "frame_duration":60}}`. Server hello returns session_id, UDP endpoint,
  hex AES key + nonce; downlink is 24000 Hz.
- Audio: 16-byte header doubles as the AES-CTR IV (payload_len BE16 @2,
  timestamp BE32 @8, sequence BE32 @12; ssrc = nonce bytes 4–8).
- **CRITICAL (the current bug area):** `{"type":"listen","state":"detect"}`
  is a *wake-word notification*, NOT a round trigger. Using it makes the
  server run its own wake-word pipeline on the uplink and it reports
  `STT: 小智` every round (its own Chinese wake word, "xiaozhi"). The spec
  (docs/websocket.md §6) round trigger is
  `{"type":"listen","state":"start","mode":"manual"}` and the matching
  `"state":"stop"`. The last flashed build uses start/stop — **unverified
  on-device** (user stopped testing before this could be confirmed).
- Server quirk (harmless): re-registers MCP tools each round, logging
  `Duplicate tool names: self__get_device_status, self__reboot` and
  `The number of tools has reached the limit: 32`.
- IDF v6 gotchas: esp-mqtt is a managed component (`espressif/mqtt`);
  mbedtls 4 removed `mbedtls/aes.h` → use PSA crypto (`psa_cipher_*`
  multipart — the one-shot functions don't take/emit the IV the way the
  protocol needs); esp-opus encoder needs `OPUS_SET_COMPLEXITY(5)` and the
  mic task needs a **32768-byte stack** or it crashes in silk NSQ.

## 5. What is verified working

- Boot → WiFi (NVS) → OTA activation → wake-word model load (`wn9_computer_tts`,
  the official English "Computer" model, identical to the official build) →
  MQTT+UDP session established (`session xxxx ready, udp …:880x,
  downlink 24000 Hz`) → OLED shows `Ready — Say "Computer"`.
- Local wake word fires reliably on-device ("Computer").
- OLED display, state display, provisioning flow, BOOT button.
- Earlier builds produced **audible TTS** (the user heard spoken answers),
  so the amp wiring and the downlink path (UDP → PSA decrypt → Opus decode
  → I2S TX at 24kHz) were proven at least once.
- WiFi-scan in the provisioning page was broken (no STA netif created in
  APSTA mode) — fixed in `wifi_prov.c` (untested since).

## 6. Open problems

### A. Every round transcribes as 小智 ("Zao") — conversation never works

Timeline of what was tried (each flashed and observed on-device):
1. Round trigger `listen detect` (text "") + uplink starts immediately →
   `STT: 小智` ~0.4s after wake. Server transcribed the wake-word tail.
2. + 600ms uplink mute after wake → still 小智 (~1.2s after wake).
3. + round arming delayed 800ms (detect sent late) → still 小智 (~1.5s).
4. + 1200ms mute + 1200ms arming → still 小智 (~1.6s after wake).
   The invariant STT-latency-after-arm (~400ms) regardless of gate proves
   the transcription is NOT from queued wake-word audio.
5. Root cause identified in the spec: `state:"detect"` is a wake-word
   notification; the server then hears its own wake word (小智) in the
   stream. **Last build switched to `state:"start"/"stop", mode:"manual"`
   — NOT YET VERIFIED.** This is the first thing to test.
   If it still fails: capture what `send_json` emits, check whether the
   server expects the uplink *before* vs *after* the start message, and
   try sending audio only after the start is acked. Also consider that
   the server may run VAD and expect `listen stop` when the user stops
   speaking — currently `stop` is only sent when TTS starts (mute).

### B. User reports "no sound" (TTS silent) — unverified, needs triage

Earlier builds produced audible TTS. Later builds may have broken playback
or the user is describing rounds where the server never sent TTS (see
problem A: an 小智 STT loop does produce TTS though — user heard it).
Triage steps: (1) confirm `audio_play` is called during Speaking
(add a log with byte counts); (2) confirm decoder is at 24kHz
(`opus: decoder @ 24000 Hz`); (3) I2S TX reconfig log
(`playback sample rate -> 24000 Hz`); (4) MAX98357A has no volume
control — check `audio_play` ring buffer isn't filling/dropping.

### C. Official xiaozhi.me builder firmware ("merged-binary.bin")

Built for the `bread-compact-wifi` profile: display pins and (almost
certainly) mic/amp pins don't match this wiring → blank OLED, deaf mic.
Its boot crashed until the flash-size nibble in BOTH image headers
(bootloader @0x0 byte 3, app @0x20000 byte 3) was patched 0x4→0x3
(16MB→8MB). Patched image: `/tmp/xiaozhi-8mb.bin`. Pins **cannot** be
changed inside the official binary (compiled-in constants). Options:
rebuild on xiaozhi.me with a matching board profile, or rewire the
breadboard to the bread-compact-wifi pinout (exact pinout NOT fetched —
reference-repo constraint).

## 7. Debugging tools that work here

- Serial capture (device reset + read):
  ```python
  import serial, time
  p = serial.Serial('/dev/cu.usbmodem212101', 115200, timeout=0.2)
  p.dtr = False; p.rts = True; time.sleep(0.1); p.rts = False
  time.sleep(0.1); p.dtr = True; p.dtr = False
  # then read p.in_waiting / p.read(n) in a loop
  ```
- Build+flash: `cd /Users/jps/xiaozhi-xiao-s3 && source
  ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1 && idf.py -p
  /dev/cu.usbmodem212101 flash`
- Key log tags: `wake_word`, `app: state:`, `mqtt_udp`, `session`,
  `audio`, `opus`, `MODEL_LOADER`.

## 8. Suggested next steps (in order)

1. **Test the already-flashed manual-mode build** ( Computer → pause →
   question). Watch `session: STT:` — it should now show the real words.
2. If STT is right but no sound: triage problem B above.
3. If STT still wrong: log every outgoing JSON (`send_json_str` already
   logs at DEBUG — raise log level) and compare against docs/mqtt-udp.md
   §4/§5 message flow; check whether the server wants audio only after
   `listen start`, and whether VAD end-of-speech needs a device-side
   `listen stop` on silence.
4. Commit the post-`a5b0e42` fixes (warmup gate, delayed arming,
   manual-mode listen, wifi-scan STA netif, conversation window) and push.
5. Update `flash/` prebuilts + README/FLASHING.md if behavior changes.