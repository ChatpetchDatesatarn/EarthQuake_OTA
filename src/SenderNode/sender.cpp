// ============================================================================
// ESP32-C3 Sender (Mesh + D7S + OLED Full Display + OTA Ready)
// Version: 2.1.1 - FIXED OTA Error Handling
// Fixed for ArduinoJson v7 API
// I2C: SDA=8, SCL=9
// D7S: I2C addr 0x55, SETTING_PIN=10
// OLED: SSD1306 128x64 @ 0x3C
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <painlessMesh.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Update.h>
#include <mbedtls/base64.h>

// --------------------------- Mesh config ---------------------------
#define MESH_PREFIX   "ESP32_MESH"
#define MESH_PASSWORD "mesh123456"
#define MESH_PORT     5555

// --------------------------- ThingsBoard identity ---------------------------
#define DEVICE_ACCESS_TOKEN "b0fOJJeK5yFIzeoTpWP6"
#define DEVICE_NAME         "NODE_C3_6"

// --------------------------- I2C pins / OLED ---------------------------
#define I2C_SDA     8
#define I2C_SCL     9
#define OLED_ADDR   0x3C
#define SCREEN_W    128
#define SCREEN_H    64
#define OLED_RESET  -1
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, OLED_RESET);

// --------------------------- D7S ---------------------------
#define D7S_ADDR       0x55
#define SETTING_PIN    10
#define D7S_READ_RETRY 3
#define D7S_DELAY_MS   5

// --------------------------- Timing ---------------------------
#define READ_INTERVAL_MS  1000UL
#define SEND_INTERVAL_MS  1000UL
#define OLED_UPDATE_MS    500UL

// ---- โหมดรีเซ็ตค่า ----
constexpr bool MODE_RESET_TIMEOUT = false;
constexpr bool MODE_RESET_ON_ZERO = true;
constexpr unsigned long RESET_TIMEOUT_MS = 30000UL;

// ---- OTA version ----
#ifndef FW_VERSION
  #define FW_VERSION "2.1.0"
#endif
#if defined(ROLE_WIFI_GATEWAY)
  #define ROLE_KEY "ROLE_WIFI_GATEWAY"
#elif defined(ROLE_MESH_GATEWAY)
  #define ROLE_KEY "ROLE_MESH_GATEWAY"
#elif defined(ROLE_SENDER_NODE)
  #define ROLE_KEY "ROLE_SENDER_NODE"
#else
  #define ROLE_KEY "UNKNOWN"
#endif

// ---- OTA Configuration (NEW) ----
#define OTA_BUFFER_SIZE         768     // 512 bytes + overhead
#define OTA_CHUNK_TIMEOUT       30000   // 30 seconds per chunk
#define OTA_MAX_FAILURES        5       // Maximum consecutive failures

// --------------------------- Globals ---------------------------
painlessMesh mesh;

struct D7SState {
  bool inited = false;
  uint16_t currentSI = 0;
  uint16_t currentPGA = 0;
  float currentTemp = 0.0f;
  uint16_t si_prev = 0;
  uint16_t pga_prev = 0;
  unsigned long lastChangeTime = 0;

  struct EventBlock {
    uint16_t si = 0;
    uint16_t pga = 0;
    float temp = 0.0f;
    bool hasData = false;
  } eventBlocks[3];
} d7s;

unsigned long tRead = 0;
unsigned long tSend = 0;
unsigned long tOledUpdate = 0;

// --------------------------- OTA State (ENHANCED) ---------------------------
static bool     ota_in_progress   = false;
static size_t   ota_expected_size = 0;
static size_t   ota_received      = 0;
static uint16_t OTA_CHUNK         = 512;  // Changed from 1024 to 512
static char     ota_new_version[16] = {0};
static uint32_t ota_gateway_node  = 0;
static unsigned long ota_last_chunk_time = 0;      // NEW
static int      ota_consecutive_failures = 0;      // NEW

// --------------------------- Statistics ---------------------------
struct Stats {
  unsigned long totalSent = 0;
  unsigned long totalFailed = 0;
  unsigned long lastSendTime = 0;
  bool meshConnected = false;
  int connectedNodes = 0;
} stats;

// --------------------------- Utils ---------------------------
uint16_t sanitize16(uint16_t v, uint16_t vmax) {
  if (v == 0xFFFF || v > vmax) return 0;
  return v;
}

float sanitizeTemp(float t) {
  if (t < -40.0f || t > 85.0f || isnan(t)) return 0.0f;
  return t;
}

// ============================================================================
// D7S helpers
// ============================================================================
uint16_t d7sRead16(uint16_t reg) {
  for (int i = 0; i < D7S_READ_RETRY; i++) {
    Wire.beginTransmission(D7S_ADDR);
    Wire.write(highByte(reg));
    Wire.write(lowByte(reg));
    uint8_t err = Wire.endTransmission(false);
    if (err == 0) {
      if (Wire.requestFrom((uint8_t)D7S_ADDR, (uint8_t)2) >= 2) {
        uint16_t v = (Wire.read() << 8) | Wire.read();
        return v;
      }
    }
    delay(D7S_DELAY_MS);
  }
  return 0xFFFF;
}

bool d7sInit() {
  pinMode(SETTING_PIN, OUTPUT);
  digitalWrite(SETTING_PIN, HIGH);
  delay(100);

  digitalWrite(SETTING_PIN, LOW);
  delay(3000);
  digitalWrite(SETTING_PIN, HIGH);
  delay(500);

  return d7sRead16(0x2000) != 0xFFFF;
}

void readEventBlock(uint8_t blockIndex) {
  if (blockIndex >= 3) return;

  uint16_t baseAddr = 0x3000 + (blockIndex * 0x100);

  uint16_t si = d7sRead16(baseAddr + 0x08);
  delay(D7S_DELAY_MS);
  uint16_t pga = d7sRead16(baseAddr + 0x0A);
  delay(D7S_DELAY_MS);
  uint16_t temp = d7sRead16(baseAddr + 0x06);

  d7s.eventBlocks[blockIndex].si = sanitize16(si, 999);
  d7s.eventBlocks[blockIndex].pga = sanitize16(pga, 4000);
  d7s.eventBlocks[blockIndex].temp = (temp == 0xFFFF) ? 0.0f : (temp / 10.0f);
  d7s.eventBlocks[blockIndex].hasData = (si > 0 || pga > 0);

  Serial.printf("Event Block %d: SI=%u, PGA=%u, Temp=%.1f\n",
                blockIndex, d7s.eventBlocks[blockIndex].si,
                d7s.eventBlocks[blockIndex].pga,
                d7s.eventBlocks[blockIndex].temp);
}

// ============================================================================
// อ่านค่า D7S พร้อมระบบรีเซ็ตอัตโนมัติ
// ============================================================================
void readD7S() {
  uint16_t siRaw  = d7sRead16(0x2000); delay(D7S_DELAY_MS);
  uint16_t pgaRaw = d7sRead16(0x2002); delay(D7S_DELAY_MS);
  uint16_t t10    = d7sRead16(0x3000 + 0x06);

  uint16_t si_current  = sanitize16(siRaw, 999);
  uint16_t pga_current = sanitize16(pgaRaw, 4000);

  d7s.currentTemp = (t10 == 0xFFFF) ? d7s.currentTemp : (t10 / 10.0f);
  d7s.currentTemp = sanitizeTemp(d7s.currentTemp);

  unsigned long now = millis();
  bool changed = (si_current != d7s.si_prev) || (pga_current != d7s.pga_prev);

  if (changed) {
    d7s.currentSI  = si_current;
    d7s.currentPGA = pga_current;
    d7s.lastChangeTime = now;
  } else {
    if (MODE_RESET_ON_ZERO) {
      if (si_current == 0 && pga_current == 0) {
        d7s.currentSI = 0;
        d7s.currentPGA = 0;
      } else {
        d7s.currentSI  = si_current;
        d7s.currentPGA = pga_current;
      }
    } else if (MODE_RESET_TIMEOUT) {
      if (now - d7s.lastChangeTime >= RESET_TIMEOUT_MS) {
        d7s.currentSI = 0;
        d7s.currentPGA = 0;
      } else {
        d7s.currentSI  = si_current;
        d7s.currentPGA = pga_current;
      }
    } else {
      d7s.currentSI  = si_current;
      d7s.currentPGA = pga_current;
    }
  }

  d7s.si_prev  = si_current;
  d7s.pga_prev = pga_current;

  Serial.printf("[D7S] si=%u  pga=%u  temp=%.1fC\n", d7s.currentSI, d7s.currentPGA, d7s.currentTemp);
}

// ============================================================================
// OLED Display - แสดงข้อมูลครบถ้วน
// ============================================================================
void updateOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // แสดงสถานะ OTA ถ้ากำลังอัปเดต
  if (ota_in_progress) {
    display.setCursor(0, 0);
    display.println("=== OTA UPDATE ===");
    display.setCursor(0, 16);
    display.printf("Ver: %s", ota_new_version);
    display.setCursor(0, 28);
    int percent = (ota_received * 100) / ota_expected_size;
    display.printf("Progress: %d%%", percent);
    display.setCursor(0, 40);
    display.printf("%u / %u bytes", (unsigned)ota_received, (unsigned)ota_expected_size);
    
    // Progress bar
    int barWidth = (percent * 128) / 100;
    display.drawRect(0, 52, 128, 12, SSD1306_WHITE);
    display.fillRect(0, 52, barWidth, 12, SSD1306_WHITE);
    
    display.display();
    return;
  }
  
  // Header - ชื่อ Device
  display.setCursor(0, 0);
  display.print(DEVICE_NAME);
  display.setCursor(80, 0);
  display.printf("v%s", FW_VERSION);
  display.drawLine(0, 9, SCREEN_W, 9, SSD1306_WHITE);
  
  // ข้อมูล D7S
  display.setCursor(0, 12);
  display.printf("SI  : %3u", d7s.currentSI);
  display.setCursor(70, 12);
  display.print(d7s.inited ? "OK" : "ERR");
  
  display.setCursor(0, 22);
  display.printf("PGA : %4u", d7s.currentPGA);
  
  display.setCursor(0, 32);
  display.printf("Temp: %.1f C", d7s.currentTemp);
  
  // สถานะ Mesh
  display.drawLine(0, 42, SCREEN_W, 42, SSD1306_WHITE);
  display.setCursor(0, 45);
  display.print("Mesh:");
  display.setCursor(35, 45);
  if (stats.meshConnected) {
    display.printf("ON (%d)", stats.connectedNodes);
  } else {
    display.print("OFF");
  }
  
  // สถานะการส่งข้อมูล
  display.setCursor(0, 55);
  display.printf("TX:%lu", stats.totalSent);
  display.setCursor(50, 55);
  display.printf("Fail:%lu", stats.totalFailed);
  
  // แสดงเวลาที่ส่งล่าสุด
  if (stats.lastSendTime > 0) {
    unsigned long timeSince = (millis() - stats.lastSendTime) / 1000;
    display.setCursor(100, 55);
    display.printf("%lus", timeSince);
  }
  
  display.display();
}

// ============================================================================
// Mesh send - FIXED for ArduinoJson v7
// ============================================================================
void sendSensorData() {
  if (ota_in_progress) return;

  JsonDocument doc;
  doc["type"] = "sensor_data";
  doc["node_id"] = mesh.getNodeId();
  doc["device_name"] = DEVICE_NAME;
  doc["access_token"] = DEVICE_ACCESS_TOKEN;
  doc["timestamp"] = millis();
  doc["fw_version"] = FW_VERSION;
  doc["role"] = ROLE_KEY;

  JsonObject eq = doc["earthquake"].to<JsonObject>();
  eq["status"] = d7s.inited ? "active" : "offline";
  eq["si"]  = d7s.currentSI;
  eq["pga"] = d7s.currentPGA;
  eq["temp"] = d7s.currentTemp;

  bool hasSignificantEvents = false;
  for (int i = 0; i < 3; i++) {
    if (d7s.eventBlocks[i].si > 5 || d7s.eventBlocks[i].pga > 10) {
      hasSignificantEvents = true;
      break;
    }
  }

  if (hasSignificantEvents) {
    JsonArray events = eq["events"].to<JsonArray>();
    for (int i = 0; i < 3; i++) {
      if (d7s.eventBlocks[i].si > 5 || d7s.eventBlocks[i].pga > 10) {
        JsonObject evt = events.add<JsonObject>();
        evt["id"] = i;
        evt["si"] = d7s.eventBlocks[i].si;
        evt["pga"] = d7s.eventBlocks[i].pga;
        evt["temp"] = d7s.eventBlocks[i].temp;
      }
    }
  }

  String s; 
  serializeJson(doc, s);

  if (s.length() > 800) {
    Serial.printf("⚠️ Payload ใหญ่เกินไป (%d bytes) - ส่งแบบย่อ\n", s.length());
    JsonDocument minDoc;
    minDoc["type"] = "sensor_data";
    minDoc["node_id"] = mesh.getNodeId();
    minDoc["access_token"] = DEVICE_ACCESS_TOKEN;
    minDoc["timestamp"] = millis();
    minDoc["fw_version"] = FW_VERSION;
    minDoc["role"] = ROLE_KEY;
    JsonObject minEq = minDoc["earthquake"].to<JsonObject>();
    minEq["status"] = d7s.inited ? "active" : "offline";
    minEq["si"] = d7s.currentSI;
    minEq["pga"] = d7s.currentPGA;
    minEq["temp"] = d7s.currentTemp;
    s = ""; 
    serializeJson(minDoc, s);
  }

  bool ok = mesh.sendBroadcast(s);
  
  if (ok) {
    stats.totalSent++;
    stats.lastSendTime = millis();
    Serial.printf("✅ Mesh send OK (%d bytes)\n", s.length());
  } else {
    stats.totalFailed++;
    Serial.printf("❌ Mesh send FAIL (%d bytes)\n", s.length());
  }
}

// ============================================================================
// OTA helpers (ENHANCED)
// ============================================================================
bool ota_begin(size_t total_size) {
  if (ota_in_progress) return false;
  Serial.printf("[OTA] begin total=%u bytes\n", (unsigned)total_size);
  if (!Update.begin(total_size)) {
    Serial.printf("[OTA] ❌ begin failed: %s\n", Update.errorString());
    return false;
  }
  ota_in_progress = true;
  ota_received = 0;
  ota_last_chunk_time = millis();  // NEW: Initialize timeout
  updateOLED();
  return true;
}

bool ota_write_b64(const char* b64, size_t b64len) {
  size_t outLenMax = b64len * 3 / 4 + 8;
  std::unique_ptr<uint8_t[]> out(new uint8_t[outLenMax]);
  size_t outLen = 0;

  int rc = mbedtls_base64_decode(out.get(), outLenMax, &outLen,
                                 (const unsigned char*)b64, b64len);
  if (rc != 0 || outLen == 0) {
    Serial.printf("[OTA] ❌ base64 decode fail (rc=%d)\n", rc);
    return false;
  }

  size_t w = Update.write(out.get(), outLen);
  ota_received += w;
  
  // อัปเดตหน้าจอ OTA progress
  static unsigned long lastOledUpdate = 0;
  if (millis() - lastOledUpdate > 200) {
    updateOLED();
    lastOledUpdate = millis();
  }
  
  if (w != outLen) {
    Serial.printf("[OTA] ❌ write mismatch (%u/%u)\n", (unsigned)w, (unsigned)outLen);
    return false;
  }
  return true;
}

bool ota_end_and_reboot() {
  if (!Update.end()) {
    Serial.printf("[OTA] ❌ end failed: %s\n", Update.errorString());
    ota_in_progress = false;
    return false;
  }
  
  // แสดงข้อความสำเร็จ
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println("OTA Complete!");
  display.setCursor(0, 35);
  display.println("Rebooting...");
  display.display();
  
  Serial.println("[OTA] ✅ Update OK. Rebooting...");
  delay(2000);
  ESP.restart();
  return true;
}

// NEW: Check OTA timeout
void checkOTAChunkTimeout() {
    if (!ota_in_progress) return;
    
    unsigned long now = millis();
    if (now - ota_last_chunk_time > OTA_CHUNK_TIMEOUT) {
        Serial.println("[OTA] ❌ Chunk timeout - aborting");
        
        // Abort OTA
        Update.abort();
        ota_in_progress = false;
        ota_consecutive_failures++;
        
        // ส่ง error กลับไป
        JsonDocument r;
        r["type"] = "ota_result";
        r["source_node"] = mesh.getNodeId();
        r["ok"] = false;
        r["msg"] = "chunk_timeout";
        r["device_name"] = DEVICE_NAME;
        r["error_count"] = ota_consecutive_failures;
        
        String s;
        serializeJson(r, s);
        mesh.sendSingle(ota_gateway_node, s);
        
        updateOLED();
        
        // ถ้า fail บ่อยเกินไป ให้ restart
        if (ota_consecutive_failures >= OTA_MAX_FAILURES) {
            Serial.println("[OTA] ❌ Too many failures - restarting in 5s");
            display.clearDisplay();
            display.setCursor(0, 20);
            display.println("OTA Failed!");
            display.setCursor(0, 35);
            display.println("Too many errors");
            display.setCursor(0, 45);
            display.println("Restarting...");
            display.display();
            delay(5000);
            ESP.restart();
        }
    }
}

// ============================================================================
// Mesh callbacks - FIXED for ArduinoJson v7 + Enhanced Error Handling
// ============================================================================
void handleOtaCheck() {
  JsonDocument j;
  j["type"] = "ota_check";
  j["role"] = ROLE_KEY;
  j["fw_version"] = FW_VERSION;
  String s; 
  serializeJson(j, s);
  mesh.sendBroadcast(s);
  Serial.printf("[OTA] ✅ send ota_check: role=%s fw=%s\n", ROLE_KEY, FW_VERSION);
}

void recvCb(uint32_t from, String &msg) {
  JsonDocument d;
  auto err = deserializeJson(d, msg);
  if (err) return;
  String type = d["type"] | "";

  if (type == "ota_offer") {
    const char* ver = d["version"] | "";
    ota_expected_size = d["size"] | 0;
    OTA_CHUNK = d["chunk"] | 512;

    if (!ver || ver[0]=='\0' || ota_expected_size==0) return;
    if (String(ver) == String(FW_VERSION)) return;

    strlcpy(ota_new_version, ver, sizeof(ota_new_version));
    ota_gateway_node = from;
    ota_last_chunk_time = millis();  // NEW: Initialize timeout

    Serial.printf("[OTA] 📥 Offer from=%u ver=%s size=%u chunk=%u\n",
                  from, ver, (unsigned)ota_expected_size, OTA_CHUNK);

    // ตอบรับพร้อม source_node
    JsonDocument a;
    a["type"] = "ota_accept";
    a["source_node"] = mesh.getNodeId();
    a["device_name"] = DEVICE_NAME;
    String s; 
    serializeJson(a, s);
    mesh.sendSingle(from, s);

    // ส่ง ota_next idx=0 ทันที
    JsonDocument n0;
    n0["type"] = "ota_next";
    n0["source_node"] = mesh.getNodeId();
    n0["idx"] = 0;
    s = "";
    serializeJson(n0, s);
    mesh.sendSingle(from, s);
    
    Serial.println("[OTA] ✅ Accepted and requested chunk 0");
  }

  else if (type == "ota_chunk") {
    if (from != ota_gateway_node) return;
    
    ota_last_chunk_time = millis();  // NEW: Update activity time
    
    const char* data_b64 = d["data"] | "";
    int idx = d["idx"] | 0;

    if (!ota_in_progress) {
      if (!ota_begin(ota_expected_size)) {
        ota_consecutive_failures++;
        
        JsonDocument r; 
        r["type"] = "ota_result"; 
        r["source_node"] = mesh.getNodeId();
        r["ok"] = false; 
        r["msg"] = "begin_fail";
        r["device_name"] = DEVICE_NAME;
        r["error_count"] = ota_consecutive_failures;
        
        String s; 
        serializeJson(r, s); 
        mesh.sendSingle(from, s);
        return;
      }
    }

    size_t b64len = strlen(data_b64);
    bool ok = ota_write_b64(data_b64, b64len);
    
    if (!ok) {
      ota_consecutive_failures++;
      Serial.printf("[OTA] ❌ Write failed (failures: %d/%d)\n", 
                   ota_consecutive_failures, OTA_MAX_FAILURES);
      
      JsonDocument r; 
      r["type"] = "ota_result"; 
      r["source_node"] = mesh.getNodeId();
      r["ok"] = false; 
      r["msg"] = "write_fail";
      r["device_name"] = DEVICE_NAME;
      r["error_count"] = ota_consecutive_failures;
      
      String s; 
      serializeJson(r, s); 
      mesh.sendSingle(from, s);
      
      // Abort OTA
      Update.abort();
      ota_in_progress = false;
      updateOLED();
      return;
    }

    // Reset failure counter on success
    ota_consecutive_failures = 0;
    
    // ขอชิ้นถัดไป พร้อม source_node
    JsonDocument n; 
    n["type"] = "ota_next"; 
    n["idx"] = idx + 1;
    n["source_node"] = mesh.getNodeId();
    String s; 
    serializeJson(n, s); 
    mesh.sendSingle(from, s);

    int percent = (ota_received * 100) / ota_expected_size;
    Serial.printf("[OTA] 📦 chunk %d OK (%d%%) - received=%u/%u\n",
                  idx, percent, (unsigned)ota_received, (unsigned)ota_expected_size);
  }

  else if (type == "ota_end") {
    if (from != ota_gateway_node) return;

    Serial.println("[OTA] 🏁 Received ota_end");

    // รายงานผล ok=true ก่อนรีบูต
    {
      JsonDocument r; 
      r["type"] = "ota_result"; 
      r["source_node"] = mesh.getNodeId();
      r["ok"] = true; 
      r["msg"] = "update_ok";
      r["new_version"] = ota_new_version;
      r["device_name"] = DEVICE_NAME;
      
      String s; 
      serializeJson(r, s); 
      mesh.sendSingle(from, s);
      
      Serial.println("[OTA] ✅ Success reported to gateway");
    }

    bool ok = ota_end_and_reboot();
    if (!ok) {
      JsonDocument r; 
      r["type"] = "ota_result"; 
      r["source_node"] = mesh.getNodeId();
      r["ok"] = false; 
      r["msg"] = "end_fail";
      r["device_name"] = DEVICE_NAME;
      
      String s; 
      serializeJson(r, s); 
      mesh.sendSingle(from, s);
    }
  }

  else if (type == "ota_abort") {
    if (ota_in_progress) {
      Update.abort();
      ota_in_progress = false;
      
      const char* reason = d["reason"] | "unknown";
      Serial.printf("[OTA] ❌ Aborted by gateway: %s\n", reason);
      
      display.clearDisplay();
      display.setCursor(0, 20);
      display.println("OTA Aborted!");
      display.setCursor(0, 35);
      display.printf("Reason: %s", reason);
      display.display();
      delay(3000);
      
      updateOLED();
    }
  }
}

void newConnCb(uint32_t nodeId) {
  Serial.printf("[Mesh] ✅ new connection: %u\n", nodeId);
  stats.meshConnected = true;
  stats.connectedNodes = mesh.getNodeList().size();
}

void changeCb() {
  Serial.println("[Mesh] 🔄 topology changed");
  stats.connectedNodes = mesh.getNodeList().size();
  stats.meshConnected = (stats.connectedNodes > 0);
}

void timeAdjCb(int32_t) {}

// ============================================================================
// Setup / Loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("=== ESP32-C3 D7S Sender v2.1.1 (FIXED OTA) ===");
  Serial.printf("Device: %s | FW: %s | Role: %s\n", DEVICE_NAME, FW_VERSION, ROLE_KEY);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("❌ SSD1306 init failed");
    while (true) delay(1000);
  }
  
  // Startup screen
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("ESP32-C3 Sender");
  display.printf("Device: %s\n", DEVICE_NAME);
  display.printf("FW: %s\n", FW_VERSION);
  display.println("D7S + Mesh + OTA");
  display.println("ERROR HANDLING!");
  display.println("Initializing...");
  display.display();
  delay(2000);

  d7s.inited = d7sInit();
  d7s.lastChangeTime = millis();
  Serial.printf("D7S init: %s\n", d7s.inited ? "OK" : "FAIL");

  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  mesh.setContainsRoot(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  mesh.onReceive(&recvCb);
  mesh.onNewConnection(&newConnCb);
  mesh.onChangedConnections(&changeCb);
  mesh.onNodeTimeAdjusted(&timeAdjCb);

  handleOtaCheck();
  readD7S();
  updateOLED();

  Serial.println("=== Ready! ===");
  Serial.printf("- Read Interval: %lu ms\n", READ_INTERVAL_MS);
  Serial.printf("- Send Interval: %lu ms\n", SEND_INTERVAL_MS);
  Serial.printf("- OLED Update: %lu ms\n", OLED_UPDATE_MS);
  Serial.printf("- OTA Chunk Size: %u bytes\n", OTA_CHUNK);
  Serial.printf("- OTA Timeout: %u ms\n", OTA_CHUNK_TIMEOUT);
}

void loop() {
  mesh.update();
  unsigned long now = millis();

  // NEW: Check OTA timeout
  checkOTAChunkTimeout();

  if (!ota_in_progress && (now - tRead >= READ_INTERVAL_MS)) {
    tRead = now;
    readD7S();
  }

  if (!ota_in_progress && (now - tSend >= SEND_INTERVAL_MS)) {
    tSend = now;
    sendSensorData();
  }

  if (now - tOledUpdate >= OLED_UPDATE_MS) {
    tOledUpdate = now;
    updateOLED();
  }
}