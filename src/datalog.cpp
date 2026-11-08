// Data log: every sensor change a node reports is stored here with its time, so
// nothing is lost while the hub is unreachable. Records are uploaded to the hub in
// acknowledged batches and removed once the hub has them.
//
//  * storage: internal flash (LittleFS) by default, or an SD card, chosen in the
//    gateway settings. The log is a ring of fixed-size segment files; when the
//    space limit is reached the oldest segment is dropped, so it never fills up.
//  * time: real time (RTC / internet / hub) when known, otherwise uptime. Uptime
//    records are fixed up once any real time is known (see resolve()).
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

#include "Gateway.h"

struct __attribute__((packed)) LogRec {
  uint32_t ts;     // epoch (flags bit0) or uptime seconds
  uint32_t dev;    // node id
  int32_t val;
  uint8_t cap;     // module index, 255 = the node's overall value
  uint8_t ch;
  uint8_t kind;    // 1 reading, 2 node value
  uint8_t flags;   // bit0 real time, bits 2..7 boot id (mod 64)
};
static_assert(sizeof(LogRec) == 16, "LogRec must be 16 bytes");

static const size_t SEG_BYTES = 16384;  // 1024 records per segment file
static const int BOOT_SLOTS = 64;
static const int BATCH = 8;

static fs::FS* store = nullptr;
static bool usingSd = false;
static int cfgStore = 0;      // 0 flash, 1 sd
static int cfgMaxKb = 256;
static int cfgSdCs = 5;
static uint8_t bootId = 0;
static uint32_t anchors[BOOT_SLOTS];   // epoch at which each boot started (0 = unknown)
static uint32_t lastUp[BOOT_SLOTS];    // last uptime seen in each boot

static uint32_t minSeq = 0, maxSeq = 0;  // oldest / newest existing segment file
static uint32_t nextSeq = 0;             // number the next new segment gets (never reused)
static bool haveFiles = false;
static uint32_t readSeq = 0;
static size_t readOff = 0;

static LogRec pend[16];
static int pendN = 0;
static unsigned long lastFlush = 0;

// batch in flight
static uint32_t batchSeq = 0;
static int batchN = 0;
static unsigned long batchSentAt = 0;
static uint32_t sentTotal = 0;

// per channel throttle
struct Last { uint32_t dev; uint8_t cap, ch; int32_t val; uint32_t ts; bool used; };
static Last lastSeen[96];

static String segPath(uint32_t seq) { return "/log/s" + String(seq) + ".bin"; }

static void scanFiles() {
  haveFiles = false;
  minSeq = 0xFFFFFFFF;
  maxSeq = 0;
  File root = store->open("/log");
  if (!root) return;
  File f = root.openNextFile();
  while (f) {
    String n = f.name();
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    if (n.startsWith("s") && n.endsWith(".bin")) {
      uint32_t s = (uint32_t)strtoul(n.c_str() + 1, nullptr, 10);
      haveFiles = true;
      if (s < minSeq) minSeq = s;
      if (s > maxSeq) maxSeq = s;
    }
    f = root.openNextFile();
  }
  if (!haveFiles) { minSeq = 0; maxSeq = 0; }
  nextSeq = haveFiles ? maxSeq + 1 : prefs.getUInt("log_next", 0);
}

static size_t sizeOf(uint32_t seq) {
  File f = store->open(segPath(seq), "r");
  if (!f) return 0;
  size_t n = f.size();
  f.close();
  return n;
}

static bool mountStore() {
  usingSd = false;
  store = nullptr;
  if (cfgStore == 1) {
    SPI.begin();
    if (SD.begin(cfgSdCs)) {
      store = &SD;
      usingSd = true;
    } else {
      Serial.println("DataLog: SD card not found, using flash");
    }
  }
  if (!store) {
    if (!LittleFS.begin(true)) {
      Serial.println("DataLog: flash filesystem failed");
      return false;
    }
    store = &LittleFS;
  }
  store->mkdir("/log");
  scanFiles();
  return true;
}

static void saveCursor() {
  prefs.putUInt("log_rs", readSeq);
  prefs.putUInt("log_ro", (uint32_t)readOff);
}

static void saveSessions() {
  prefs.putBytes("anch", anchors, sizeof(anchors));
  prefs.putBytes("lastup", lastUp, sizeof(lastUp));
}

void dataLogConfigure(int storeKind, int maxKb, int sdCs) {
  cfgStore = storeKind == 1 ? 1 : 0;
  cfgSdCs = sdCs > 0 ? sdCs : 5;
  int cap = cfgStore == 1 ? 1048576 : 1024;
  cfgMaxKb = maxKb < 32 ? 32 : (maxKb > cap ? cap : maxKb);
  prefs.putInt("log_store", cfgStore);
  prefs.putInt("log_kb", cfgMaxKb);
  prefs.putInt("sd_cs", cfgSdCs);
}

void dataLogInit() {
  cfgStore = prefs.getInt("log_store", 0);
  cfgMaxKb = prefs.getInt("log_kb", 256);
  cfgSdCs = prefs.getInt("sd_cs", 5);
  memset(anchors, 0, sizeof(anchors));
  memset(lastUp, 0, sizeof(lastUp));
  prefs.getBytes("anch", anchors, sizeof(anchors));
  prefs.getBytes("lastup", lastUp, sizeof(lastUp));
  bootId = (uint8_t)(prefs.getUChar("boot_n", 0) + 1) % BOOT_SLOTS;
  prefs.putUChar("boot_n", bootId);
  anchors[bootId] = 0;
  lastUp[bootId] = 0;
  memset(lastSeen, 0, sizeof(lastSeen));
  if (!mountStore()) return;
  readSeq = prefs.getUInt("log_rs", 0);
  readOff = prefs.getUInt("log_ro", 0);
  if (!haveFiles || readSeq < minSeq || readSeq > maxSeq) {
    readSeq = minSeq;
    readOff = 0;
  }
  Serial.printf("DataLog: %s, limit %d KB, boot %u, %s\n", usingSd ? "SD card" : "flash", cfgMaxKb, bootId,
                haveFiles ? "records waiting" : "empty");
  saveSessions();
}

// Real time became known: boots that ran on uptime can be dated from now on.
void dataLogTimeKnown() {
  uint32_t e = clockNow();
  if (e == 0) return;
  uint32_t up = uptimeSec();
  anchors[bootId] = e > up ? e - up : e;
  saveSessions();
}

static void appendRecords(const LogRec* r, int n) {
  if (!store || n <= 0) return;
  uint32_t segs = (uint32_t)((cfgMaxKb * 1024UL) / SEG_BYTES);
  if (segs < 2) segs = 2;
  uint32_t cur = maxSeq;
  size_t sz = haveFiles ? sizeOf(cur) : 0;
  if (!haveFiles || sz + n * sizeof(LogRec) > SEG_BYTES) {
    cur = nextSeq++;
    if (!haveFiles) { minSeq = cur; readSeq = cur; readOff = 0; }
    haveFiles = true;
    maxSeq = cur;
    // out of space: drop the oldest data so new data can be stored
    while (maxSeq - minSeq + 1 > segs) {
      store->remove(segPath(minSeq));
      minSeq++;
    }
    if (readSeq < minSeq) { readSeq = minSeq; readOff = 0; saveCursor(); }
  }
  File f = store->open(segPath(cur), FILE_APPEND);
  if (!f) { Serial.println("DataLog: write failed"); return; }
  f.write((const uint8_t*)r, n * sizeof(LogRec));
  f.close();
}

void dataLogFlush() {
  if (pendN > 0) {
    appendRecords(pend, pendN);
    pendN = 0;
  }
  lastFlush = millis();
}

static void push(uint32_t dev, uint8_t cap, uint8_t ch, uint8_t kind, int32_t val) {
  // Throttle: a changed value is logged at most every 30 s, an unchanged one every 10 min.
  uint32_t now = millis() / 1000;
  Last* slot = nullptr;
  Last* free = nullptr;
  for (auto& l : lastSeen) {
    if (l.used && l.dev == dev && l.cap == cap && l.ch == ch) { slot = &l; break; }
    if (!l.used && !free) free = &l;
  }
  if (!slot) {
    slot = free ? free : &lastSeen[(dev + cap + ch) % 96];
    slot->used = true; slot->dev = dev; slot->cap = cap; slot->ch = ch; slot->ts = 0; slot->val = val + 1;
  }
  bool changed = slot->val != val;
  uint32_t age = now - slot->ts;
  if (slot->ts != 0 && !((changed && age >= 30) || age >= 600)) return;
  slot->val = val;
  slot->ts = now ? now : 1;

  LogRec r;
  bool real = clockReal();
  r.ts = real ? clockNow() : uptimeSec();
  r.dev = dev; r.val = val; r.cap = cap; r.ch = ch; r.kind = kind;
  r.flags = (real ? 1 : 0) | (uint8_t)(bootId << 2);
  lastUp[bootId] = uptimeSec();
  if (pendN >= 16) dataLogFlush();
  pend[pendN++] = r;
}

// Called for every frame received from a node.
void dataLogFrame(const uint8_t* d, int len) {
  if (len < 8 || d[0] != 0xAA) return;
  uint8_t type = d[1];
  uint32_t dev = (uint32_t)d[2] | ((uint32_t)d[3] << 8) | ((uint32_t)d[4] << 16) | ((uint32_t)d[5] << 24);
  if (type == MSG_TELEMETRY && len >= 9) {
    push(dev, 255, 0, 2, (int32_t)((uint16_t)d[6] | ((uint16_t)d[7] << 8)));
  } else if (type == MSG_READINGS && len >= 8) {
    int n = d[6];
    if (len < 8 + n) return;
    for (int i = 0; i + 6 <= n; i += 6) {
      const uint8_t* p = d + 7 + i;
      int32_t v = (int32_t)((uint32_t)p[2] | ((uint32_t)p[3] << 8) | ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24));
      push(dev, p[0], p[1], 1, v);
    }
  }
}

// Turn a stored record's timestamp into real time. est=true when it had to be
// worked out from the next boot that did know the time (hub/clock offline then).
static bool resolve(const LogRec& r, uint32_t& epoch, bool& est) {
  est = false;
  if (r.flags & 1) { epoch = r.ts; return true; }
  uint8_t b = r.flags >> 2;
  if (anchors[b]) { epoch = anchors[b] + r.ts; return true; }
  // walk forward to the next boot with a known start and subtract the boots in between
  for (int step = 1; step < BOOT_SLOTS; step++) {
    uint8_t nb = (b + step) % BOOT_SLOTS;
    if (nb == bootId && !anchors[nb]) return false;
    if (anchors[nb]) {
      uint32_t t = anchors[nb];
      for (int k = 1; k <= step; k++) {
        uint8_t mid = (b + step - k) % BOOT_SLOTS;
        uint32_t len = lastUp[mid];
        t = t > len ? t - len : 0;
      }
      epoch = t + r.ts;
      est = true;
      return true;
    }
  }
  return false;
}

static void sendBatch() {
  dataLogFlush();
  if (!haveFiles) return;
  File f = store->open(segPath(readSeq), "r");
  if (!f) {
    // segment vanished (clipped): continue with the next one
    if (readSeq < maxSeq) { readSeq++; readOff = 0; saveCursor(); }
    return;
  }
  size_t size = f.size();
  f.seek(readOff);
  String line;
  int n = 0;
  LogRec r;
  while (n < BATCH && f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
    uint32_t epoch; bool est;
    if (!resolve(r, epoch, est)) break;  // time still unknown: keep it for later
    if (n) line += ';';
    line += String(epoch) + "," + (est ? "1" : "0") + "," + String(r.dev) + "," + String(r.cap) + "," + String(r.ch) + "," + String(r.kind) + "," + String(r.val);
    n++;
  }
  f.close();
  if (n == 0) {
    // nothing left in this segment: if it is not the one being written, drop it
    if (readOff >= size && readSeq < maxSeq) {
      store->remove(segPath(readSeq));
      readSeq++;
      readOff = 0;
      minSeq = readSeq;
      saveCursor();
    }
    return;
  }
  batchSeq++;
  batchN = n;
  batchSentAt = millis();
  tcpClient.printf("LOG:%u:%d:%s\n", batchSeq, n, line.c_str());
}

// The hub confirmed a batch: advance the cursor and free the space it used.
void dataLogAck(uint32_t seq) {
  if (seq != batchSeq || batchN == 0) return;
  readOff += (size_t)batchN * sizeof(LogRec);
  sentTotal += batchN;
  batchN = 0;
  size_t size = sizeOf(readSeq);
  if (readOff >= size) {
    if (readSeq < maxSeq) {
      store->remove(segPath(readSeq));
      readSeq++;
      minSeq = readSeq;
      readOff = 0;
      saveCursor();
    } else if (pendN == 0) {
      // the segment being written is fully uploaded: empty it
      store->remove(segPath(readSeq));
      haveFiles = false;
      readOff = 0;
      prefs.putUInt("log_next", nextSeq);
      saveCursor();
    }
  }
}

static uint32_t pendingRecords() {
  if (!haveFiles) return pendN;
  uint32_t bytes = 0;
  for (uint32_t s = readSeq; s <= maxSeq; s++) bytes += sizeOf(s);
  bytes = bytes > readOff ? bytes - readOff : 0;
  return bytes / sizeof(LogRec) + pendN;
}

void dataLogLoop() {
  if (!store) return;
  if (pendN > 0 && millis() - lastFlush > 5000) dataLogFlush();
  static unsigned long lastSessions = 0;
  if (millis() - lastSessions > 300000UL) {
    lastSessions = millis();
    lastUp[bootId] = uptimeSec();
    saveSessions();
    saveCursor();
  }
  if (!tcpClient.connected()) { batchN = 0; return; }

  static unsigned long lastTry = 0;
  if (batchN > 0) {
    if (millis() - batchSentAt > 10000) batchN = 0;  // no ack: send it again
    return;
  }
  if (millis() - lastTry > 1500) {
    lastTry = millis();
    sendBatch();
  }
  static unsigned long lastStat = 0;
  if (millis() - lastStat > 30000) {
    lastStat = millis();
    tcpClient.printf("LOGSTAT:%u:%d:%d:%d:%u:%u\n", pendingRecords(), usingSd ? 1 : 0, cfgMaxKb,
                     rtcPresent ? (rtcValid ? 2 : 1) : 0, (unsigned)timeSource, sentTotal);
  }
}
