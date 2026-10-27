# Crow Hub

A kitchen hub for the **Elecrow CrowPanel Advance 7.0"** (ESP32-S3, 800x480 touch screen): internet radio with
worldwide station search, podcasts, weather, your calendar, kitchen timers, a photo frame / night clock, a phone
remote, Home Assistant tiles, voice commands, and a LoRa "world ended" text mesh.

Wi-Fi, weather location, favourite stations, podcasts and calendars are all set up **on the hub or from your phone**,
and kept on the hub - nothing personal needs to go into the code.

## Build and flash

PlatformIO only (VS Code + the PlatformIO extension): open this folder, then **Build** / **Upload**.

- `platformio.ini` pulls everything: the pioarduino platform (Arduino-ESP32 3.3), the libraries (LVGL 9.1,
  LovyanGFX, ESP32-audioI2S, RadioLib, ArduinoJson), the board settings (ESP32-S3 N16R8: 16 MB flash, 8 MB OPI
  PSRAM) and `partitions.csv` (6 MB app, ~10 MB for photos, logos and settings).
- It also **rebuilds the Arduino core** with display-friendly settings (`custom_sdkconfig`). The first build takes
  ~35 minutes and is cached afterwards. Any edit to that block triggers another full rebuild.
- On Windows, build from VS Code or PowerShell - ESP-IDF's tools refuse to run from Git Bash.
- Board switches S1 S0 = `00` (mic + onboard speaker).

The Arduino IDE isn't supported any more: the stock core lacks the settings that keep the screen from jittering.

## First boot

1. Pick your Wi-Fi on the screen and type the password. It's saved on the hub; if the network isn't there at boot
   (router restarting after a power cut, say) the hub keeps retrying quietly.
2. **More > Weather > Change**: type your town.
3. **More > Phone remote**: scan the QR code with your phone (same Wi-Fi). Bookmark that page.

## What's on it

| Page | What it does |
|---|---|
| **Home** | Clock, weather, next appointment, mini player, quick actions |
| **Radio** | Swipeable carousel of favourites + recently played; now playing with logo, title, favourite, rename, sleep timer. **Find stations**: search ~50,000 stations ([radio-browser.info](https://www.radio-browser.info)), browse 110 genres or by country, or *Feeling lucky*. BBC stations work (HLS) and show the current track. |
| **Podcasts** | Follow shows; **New** lists the latest episodes of everything you follow (unplayed ones marked). Search Apple's podcast directory; SoundCloud accounts with a public feed work too (Benji B and The Lot Radio are followed to start with). Episodes have -30 / +30 s and a time bar. |
| **Weather** | Now, next hours, 7 days; tap a day for its 3-hourly breakdown ([Open-Meteo](https://open-meteo.com), no account) |
| **Agenda** | Next two weeks from your calendars |
| **House** | Home Assistant tiles (lights, plugs, sensors, scenes) |
| **Timers** | Three kitchen timers with alarm |
| **More** | Brightness, phone remote QR, Wi-Fi, weather location, screensaver, LoRa mode, system info |

**Screensaver** (after `AMBIENT_AFTER_MIN` idle minutes): photos or a big clock - *More > Photo frame > Show:*
Auto (photos by day, clock at night) / Photos / Clock. Photos are shuffled, panned when they don't fit and shown
whole on a blurred backdrop when they're portrait. Everything on it drifts a few pixels each minute against burn-in.

## Phone remote

`http://crowhub.local/?k=<token>` - the hub makes up the token on first boot; the More page shows the link and a QR code.

- Radio: play / pause, next, volume, your favourites
- Timers, "show on screen", "speak it"
- **Agenda** and **Calendars**: paste a calendar's *secret iCal link* (Google: calendar settings > Integrate calendar
  > Secret address in iCal format; Outlook: Settings > Calendar > Shared calendars > Publish > ICS; iCloud: share
  the calendar > Public Calendar). Up to 4; kept on the hub only.
- Photo frame: upload photos from your phone (shrunk in the browser, whole photo kept)

## Settings files

- `config.h` - hub name, time zone, units, audio output and tone, Home Assistant tiles, screensaver timing, LoRa.
  It's in git, so keep anything private out of it (calendar links belong on the phone page).
- `secrets.h` - copy from `secrets.example.h`: `HA_URL` and `HA_TOKEN` for Home Assistant. Git-ignored.

## Home Assistant (optional)

- Long-lived access token (HA profile > Security) -> `HA_TOKEN` in `secrets.h`; list tiles in `HA_TILES` in
  `config.h` (T = toggle, S = sensor, B = button / scene).
- **Voice** needs speech-to-text: install the Whisper (and optionally Piper) add-ons and set `HA_STT_ENTITY` /
  `HA_TTS_ENTITY`. Tap the mic and speak; without Home Assistant the hub can record but not understand you.
- Push messages to the hub (doorbell etc.):
  ```yaml
  rest_command:
    crow_notify:
      url: "http://crowhub.local/notify?k=<token>&msg={{ msg | urlencode }}&say=1"
  ```

## Audio

- Default: the onboard amp (SPK connector, 4-8 ohm speaker). `SPK_TONE_*` in `config.h` shape it (bass cut for
  small speakers; set all to 0 for a good speaker).
- External I2S DAC / amp (PCM5102, MAX98357A...): `AUDIO_OUT_EXTERNAL 1`; BCLK -> IO43, LRC -> IO44, DIN -> IO19
  (the UART header sockets). IO19 is also the mic clock: the code hands the pin back after each recording.
- The ESP32-S3 has Bluetooth LE only, so no Bluetooth speakers - use a small Bluetooth transmitter on a DAC's output.

## Hardware notes

- IO4/5/6 are shared by the SD card, LoRa SPI and the onboard speaker, so the SD card is off by default
  (`ENABLE_SD`) and LoRa is a separate boot mode (More > Doomsday mode > Arm, then switches S1 S0 = `01`).
- The screen is driven with ESP-IDF's `esp_lcd` RGB driver and bounce buffers; LovyanGFX is only used to decode
  images. Touch (GT911) and the helper MCU (backlight, speaker switch, I2C 0x30) share one I2C bus via `Wire`.
- Internal RAM is the tight resource (Wi-Fi, TLS and the display's DMA buffers need it), so big buffers live in
  PSRAM: LVGL's heap, TLS, photos, logos, station lists.

## Code layout

| File | Role |
|---|---|
| `internetRadio.ino` | Display + touch setup, navigation, the network task, the UI tick, setup / loop, serial console |
| `stations.h`, `audio_engine.h`, `radio_browser.h`, `radio_ui.h` | Radio: station data, the audio task, online lookups, the Radio page |
| `podcasts.h` | Followed shows, RSS reader, "New", Apple directory search |
| `weather.h`, `agenda.h`, `house.h`, `timers.h`, `ambient.h`, `voice.h`, `lora.h`, `remote.h` | One feature each |
| `art.h` | Logo / artwork thumbnails (download, decode, cache on flash) |
| `ui_*.h` | The screens (Wi-Fi, Home, Podcasts, Find stations, weather location, rename, More) |
| `ui_common.h`, `net_util.h`, `rtc.h` | Shared widgets, HTTP helpers, real-time clock |

Tasks: the UI (LVGL, core 1), `netTask` (every HTTP request, core 1), audio (core 0), voice (core 0). Data that the
UI owns (favourites, podcasts, search results) is only changed by the UI; `netTask` works on copies and hands
results back - see the comments at the top of `stations.h` and `radio_browser.h`.

**Serial console** (115200 baud): `play <url>`, `ep <url>` (as a podcast episode), `seek <+/-seconds>`, `pos`,
`mic` (record and print the levels), `remote` (print the phone remote link).

## Known limitations

- Voice needs Home Assistant (Whisper) for speech-to-text; no wake word yet (tap the mic).
- TLS certificates aren't checked (`setInsecure`) - fine on a home network, don't port-forward the hub.
- iCal: no "2nd Tuesday"-style rules, edited single instances or non-UK time zones.
- Video streams (The Lot Radio's live stream, Boiler Room) can't be played; Mixcloud shows only in their own player.
- Podcast episodes start from the beginning (no resume position yet).
