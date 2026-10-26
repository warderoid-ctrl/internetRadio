#pragma once
// Battery-backed clock (PCF8563 @ 0x51). Keeps the time through power cuts and with no internet - handy for LoRa mode.

#define RTC_ADDR 0x51
static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static bool rtcRead(struct tm &t) {
  uint8_t b[7];
  if (i2cMx) xSemaphoreTake(i2cMx, portMAX_DELAY);
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x02);
  bool ok = (Wire.endTransmission(false) == 0) && (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7) == 7);
  if (ok) for (int i = 0; i < 7; i++) b[i] = Wire.read();
  if (i2cMx) xSemaphoreGive(i2cMx);
  if (!ok || (b[0] & 0x80)) return false;                       // VL bit: clock lost power, don't trust it
  memset(&t, 0, sizeof(t));
  t.tm_sec = bcd2dec(b[0] & 0x7F); t.tm_min = bcd2dec(b[1] & 0x7F); t.tm_hour = bcd2dec(b[2] & 0x3F);
  t.tm_mday = bcd2dec(b[3] & 0x3F); t.tm_wday = b[4] & 7; t.tm_mon = bcd2dec(b[5] & 0x1F) - 1;
  t.tm_year = bcd2dec(b[6]) + 100; t.tm_isdst = -1;
  return t.tm_year >= 124;                                      // 2024 or later
}

static void rtcWrite(const struct tm &t) {
  if (i2cMx) xSemaphoreTake(i2cMx, portMAX_DELAY);
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x02);
  Wire.write(dec2bcd(t.tm_sec)); Wire.write(dec2bcd(t.tm_min)); Wire.write(dec2bcd(t.tm_hour));
  Wire.write(dec2bcd(t.tm_mday)); Wire.write(t.tm_wday & 7); Wire.write(dec2bcd(t.tm_mon + 1)); Wire.write(dec2bcd(t.tm_year - 100));
  Wire.endTransmission();
  if (i2cMx) xSemaphoreGive(i2cMx);
}

// at boot: seed the system clock from the RTC (stored as UK local time)
static void rtcToSystem() {
  setenv("TZ", TZ_INFO, 1); tzset();
  struct tm t;
  if (!rtcRead(t)) return;
  struct timeval tv = { mktime(&t), 0 };
  settimeofday(&tv, nullptr);
}
// after NTP: store the correct local time into the RTC
static void systemToRtc() {
  struct tm t;
  if (getLocalTime(&t, 0)) rtcWrite(t);
}
