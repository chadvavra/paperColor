# PaperColor ToDo

A Bluetooth LE todo list application for the **M5Stack PaperColor** e-ink display, paired with a web-based companion app that runs in Chrome on Android. Tasks persist to a microSD card and survive power cycles. Voice input is handled by the Android Web Speech API — no offline STT required on the device.

---

## Hardware

- **M5Stack PaperColor** (SKU: C151) — ESP32-S3R8, 4" E-Ink Spectra 6 color display, 400×600
- **microSD card** — FAT32 formatted, any capacity up to 32GB

No additional hardware required.

---

## Features

- Add tasks by voice or text from the companion web app
- Tasks synced to the PaperColor over Bluetooth LE
- 6-color e-ink display (white, black, red, yellow, green, blue)
- Selected item highlighted in yellow
- Tasks persist to `/todos.json` on the microSD card
- Auto deep-sleep after 30 seconds of inactivity (sleep suppressed during active BLE connection)
- Wake from deep sleep via any button press
- Battery-friendly: standby ~92µA in deep sleep

---

## File Structure

```
PaperColor_ToDo/
├── PaperColor_ToDo.ino   # Arduino sketch — flashed to the PaperColor
├── companion.html         # Web app — hosted on HTTPS, opened in Chrome on Android
└── README.md
```

---

## Arduino Setup

### Board & Libraries

| Item | Value |
|------|-------|
| Board target | `M5PaperColor` |
| Arduino IDE | 2.x recommended |

Install the following via **Arduino Library Manager**:

| Library | Min Version |
|---------|-------------|
| M5Unified | 0.2.15 |
| M5GFX | 0.2.21 |
| NimBLE-Arduino | 2.x |
| ArduinoJson | 6.21.0 |
| M5PM1 | latest |

> The SD library conflict warning (`Multiple libraries found for SD.h`) is harmless — Arduino will correctly use the M5Stack bundled version.

### Flashing

1. Connect PaperColor via USB-C
2. Press and hold the side reset button to enter download mode
3. Select board `M5PaperColor` and the correct port
4. Upload `PaperColor_ToDo.ino`

### SD Card

The PaperColor's SD card is powered through the **M5PM1 GPIO expander** — it cannot be initialized with a simple `SD.begin()` call. The sketch handles this automatically, but the card must be:

- **FAT32** formatted (not exFAT, not NTFS)
- Inserted before boot

Cards larger than 32GB are typically exFAT by default — reformat them as FAT32 using Disk Utility (macOS) or a third-party tool on Windows.

#### SD SPI Pins (reference)

| Signal | GPIO |
|--------|------|
| CS     | G47  |
| SCK    | G15  |
| MOSI   | G13  |
| MISO   | G14  |

---

## Button Map

| Button | GPIO | Action |
|--------|------|--------|
| A | G10 | Scroll up / select previous item |
| B | G9  | Scroll down / select next item |
| C | G1  | Toggle selected item done/undone |

The currently selected item is highlighted in **yellow**. The footer shows `A:Up  B:Dn  C:Done`.

---

## Companion Web App

### Requirements

- **Chrome on Android** (Web Bluetooth + Web Speech API)
- Served over **HTTPS** — Web Bluetooth does not work on `http://` or `file://` URLs

### Hosting Options

| Option | How |
|--------|-----|
| Your own web server | Upload `companion.html`, access via `https://` |
| Netlify Drop | Drag `companion.html` to [app.netlify.com/drop](https://app.netlify.com/drop) |
| ngrok + local server | `python3 -m http.server 8080` + `npx ngrok http 8080` |

### Usage

1. Flash the PaperColor and power it on — it begins advertising as `PaperColor-ToDo`
2. Open the companion URL in Chrome on Android
3. Tap **Connect** — a device picker will appear
4. Select **PaperColor-ToDo** from the list
5. Tap **+** to open the add panel
6. Tap the **microphone** icon and speak your task, or type it manually
7. Tap the send button — the task appears on the PaperColor immediately

> **Location permission required on Android:** Chrome requires Location permission enabled to scan for BLE devices. Go to Settings → Apps → Chrome → Permissions → Location → Allow.

### Companion App Features

- Voice input via Web Speech API (Chrome built-in, no API key needed)
- Tap a checkbox to toggle a task done/undone
- Tap the trash icon to delete a task
- Stats bar shows total, done, and remaining counts
- BLE connection status indicator (green = connected, red = disconnected)

---

## BLE Protocol

The app uses the **Nordic UART Service (NUS)** UUIDs for maximum compatibility.

| Role | UUID |
|------|------|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (phone → device) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (device → phone, notify) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

### Commands (phone → device)

| Command | Description |
|---------|-------------|
| `ADD:<text>` | Add a new todo item |
| `DONE:<id>` | Toggle the item with this id done/undone |
| `DEL:<id>` | Delete the item with this id |
| `SYNC` | Request the device to push the full list |
| `CLEAR` | Delete all todos |

### Response (device → phone, JSON notify)

```json
{
  "todos": [
    { "id": 1, "text": "Buy milk", "done": false },
    { "id": 2, "text": "Call dentist", "done": true }
  ],
  "count": 2
}
```

Large payloads are fragmented across BLE notifications. The web app accumulates chunks until valid JSON is received.

---

## Power & Sleep

The PaperColor's e-ink display holds its image with **zero power** — no refresh needed while idle.

| State | Current |
|-------|---------|
| Deep sleep | ~92 µA |
| BLE advertising (idle) | ~10–15 mA |
| Full load (display refresh) | ~212 mA |

**Sleep behavior:**
- Device sleeps after **30 seconds** of no button presses or BLE commands
- Sleep is suppressed while a BLE connection is active
- Footer updates to `Press any button to wake` before sleeping
- On wake, the device reinitializes fully — BLE re-advertises, todos reload from SD

The sleep timeout can be adjusted by changing `SLEEP_TIMEOUT_MS` in the sketch (value in milliseconds).

---

## Known Limitations

- Maximum 20 todo items (increase `MAX_TODOS` in sketch if needed)
- Task text is capped at 60 characters in the web app input
- Web Bluetooth is not supported on iOS/iPadOS (Apple restriction) or Firefox
- Deep sleep + wake requires FAT32 SD card to be present for task persistence

---

## Pinmap Reference (PaperColor C151)

Key pins used by this project — full pinmap at [docs.m5stack.com/en/core/PaperColor](https://docs.m5stack.com/en/core/PaperColor).

| Function | GPIO |
|----------|------|
| Button A | G10 |
| Button B | G9  |
| Button C | G1  |
| SD CS    | G47 |
| SD SCK   | G15 |
| SD MOSI  | G13 |
| SD MISO  | G14 |
| SYS I2C SDA (M5PM1, SHT40, RTC) | G3 |
| SYS I2C SCL (M5PM1, SHT40, RTC) | G2 |
| Grove PORT.A SDA | G5 |
| Grove PORT.A SCL | G4 |
