/*
 * STANDALONE WiFi Gateway with Built-in OTA Backend
 * Version: 3.0.2 - COMPLETE & FIXED
 * 
 * All functions implemented - No linker errors!
 * 
 * Hardware: ESP32-S3
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <map>
#include <vector>

// =============================================================================
// CONFIGURATION
// =============================================================================

#define WIFI_SSID       "iPhone Perch"
#define WIFI_PASSWORD   "00000000"
#define HOSTNAME        "earthquake-gateway"

#define THINGBOARD_SERVER   "demo.thingsboard.io"
#define THINGBOARD_PORT     80

#define RS232_TX_PIN    16
#define RS232_RX_PIN    15
#define RS232_BAUD      115200

#define I2C_SDA         4
#define I2C_SCL         5

#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDRESS    0x3C

#ifndef FW_VERSION
  #define FW_VERSION "2.1.0"
#endif
#define ROLE_KEY "ROLE_WIFI_GATEWAY"

#define MANIFEST_URL "https://raw.githubusercontent.com/ChatpetchDatesatarn/EarthQuake_OTA/main/ota/manifest.json"

#define AUTO_OTA_ENABLED        true
#define AUTO_OTA_COOLDOWN       300000
#define MANIFEST_CACHE_TIME     300000
#define OTA_CHUNK_SIZE          512
#define OTA_MAX_RETRIES         3
#define OTA_RETRY_DELAY         1000
#define OTA_TIMEOUT             300000

// =============================================================================
// GLOBAL OBJECTS
// =============================================================================

HardwareSerial rs232Serial(2);
WebServer webServer(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =============================================================================
// DATA STRUCTURES
// =============================================================================

struct NodeInfo {
    uint32_t id;
    String name;
    String role;
    String version;
    String token;
    String status;
    unsigned long lastSeen;
    int rssi;
    float temperature;
    float si;
    float pga;
};

struct OTASession {
    uint32_t nodeId;
    String nodeName;
    String version;
    std::vector<uint8_t> firmwareData;
    size_t totalSize;
    size_t sentBytes;
    int currentChunk;
    unsigned long startTime;
    unsigned long lastActivity;
    bool isAuto;
    String sha256Expected;
    int failedChunks;
    bool timedOut;
};

struct ManifestCache {
    String version;
    String assetsJson;
    String sha256Json;
    unsigned long lastFetch;
    bool valid;
};

// =============================================================================
// GLOBAL VARIABLES
// =============================================================================

std::map<uint32_t, NodeInfo> nodes;
std::map<uint32_t, OTASession> otaSessions;
std::map<uint32_t, unsigned long> lastAutoOTA;

ManifestCache manifestCache = {.valid = false};

struct Statistics {
    unsigned long totalPacketsRX = 0;
    unsigned long totalPacketsTX = 0;
    unsigned long totalOTASuccess = 0;
    unsigned long totalOTAFailed = 0;
    unsigned long startTime;
} stats;

bool autoOTAEnabled = AUTO_OTA_ENABLED;
String rs232Buffer = "";

// =============================================================================
// FUNCTION DECLARATIONS
// =============================================================================

void setupWiFi();
void setupWebServer();
void setupMDNS();
void setupRS232();
void setupOLED();

bool fetchManifest();
void handleOTACheck(JsonDocument& data);
bool downloadFirmware(String url, std::vector<uint8_t>& output);
bool verifySHA256(const std::vector<uint8_t>& data, String expectedHash);
void sendOTAOffer(uint32_t nodeId, String version, size_t fileSize);
void sendOTAChunk(uint32_t nodeId, int chunkIndex);
void sendOTAEnd(uint32_t nodeId);
void sendOTAAbort(uint32_t nodeId, String reason);
void handleOTAAccept(JsonDocument& data);
void handleOTANext(JsonDocument& data);
void handleOTAResult(JsonDocument& data);
bool compareVersions(String current, String latest);
void checkOTATimeout();

void handleRS232Data();
void sendToRS232(const String& data);
void processGatewayMessage(const String& message);

void sendToThingsBoard(const NodeInfo& node);

void handleRoot();
void handleGetNodes();
void handleGetStats();
void handleGetManifest();
void handleRefreshManifest();
void handleTriggerOTA();
void handleToggleAutoOTA();
void handleGetOTASessions();
void handleNotFound();

void updateNodeFromMessage(JsonDocument& data);
void cleanupInactiveNodes();
void updateDisplay();

// =============================================================================
// SETUP
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n========================================");
    Serial.println("WiFi Gateway v3.0.2 - COMPLETE");
    Serial.println("========================================");
    
    stats.startTime = millis();
    
    setupOLED();
    setupRS232();
    setupWiFi();
    setupMDNS();
    setupWebServer();
    
    Serial.println("\nFetching manifest from GitHub...");
    if (fetchManifest()) {
        Serial.printf("Manifest ready: v%s\n", manifestCache.version.c_str());
    } else {
        Serial.println("Manifest fetch failed - will retry later");
    }
    
    Serial.println("\n========================================");
    Serial.println("Gateway Ready!");
    Serial.printf("Web UI: http://%s.local\n", HOSTNAME);
    Serial.printf("IP: http://%s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Auto OTA: %s\n", autoOTAEnabled ? "ENABLED" : "DISABLED");
    Serial.println("========================================\n");
}

// =============================================================================
// MAIN LOOP
// =============================================================================

void loop() {
    webServer.handleClient();
    handleRS232Data();
    
    static unsigned long lastCleanup = 0;
    if (millis() - lastCleanup > 30000) {
        cleanupInactiveNodes();
        lastCleanup = millis();
    }
    
    static unsigned long lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate > 500) {
        updateDisplay();
        lastDisplayUpdate = millis();
    }
    
    static unsigned long lastOTACheck = 0;
    if (millis() - lastOTACheck > 5000) {
        checkOTATimeout();
        lastOTACheck = millis();
    }
    
    delay(1);
}

// =============================================================================
// NETWORK SETUP
// =============================================================================

void setupWiFi() {
    Serial.println("Connecting to WiFi...");
    
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected!");
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    } else {
        Serial.println("\nWiFi Failed!");
    }
}

void setupMDNS() {
    if (MDNS.begin(HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local\n", HOSTNAME);
    }
}

void setupRS232() {
    rs232Serial.setRxBufferSize(4096);
    rs232Serial.begin(RS232_BAUD, SERIAL_8N1, RS232_RX_PIN, RS232_TX_PIN);
    Serial.println("RS232 Ready");
}

void setupOLED() {
    Wire.begin(I2C_SDA, I2C_SCL);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println("Gateway v3.0.2");
        display.println("Complete Backend");
        display.println("");
        display.println("Initializing...");
        display.display();
        Serial.println("OLED Ready");
    }
}

// =============================================================================
// WEB SERVER
// =============================================================================

void setupWebServer() {
    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/api/nodes", HTTP_GET, handleGetNodes);
    webServer.on("/api/stats", HTTP_GET, handleGetStats);
    webServer.on("/api/manifest", HTTP_GET, handleGetManifest);
    webServer.on("/api/manifest/refresh", HTTP_POST, handleRefreshManifest);
    webServer.on("/api/ota/trigger", HTTP_POST, handleTriggerOTA);
    webServer.on("/api/ota/auto/toggle", HTTP_POST, handleToggleAutoOTA);
    webServer.on("/api/ota/sessions", HTTP_GET, handleGetOTASessions);
    webServer.onNotFound(handleNotFound);
    
    webServer.begin();
    Serial.println("Web Server Started on port 80");
}

void handleRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Gateway v3.0.2</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 1400px; margin: 0 auto; }
        .header {
            background: white;
            border-radius: 15px;
            padding: 30px;
            margin-bottom: 20px;
            box-shadow: 0 10px 30px rgba(0,0,0,0.2);
        }
        h1 { color: #667eea; font-size: 2em; margin-bottom: 10px; }
        .subtitle { color: #666; font-size: 1.1em; }
        .badge { 
            display: inline-block;
            background: #10b981;
            color: white;
            padding: 5px 15px;
            border-radius: 20px;
            font-size: 0.9em;
            font-weight: 600;
            margin-left: 10px;
        }
        .cards {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 20px;
            margin-bottom: 20px;
        }
        .card {
            background: white;
            border-radius: 15px;
            padding: 25px;
            box-shadow: 0 10px 30px rgba(0,0,0,0.2);
        }
        .card h2 { color: #667eea; margin-bottom: 15px; font-size: 1.3em; }
        .stat { 
            display: flex;
            justify-content: space-between;
            padding: 10px 0;
            border-bottom: 1px solid #f0f0f0;
        }
        .stat:last-child { border-bottom: none; }
        .stat-label { color: #666; }
        .stat-value { font-weight: bold; color: #333; }
        .button {
            background: #667eea;
            color: white;
            border: none;
            padding: 12px 24px;
            border-radius: 8px;
            cursor: pointer;
            font-size: 1em;
            font-weight: 600;
            transition: all 0.3s;
            width: 100%;
            margin-top: 10px;
        }
        .button:hover { background: #5568d3; transform: translateY(-2px); }
        .button.success { background: #10b981; }
        .button.success:hover { background: #059669; }
        .button.warning { background: #f59e0b; }
        .button.warning:hover { background: #d97706; }
        .node-list {
            background: white;
            border-radius: 15px;
            padding: 25px;
            box-shadow: 0 10px 30px rgba(0,0,0,0.2);
        }
        .node-item {
            padding: 15px;
            border-bottom: 1px solid #f0f0f0;
            display: grid;
            grid-template-columns: 1fr auto;
            gap: 15px;
            align-items: center;
        }
        .node-item:last-child { border-bottom: none; }
        .node-name { font-weight: bold; color: #333; margin-bottom: 5px; font-size: 1.1em; }
        .node-details { color: #666; font-size: 0.9em; margin-top: 5px; }
        .node-sensor { 
            display: flex;
            gap: 15px;
            margin-top: 8px;
            font-size: 0.85em;
        }
        .sensor-value {
            background: #f3f4f6;
            padding: 4px 12px;
            border-radius: 6px;
            color: #374151;
            font-weight: 600;
        }
        .node-status {
            padding: 8px 20px;
            border-radius: 20px;
            font-size: 0.85em;
            font-weight: 600;
            text-align: center;
            min-width: 100px;
        }
        .node-status.online { background: #d1fae5; color: #059669; }
        .node-status.offline { background: #fee2e2; color: #dc2626; }
        .node-status.updating { 
            background: #dbeafe; 
            color: #2563eb;
            animation: pulse 2s infinite;
        }
        @keyframes pulse { 
            0%, 100% { opacity: 1; } 
            50% { opacity: 0.6; } 
        }
        .empty-state {
            text-align: center;
            color: #666;
            padding: 40px 20px;
        }
        .loading {
            display: inline-block;
            width: 20px;
            height: 20px;
            border: 3px solid #f3f3f3;
            border-top: 3px solid #667eea;
            border-radius: 50%;
            animation: spin 1s linear infinite;
        }
        @keyframes spin {
            0% { transform: rotate(0deg); }
            100% { transform: rotate(360deg); }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🌍 Earthquake Gateway</h1>
            <p class="subtitle">
                Standalone Backend
                <span class="badge">v3.0.2</span>
            </p>
        </div>
        
        <div class="cards">
            <div class="card">
                <h2>📊 System Status</h2>
                <div class="stat">
                    <span class="stat-label">Total Nodes</span>
                    <span class="stat-value" id="totalNodes">-</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Online Nodes</span>
                    <span class="stat-value" id="onlineNodes">-</span>
                </div>
                <div class="stat">
                    <span class="stat-label">OTA Success</span>
                    <span class="stat-value" id="otaSuccess">-</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Uptime</span>
                    <span class="stat-value" id="uptime">-</span>
                </div>
            </div>
            
            <div class="card">
                <h2>🚀 Auto OTA</h2>
                <div class="stat">
                    <span class="stat-label">Status</span>
                    <span class="stat-value" id="autoOTAStatus">-</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Latest Version</span>
                    <span class="stat-value" id="latestVersion">-</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Active Sessions</span>
                    <span class="stat-value" id="activeSessions">-</span>
                </div>
                <button class="button warning" onclick="toggleAutoOTA()">Toggle Auto OTA</button>
                <button class="button success" onclick="refreshManifest()">🔄 Refresh Manifest</button>
            </div>
            
            <div class="card">
                <h2>🔗 Network Info</h2>
                <div class="stat">
                    <span class="stat-label">Gateway IP</span>
                    <span class="stat-value" id="gatewayIP">-</span>
                </div>
                <div class="stat">
                    <span class="stat-label">WiFi RSSI</span>
                    <span class="stat-value" id="wifiRSSI">-</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Free Heap</span>
                    <span class="stat-value" id="freeHeap">-</span>
                </div>
                <div class="stat">
                    <span class="stat-label">Packets RX/TX</span>
                    <span class="stat-value" id="packets">-</span>
                </div>
            </div>
        </div>
        
        <div class="node-list">
            <h2>📡 Connected Nodes</h2>
            <div id="nodesList">
                <div class="empty-state">
                    <div class="loading"></div>
                    <p style="margin-top: 15px;">Loading...</p>
                </div>
            </div>
        </div>
    </div>
    
    <script>
        function formatUptime(ms) {
            const s = Math.floor(ms / 1000);
            const m = Math.floor(s / 60);
            const h = Math.floor(m / 60);
            const d = Math.floor(h / 24);
            
            if (d > 0) return d + 'd ' + (h % 24) + 'h';
            if (h > 0) return h + 'h ' + (m % 60) + 'm';
            if (m > 0) return m + 'm ' + (s % 60) + 's';
            return s + 's';
        }
        
        function updateData() {
            fetch('/api/stats')
                .then(r => r.json())
                .then(data => {
                    document.getElementById('totalNodes').textContent = data.total_nodes || 0;
                    document.getElementById('onlineNodes').textContent = data.online_nodes || 0;
                    document.getElementById('otaSuccess').textContent = data.ota_success || 0;
                    document.getElementById('uptime').textContent = formatUptime(data.uptime || 0);
                    document.getElementById('autoOTAStatus').textContent = data.auto_ota ? '✅ ON' : '⏸️ OFF';
                    document.getElementById('latestVersion').textContent = data.latest_version || 'N/A';
                    document.getElementById('gatewayIP').textContent = data.gateway_ip || '-';
                    document.getElementById('wifiRSSI').textContent = (data.wifi_rssi || 0) + ' dBm';
                    document.getElementById('freeHeap').textContent = Math.floor((data.free_heap || 0) / 1024) + ' KB';
                    document.getElementById('packets').textContent = (data.packets_rx || 0) + ' / ' + (data.packets_tx || 0);
                })
                .catch(err => console.error(err));
            
            fetch('/api/ota/sessions')
                .then(r => r.json())
                .then(sessions => {
                    document.getElementById('activeSessions').textContent = sessions.length || 0;
                })
                .catch(err => console.error(err));
            
            fetch('/api/nodes')
                .then(r => r.json())
                .then(nodes => {
                    const list = document.getElementById('nodesList');
                    if (nodes.length === 0) {
                        list.innerHTML = '<div class="empty-state"><p>No nodes detected</p></div>';
                        return;
                    }
                    list.innerHTML = nodes.map(node => `
                        <div class="node-item">
                            <div class="node-info">
                                <div class="node-name">📷 ${node.name || 'Node_' + node.id}</div>
                                <div class="node-details">
                                    <strong>ID:</strong> ${node.id} | 
                                    <strong>Ver:</strong> ${node.version || '?'} | 
                                    <strong>Role:</strong> ${node.role || 'N/A'}
                                </div>
                                <div class="node-sensor">
                                    <span class="sensor-value">SI: ${node.si || 0}</span>
                                    <span class="sensor-value">PGA: ${node.pga || 0}</span>
                                    <span class="sensor-value">${node.temperature || 0}°C</span>
                                </div>
                            </div>
                            <div>
                                <div class="node-status ${node.status}">${node.status.toUpperCase()}</div>
                            </div>
                        </div>
                    `).join('');
                })
                .catch(err => console.error(err));
        }
        
        function toggleAutoOTA() {
            fetch('/api/ota/auto/toggle', { method: 'POST' })
                .then(r => r.json())
                .then(data => {
                    alert('Auto OTA ' + (data.enabled ? 'ENABLED' : 'DISABLED'));
                    updateData();
                })
                .catch(err => alert('Failed'));
        }
        
        function refreshManifest() {
            const btn = event.target;
            btn.disabled = true;
            btn.textContent = 'Refreshing...';
            
            fetch('/api/manifest/refresh', { method: 'POST' })
                .then(r => r.json())
                .then(data => {
                    alert('Manifest refreshed! v' + data.version);
                    updateData();
                    btn.disabled = false;
                    btn.textContent = '🔄 Refresh Manifest';
                })
                .catch(err => {
                    alert('Failed');
                    btn.disabled = false;
                    btn.textContent = '🔄 Refresh Manifest';
                });
        }
        
        updateData();
        setInterval(updateData, 3000);
    </script>
</body>
</html>
    )rawliteral";
    
    webServer.send(200, "text/html", html);
}

void handleGetNodes() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    for (const auto& p : nodes) {
        JsonObject obj = arr.add<JsonObject>();
        obj["id"] = p.second.id;
        obj["name"] = p.second.name;
        obj["role"] = p.second.role;
        obj["version"] = p.second.version;
        obj["token"] = p.second.token;
        obj["status"] = p.second.status;
        obj["rssi"] = p.second.rssi;
        obj["temperature"] = p.second.temperature;
        obj["si"] = p.second.si;
        obj["pga"] = p.second.pga;
        obj["lastSeen"] = p.second.lastSeen;
    }
    
    String response;
    serializeJson(doc, response);
    webServer.send(200, "application/json", response);
}

void handleGetStats() {
    int online = 0;
    for (const auto& p : nodes) {
        if (p.second.status == "online") online++;
    }
    
    JsonDocument doc;
    doc["total_nodes"] = nodes.size();
    doc["online_nodes"] = online;
    doc["ota_success"] = stats.totalOTASuccess;
    doc["ota_failed"] = stats.totalOTAFailed;
    doc["packets_rx"] = stats.totalPacketsRX;
    doc["packets_tx"] = stats.totalPacketsTX;
    doc["uptime"] = millis() - stats.startTime;
    doc["auto_ota"] = autoOTAEnabled;
    doc["latest_version"] = manifestCache.version;
    doc["gateway_ip"] = WiFi.localIP().toString();
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["free_heap"] = ESP.getFreeHeap();
    
    String response;
    serializeJson(doc, response);
    webServer.send(200, "application/json", response);
}

void handleGetManifest() {
    if (fetchManifest()) {
        JsonDocument doc;
        doc["version"] = manifestCache.version;
        doc["cached_at"] = manifestCache.lastFetch;
        
        JsonDocument assets, sha;
        deserializeJson(assets, manifestCache.assetsJson);
        deserializeJson(sha, manifestCache.sha256Json);
        
        doc["assets"] = assets;
        doc["sha256"] = sha;
        
        String response;
        serializeJson(doc, response);
        webServer.send(200, "application/json", response);
    } else {
        webServer.send(500, "application/json", "{\"error\":\"Failed\"}");
    }
}

void handleRefreshManifest() {
    manifestCache.valid = false;
    
    if (fetchManifest()) {
        JsonDocument doc;
        doc["success"] = true;
        doc["version"] = manifestCache.version;
        doc["timestamp"] = millis();
        
        String response;
        serializeJson(doc, response);
        webServer.send(200, "application/json", response);
    } else {
        webServer.send(500, "application/json", "{\"error\":\"Failed\"}");
    }
}

void handleTriggerOTA() {
    if (!webServer.hasArg("plain")) {
        webServer.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    
    JsonDocument doc;
    deserializeJson(doc, webServer.arg("plain"));
    
    uint32_t nodeId = doc["node_id"];
    
    auto it = nodes.find(nodeId);
    if (it == nodes.end()) {
        webServer.send(404, "application/json", "{\"error\":\"Node not found\"}");
        return;
    }
    
    if (!fetchManifest()) {
        webServer.send(500, "application/json", "{\"error\":\"Manifest unavailable\"}");
        return;
    }
    
    JsonDocument assets, sha;
    deserializeJson(assets, manifestCache.assetsJson);
    deserializeJson(sha, manifestCache.sha256Json);
    
    String role = it->second.role;
    String url = assets[role].as<String>();
    String ver = manifestCache.version;
    String hash = sha[role].as<String>();
    
    if (url.length() == 0) {
        webServer.send(404, "application/json", "{\"error\":\"FW not found\"}");
        return;
    }
    
    std::vector<uint8_t> fw;
    if (!downloadFirmware(url, fw)) {
        webServer.send(500, "application/json", "{\"error\":\"Download failed\"}");
        return;
    }
    
    if (hash.length() > 0 && !verifySHA256(fw, hash)) {
        webServer.send(500, "application/json", "{\"error\":\"SHA256 failed\"}");
        return;
    }
    
    sendOTAOffer(nodeId, ver, fw.size());
    
    otaSessions[nodeId] = {
        .nodeId = nodeId,
        .nodeName = it->second.name,
        .version = ver,
        .firmwareData = fw,
        .totalSize = fw.size(),
        .sentBytes = 0,
        .currentChunk = 0,
        .startTime = millis(),
        .lastActivity = millis(),
        .isAuto = false,
        .sha256Expected = hash,
        .failedChunks = 0,
        .timedOut = false
    };
    
    it->second.status = "updating";
    
    JsonDocument resp;
    resp["success"] = true;
    resp["message"] = "OTA initiated";
    resp["node_id"] = nodeId;
    resp["version"] = ver;
    resp["size"] = fw.size();
    
    String respStr;
    serializeJson(resp, respStr);
    webServer.send(200, "application/json", respStr);
}

void handleToggleAutoOTA() {
    autoOTAEnabled = !autoOTAEnabled;
    
    JsonDocument doc;
    doc["success"] = true;
    doc["enabled"] = autoOTAEnabled;
    
    String response;
    serializeJson(doc, response);
    webServer.send(200, "application/json", response);
    
    Serial.printf("Auto OTA: %s\n", autoOTAEnabled ? "ENABLED" : "DISABLED");
}

void handleGetOTASessions() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    
    for (const auto& p : otaSessions) {
        JsonObject obj = arr.add<JsonObject>();
        obj["node_id"] = p.second.nodeId;
        obj["node_name"] = p.second.nodeName;
        obj["version"] = p.second.version;
        obj["progress"] = (p.second.sentBytes * 100) / p.second.totalSize;
        obj["sent_bytes"] = p.second.sentBytes;
        obj["total_bytes"] = p.second.totalSize;
        obj["is_auto"] = p.second.isAuto;
        obj["elapsed_ms"] = millis() - p.second.startTime;
        obj["failed_chunks"] = p.second.failedChunks;
        obj["timed_out"] = p.second.timedOut;
    }
    
    String response;
    serializeJson(doc, response);
    webServer.send(200, "application/json", response);
}

void handleNotFound() {
    webServer.send(404, "text/plain", "Not Found");
}

// =============================================================================
// MANIFEST & OTA MANAGEMENT
// =============================================================================

bool fetchManifest() {
    if (manifestCache.valid && (millis() - manifestCache.lastFetch < MANIFEST_CACHE_TIME)) {
        Serial.println("Using cached manifest");
        return true;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected");
        return false;
    }
    
    Serial.println("Downloading manifest...");
    HTTPClient http;
    http.begin(MANIFEST_URL);
    http.setTimeout(15000);
    
    int code = http.GET();
    Serial.printf("HTTP Response: %d\n", code);
    
    if (code == 200) {
        String payload = http.getString();
        Serial.printf("Original size: %d bytes\n", payload.length());
        
        // Clean JSON
        payload.replace("\n", "");
        payload.replace("\r", "");
        payload.replace("\t", "");
        while (payload.indexOf("  ") >= 0) {
            payload.replace("  ", " ");
        }
        payload.replace(" :", ":");
        payload.replace(": ", ":");
        payload.replace(" ,", ",");
        payload.replace(", ", ",");
        payload.replace(" {", "{");
        payload.replace("{ ", "{");
        payload.replace(" }", "}");
        payload.replace("} ", "}");
        payload.replace(" [", "[");
        payload.replace("[ ", "[");
        payload.replace(" ]", "]");
        payload.replace("] ", "]");
        payload.trim();
        
        Serial.printf("Cleaned size: %d bytes\n", payload.length());
        
        // Extract version
        int vIdx = payload.indexOf("\"version\":");
        int vStart = payload.indexOf("\"", vIdx + 10) + 1;
        int vEnd = payload.indexOf("\"", vStart);
        String version = payload.substring(vStart, vEnd);
        
        if (version.length() == 0) {
            Serial.println("Cannot extract version");
            http.end();
            return false;
        }
        
        manifestCache.version = version;
        Serial.printf("Version: %s\n", version.c_str());
        
        // Extract assets
        int assetsStart = payload.indexOf("\"assets\":{") + 10;
        int assetsEnd = payload.indexOf("},\"sha256\"");
        if (assetsEnd < 0) assetsEnd = payload.indexOf("}}", assetsStart) + 1;
        
        // Extract sha256
        int sha256Start = payload.indexOf("\"sha256\":{") + 10;
        int sha256End = payload.lastIndexOf("}}");
        
        if (assetsStart > 0 && assetsEnd > assetsStart) {
            manifestCache.assetsJson = "{" + payload.substring(assetsStart, assetsEnd) + "}";
            Serial.printf("Assets JSON: %d bytes\n", manifestCache.assetsJson.length());
        } else {
            Serial.println("Cannot extract assets");
            http.end();
            return false;
        }
        
        if (sha256Start > 0 && sha256End > sha256Start) {
            manifestCache.sha256Json = "{" + payload.substring(sha256Start, sha256End) + "}";
            Serial.printf("SHA256 JSON: %d bytes\n", manifestCache.sha256Json.length());
        } else {
            manifestCache.sha256Json = "{}";
            Serial.println("No SHA256 data");
        }
        
        manifestCache.lastFetch = millis();
        manifestCache.valid = true;
        
        Serial.printf("Manifest cached: v%s\n", version.c_str());
        http.end();
        return true;
    }
    
    Serial.printf("HTTP error: %d\n", code);
    http.end();
    return false;
}

bool downloadFirmware(String url, std::vector<uint8_t>& output) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    
    int code = http.GET();
    
    if (code == 200) {
        int len = http.getSize();
        output.reserve(len);
        
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buffer[512];
        int downloaded = 0;
        
        while (http.connected() && (len > 0 || len == -1)) {
            size_t available = stream->available();
            if (available) {
                int c = stream->readBytes(buffer, min(available, sizeof(buffer)));
                output.insert(output.end(), buffer, buffer + c);
                downloaded += c;
                
                if (len > 0) {
                    len -= c;
                    Serial.printf("Downloading: %d%%\r", (downloaded * 100) / (downloaded + len));
                }
            }
            delay(1);
        }
        
        http.end();
        Serial.printf("\nDownloaded: %d bytes\n", output.size());
        return true;
    }
    
    http.end();
    Serial.printf("Download failed: %d\n", code);
    return false;
}

bool verifySHA256(const std::vector<uint8_t>& data, String expectedHash) {
    if (data.size() == 0 || expectedHash.length() != 64) {
        Serial.println("Invalid SHA256 input");
        return false;
    }
    
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data.data(), data.size());
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    
    char calculated[65];
    for (int i = 0; i < 32; i++) {
        sprintf(&calculated[i * 2], "%02x", hash[i]);
    }
    calculated[64] = '\0';
    
    String calc = String(calculated);
    calc.toLowerCase();
    String exp = expectedHash;
    exp.toLowerCase();
    
    bool match = (calc == exp);
    
    Serial.printf("Calculated: %s\n", calculated);
    Serial.printf("Expected  : %s\n", expectedHash.c_str());
    Serial.printf("SHA256 %s\n", match ? "MATCH" : "MISMATCH");
    
    return match;
}

bool compareVersions(String current, String latest) {
    int c1, c2, c3, l1, l2, l3;
    sscanf(current.c_str(), "%d.%d.%d", &c1, &c2, &c3);
    sscanf(latest.c_str(), "%d.%d.%d", &l1, &l2, &l3);
    
    if (l1 > c1) return true;
    if (l1 < c1) return false;
    if (l2 > c2) return true;
    if (l2 < c2) return false;
    return l3 > c3;
}

void handleOTACheck(JsonDocument& data) {
    if (!autoOTAEnabled) return;
    
    uint32_t nodeId = data["source_node"];
    String role = data["role"].as<String>();
    String currentFW = data["fw_version"].as<String>();
    
    unsigned long lastUpdate = lastAutoOTA[nodeId];
    if (millis() - lastUpdate < AUTO_OTA_COOLDOWN) {
        return;
    }
    
    if (!fetchManifest()) return;
    
    String latestFW = manifestCache.version;
    
    if (!compareVersions(currentFW, latestFW)) {
        Serial.printf("Node %u is up-to-date (%s)\n", nodeId, currentFW.c_str());
        return;
    }
    
    Serial.println("\n========================================");
    Serial.printf("AUTO OTA: Node %u needs update\n", nodeId);
    Serial.printf("Current: %s -> Latest: %s\n", currentFW.c_str(), latestFW.c_str());
    Serial.printf("Role: %s\n", role.c_str());
    Serial.println("========================================");
    
    JsonDocument assets, sha;
    deserializeJson(assets, manifestCache.assetsJson);
    deserializeJson(sha, manifestCache.sha256Json);
    
    String url = assets[role].as<String>();
    String hash = sha[role].as<String>();
    
    if (url.length() == 0) {
        Serial.println("No firmware URL");
        return;
    }
    
    std::vector<uint8_t> fw;
    if (!downloadFirmware(url, fw)) {
        Serial.println("Download failed");
        return;
    }
    
    if (hash.length() > 0 && !verifySHA256(fw, hash)) {
        Serial.println("SHA256 verification failed");
        return;
    }
    
    NodeInfo& node = nodes[nodeId];
    node.id = nodeId;
    node.role = role;
    node.version = currentFW;
    node.status = "updating";
    node.lastSeen = millis();
    
    sendOTAOffer(nodeId, latestFW, fw.size());
    
    otaSessions[nodeId] = {
        .nodeId = nodeId,
        .nodeName = node.name,
        .version = latestFW,
        .firmwareData = fw,
        .totalSize = fw.size(),
        .sentBytes = 0,
        .currentChunk = 0,
        .startTime = millis(),
        .lastActivity = millis(),
        .isAuto = true,
        .sha256Expected = hash,
        .failedChunks = 0,
        .timedOut = false
    };
    
    lastAutoOTA[nodeId] = millis();
    
    Serial.println("AUTO OTA initiated!");
}

// =============================================================================
// OTA PROTOCOL
// =============================================================================

void sendOTAOffer(uint32_t nodeId, String version, size_t fileSize) {
    JsonDocument doc;
    doc["type"] = "ota_offer";
    doc["target_node"] = String(nodeId);
    doc["version"] = version;
    doc["size"] = fileSize;
    doc["chunk"] = OTA_CHUNK_SIZE;
    
    String message;
    serializeJson(doc, message);
    sendToRS232(message);
    
    Serial.printf("OTA Offer sent to node %u\n", nodeId);
}

void sendOTAChunk(uint32_t nodeId, int chunkIndex) {
    auto it = otaSessions.find(nodeId);
    if (it == otaSessions.end()) return;
    
    OTASession& session = it->second;
    session.lastActivity = millis();
    
    size_t start = chunkIndex * OTA_CHUNK_SIZE;
    size_t end = min(start + OTA_CHUNK_SIZE, session.totalSize);
    
    if (start >= session.totalSize) {
        sendOTAEnd(nodeId);
        return;
    }
    
    size_t chunkLen = end - start;
    size_t b64MaxLen = ((chunkLen + 2) / 3) * 4 + 1;
    char* b64 = new char[b64MaxLen];
    
    size_t outLen;
    mbedtls_base64_encode(
        (unsigned char*)b64, b64MaxLen, &outLen,
        &session.firmwareData[start], chunkLen
    );
    b64[outLen] = '\0';
    
    JsonDocument doc;
    doc["type"] = "ota_chunk";
    doc["target_node"] = String(nodeId);
    doc["idx"] = chunkIndex;
    doc["data"] = b64;
    
    String message;
    serializeJson(doc, message);
    sendToRS232(message);
    
    delete[] b64;
    
    session.sentBytes = end;
    int progress = (session.sentBytes * 100) / session.totalSize;
    
    Serial.printf("Chunk %d sent: %d%% (%u/%u bytes)\n", 
                  chunkIndex, progress, session.sentBytes, session.totalSize);
}

void sendOTAEnd(uint32_t nodeId) {
    JsonDocument doc;
    doc["type"] = "ota_end";
    doc["target_node"] = String(nodeId);
    
    String message;
    serializeJson(doc, message);
    sendToRS232(message);
    
    Serial.printf("OTA End sent to node %u\n", nodeId);
}

void sendOTAAbort(uint32_t nodeId, String reason) {
    JsonDocument doc;
    doc["type"] = "ota_abort";
    doc["target_node"] = String(nodeId);
    doc["reason"] = reason;
    
    String message;
    serializeJson(doc, message);
    sendToRS232(message);
    
    Serial.printf("OTA Abort sent to node %u: %s\n", nodeId, reason.c_str());
}

void handleOTAAccept(JsonDocument& data) {
    uint32_t nodeId = data["source_node"];
    Serial.printf("Node %u accepted OTA\n", nodeId);
    
    if (otaSessions.find(nodeId) != otaSessions.end()) {
        otaSessions[nodeId].lastActivity = millis();
        sendOTAChunk(nodeId, 0);
    }
}

void handleOTANext(JsonDocument& data) {
    uint32_t nodeId = data["source_node"];
    int nextIdx = data["idx"] | 0;
    
    if (otaSessions.find(nodeId) != otaSessions.end()) {
        otaSessions[nodeId].lastActivity = millis();
        otaSessions[nodeId].failedChunks = 0;
        sendOTAChunk(nodeId, nextIdx);
    }
}

void handleOTAResult(JsonDocument& data) {
    uint32_t nodeId = data["source_node"];
    bool success = data["ok"] | false;
    String message = data["msg"].as<String>();
    String newVersion = data["new_version"].as<String>();
    
    Serial.println("\n========================================");
    Serial.printf("%s OTA Result from Node %u\n", success ? "SUCCESS" : "FAILED", nodeId);
    Serial.printf("Message: %s\n", message.c_str());
    if (newVersion.length() > 0) {
        Serial.printf("New Version: %s\n", newVersion.c_str());
    }
    Serial.println("========================================\n");
    
    if (success) {
        stats.totalOTASuccess++;
        if (nodes.find(nodeId) != nodes.end()) {
            nodes[nodeId].version = newVersion;
            nodes[nodeId].status = "online";
        }
    } else {
        stats.totalOTAFailed++;
        if (nodes.find(nodeId) != nodes.end()) {
            nodes[nodeId].status = "online";
        }
    }
    
    otaSessions.erase(nodeId);
}

void checkOTATimeout() {
    unsigned long now = millis();
    
    for (auto it = otaSessions.begin(); it != otaSessions.end(); ) {
        OTASession& session = it->second;
        
        if (now - session.lastActivity > OTA_TIMEOUT) {
            Serial.printf("OTA timeout for node %u\n", session.nodeId);
            
            sendOTAAbort(session.nodeId, "timeout");
            
            if (nodes.find(session.nodeId) != nodes.end()) {
                nodes[session.nodeId].status = "online";
            }
            
            stats.totalOTAFailed++;
            it = otaSessions.erase(it);
        } else {
            ++it;
        }
    }
}

// =============================================================================
// RS232 COMMUNICATION
// =============================================================================

void sendToRS232(const String& data) {
    rs232Serial.println(data);
    rs232Serial.flush();
    stats.totalPacketsTX++;
}

void handleRS232Data() {
    while (rs232Serial.available()) {
        char c = rs232Serial.read();
        
        if (c == '\n' || c == '\r') {
            if (rs232Buffer.length() > 0) {
                processGatewayMessage(rs232Buffer);
                rs232Buffer = "";
            }
        } else {
            rs232Buffer += c;
        }
    }
}

void processGatewayMessage(const String& message) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    
    if (error) return;
    
    String msgType = doc["type"].as<String>();
    stats.totalPacketsRX++;
    
    Serial.printf("RX: %s\n", msgType.c_str());
    
    if (msgType == "ota_check_forward") {
        handleOTACheck(doc);
    }
    else if (msgType == "ota_accept") {
        handleOTAAccept(doc);
    }
    else if (msgType == "ota_next") {
        handleOTANext(doc);
    }
    else if (msgType == "ota_result") {
        handleOTAResult(doc);
    }
    else if (msgType == "mesh_data" || msgType == "sensor_data") {
        updateNodeFromMessage(doc);
        
        uint32_t nodeId = doc["source_node"] | doc["node_id"].as<uint32_t>();
        if (nodes.find(nodeId) != nodes.end()) {
            sendToThingsBoard(nodes[nodeId]);
        }
    }
    else if (msgType == "node_connected") {
        uint32_t nodeId = doc["node_id"];
        if (nodes.find(nodeId) != nodes.end()) {
            nodes[nodeId].status = "online";
            nodes[nodeId].lastSeen = millis();
        }
    }
    else if (msgType == "node_disconnected") {
        uint32_t nodeId = doc["node_id"];
        if (nodes.find(nodeId) != nodes.end()) {
            nodes[nodeId].status = "offline";
        }
    }
}

void updateNodeFromMessage(JsonDocument& data) {
    uint32_t nodeId = data["source_node"] | data["node_id"].as<uint32_t>();
    
    NodeInfo& node = nodes[nodeId];
    node.id = nodeId;
    node.lastSeen = millis();
    node.status = "online";
    
    if (!data["device_name"].isNull()) {
        node.name = data["device_name"].as<String>();
    }
    if (!data["access_token"].isNull()) {
        node.token = data["access_token"].as<String>();
    }
    if (!data["fw_version"].isNull()) {
        node.version = data["fw_version"].as<String>();
    }
    if (!data["role"].isNull()) {
        node.role = data["role"].as<String>();
    }
    
    JsonObject sensorData = data["data"].as<JsonObject>();
    if (!sensorData.isNull() && !sensorData["earthquake"].isNull()) {
        JsonObject eq = sensorData["earthquake"];
        node.si = eq["si"] | 0.0;
        node.pga = eq["pga"] | 0.0;
        node.temperature = eq["temp"] | 0.0;
    }
}

void sendToThingsBoard(const NodeInfo& node) {
    if (node.token.length() == 0) return;
    
    HTTPClient http;
    String url = "http://" + String(THINGBOARD_SERVER) + ":" + String(THINGBOARD_PORT) +
                 "/api/v1/" + node.token + "/telemetry";
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    JsonDocument doc;
    doc["si"] = node.si;
    doc["pga"] = node.pga;
    doc["temp"] = node.temperature;
    
    String payload;
    serializeJson(doc, payload);
    
    int code = http.POST(payload);
    
    if (code == 200) {
        Serial.printf("ThingsBoard: Node %u OK\n", node.id);
    } else {
        Serial.printf("ThingsBoard: Failed %d\n", code);
    }
    
    http.end();
}

void cleanupInactiveNodes() {
    unsigned long now = millis();
    for (auto it = nodes.begin(); it != nodes.end(); ) {
        if (now - it->second.lastSeen > 60000 && it->second.status == "online") {
            it->second.status = "offline";
            Serial.printf("Node %u offline\n", it->first);
        }
        ++it;
    }
}

// =============================================================================
// OLED DISPLAY
// =============================================================================

void updateDisplay() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    
    display.setCursor(0, 0);
    display.println("=== GATEWAY v3.0.2 ===");
    
    display.setCursor(0, 12);
    display.printf("IP: %s", WiFi.localIP().toString().c_str());
    
    display.setCursor(0, 24);
    int online = 0;
    for (const auto& p : nodes) {
        if (p.second.status == "online") online++;
    }
    display.printf("Nodes: %d/%d", online, nodes.size());
    
    display.setCursor(0, 36);
    display.printf("OTA: %d/%d", stats.totalOTASuccess, stats.totalOTAFailed);
    
    display.setCursor(0, 48);
    display.printf("Auto: %s", autoOTAEnabled ? "ON" : "OFF");
    
    display.setCursor(0, 56);
    display.print("COMPLETE");
    
    display.display();
}