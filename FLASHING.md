# Flashing the firmware — complete beginner's guide

This page explains, from zero, how to put the firmware onto the XIAO
ESP32-S3. Pick **one** of the two routes:

- **Route A — web flasher (easiest).** Nothing to install; you need the
  Google Chrome or Microsoft Edge browser and the three ready-made
  binary files that ship in this repo's [`flash/`](flash/) folder.
- **Route B — flash locally.** You install Espressif's tools on your
  computer. This is the route you need if you want to *modify* the
  firmware, not just install it.

In both cases the board is programmed over its USB-C cable — there is
no separate programmer to buy.

---

## Route A — flash from the browser (no install)

**What you need**

- The XIAO ESP32-S3 board wired up (pin map in [`README.md`](README.md))
- A USB-C **data** cable (a charge-only cable will not show a port)
- Google Chrome or Microsoft Edge on a desktop/laptop
  (Firefox/Safari cannot talk to USB serial devices)

**Step by step**

1. Download the three files from this repo's `flash/` folder — click
   each file on GitHub, then the **Download raw file** button:
   - `bootloader.bin`
   - `partition-table.bin`
   - `xiaozhi_xiao_s3.bin`
2. Plug the board into your computer with the USB-C cable.
3. Open **<https://esptool.spacehuhn.com>** in Chrome or Edge.
   (The official Espressif equivalent is
   <https://espressif.github.io/esptool-js/> — the steps are identical.)
4. Set **Baud rate** to `460800`.
5. Click **Connect**. Your browser pops up a list of serial ports —
   choose the one containing `USB Serial/JTAG` or `usbmodem…`, then
   click **Connect** again in the dialog.
6. *(First install only)* Click **Erase flash** and wait for
   *Finish*. This wipes any previous WiFi settings so the setup
   wizard starts clean. You can skip it when re-flashing a board you
   already provisioned.
7. In the **Files** area, add each of the three files with its start
   address — this part is critical, the addresses are not optional:

   | File | Start address (offset) |
   |---|---|
   | `bootloader.bin` | `0x0` |
   | `partition-table.bin` | `0x8000` |
   | `xiaozhi_xiao_s3.bin` | `0x10000` |

   Click **Add file**, pick the file, and type the address from the
   table into the box next to it. Repeat for all three.
8. Click **Program**. Flashing takes under a minute.
9. When it says *Hard resetting…*, unplug and replug the board (or tap
   its RESET button). Go to [First boot](#first-boot) below.

---

## Route B — flash locally

There are two ways to do this locally. **B1** uses a point-and-click
GUI inside Visual Studio Code — recommended for beginners. **B2** uses
the command line, which is what the project developer uses.

### B1 — the graphical way (VS Code + ESP-IDF extension)

#### Install the software (once)

1. Install **Visual Studio Code**: <https://code.visualstudio.com>
2. Open VS Code → click the **Extensions** icon in the left sidebar
   (four squares) → search for **ESP-IDF** → install
   **"ESP-IDF" by Espressif Systems**.
   Direct link: <https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension>
3. Press **F1** (or Cmd/Ctrl+Shift+P), type **ESP-IDF: Configure
   ESP-IDF Extension**, press Enter, choose **EXPRESS (quick install)**.
4. In the wizard set exactly these values:
   - **Version**: `v6.0.2`  ← this project is built against v6.0.2
   - **Download server**: Espressif
   - **Install paths**: leave the defaults
   Click **Install** and wait (~10 min; it downloads the toolchain).

#### Flash the project

5. From this repo: click **Code → Download ZIP** on GitHub, unzip it —
   or if you have git: `git clone https://github.com/LordWhiteley-UK/Chatbot-Xiaozhi-ESP32-S3.git`
6. In VS Code: **File → Open Folder…** → choose the unzipped project
   folder (the one containing `CMakeLists.txt`).
7. Plug the board in. At the **bottom bar** of VS Code click the
   **⚙ (set target)** button → choose **esp32s3** → if asked, choose
   the option for **8 MB** flash / USB Serial/JTAG console is already
   preset in the project's `sdkconfig.defaults`.
8. Click the **⚡ (build)** button in the bottom bar and wait for the
   compile to finish (first build takes a few minutes).
9. Click the **⚡→ (flash)** button (lightning bolt with an arrow).
   If several serial ports are listed, pick the one named
   `usbmodem…` (macOS) or `COM…` (Windows) that appeared when you
   plugged the board in. VS Code flashes the same three files at the
   same offsets as Route A automatically.
10. Click the **📺 (monitor)** button to watch the board's log output.
    (Stop the monitor with **Ctrl+]**.)

### B2 — the command-line way

Install prerequisites from Espressif's official walk-through (toolchain
+ `idf.py`): <https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/index.html>

Then:

```sh
git clone https://github.com/LordWhiteley-UK/Chatbot-Xiaozhi-ESP32-S3.git
cd Chatbot-Xiaozhi-ESP32-S3

# macOS / Linux — once per terminal session:
. $HOME/esp/esp-idf-v6.0.2/export.sh

idf.py set-target esp32s3     # one-time; picks the chip
idf.py build                  # compiles everything
idf.py -p <PORT> flash        # upload; see port names below
idf.py -p <PORT> monitor      # watch the log; Ctrl+] to exit
```

**`<PORT>` values:**

- **macOS**: `/dev/cu.usbmodem…` (e.g. `/dev/cu.usbmodem212101` — the
  trailing digits differ per board)
- **Windows**: a `COM` port, e.g. `COM5` (check Device Manager →
  *Ports (COM & LPT)* for "USB Serial Device")
- **Linux**: `/dev/ttyACM0` typically

`idf.py flash` writes the three images to `0x0`, `0x8000` and
`0x10000` for you — the same addresses as Route A.

> **Alternative for Windows without VS Code:** Espressif's
> **Flash Download Tool** (GUI): <https://www.espressif.com/en/support/download/other-tools>
> — select chip type `ESP32-S3`, *Develop* mode, *UART*, then add the
> three files with the offsets from Route A.

### Setup parameters specific to this project

These are already baked into `sdkconfig.defaults` — you only need them
if something asks:

| Parameter | Value |
|---|---|
| Chip / target | `esp32s3` |
| ESP-IDF version | **v6.0.2** |
| Flash size | 8 MB |
| PSRAM | Octal, 80 MHz (N8R8 module) |
| Console | USB Serial/JTAG (not UART0 — GPIO43/44 are the mic) |

---

## First boot

The OLED shows:

```
WiFi setup
──────────────────────────
In WiFi, join Xiaozhi.
Then open
192.168.4.1 in browser
```

1. On your phone/laptop open WiFi settings, join the network whose name
   starts with **Xiaozhi-** (it is open, no password).
2. Browse to <http://192.168.4.1>, pick your home WiFi, enter its
   password, submit. The board reboots onto your network.
3. It then contacts the xiaozhi.me OTA endpoint. If the device is not
   yet bound to an xiaozhi.me account, the screen shows a 6-digit code —
   enter it at <https://xiaozhi.me> to bind.
4. Once bound, the screen reads **Ready**. Hold the **BOOT** button to
   talk; release to send.

---

## Troubleshooting

**No serial port appears at all**
- Try a different USB-C cable — charge-only cables are the #1 cause.
- On macOS/Linux check `ls /dev/cu.usbmodem*` (or `ls /dev/ttyACM*`).
- On Windows, if the port shows an error icon, install the driver from
  <https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers>
  (rarely needed on Windows 10+).

**Flasher says "Connecting…" forever / fails**
1. **Hold** the **BOOT** button on the board
2. **Tap RESET** once, then release BOOT
3. Click **Connect** again

**Port name keeps changing between attempts** — the board re-enumerates
after flashing. Just re-open the port list and pick the newest
`usbmodem…` / `COM…` entry.

**Flashed OK but the screen stays blank** — unplug/replug once. If
still blank, hold BOOT + tap RESET to boot from a clean reset.