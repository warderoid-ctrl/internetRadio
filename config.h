#pragma once
/*
  CROW HUB - your settings. Edit this file, nothing else needs touching to get going.
  Anything left empty simply switches that feature off (the page tells you what's missing).
*/

// ───────────── you & your house ─────────────
#define HUB_NAME          "Ed's Kitchen"
#define TZ_INFO           "GMT0BST,M3.5.0/1,M10.5.0"   // UK time with daylight saving
#define WEATHER_LAT       51.5072f                      // <-- change to your town (this is London)
#define WEATHER_LON       -0.1276f
#define WEATHER_PLACE     "London"
#define USE_MPH           1                              // 1 = mph, 0 = km/h
#define USE_CELSIUS       1
#define AUTOPLAY_ON_BOOT  1                              // start the last station when Wi-Fi comes up

// ───────────── audio output ─────────────
// 0 = the onboard speaker connector (SPK). Switches S1 S0 = 00. Use this for first tests with a small 4-8 ohm speaker.
// 1 = external I2S amp, e.g. your Adafruit I2S 3W bonnet (MAX98357A), wired to the two UART header sockets:
//        bonnet BCLK -> IO43  (UART0-OUT "TX0")      bonnet LRC -> IO44  (UART0-OUT "RX0")
//        bonnet DIN  -> IO19  (UART1-OUT "RX1")      bonnet VIN -> 5V, GND -> GND  (the headers only give 3V3 + GND,
//        so take 5V from the BAT/USB side or run the bonnet from 3V3 for a quieter output)
//     DIN shares IO19 with the microphone clock: the code re-routes it after each voice recording.
//     Side effects: IO43/44 are the USB-serial pins, so Serial debug output stops, and unplug BCLK/LRC while flashing.
//     With this on, the SD card (switches 11) and the microphone both keep working.
#define AUDIO_OUT_EXTERNAL 0
#define DAC_BCLK   43
#define DAC_LRC    44
#define DAC_DOUT   19

// ───────────── Home Assistant (optional) ─────────────
// Create a token: HA profile page > Security > Long-lived access tokens.
// HA_URL and HA_TOKEN live in secrets.h (not in git): copy secrets.example.h to secrets.h and fill them in.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef HA_URL
#define HA_URL     ""
#endif
#ifndef HA_TOKEN
#define HA_TOKEN   ""
#endif
#define HA_STT_ENTITY  "stt.faster_whisper" // speech-to-text entity (Whisper add-on). Needed for voice.
#define HA_TTS_ENTITY  "tts.piper"          // text-to-speech entity (Piper add-on). Optional: spoken replies.
#define HA_LANGUAGE    "en"

// Tiles for the HOUSE page. kind: T = toggle (light/switch/fan/input_boolean), S = sensor (read-only),
// B = button (scene / script / automation / button entity: tap to run)
struct HaTile { const char *label; const char *entity; char kind; };
static const HaTile HA_TILES[] = {
  {"Kitchen",   "light.kitchen",          'T'},
  {"Lounge",    "light.living_room",      'T'},
  {"Hall",      "light.hallway",          'T'},
  {"Kettle",    "switch.kettle",          'T'},
  {"Temp",      "sensor.living_room_temperature", 'S'},
  {"Front door","binary_sensor.front_door",'S'},
  {"Movie",     "scene.movie_time",       'B'},
  {"Goodnight", "script.goodnight",       'B'},
};

// ───────────── calendars / appointments (optional) ─────────────
// Option A (no Home Assistant needed): paste "secret iCal" links. Outlook: Calendar > Settings > Shared calendars
// > Publish a calendar > ICS link.  Google: Calendar settings > "Secret address in iCal format".
// Times are treated as UK local time (fine for a UK calendar).
static const char *ICS_URLS[] = {
  // "https://outlook.live.com/owa/calendar/....../calendar.ics",
  nullptr
};
// Option B: Home Assistant calendar entities (HA does the recurrence + timezone maths)
static const char *HA_CALENDARS[] = {
  // "calendar.family",
  nullptr
};
#define REMIND_MINUTES_BEFORE  10     // buzz + banner this long before an appointment (0 = off)

// ───────────── phone remote + HA pushes ─────────────
#define REMOTE_ENABLED 1
// The access token is made up randomly on first boot and kept on the hub; scan the QR on the More page.
// LAN only, don't port-forward this.

// ───────────── screens / photos ─────────────
#define AMBIENT_AFTER_MIN   5          // idle minutes before the photo frame / clock starts (0 = never)
#define AMBIENT_SLIDE_SEC   20
#define NIGHT_START_HOUR    22         // dim clock between these hours
#define NIGHT_END_HOUR      6
#define ENABLE_SD           0          // 1 = also read photos from the SD card. ONLY with switches S1 S0 = 11 AND an external DAC
                                       // (the SD card shares IO4/5/6 with the onboard speaker amp).
// Photos can also be uploaded from your phone to the hub's own flash (no SD needed): see the Remote page.

// ───────────── LoRa "when the world ends" mode ─────────────
#define LORA_FREQ_MHZ   868.0f         // UK/EU. (US 915.0)
#define LORA_NODE_NAME  "Ed"           // shown to other nodes
#define LORA_POWER_DBM  14
