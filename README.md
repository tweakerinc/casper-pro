# Casper Pro

**Casper firmware for Xteink X4 Pro** (ESP32-S3, GT911 touch, warm/cold frontlight).

Same reader UX and features as Casper on X3/X4, with hardware-specific support for Pro.

| | Casper (C3) | Casper Pro (this tree) |
|--|--|--|
| MCU | ESP32-C3 | **ESP32-S3 + 8 MB PSRAM** |
| Devices | X3 + X4 | **X4 Pro only** |
| Touch | No (buttons) | **GT911** |
| Frontlight | No | **Brightness + color temperature** |
| Default env | `default` | **`x4pro`** |

Storage remains `/.crosspoint` so books/settings can move between devices.

## Stock firmware backup

**Do this before any custom flash.** See `stock-firmware/README.md`.

Full dump already taken:

- `stock-firmware/x4pro_stock_full_16MB.bin` (16,777,216 bytes)
- SHA256 in that folder’s README

Restore stock:

```powershell
python -m esptool --chip esp32s3 --port COM15 --baud 460800 write-flash 0x0 stock-firmware/x4pro_stock_full_16MB.bin
```

## Build

**Use the no-space path** `C:\Users\m\Documents\CasperPro` (ESP-IDF fails if the project path contains spaces). A spaced copy may still exist at `Casper Pro`; treat **CasperPro** as the working tree.

```powershell
cd C:\Users\m\Documents\CasperPro
# First build rebuilds Arduino core for S3 (slow once)
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e x4pro
```

Firmware output (after successful build):

- `.pio\build\x4pro\firmware.bin` (~5.5 MB app image)

Upload (only after stock dump is safe — already done):

```powershell
# Put device in download mode if needed (hold BOOT / power cycle with USB data cable)
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e x4pro -t upload --upload-port COMxx
```

Replace `COMxx` with the ESP32-S3 USB Serial / JTAG port (previously COM15 on this machine).

## Hardware notes (freeink-sdk)

See `freeink-sdk/docs/xteink-x4pro-support.md`.

- Display SPI, GT911, dual PWM light, SDMMC, CW2017 gauge, BM8563 RTC  
- Physical: Left=GPIO0, Right=GPIO7, Power=GPIO3; Back/Confirm via touch + Home key  

## Status

- [x] Full stock dump (`stock-firmware/x4pro_stock_full_16MB.bin`)  
- [x] Project forked from Casper  
- [x] `env:x4pro` (S3 + X4PRO + SDMMC + PSRAM)  
- [x] Frontlight settings (Display → Frontlight / Brightness / Color Temperature)  
- [x] First `pio run -e x4pro` SUCCESS (RAM ~20%, Flash ~83% of app partition)  
- [ ] First flash + hardware validation  
- [ ] Touch UX polish on every activity  
