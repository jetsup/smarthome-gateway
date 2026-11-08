// Gateway clock. Best source first:
//   1. DS3231 RTC on I2C (checked for every few seconds, so it can be plugged in later)
//   2. internet time (NTP) once WiFi is up
//   3. the hub's clock (TIME: line)
//   4. none: timestamps fall back to uptime and are fixed once a real time is known
#include <Wire.h>
#include <time.h>

#include "Gateway.h"

// timeBaseEpoch / timeBaseMillis / timeSource live in espnow.cpp (they are also
// broadcast to the nodes): source 0 none, 1 rtc, 2 hub, 3 ntp.
extern uint32_t timeBaseEpoch;
extern uint32_t timeBaseMillis;
extern uint8_t timeSource;

static const uint8_t RTC_ADDR = 0x68;
static int rtcSda = 21, rtcScl = 22;
static bool wireStarted = false;
bool rtcPresent = false;
bool rtcValid = false;  // oscillator ran without interruption since it was last set

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static uint32_t toEpoch(int y, int m, int d, int hh, int mm, int ss) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  return (uint32_t)(days * 86400L + hh * 3600L + mm * 60L + ss);
}

static bool rtcProbe() {
  Wire.beginTransmission(RTC_ADDR);
  return Wire.endTransmission() == 0;
}

static bool rtcReadEpoch(uint32_t& out) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)RTC_ADDR, 7) != 7) return false;
  uint8_t s = Wire.read(), mi = Wire.read(), h = Wire.read();
  Wire.read();  // weekday
  uint8_t d = Wire.read(), mo = Wire.read(), y = Wire.read();
  out = toEpoch(2000 + bcd2bin(y), bcd2bin(mo & 0x1F), bcd2bin(d), bcd2bin(h & 0x3F), bcd2bin(mi), bcd2bin(s & 0x7F));
  return true;
}

static bool rtcOscStopped() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x0F);
  if (Wire.endTransmission(false) != 0) return true;
  if (Wire.requestFrom((int)RTC_ADDR, 1) != 1) return true;
  return (Wire.read() & 0x80) != 0;
}

static void rtcWriteEpoch(uint32_t epoch) {
  time_t t = epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0);
  Wire.write(bin2bcd(tmv.tm_sec));
  Wire.write(bin2bcd(tmv.tm_min));
  Wire.write(bin2bcd(tmv.tm_hour));
  Wire.write(bin2bcd(tmv.tm_wday + 1));
  Wire.write(bin2bcd(tmv.tm_mday));
  Wire.write(bin2bcd(tmv.tm_mon + 1));
  Wire.write(bin2bcd(tmv.tm_year - 100));
  Wire.endTransmission();
  // clear the "oscillator stopped" flag
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)0x0F);
  Wire.write((uint8_t)0);
  Wire.endTransmission();
}

void clockConfigure(int sda, int scl) {
  rtcSda = sda;
  rtcScl = scl;
  if (wireStarted) Wire.end();
  Wire.begin(rtcSda, rtcScl, 100000);
  Wire.setTimeOut(50);
  wireStarted = true;
  rtcPresent = false;
}

void clockInit() {
  rtcSda = prefs.getInt("rtc_sda", 21);
  rtcScl = prefs.getInt("rtc_scl", 22);
  clockConfigure(rtcSda, rtcScl);
}

uint32_t uptimeSec() { return millis() / 1000; }

bool clockReal() { return timeBaseEpoch != 0; }

uint32_t clockNow() {
  if (timeBaseEpoch == 0) return 0;
  return timeBaseEpoch + (millis() - timeBaseMillis) / 1000;
}

static void setBase(uint32_t epoch, uint8_t src) {
  bool first = timeBaseEpoch == 0;
  timeBaseEpoch = epoch;
  timeBaseMillis = millis();
  timeSource = src;
  if (first) {
    Serial.printf("Clock: real time known (%u, source %u)\n", epoch, src);
    dataLogTimeKnown();  // fix timestamps recorded on uptime so far
    broadcastTime();
  }
}

// The hub's clock is only used when neither the RTC nor the internet can tell the time.
void clockFromHub(uint32_t epoch, bool hubHasRtc) {
  if (timeSource == 1 || timeSource == 3) return;
  setBase(epoch, hubHasRtc ? 1 : 2);
  timeSource = hubHasRtc ? 1 : 2;
}

static bool ntpStarted = false;

void clockLoop() {
  static unsigned long last = 0;
  if (millis() - last < 5000) return;
  last = millis();

  // 1. RTC: keep checking that it is there (it can be added or fail at any time)
  static unsigned long lastProbe = 0;
  if (!rtcPresent || millis() - lastProbe > 60000) {
    lastProbe = millis();
    bool was = rtcPresent;
    rtcPresent = rtcProbe();
    if (rtcPresent != was) Serial.printf("Clock: RTC %s\n", rtcPresent ? "found" : "not found");
  }
  if (rtcPresent) {
    rtcValid = !rtcOscStopped();
    uint32_t e;
    if (rtcValid && rtcReadEpoch(e) && e > 1600000000UL) {
      static unsigned long lastSync = 0;
      if (timeSource != 1 || millis() - lastSync > 600000UL) {
        lastSync = millis();
        setBase(e, 1);
        timeSource = 1;
      }
      return;
    }
  }

  // 2. internet time
  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpStarted) {
      configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
      ntpStarted = true;
    }
    time_t now = time(nullptr);
    if (now > 1600000000L) {
      static unsigned long lastNtp = 0;
      if (timeSource != 3 || millis() - lastNtp > 600000UL) {
        lastNtp = millis();
        setBase((uint32_t)now, 3);
        timeSource = 3;
      }
      // an RTC that lost power (or is new) gets the right time
      if (rtcPresent && !rtcValid) {
        rtcWriteEpoch((uint32_t)now);
        Serial.println("Clock: RTC set from internet time");
      }
    }
  } else {
    ntpStarted = false;  // start over when the network comes back
  }
  // 3./4. hub time arrives through clockFromHub(); until then timestamps use uptime.
}
