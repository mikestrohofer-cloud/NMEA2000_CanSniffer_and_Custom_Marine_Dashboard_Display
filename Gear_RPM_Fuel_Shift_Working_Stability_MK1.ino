#include <SPI.h>
#include <mcp_can.h>
#include <U8x8lib.h>

// ================== CONFIG ==================
#define CAN_CS_PIN   10
#define CAN_INT_PIN  2           // MCP2515 INT -> Nano D2
#define CAN_SPEED    CAN_250KBPS
#define CAN_CLOCK    MCP_8MHZ

// OLED text-only (still useful locally)
U8X8_SSD1306_128X64_NONAME_HW_I2C oled(U8X8_PIN_NONE);

// PGNs we care about
static const uint32_t PGN_127489   = 127489UL; // Fuel rate (Fast Packet)
static const uint32_t PGN_SUZ_GEAR = 65298UL;  // Suzuki proprietary (0xFF12)
static const uint32_t PGN_127488   = 127488UL; // Engine params rapid update (you saw trim + RPM here)

// Data freshness (ms)
#define STALE_FUEL_MS 1500UL
#define STALE_GEAR_MS 1000UL

#define OLED_PERIOD_MS 150UL

// CSV output to mini screen
#define CSV_PERIOD_MS 50UL        // 20 Hz
#define CAN_STALE_MS  1000UL      // if CAN stale -> force Neutral / zeroes

// Optional: filter by source address (0xFF = accept any)
#define ENGINE_SA_FILTER 0xFF

// ================== RPM CALIBRATION ==================
// Empirical fix: Suzuki 127488 RPM reads high (~900 shows ~650 actual)
#define RPM_CAL 0.72f

// ================== CAN ==================
MCP_CAN CAN0(CAN_CS_PIN);

// ================== INT FLAG ==================
volatile bool canIntFlag = false;
void onCanInt() { canIntFlag = true; }

// ================== VALUE STATE ==================
volatile float fuel_gph = 0.0f;
volatile char  gearCharV = 'N';      // 'F' 'N' 'R'
volatile uint8_t engine_instance = 0;

volatile int rpm_calibrated = 0;
uint32_t last_rpm_ms = 0;

volatile int trim_percent = -1;      // optional: used for local OLED only
uint32_t last_trim_ms = 0;

uint32_t last_fuel_ms = 0;
uint32_t last_gear_ms = 0;

uint8_t lastSuzB4 = 0x00;

// ================== HELPERS ==================
static inline uint32_t extractPGN(uint32_t canId) {
  uint8_t dp = (canId >> 24) & 0x01;
  uint8_t pf = (canId >> 16) & 0xFF;
  uint8_t ps = (canId >> 8)  & 0xFF;
  uint32_t pgn = ((uint32_t)dp << 16) | ((uint32_t)pf << 8);
  if (pf >= 240) pgn |= ps;
  return pgn;
}

static inline uint8_t extractSA(uint32_t canId) {
  return (uint8_t)(canId & 0xFF);
}

static inline uint16_t u16le(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline int16_t s16le(const uint8_t* p) {
  return (int16_t)u16le(p);
}

// Pi expects: {1:"R", 2:"N", 3:"F"}
static int gearToShiftIndicator(char g) {
  if (g == 'R') return 1;
  if (g == 'N') return 2;
  if (g == 'F') return 3;
  return 2; // safe default Neutral
}

// Suzuki gear decode (proven on your system)
static char decodeSuzukiGearB4(uint8_t b4) {
  if (b4 == 0x7F) return 'N';          // special neutral case
  uint8_t top = b4 & 0xC0;
  if (top == 0xC0) return 'F';
  if (top == 0x80) return 'N';
  if (top == 0x40) return 'R';
  return '?';
}

// ================== 127488 RPM + TRIM ==================
// Note: you're seeing trim% track correctly from this PGN on your Suzuki gateway.
// RPM bytes are standard (bytes 1..2, 0.25 RPM/bit). Trim byte is vendor-ish;
// based on your captures it behaves like 0..100 (byte 5). We'll keep it simple.
static void decode127488_rpm_trim(const uint8_t* d, uint8_t len) {
  if (len < 6) return;

  // RPM: bytes 1..2 little-endian
  uint16_t raw = (uint16_t)d[1] | ((uint16_t)d[2] << 8);
  if (raw != 0xFFFF && raw != 0x7FFF) {
    float rpm = raw * 0.25f;
    rpm *= RPM_CAL;
    rpm_calibrated = (int)(rpm + 0.5f);
    last_rpm_ms = millis();
  }

  // TRIM: based on your logs, byte 5 tracks 0..100-ish.
  // If it ever goes 0xFF, treat as invalid.
  uint8_t t = d[5];
  if (t != 0xFF) {
    trim_percent = (int)t;
    last_trim_ms = millis();
  }
}

// ================== FAST PACKET (127489) ==================
struct FastPacket {
  bool active = false;
  uint8_t sa = 0xFF;
  uint8_t seq = 0;
  uint8_t expectedLen = 0;
  uint8_t buf[32];
  uint8_t filled = 0;
  uint32_t lastMs = 0;

  void reset() { active=false; sa=0xFF; seq=0; expectedLen=0; filled=0; lastMs=0; }

  void begin(uint8_t _sa, uint8_t _seq, uint8_t totalLen, const uint8_t* frame) {
    reset();
    active = true; sa=_sa; seq=_seq; expectedLen=totalLen; lastMs=millis();
    uint8_t toCopy = 6;
    if (toCopy > expectedLen) toCopy = expectedLen;
    for (uint8_t i=0;i<toCopy;i++) buf[i] = frame[2+i];
    filled = toCopy;
  }

  void add(uint8_t _sa, uint8_t _seq, uint8_t frameIdx, const uint8_t* frame) {
    if (!active) return;
    if (_sa != sa) return;
    if (_seq != seq) return;
    lastMs = millis();

    uint16_t base = 6 + (uint16_t)(frameIdx - 1) * 7;
    if (base >= expectedLen) return;

    uint8_t toCopy = 7;
    if (base + toCopy > expectedLen) toCopy = expectedLen - base;
    for (uint8_t i=0;i<toCopy;i++) buf[base+i] = frame[1+i];

    uint16_t newFilled = base + toCopy;
    if (newFilled > filled) filled = (uint8_t)newFilled;
  }

  bool complete() const { return active && expectedLen > 0 && filled >= expectedLen; }
};

FastPacket fp489;

static void decode127489_payload(const uint8_t* p, uint8_t n) {
  if (n < 11) return;
  engine_instance = p[0];

  // Fuel rate at payload offset 9..10, 0.1 L/h signed
  int16_t fuel_raw = s16le(&p[9]);

  // NA guards
  if (fuel_raw == (int16_t)0x7FFF || fuel_raw == -1) return;

  float fuel_Lph = fuel_raw * 0.1f;
  if (fuel_Lph < 0) fuel_Lph = 0;

  fuel_gph = fuel_Lph * 0.264172052f; // L/h -> GPH
  last_fuel_ms = millis();
}

static void handle127489_frame(uint8_t sa, const uint8_t* d, uint8_t len) {
  if (len < 8) return;

  if (fp489.active && (millis() - fp489.lastMs) > 250UL) fp489.reset();

  uint8_t hdr = d[0];
  uint8_t seq = (hdr >> 5) & 0x07;
  uint8_t frameIdx = hdr & 0x1F;

  if (frameIdx == 0) {
    uint8_t totalLen = d[1];
    if (totalLen > sizeof(fp489.buf)) { fp489.reset(); return; }
    fp489.begin(sa, seq, totalLen, d);
  } else {
    fp489.add(sa, seq, frameIdx, d);
  }

  if (fp489.complete()) {
    decode127489_payload(fp489.buf, fp489.expectedLen);
    fp489.reset();
  }
}

// ================== SUZUKI GEAR (65298) ==================
static void decodeSuzukiGear(const uint8_t* d, uint8_t len) {
  if (len < 5) return; // we use d[4]
  lastSuzB4 = d[4];
  char g = decodeSuzukiGearB4(lastSuzB4);
  if (g == 'F' || g == 'N' || g == 'R') {
    gearCharV = g;
    last_gear_ms = millis();
  }
}

// ================== OLED ==================
static void oledBoot(const char* l1, const char* l2) {
  oled.clear();
  oled.drawString(0, 0, l1);
  oled.drawString(0, 2, l2);
}

static void oledRender() {
  uint32_t now = millis();
  bool fuel_stale = (now - last_fuel_ms) > STALE_FUEL_MS;
  bool gear_stale = (now - last_gear_ms) > STALE_GEAR_MS;
  bool rpm_stale  = (now - last_rpm_ms)  > CAN_STALE_MS;

  char line0[17], line2[17], line4[17], line6[17];

  if (fuel_stale) snprintf(line0, sizeof(line0), "FUEL: ---- GPH");
  else {
    int whole = (int)fuel_gph;
    int frac  = (int)((fuel_gph - whole) * 100.0f + 0.5f);
    if (frac < 0) frac = 0;
    if (frac > 99) frac = 99;
    snprintf(line0, sizeof(line0), "FUEL:%d.%02d GPH", whole, frac);
  }

  if (gear_stale) snprintf(line2, sizeof(line2), "GEAR:? RPM:----");
  else {
    char gbuf[2] = { gearCharV, 0 };
    if (rpm_stale) snprintf(line2, sizeof(line2), "GEAR:%s RPM:----", gbuf);
    else snprintf(line2, sizeof(line2), "GEAR:%s RPM:%4d", gbuf, rpm_calibrated);
  }

  // Show trim on line4
  if ((now - last_trim_ms) > CAN_STALE_MS || trim_percent < 0) {
    snprintf(line4, sizeof(line4), "TRIM: --%%  B4:%02X", lastSuzB4);
  } else {
    int tp = trim_percent;
    if (tp < 0) tp = 0;
    if (tp > 100) tp = 100;
    snprintf(line4, sizeof(line4), "TRIM:%3d%% B4:%02X", tp, lastSuzB4);
  }

  snprintf(line6, sizeof(line6), "AGE F:%lu G:%lu",
           fuel_stale ? 9999UL : (unsigned long)(now - last_fuel_ms),
           gear_stale ? 9999UL : (unsigned long)(now - last_gear_ms));

  oled.clear();
  oled.drawString(0, 0, line0);
  oled.drawString(0, 2, line2);
  oled.drawString(0, 4, line4);
  oled.drawString(0, 6, line6);
}

// ================== CAN DRAIN ==================
static void processCAN() {
  unsigned long rxId = 0;
  unsigned char len = 0;
  unsigned char buf[8];

  while (CAN0.checkReceive() == CAN_MSGAVAIL) {
    CAN0.readMsgBuf(&rxId, &len, buf);

    uint32_t id = (uint32_t)rxId;
    uint8_t sa = extractSA(id);
    if (ENGINE_SA_FILTER != 0xFF && sa != ENGINE_SA_FILTER) continue;

    uint32_t pgn = extractPGN(id);

    if (pgn == PGN_127489) {
      handle127489_frame(sa, buf, len);
    } else if (pgn == PGN_SUZ_GEAR) {
      decodeSuzukiGear(buf, len);
    } else if (pgn == PGN_127488) {
      decode127488_rpm_trim(buf, len);
    }
  }
}

// ================== CSV OUTPUT ==================
static void sendCSV() {
  // Pi expects: rudder_raw,rpm_raw,shift_indicator,fuel_gph
  uint32_t now = millis();

  // Map TRIM% (0..100) -> rudder_raw (0..4095)
  int rudder_raw = 2047; // centered
  if (now - last_trim_ms <= CAN_STALE_MS && trim_percent >= 0) {
    int tp = trim_percent;
    if (tp < 0) tp = 0;
    if (tp > 100) tp = 100;
    rudder_raw = (tp * 4095L) / 100L;
  }

  // HOLD last RPM instead of forcing 0 when stale
  // If we've never received RPM (last_rpm_ms == 0), send 0.
  int rpm_raw = (last_rpm_ms == 0) ? 0 : rpm_calibrated;

  // If gear CAN is stale, force Neutral (keep this behavior)
  if (now - last_gear_ms > CAN_STALE_MS) {
    gearCharV = 'N';
  }
  int shift_indicator = gearToShiftIndicator(gearCharV);

  // HOLD last fuel instead of forcing 0 when stale
  // If we've never received fuel (last_fuel_ms == 0), send 0.0.
  float fuelOut = (last_fuel_ms == 0) ? 0.0f : fuel_gph;

  Serial.print(rudder_raw);
  Serial.print(',');
  Serial.print(rpm_raw);
  Serial.print(',');
  Serial.print(shift_indicator);
  Serial.print(',');
  Serial.println(fuelOut, 3);
}


// ================== MAIN ==================
void setup() {
  // IMPORTANT: mini screen baud
  Serial.begin(57600);
  delay(200);

  // Prime the mini screen with valid frames immediately
  for (uint8_t i = 0; i < 20; i++) {
    Serial.println(F("2047,0,2,0.000"));
    delay(20);
  }

  oled.begin();
  oled.setFont(u8x8_font_chroma48medium8_r);
  oledBoot("N2K GAUGE", "Boot...");

  pinMode(CAN_INT_PIN, INPUT_PULLUP);

  // CAN init with retries
  uint32_t t0 = millis();
  bool canOk = false;
  while (millis() - t0 < 5000UL) {
    if (CAN0.begin(MCP_ANY, CAN_SPEED, CAN_CLOCK) == CAN_OK) { canOk = true; break; }
    delay(200);
  }

  if (canOk) {
    // Accept-all (avoid mystery filter state)
    CAN0.init_Mask(0, 1, 0x00000000);
    CAN0.init_Mask(1, 1, 0x00000000);
    for (uint8_t i = 0; i < 6; i++) CAN0.init_Filt(i, 1, 0x00000000);

    CAN0.setMode(MCP_NORMAL);

    attachInterrupt(digitalPinToInterrupt(CAN_INT_PIN), onCanInt, FALLING);
    oledBoot("CAN OK", "CSV live");
  } else {
    oledBoot("CAN FAIL", "CSV live");
  }

  last_fuel_ms = last_gear_ms = last_rpm_ms = last_trim_ms = 0;
}

void loop() {
  if (canIntFlag) {
    canIntFlag = false;
    processCAN();
  }
  // Safety drain
  processCAN();

  // OLED refresh
  static uint32_t lastOledMs = 0;
  uint32_t now = millis();
  if (now - lastOledMs >= OLED_PERIOD_MS) {
    lastOledMs = now;
    oledRender();
  }

  // CSV output for mini screen
  static uint32_t lastCsv = 0;
  if (now - lastCsv >= CSV_PERIOD_MS) {
    lastCsv = now;
    sendCSV();
  }
}


