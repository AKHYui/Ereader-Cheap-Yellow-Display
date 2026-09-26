**English** · [简体中文](README.zh-CN.md)

# Ereader · ESP32 Cheap Yellow Display Handheld

Firmware for the **ESP32-2432S028R (Cheap Yellow Display / CYD)**: read TXT novels, browse
images, transfer files over Wi-Fi, 6-digit boot password, PWM brightness control.

The entire UI is a hand-written **row-band streaming renderer**. There is only one 240×64 DMA
band for the whole screen, and every screen is composed row by row before being pushed to the
panel. This board has no PSRAM and its largest contiguous free block is only ~80 KB, so
"decode and push at the same time" is the only viable way to show a 1080p image here
(see [Implementation notes](#implementation-notes)).

![Main menu](screenshots/01-main.png)

---

## Features

| Module | Capability |
|---|---|
| **Reader** | Scans `.txt` files under `/sdcard/novels`; auto-detects UTF-8 / GBK / GB2312, with or without BOM; automatic pagination and blank-line filtering; white text on black; **bookmarks** (the page number survives a reboot) |
| **Images** | Browsing of JPEG / BMP under `/sdcard/images`; aspect-fit, never stretched; 1080p images work; **delete** the current image (with confirmation) |
| **Network** | Join a WLAN (scan, on-screen password keyboard, status); **AP mode** — the board hosts its own hotspot, so files can be transferred with no router at all; browser upload page (target-directory picker + file list) |
| **Settings** | **Device security**: 6-digit boot password (asked on every boot once enabled); **brightness**: 1–100% via PWM; forget network |
| **Filesystem** | `images` / `novels` are created automatically when a card is inserted — a blank card works immediately |

## Screenshots

> These renders are generated from the same layout constants the firmware uses, so the layout
> matches the real panel. The fonts shown here are system fonts; the device draws its own
> bitmap fonts.

### Reader

![Novel list / reading page / bookmark dialog / save confirmation](screenshots/02-reader.png)

### Images

![Image viewer and delete confirmation](screenshots/03-images.png)

### Network

![Network menu / WLAN / AP mode / file receive](screenshots/04-network.png)

### Settings

![Settings / device security / password keypad / brightness](screenshots/05-settings.png)

---

## Hardware

| Item | Value |
|---|---|
| Board | ESP32-2432S028R (CYD), 2.8" 240×320 resistive touch |
| SoC | ESP32-D0WD-V3, dual-core 240 MHz |
| Flash | 4 MB (single `factory` partition; app ≈ 3.0 MB, ~21% of the partition left) |
| PSRAM | **none** |
| Storage | microSD card over SPI (optional — the device boots without one) |

> ⚠️ **There are two panel variants.** Boards with a single Micro-USB port use an **ILI9341**;
> boards with two ports (Micro-USB + USB-C) use an **ST7789**. This project targets
> **ST7789 + no inversion + BGR** (see `main/drivers/board_pins.h`). If you have the ILI9341
> version you must change the panel parameters in that file.

### Pinout

| Function | Pins |
|---|---|
| TFT CS / DC / MOSI / MISO / SCLK / BL | 15 / 2 / 13 / 12 / 14 / **21** |
| Touch XPT2046 CS / MOSI / MISO / CLK / IRQ | 33 / 32 / 39 / 25 / 36 |
| microSD CS / MOSI / MISO / SCLK | 5 / 23 / 19 / 18 |
| On-board RGB LED | R=4 G=16 B=17 |

---

## Build and flash

### Prebuilt firmware

If you would rather not build it yourself, grab `ereader-fw-*.zip` from
[Releases](../../releases). It contains a merged flash image, the individual binaries, and
one-click flashing scripts (`flash.bat` / `flash.sh`) with instructions in `FLASH.txt`.

### Requirements

- **ESP-IDF v5.5** (other 5.x releases generally work)
- Python 3.8+
- The first build **needs network access**: the component manager downloads one dependency,
  `bitbank2/jpegdec ^1.6.2` (JPEG decoder — see `main/idf_component.yml`)

### Steps

```bash
# 0) activate the ESP-IDF environment
. $IDF_PATH/export.sh

# 1) target chip (already pinned to esp32 in sdkconfig.defaults, usually not needed)
idf.py set-target esp32

# 2) build
idf.py build

# 3) flash and watch the serial log (replace COM9 with your own port)
idf.py -p COM9 flash monitor
```

The app binary is `build/ereader.bin`; it can be flashed directly to `0x20000` (see the
partition table below).

### Behind a restrictive network

The component manager fetches over HTTPS from `components.espressif.com`. Give one build a proxy:

```bash
export HTTP_PROXY=http://127.0.0.1:8080
export HTTPS_PROXY=http://127.0.0.1:8080
idf.py build
```

Once the dependency is in `managed_components/`, builds work offline.

### Manual flashing (without idf.py)

```bash
esptool.py --chip esp32 --port COM9 --baud 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xe000  build/ota_data_initial.bin \
  0x20000 build/ereader.bin
```

### Partition table

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20 KB |
| otadata | data/ota | 0xE000 | 8 KB |
| phy_init | data/phy | 0x10000 | 4 KB |
| **factory** | app | **0x20000** | 3.81 MB |

---

## Usage

### 1. Prepare the SD card

Format it as **FAT32** and insert it. On boot the firmware creates these two directories:

```
/sdcard/images    <- images (jpg / jpeg / bmp; subdirectories are not scanned)
/sdcard/novels    <- novels (txt)
```

### 2. Transfer files

Two ways, depending on whether you have a router nearby.

**With a router** — `Network → WLAN`, join your hotspot, note the IP shown on screen, then open
that IP in a browser on a computer or phone on the same network.

**Without a router** — `Network → AP mode`. The board starts its own hotspot (the screen shows
the SSID, the password and the address together; default password `12345678`). Join it from your
phone and open `192.168.4.1`.

Either way you get the same upload page: pick a directory, pick files, upload. **Images are
converted to baseline JPEG and downscaled to 2× the screen size in the browser before being
uploaded** — a phone photo uploaded as-is would be decoded at 1/8 resolution, and since the board
has only a decoder (no encoder), that conversion can only happen on the browser side.

### 3. Boot password

`Settings → Device security → Enable password`, then enter a 6-digit PIN twice. Once enabled, a
lock keypad appears on every boot. Disabling it does not require the old PIN (reaching the
settings page already means you passed the boot check).

---

## Repository layout

```
.
├── CMakeLists.txt              top-level project (target chip esp32)
├── partitions.csv              partition table (single factory partition, app @ 0x20000)
├── sdkconfig.defaults          default config (flash / FreeRTOS / FATFS ...)
├── screenshots/                images used by this README
└── main/
    ├── main.c                  entry point + shell (main menu -> dispatch -> back to menu)
    ├── drivers/                display / touch / SD card / board definitions
    ├── nui/                    UI shell: main menu, settings, device security, drawing layer, fonts
    ├── picview/                image viewer (decoder + row-band display + touch + scanning)
    ├── reader/                 reader (TXT pagination, 16px fonts, button bar, bookmarks)
    ├── net/                    networking (Wi-Fi wrapper, HTTP upload, AP mode, screens)
    └── book/                   GBK tables and filename encoding conversion
```

---

## Implementation notes

A few decisions that shape everything else:

**Row-band streaming render**
Only one 240×64 DMA band (30,720 bytes) exists for the whole module. Every screen is composed row
by row: scan the panel from top to bottom and, for each row, lay down every shape that falls on
it. **The band only advances forward, so drawing must proceed in increasing y** — anything drawn
"backwards" is silently dropped.

**The color path has three independent stages**
Byte order (swapping the two bytes of each pixel), BGR element order, and panel inversion are
independent, produce different symptoms, and **can only be verified on a real panel** (a captured
frame buffer is taken before the swap). The values settled on here are
`swap=0 / mirror_x=1 / BGR=1 / invert=0`, and the byte swap happens in exactly one place:
`pv_disp.c::flush_band()`.

**Two-level bitmap fonts**
Body text uses 16px: a main 4bpp set (ASCII + GB2312 + symbols) and an extension 2bpp set (GBK
extended area). The two levels have different bit depths, so rendering must use the `row_bytes`
and `bpp` stored in the glyph struct — never a global constant. The UI has its own 32px font set
(only the characters the UI actually uses).

**Image resampling uses area averaging**
Source and target sizes are rarely integer multiples, and nearest-neighbour would drop about 26%
of rows and columns and leave visible aliasing. Here all source pixels mapping to one target
pixel are averaged, which is equivalent to a single box filter.

**Touch is detected by pressure, not by coordinate range**
When an XPT2046 is left floating, X/Y read values such as 808/1812 that **fall inside the
calibrated range**, so range checks do not catch them. Press detection uses the Z pressure value
with two thresholds and hysteresis, and coordinates are latched only **two ticks after a press is
confirmed** (the first few ticks of a touch are not trustworthy).

**Name and path buffers are sized to the FATFS limits**
FATFS allows filenames up to 255 bytes. Storing them in a 64-byte fixed buffer meant longer names
were **silently truncated by `strncpy`**, producing a "ghost name" that does not exist on disk —
the symptom being an entry that is listed but can neither be opened nor deleted. The code now uses
a name pool and refuses overflow explicitly (skip and warn), and path assembly detects truncation
as well.

---

## Known limitations

- **Progressive JPEGs can only be decoded at about 1/8 resolution** (behaviour of the decoding
  library, not a configuration issue). For sharp images, re-save them as **baseline** JPEG.
- No PSRAM: the largest contiguous free block is ~80 KB, so very large or very numerous images
  may fail.
- The panel is a single-touch resistive screen, so **only taps are supported** — no swiping, no
  long press, no pinch.
- Touch and panel parameters were measured on this specific board; a different board or panel may
  need recalibration. Panel parameters live in `main/drivers/board_pins.h` (they can also be
  overridden through the NVS namespace `panelcfg`, see `lcd_cfg_load()`).

---

## License

Released under the **PolyForm Noncommercial License 1.0.0** (full text in [LICENSE](LICENSE)).

| | |
|---|---|
| ✅ Permitted | Use, modification and distribution for **noncommercial purposes**: personal study, research, experiments, hobby projects, private entertainment |
| ✅ Permitted | Use by charitable organisations, educational institutions, public research bodies, public safety / health organisations, environmental organisations and government institutions |
| ❌ Not permitted | Any **commercial use** (including internal commercial projects at a company) — commercial use requires prior written permission from the author |

Licensor and software information:

```
Licensor:        AKHYui
Software:        Ereader-Cheap-Yellow-Display
Required Notice: Copyright 2026 AKHYui
```

> ⚠️ Note that **forbidding commercial use does not meet the Open Source definition**, so this is a
> **source-available** licence rather than an OSI-approved open source licence. It fits the case of
> "the code is public so people can read and learn from it, but it should not be sold". For
> commercial use, please open an issue.

### Third-party dependencies

The dependency below is downloaded **at build time** by the ESP-IDF component manager. It is **not
included in this repository** and remains under its own licence, unaffected by this project's licence:

| Dependency | Purpose | Licence |
|---|---|---|
| `bitbank2/jpegdec` | JPEG decoding (with 1/2, 1/4 and 1/8 hardware downscaling) | Apache-2.0 |

### About the fonts

The bitmap fonts in this repository (`main/reader/rd_font16*.bin`, `main/nui/nui_glyphs.c`,
`main/nui/nui_ascii.c`) are **bitmap data rendered from Windows system fonts**, not the font files
themselves. Personal use is fine; **for commercial use, please verify the licensing terms of the
relevant fonts yourself.**

## Credits

Written by the author in collaboration with DeepSeek-V4.1-Flash.
