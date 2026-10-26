# Crow Hub - kitchen hub for the Elecrow CrowPanel Advance 7.0" (V1.5)

Internet radio, weather, agenda, Home Assistant tiles, kitchen timers, voice commands, photo frame / night clock,
phone remote with photo upload, and a LoRa "world ended" mesh. **Status: syntax-checked against LVGL 9.1.0 and
ArduinoJson 7 with host stubs only. It has NOT been compiled for the ESP32 or run on hardware - expect a few
first-flash fixes.**

## 1. Setup
### PlatformIO (recommended)
Open this folder in VS Code with the PlatformIO extension and hit Build / Upload. `platformio.ini` pulls the
Arduino-ESP32 3.3 core (pioarduino), all libraries, the 16 MB / OPI-PSRAM board settings and `partitions.csv`.
Optional: copy `secrets.example.h` to `secrets.h` for Home Assistant. On first boot pick your Wi-Fi on the screen;
it is saved on the hub.

### Arduino IDE
1. Arduino IDE, Arduino-ESP32 core 3.3.x. Board: ESP32S3 Dev Module, Flash 16MB, PSRAM OPI, partition scheme from
   this folder's `partitions.csv` (if not picked up, choose "Huge APP").
2. Install Elecrow's libraries from their V1.5 course (LVGL 9.1.0, LovyanGFX 1.2.25, ESP32-audioI2S 3.4.7,
   RadioLib 7.7.1, TAMC_GT911) plus **ArduinoJson 7** from Library Manager.
3. Copy `LovyanGFX_Driver.h` and `pins_config.h` from Elecrow's lesson-03 example into this sketch folder.
4. Copy this folder's `include/lv_conf.h` over `Arduino/libraries/lv_conf.h` (enables the fonts and QR code used).
5. Slide switches S1 S0 = `00` (mic + onboard speaker). Edit `config.h` (and `secrets.h` for HA), upload.

## 2. config.h
Hub name, time zone, weather lat/lon, units, `HA_TILES[]`, calendar `ICS_URLS[]`, ambient/night hours, LoRa name and frequency (868 MHz UK).

## 3. Audio: Adafruit I2S bonnet (MAX98357A)
Defaults in `config.h`: BCLK -> IO43, LRC -> IO44, DIN -> IO19 (UART0-OUT / UART1-OUT headers), VIN 3V3, GND.
Set `AUDIO_OUT_EXTERNAL 1` to use it. IO19 is also the mic clock, so the code re-routes DIN after each
voice recording; this logic is untested. With `AUDIO_OUT_EXTERNAL 0` the onboard speaker is used (4-8 ohm speaker on SPK).

## 4. Home Assistant
- Long-lived token -> `HA_TOKEN` in `secrets.h` (git-ignored; start from `secrets.example.h`). Tiles: kind T = toggle, S = sensor, B = button.
- Voice: install the Whisper (STT) and Piper (TTS) add-ons; set `HA_STT_ENTITY`/`HA_TTS_ENTITY`. Without HA, speech
  uses Google TTS and a small local command set.
- Push to the hub (doorbell etc.):
  ```yaml
  rest_command:
    crow_notify:
      url: "http://crowhub.local/notify?k=<token>&msg={{ msg | urlencode }}&say=1"
  ```

## 5. Phone remote
`http://crowhub.local/?k=<token>` (the hub makes a random token on first boot; URL + QR on the More page): radio, timers, show/speak text, photo upload
(resized in the browser, stored in LittleFS, no SD needed).

## 6. Pin conflicts
IO4/5/6 are shared by SD, LoRa SPI and the onboard speaker. So: SD is off by default (`ENABLE_SD`), and LoRa runs
as a separate boot mode (More -> Doomsday arm -> reboot; "Back to hub mode" button returns). Switch S1 S0 `01`
selects the wireless module slot for LoRa.

## 7. Ideas included
Night clock with auto dim, sleep timer, 3 parallel timers with alarm overlay, kitchen conversions by voice,
jokes/dice, RTC-kept time, reminder toasts before appointments, duck-the-radio while speaking, phone photo upload,
HA `/notify` hook, survival mesh with 3-hop rebroadcast and 5-minute beacons.
Later: wake word via ESP-SR (phase 2), and the ESP32 Audio Kit as a Wi-Fi "speaker satellite".

## 8. Known limitations
No wake word yet (tap the mic). TLS is unverified (`setInsecure`). iCal: no nth-weekday rules, edited instances or
non-UK zones. Stream URLs may have died - edit the list in `radio_app.h`.
