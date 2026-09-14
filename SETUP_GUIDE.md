# XIAO ESP32-S3 Voice Chatbot — Complete Setup Guide

From a freshly assembled board to a chatbot that answers you in English.
Covers Windows 11 and macOS, WiFi setup, xiaozhi.me registration, language
configuration, and the button controls.

---

## What you need

- The assembled board: Seeed XIAO ESP32-S3 + INMP441 mic + MAX98357A amp +
  speaker + SSD1309 OLED (pin map in [`README.md`](README.md))
- A USB-C **data** cable (a charge-only cable will not work)
- A computer (Windows 11 or macOS) for flashing
- A phone or tablet for the WiFi setup step
- A free account at [xiaozhi.me](https://xiaozhi.me)

---

## Part 1 — Flash the firmware

Pick **one** route. Route A is easiest (nothing to install). Route B is for
developers who want to build from source.

### Route A — Web flasher (no install, Windows 11 & macOS)

You need **Google Chrome** or **Microsoft Edge** (Firefox/Safari cannot talk
to USB serial devices).

1. **Download the four firmware files** from this repo's
   [`flash/`](flash/) folder on GitHub — click each file, then
   **Download raw file**:
   - `bootloader.bin`
   - `partition-table.bin`
   - `xiaozhi_xiao_s3.bin`
   - `srmodels.bin` (the "Computer" wake-word model)
2. Plug the board into your computer with the USB-C cable.
3. Open **<https://esptool.spacehuhn.com>** in Chrome or Edge.
   (Espressif's official equivalent: <https://espressif.github.io/esptool-js/>)
4. Set **Baud rate** to `460800`.
5. Click **Connect**. The browser lists serial ports — pick the one
   containing `USB Serial/JTAG` or `usbmodem…` (macOS) / `COM…` (Windows).
6. *(First install only)* Click **Erase flash** and wait for *Finish*.
   This wipes any old WiFi settings so the setup wizard starts clean.
7. In the **Files** area, add each of the four files with its start
   address — the addresses are critical:

   | File | Start address |
   |---|---|
   | `bootloader.bin` | `0x0` |
   | `partition-table.bin` | `0x8000` |
   | `xiaozhi_xiao_s3.bin` | `0x10000` |
   | `srmodels.bin` | `0x290000` |

8. Click **Program**. Flashing takes under a minute.
9. When it says *Hard resetting…*, unplug and replug the board (or tap
   RESET). Continue to [Part 2](#part-2--first-boot-connect-to-wifi).

### Route B — Build from source (developers)

#### Windows 11

**Install the tools (once):**

1. Install **Visual Studio Code**: <https://code.visualstudio.com>
2. In VS Code, open the **Extensions** panel (four-squares icon), search
   for **ESP-IDF**, install **"ESP-IDF" by Espressif Systems**.
3. Press **F1** → type **ESP-IDF: Configure ESP-IDF Extension** → choose
   **EXPRESS (quick install)**.
4. In the wizard set **Version: `v5.3.3`** (this project is built against
   v5.3.3), leave the other defaults, click **Install** (~10 min).

**Get the code and flash:**

5. Download the repo: on GitHub click **Code → Download ZIP** and unzip,
   or run `git clone https://github.com/LordWhiteley-UK/Chatbot-Xiaozhi-ESP32-S3.git`
6. In VS Code: **File → Open Folder…** → choose the project folder (the
   one containing `CMakeLists.txt`).
7. Plug the board in. In the bottom bar click **⚙ (set target)** →
   choose **esp32s3**.
8. Click **⚡ (build)** and wait for the compile to finish.
9. Click **⚡→ (flash)**. If several ports are listed, pick the `COM…`
   port that appeared when you plugged the board in.
10. Click **📺 (monitor)** to watch the log. Stop with **Ctrl+]**.

> **Alternative Windows GUI:** Espressif's **Flash Download Tool**
> (<https://www.espressif.com/en/support/download/other-tools>) — select
> chip `ESP32-S3`, *Develop* mode, *UART*, then add the four files from
> `flash/` with the offsets from Route A.

#### macOS

**Install the tools (once):** follow Espressif's official walk-through for
ESP-IDF v5.3.3:
<https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/get-started/index.html>

**Get the code and flash:**

```bash
git clone https://github.com/LordWhiteley-UK/Chatbot-Xiaozhi-ESP32-S3.git
cd Chatbot-Xiaozhi-ESP32-S3

source ~/esp/esp-idf/export.sh        # once per terminal session
idf.py set-target esp32s3             # one-time
idf.py build                          # compiles everything
idf.py -p /dev/cu.usbmodemXXXX flash monitor   # upload + watch log
```

- Find the port with `ls /dev/cu.usb*` — it looks like
  `/dev/cu.usbmodem13401` (the digits differ per board and change when
  the board re-enumerates).
- Exit the monitor with **Ctrl+]**.
- If you have more than one ESP32 board plugged in, always pass `-p` with
  the correct port so you don't flash the wrong device.
- After a `git pull` that changes `sdkconfig.defaults`:
  `rm -f sdkconfig sdkconfig.old && idf.py fullclean && idf.py build`

---

## Part 2 — First boot: connect to WiFi

1. Power the board. The OLED shows:

   ```
   WiFi setup
   ──────────────────────────
   In WiFi, join Xiaozhi.
   Then open
   192.168.4.1 in browser
   ```

2. On your **phone or tablet**, open WiFi settings and join the network
   whose name starts with **Xiaozhi-** (it is open — no password).
3. Open a browser and go to **http://192.168.4.1**.
4. The page shows a **list of nearby WiFi networks** (the board scans for
   them). Pick your home WiFi from the list, enter its password, and
   submit.
5. The board saves the credentials, reboots, and connects to your home
   WiFi. The OLED shows `Activating` while it contacts the server.
6. If you typed the wrong password, the board automatically returns to
   provisioning mode — just repeat from step 2.

---

## Part 3 — Register the device at xiaozhi.me

1. Once on your home WiFi, the board contacts the xiaozhi.me activation
   server automatically.
2. If the device is not yet bound to an account, the OLED shows a
   **6-digit code** in large digits:

   ```
   Enter code at
   xiaozhi.me to bind:
   123456
   ```

3. On your computer or phone, go to **<https://xiaozhi.me>** and create
   an account (or sign in).
4. In the console, find the option to **add / bind a device** and enter
   the 6-digit code from the OLED.
5. The board polls the server; within seconds the code disappears and the
   screen shows **Ready — Say "Computer"**. The device is now bound to
   your account.

---

## Part 4 — Configure the agent for English

The device sends `"language":"en"` in its hello message, which sets the
**speech recognition** language. But the **LLM's answer language** and the
**voice** are controlled by your agent settings on the xiaozhi.me console.

1. Go to **<https://xiaozhi.me/console/agents>** and open your agent.
2. **System prompt** — make sure it tells the LLM to answer in English.
   If it's in Chinese (or empty), the bot may answer in Chinese even to
   English questions. Use something like:

   > You are a helpful voice assistant. Always respond in English.

3. **Voice** — select an English voice, e.g. **Skye en-US** (female).
   This is the text-to-speech voice the bot speaks with.
4. Save. The device picks up the new settings on its next session.

> Note: the goodbye message ("I have to go now…") may use a different
> voice than the conversation voice — that is server-side behaviour, not
> a firmware setting.

---

## Part 5 — Talk to it

1. Say the wake word **"Computer"**. The device wakes (wide eyes on the
   face display), waits ~1.2 s (skips the word's own tail), then opens a
   listening round.
2. Ask your question. The server detects when you stop talking and
   speaks the answer.
3. **Conversation mode:** after each answer the device keeps listening
   for **10 seconds** — ask follow-up questions without the wake word.
   After 10 s of silence it goes back to sleep.

### Button controls

Both the on-board **BOOT** button and the optional **external push
button** (D4 pad / GPIO5 to GND) share the same multi-press logic:

| Gesture | Action |
|---|---|
| **1 press** | Wake / barge-in — start listening, or stop the current answer mid-speech |
| **2 presses** | Volume up |
| **3 presses** | Volume down |
| **Long press** (>1 s) | Toggle face / text display |

Volume levels: 20%, 40%, 60%, 80%, 100% (default 40%).

Long-press to switch to the animated face: closed eyes and floating
"z z" when sleeping, wide eyes when listening, mouth moving in sync with
the audio when talking.

---

## Troubleshooting

**No serial port appears at all**
- Try a different USB-C cable — charge-only cables are the #1 cause.
- macOS/Linux: `ls /dev/cu.usbmodem*` (or `ls /dev/ttyACM*`).
- Windows: check Device Manager → *Ports (COM & LPT)*.

**Flasher says "Connecting…" forever**
1. **Hold** the **BOOT** button on the board
2. **Tap RESET** once, then release BOOT
3. Click **Connect** again

**Port name keeps changing** — the board re-enumerates after flashing.
Re-open the port list and pick the newest `usbmodem…` / `COM…` entry.

**Flashed OK but the screen stays blank** — unplug/replug once. If still
blank, hold BOOT + tap RESET.

**Bot answers in Chinese** — check the agent's system prompt at
xiaozhi.me/console/agents and make sure it says to respond in English
(see Part 4).

**Bot speaks with a mixed voice** — the goodbye message may use a
different TTS voice than the conversation voice; check the agent's voice
settings on the xiaozhi.me console.

**Board seems stuck after pressing the button mid-answer** — the barge-in
stops the answer and re-opens listening after ~1.2 s. Wait for the face
to show the listening state, then speak. If the speaker keeps repeating
a syllable, re-flash the latest firmware (this was fixed in the
2026-09-12 release).
