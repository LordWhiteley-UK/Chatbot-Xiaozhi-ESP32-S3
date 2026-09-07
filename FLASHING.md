# Flashing the firmware with a web browser (no toolchain install)

You do not need ESP-IDF or any command-line tools to program the board.
This guide uses a browser-based flasher (Google Chrome or Microsoft Edge
only — they are the browsers that support Web Serial) with the prebuilt
binaries in this repository's [`flash/`](flash/) directory.

## What you need

- The XIAO ESP32-S3 board with the mic / amp / OLED wired up (pin map in
  [`README.md`](README.md))
- A USB-C data cable plugged into the **same USB-C connector the board
  uses for serial** (on this board there is only one)
- Chrome or Edge on a desktop/laptop

## The files and where they go

All three are in the [`flash/`](flash/) directory of this repo
(download them from GitHub, or build them yourself with `idf.py build`):

| File | Flash offset | What it is |
|---|---|---|
| `flash/bootloader.bin` | `0x0` | 2nd-stage bootloader |
| `flash/partition-table.bin` | `0x8000` | partition table |
| `flash/xiaozhi_xiao_s3.bin` | `0x10000` | the firmware itself |

The offsets matter — the web flasher asks for a start address per file.

## Step by step

1. **Open the flasher.** Go to <https://esptool.spacehuhn.com>
   (the official Espressif equivalent is
   <https://espressif.github.io/esptool-js/>, same procedure).
2. **Connect the board** by USB and switch it on.
3. On the flasher page choose **460800** baud (921600 also works),
   then click **Connect**. The browser shows a list of serial ports —
   pick the one named `USB Serial/JTAG` or `usbmodem…`.
   If no port appears, see "Board won't connect" below.
4. Pick **Erase flash** *before* the first install (this wipes any old
   WiFi credentials so provisioning starts clean), or skip it for an
   update of an already-provisioned board.
5. In the **Files** section add the three rows from the table above:
   click **Add file**, choose `bootloader.bin`, set its offset to
   `0x0`; then `partition-table.bin` at `0x8000`; then
   `xiaozhi_xiao_s3.bin` at `0x10000`.
6. Click **Program**. Flashing takes under a minute at 460800 baud.
7. When it reports *Finishing / Hard resetting…*, unplug and replug
   the board (or tap its RESET button).

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

## Board won't connect?

The ESP32-S3's USB Serial/JTAG normally resets into download mode
automatically. If the flasher can't connect:

1. **Hold** the **BOOT** button on the board
2. **Tap RESET** once, then release BOOT
3. Click **Connect** again — the port should now enumerate

If the port name keeps changing between attempts, re-open the port
list and pick the newest `usbmodem…` entry.

## Building the binaries yourself

```sh
git clone https://github.com/LordWhiteley-UK/Chatbot-Xiaozhi-ESP32-S3.git
cd Chatbot-Xiaozhi-ESP32-S3
# install ESP-IDF v6.0.2, then:
idf.py set-target esp32s3
idf.py build
```

The three files land in `build/bootloader/`, `build/partition_table/`
and `build/` — same names, same offsets as the table above. To flash
from the command line instead: `idf.py -p <PORT> flash`.