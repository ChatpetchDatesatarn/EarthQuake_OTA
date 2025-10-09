#include <painlessMesh.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/TomThumb.h>
#include <map>
#include <vector>
#include <queue>

// =============================================================================
// OPTIMIZED MESH CONFIGURATION FOR MAXIMUM SPEED
// =============================================================================
#define MESH_PREFIX     "ESP32_MESH"
#define MESH_PASSWORD   "mesh123456"
#define MESH_PORT       5555

// RS232 Configuration - Optimized
#define RS232_TX_PIN    16  // Connect to WiFiGateway RX (pin 15)
#define RS232_RX_PIN    15  // Connect to WiFiGateway TX (pin 16)
#define RS232_BAUD      115200
#define RS232_BUFFER_SIZE 4096  // Larger buffer for better throughput

// I2C Configuration
#define I2C_SDA         4
#define I2C_SCL         5

// OLED Configuration
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDRESS    0x3C

// Button Configuration
#define BTN_UP_LEFT     17
#define BTN_DOWN_RIGHT  18
#define BTN_SELECT      0

// OPTIMIZED Timing Configuration for FAST MESH CONNECTIONS
#define HEARTBEAT_INTERVAL      20000   // 20 seconds
#define NODE_TIMEOUT            30000   // 30 seconds
#define RS232_SEND_INTERVAL     500     // 0.5 seconds
#define MESH_STABILITY_CHECK    2000    // 2 seconds
#define CONNECTION_RETRY_DELAY  1000    // 1 second retry delay
#define BATCH_PROCESS_INTERVAL  20      // 20ms batch processing

// Advanced Mesh Optimization Parameters
#define MESH_MAX_CONNECTIONS    10      // Maximum simultaneous connections
#define MESSAGE_QUEUE_SIZE      200     // Increased queue size
#define PRIORITY_MESSAGE_LIMIT  40      // High priority message limit
#define CONGESTION_THRESHOLD    70      // Queue congestion threshold (70%)
#define FLOW_CONTROL_ENABLED    true    // Enable flow control

// OTA Configuration
#ifndef FW_VERSION
  #define FW_VERSION "2.1.0"
#endif
#define ROLE_KEY "ROLE_MESH_GATEWAY"

// =============================================================================
// OPTIMIZED DATA STRUCTURES
// =============================================================================

// Fast message queue for batch processing
struct MeshMessage {
    String data;
    uint32_t targetNode;
    unsigned long timestamp;
    int priority;  // 0 = highest, 2 = lowest
    int retryCount;
};

// Enhanced node information with performance metrics
struct OptimizedNodeInfo {
    uint32_t nodeId;
    unsigned long lastSeen;
    unsigned long firstSeen;
    String lastData;
    bool isActive;
    int missedHeartbeats;
    int signalStrength;
    unsigned long totalMessages;
    unsigned long averageResponseTime;
    String deviceName;
    String accessToken;
    String fwVersion;
    bool isHighPriority;
};

// Connection quality metrics
struct ConnectionMetrics {
    unsigned long totalConnections = 0;
    unsigned long totalDisconnections = 0;
    unsigned long totalReconnections = 0;
    unsigned long averageConnectionTime = 0;
    unsigned long fastestConnection = ULONG_MAX;
    unsigned long slowestConnection = 0;
};

// =============================================================================
// GLOBAL VARIABLES - OPTIMIZED
// =============================================================================
painlessMesh mesh;
HardwareSerial rs232Serial(2);

// OLED Display
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Enhanced Menu System
enum MenuState {
    MENU_MAIN,
    MENU_MESH_INFO,
    MENU_NODES_INFO,
    MENU_PERFORMANCE,
    MENU_CONNECTION_QUALITY
};

struct MenuSystem {
    MenuState currentMenu = MENU_MAIN;
    int selectedItem = 0;
    unsigned long lastButtonPress = 0;
    bool needsUpdate = true;
} menu;

struct ButtonState {
    bool upLeftPressed = false;
    bool downRightPressed = false;
    bool selectPressed = false;
    unsigned long lastDebounce = 0;
} buttons;

// Optimized node management
std::map<uint32_t, OptimizedNodeInfo> activeNodes;
std::queue<MeshMessage> messageQueue;
std::queue<MeshMessage> priorityQueue;

// Timing variables
unsigned long lastHeartbeatCheck = 0;
unsigned long lastMeshStatusCheck = 0;
unsigned long lastRS232Send = 0;
unsigned long lastBatchProcess = 0;
unsigned long lastStabilityCheck = 0;

// Enhanced statistics
struct OptimizedMeshStats {
    unsigned long totalMessagesReceived = 0;
    unsigned long totalMessagesSent = 0;
    unsigned long totalNodesConnected = 0;
    unsigned long totalBatchesProcessed = 0;
    unsigned long totalPriorityMessages = 0;
    unsigned long averageProcessingTime = 0;
    unsigned long queuedMessages = 0;
    unsigned long droppedMessages = 0;
    ConnectionMetrics connectionMetrics;
};

OptimizedMeshStats stats;

// ==== OTA helpers (RS232 <-> Mesh) ====
static bool isOtaJsonLine(const String& s) {
  int t = s.indexOf("\"type\"");
  if (t < 0) return false;
  int v = s.indexOf("ota_", t);
  return v >= 0;
}

// ดึง target_node แบบเร็ว (string) ถ้าไม่มีให้คืน ""
static String quickGetTargetNode(const String& s) {
  int k = s.indexOf("\"target_node\"");
  if (k < 0) return "";
  int c = s.indexOf(':', k);
  if (c < 0) return "";
  int q1 = s.indexOf('"', c);
  if (q1 < 0) return "";
  int q2 = s.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return s.substring(q1 + 1, q2);
}

// ส่งสตริง JSON ota_* เข้าสู่ mesh (single-cast ไป node เป้าหมาย)
static void forwardOtaToMesh(const String& jsonLine) {
  String tgt = quickGetTargetNode(jsonLine);
  if (tgt.length() == 0) {
    Serial.println("[MESH GW] OTA no target_node -> skip");
    return;
  }
  uint32_t nodeId = strtoul(tgt.c_str(), nullptr, 10);
  bool ok = mesh.sendSingle(nodeId, jsonLine.c_str());
  Serial.printf("[MESH GW] OTA->Mesh node=%u %s\n", nodeId, ok ? "SENT" : "FAILED");
}

// =============================================================================
// FUNCTION DECLARATIONS
// =============================================================================
void receivedCallbackOptimized(uint32_t from, String &msg);
void newConnectionCallbackOptimized(uint32_t nodeId);
void changedConnectionCallbackOptimized();
void nodeTimeAdjustedCallbackOptimized(int32_t offset);
void sendOptimizedHeartbeat();
void checkNodeHealthOptimized();
void sendToRS232Optimized(const String& data);
void updateMeshStatusOptimized();
void printOptimizedMeshInfo();
void handleRS232ResponseOptimized();
void processBatchMessages();
void optimizeMeshSettings();
void handlePriorityMessages();
bool isHighPriorityMessage(const String& msg);
void updateConnectionMetrics(uint32_t nodeId, bool connected);

// OLED and Menu functions
void setupOLED();
void setupButtons();
void updateDisplay();
void handleButtons();
void drawMainMenu();
void drawMeshInfo();
void drawNodesInfo();
void drawPerformanceInfo();
void drawConnectionQuality();
void navigateMenu(bool up, bool select);

// =============================================================================
// SETUP FUNCTION - OPTIMIZED
// =============================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("=== OPTIMIZED Mesh Gateway v2.1.0 Starting ===");
    Serial.println("ArduinoJson v7 Compatible | OTA Ready");
    
    // Initialize OLED and buttons
    setupOLED();
    setupButtons();
    
    // Initialize RS232 with larger buffer
    rs232Serial.begin(RS232_BAUD, SERIAL_8N1, RS232_RX_PIN, RS232_TX_PIN);
    rs232Serial.setRxBufferSize(RS232_BUFFER_SIZE);
    Serial.printf("RS232 initialized with %d byte buffer\n", RS232_BUFFER_SIZE);
    
    // Initialize mesh network with MAXIMUM OPTIMIZATION
    mesh.setDebugMsgTypes(ERROR | STARTUP);
    mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
    
    // ADVANCED MESH OPTIMIZATION SETTINGS
    optimizeMeshSettings();
    
    // Set optimized mesh callbacks
    mesh.onReceive(&receivedCallbackOptimized);
    mesh.onNewConnection(&newConnectionCallbackOptimized);
    mesh.onChangedConnections(&changedConnectionCallbackOptimized);
    mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallbackOptimized);
    
    Serial.println("Mesh network initialized!");
    Serial.printf("Gateway ID: %u\n", mesh.getNodeId());
    Serial.printf("FW Version: %s | Role: %s\n", FW_VERSION, ROLE_KEY);
    
    // Send optimized startup message
    JsonDocument startupMsg;
    startupMsg["type"] = "gateway_startup";
    startupMsg["gateway_id"] = mesh.getNodeId();
    startupMsg["timestamp"] = millis();
    startupMsg["status"] = "optimized_online";
    startupMsg["version"] = FW_VERSION;
    startupMsg["role"] = ROLE_KEY;
    startupMsg["features"] = "fast_mesh,batch_processing,priority_queue,ota_ready";
    
    String startupStr;
    serializeJson(startupMsg, startupStr);
    sendToRS232Optimized(startupStr);
    
    Serial.println("=== Gateway Ready! ===");
}

// =============================================================================
// MAIN LOOP - OPTIMIZED FOR MAXIMUM PERFORMANCE
// =============================================================================
void loop() {
    mesh.update();
    
    unsigned long currentTime = millis();
    
    // Handle button inputs
    handleButtons();
    
    // Update OLED display
    updateDisplay();
    
    // PRIORITY 1: Process high-priority messages immediately
    handlePriorityMessages();
    
    // PRIORITY 2: Batch process regular messages
    if (currentTime - lastBatchProcess >= BATCH_PROCESS_INTERVAL) {
        processBatchMessages();
        lastBatchProcess = currentTime;
    }
    
    // PRIORITY 3: Fast heartbeat system
    if (currentTime - lastHeartbeatCheck >= HEARTBEAT_INTERVAL) {
        sendOptimizedHeartbeat();
        checkNodeHealthOptimized();
        lastHeartbeatCheck = currentTime;
    }
    
    // PRIORITY 4: Mesh stability and status updates
    if (currentTime - lastStabilityCheck >= MESH_STABILITY_CHECK) {
        updateMeshStatusOptimized();
        lastStabilityCheck = currentTime;
    }
    
    // PRIORITY 5: Detailed mesh info
    if (currentTime - lastMeshStatusCheck >= 10000) {
        printOptimizedMeshInfo();
        lastMeshStatusCheck = currentTime;
    }
    
    // Handle RS232 responses
    handleRS232ResponseOptimized();
    
    delay(1);
}

// =============================================================================
// ADVANCED MESH OPTIMIZATION
// =============================================================================
void optimizeMeshSettings() {
    Serial.println("Applying advanced mesh optimizations...");
    
    mesh.setContainsRoot(true);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    
    Serial.println("Advanced mesh optimizations applied!");
}

// =============================================================================
// OPTIMIZED MESSAGE HANDLING - ArduinoJson v7
// =============================================================================
void receivedCallbackOptimized(uint32_t from, String &msg) {
    stats.totalMessagesReceived++;
    unsigned long startTime = millis();
    
    // Check for congestion and implement flow control
    if (FLOW_CONTROL_ENABLED) {
        unsigned long totalQueueSize = messageQueue.size() + priorityQueue.size();
        if (totalQueueSize > (MESSAGE_QUEUE_SIZE * CONGESTION_THRESHOLD / 100)) {
            Serial.printf("CONGESTION! Queue: %lu/%d (%.1f%%) - Checking priority from %u\n", 
                         totalQueueSize, MESSAGE_QUEUE_SIZE, 
                         (float)totalQueueSize / MESSAGE_QUEUE_SIZE * 100, from);
            
            if (!isHighPriorityMessage(msg)) {
                stats.droppedMessages++;
                return;
            }
            Serial.printf("HIGH PRIORITY message accepted during congestion from %u\n", from);
        }
    }
    
    // ArduinoJson v7: Use JsonDocument instead of StaticJsonDocument
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, msg);
    
    if (error) {
        Serial.printf("JSON parse error from %u: %s\n", from, error.c_str());
        Serial.printf("Message size: %d bytes\n", msg.length());
        stats.droppedMessages++;
        return;
    }
    
    Serial.printf("JSON parsed successfully (%d bytes) from node %u\n", msg.length(), from);
    
    // Update node info
    OptimizedNodeInfo& nodeInfo = activeNodes[from];
    nodeInfo.nodeId = from;
    nodeInfo.lastSeen = millis();
    nodeInfo.lastData = msg;
    nodeInfo.isActive = true;
    nodeInfo.missedHeartbeats = 0;
    nodeInfo.totalMessages++;
    
    // Extract device info - ArduinoJson v7 compatible
    if (!doc["device_name"].isNull()) {
    nodeInfo.deviceName = doc["device_name"].as<String>();
    }
    if (!doc["access_token"].isNull()) {
    nodeInfo.accessToken = doc["access_token"].as<String>();
    }
    if (!doc["fw_version"].isNull()) {
    nodeInfo.fwVersion = doc["fw_version"].as<String>();
    }
    
    String messageType = doc["type"] | "";
    
        // ==== OTA replies from node (mesh -> RS232) ====
    if (messageType.startsWith("ota_")) {
        // ตัวอย่าง: ota_accept, ota_next, ota_result, ota_error
        // ส่งกลับขึ้นไป Wi-Fi GW/Backend ตรง ๆ
        sendToRS232Optimized(msg);
        Serial.printf("[MESH GW] Mesh->RS232 OTA from %u\n", from);
        return; // ไม่ต้องประมวลผลต่อ
    }

    // Determine priority
    bool isHighPriority = isHighPriorityMessage(msg);
    nodeInfo.isHighPriority = isHighPriority;
    
    if (messageType == "sensor_data") {
        // Create optimized forward message - ArduinoJson v7
        JsonDocument forwardMsg;
        forwardMsg["type"] = "mesh_data";
        forwardMsg["source_node"] = from;
        forwardMsg["gateway_id"] = mesh.getNodeId();
        forwardMsg["timestamp"] = millis();
        forwardMsg["priority"] = isHighPriority ? "high" : "normal";
        
        // Forward essential fields
        if (!doc["access_token"].isNull()) {
            forwardMsg["access_token"] = doc["access_token"];
        }
        if (!doc["device_name"].isNull()) {
            forwardMsg["device_name"] = doc["device_name"];
        }
        if (!doc["fw_version"].isNull()) {
            forwardMsg["fw_version"] = doc["fw_version"];
        }
        
        // Copy sensor data - ArduinoJson v7 uses to<JsonObject>()
        JsonObject dataObj = forwardMsg["data"].to<JsonObject>();
        for (JsonPair kv : doc.as<JsonObject>()) {
            dataObj[kv.key()] = kv.value();
        }
        
        String forwardStr;
        serializeJson(forwardMsg, forwardStr);
        
        // Queue message based on priority
        MeshMessage meshMsg;
        meshMsg.data = forwardStr;
        meshMsg.targetNode = 0;
        meshMsg.timestamp = millis();
        meshMsg.priority = isHighPriority ? 0 : 1;
        meshMsg.retryCount = 0;
        
        if (isHighPriority) {
            priorityQueue.push(meshMsg);
            stats.totalPriorityMessages++;
            Serial.printf("HIGH PRIORITY from node %u queued\n", from);
            sendToRS232Optimized(forwardStr);
        } else {
            messageQueue.push(meshMsg);
            Serial.printf("Normal message from node %u queued\n", from);
            sendToRS232Optimized(forwardStr);
        }
        
        stats.queuedMessages++;
        
    } else if (messageType == "heartbeat_response") {
        Serial.printf("Heartbeat from node %u\n", from);
        
    } else if (messageType == "node_status") {
        if (!doc["signal_strength"].isNull()) {
            nodeInfo.signalStrength = doc["signal_strength"];
        }
        
        // Forward status to WiFiGateway - ArduinoJson v7
        JsonDocument statusMsg;
        statusMsg["type"] = "node_status";
        statusMsg["source_node"] = from;
        statusMsg["gateway_id"] = mesh.getNodeId();
        statusMsg["timestamp"] = millis();
        
        if (!doc["access_token"].isNull()) statusMsg["access_token"] = doc["access_token"];
        if (!doc["device_name"].isNull()) statusMsg["device_name"] = doc["device_name"];
        if (!doc["status"].isNull()) statusMsg["status"] = doc["status"];
        if (!doc["signal_strength"].isNull()) statusMsg["signal_strength"] = doc["signal_strength"];
        if (!doc["fw_version"].isNull()) statusMsg["fw_version"] = doc["fw_version"];
        
        String statusStr;
        serializeJson(statusMsg, statusStr);
        sendToRS232Optimized(statusStr);
        
    } else if (messageType == "ota_check") {
        // OTA check from sender node
        Serial.printf("[OTA] Check from node %u: role=%s fw=%s\n", 
                     from, 
                     doc["role"].as<const char*>(),
                     doc["fw"].as<const char*>());
        
        // Forward to WiFi Gateway for OTA management
        JsonDocument otaCheck;
        otaCheck["type"] = "ota_check_forward";
        otaCheck["source_node"] = from;
        otaCheck["gateway_id"] = mesh.getNodeId();
        otaCheck["role"] = doc["role"];
        otaCheck["fw_version"] = doc["fw"];
        otaCheck["timestamp"] = millis();
        
        String otaCheckStr;
        serializeJson(otaCheck, otaCheckStr);
        sendToRS232Optimized(otaCheckStr);
    }
    
    // Update performance metrics
    unsigned long processingTime = millis() - startTime;
    stats.averageProcessingTime = (stats.averageProcessingTime + processingTime) / 2;
    
    Serial.printf("Processed in %lums | Queue: %d | Priority: %d\n", 
                  processingTime, messageQueue.size(), priorityQueue.size());
}

bool isHighPriorityMessage(const String& msg) {
    return msg.indexOf("earthquake") != -1 || 
           msg.indexOf("emergency") != -1 || 
           msg.indexOf("alert") != -1 ||
           msg.indexOf("\"si\":") != -1 ||
           msg.indexOf("\"pga\":") != -1;
}

void handlePriorityMessages() {
    while (!priorityQueue.empty()) {
        MeshMessage msg = priorityQueue.front();
        priorityQueue.pop();
        stats.queuedMessages--;
        
        sendToRS232Optimized(msg.data);
        Serial.println("HIGH PRIORITY message sent immediately");
    }
}

void processBatchMessages() {
    if (messageQueue.empty()) return;
    
    int processed = 0;
    const int maxBatchSize = 5;
    
    while (!messageQueue.empty() && processed < maxBatchSize) {
        MeshMessage msg = messageQueue.front();
        messageQueue.pop();
        stats.queuedMessages--;
        
        sendToRS232Optimized(msg.data);
        processed++;
    }
    
    if (processed > 0) {
        stats.totalBatchesProcessed++;
        Serial.printf("BATCH processed: %d messages\n", processed);
    }
}

// =============================================================================
// OPTIMIZED CONNECTION MANAGEMENT
// =============================================================================
void newConnectionCallbackOptimized(uint32_t nodeId) {
    unsigned long connectionTime = millis();
    Serial.printf("FAST connection: %u\n", nodeId);
    
    stats.totalNodesConnected++;
    updateConnectionMetrics(nodeId, true);
    
    OptimizedNodeInfo& nodeInfo = activeNodes[nodeId];
    nodeInfo.nodeId = nodeId;
    nodeInfo.firstSeen = connectionTime;
    nodeInfo.lastSeen = connectionTime;
    nodeInfo.isActive = true;
    nodeInfo.missedHeartbeats = 0;
    nodeInfo.totalMessages = 0;
    nodeInfo.averageResponseTime = 0;
    nodeInfo.isHighPriority = false;
    
    // Send welcome message - ArduinoJson v7
    JsonDocument welcomeMsg;
    welcomeMsg["type"] = "welcome";
    welcomeMsg["gateway_id"] = mesh.getNodeId();
    welcomeMsg["timestamp"] = connectionTime;
    welcomeMsg["version"] = FW_VERSION;
    welcomeMsg["optimized"] = true;
    
    String welcomeStr;
    serializeJson(welcomeMsg, welcomeStr);
    mesh.sendSingle(nodeId, welcomeStr);
    
    // Notify WiFiGateway - ArduinoJson v7
    JsonDocument notifyMsg;
    notifyMsg["type"] = "node_connected";
    notifyMsg["node_id"] = nodeId;
    notifyMsg["gateway_id"] = mesh.getNodeId();
    notifyMsg["timestamp"] = connectionTime;
    notifyMsg["connection_speed"] = "optimized";
    
    String notifyStr;
    serializeJson(notifyMsg, notifyStr);
    sendToRS232Optimized(notifyStr);
}

void changedConnectionCallbackOptimized() {
    Serial.println("Connection topology changed");
    
    std::list<uint32_t> nodeList = mesh.getNodeList();
    
    for (auto& pair : activeNodes) {
        bool found = false;
        for (uint32_t nodeId : nodeList) {
            if (pair.first == nodeId) {
                found = true;
                break;
            }
        }
        if (!found && pair.second.isActive) {
            pair.second.isActive = false;
            updateConnectionMetrics(pair.first, false);
            
            // Disconnect notification - ArduinoJson v7
            JsonDocument disconnectMsg;
            disconnectMsg["type"] = "node_disconnected";
            disconnectMsg["node_id"] = pair.first;
            disconnectMsg["gateway_id"] = mesh.getNodeId();
            disconnectMsg["timestamp"] = millis();
            disconnectMsg["reason"] = "topology_change";
            
            String disconnectStr;
            serializeJson(disconnectMsg, disconnectStr);
            sendToRS232Optimized(disconnectStr);
        }
    }
}

void updateConnectionMetrics(uint32_t nodeId, bool connected) {
    if (connected) {
        stats.connectionMetrics.totalConnections++;
    } else {
        stats.connectionMetrics.totalDisconnections++;
    }
}

void nodeTimeAdjustedCallbackOptimized(int32_t offset) {
    Serial.printf("Time adjusted: %d ms offset\n", offset);
}

// =============================================================================
// OPTIMIZED HEARTBEAT AND HEALTH MONITORING
// =============================================================================
void sendOptimizedHeartbeat() {
    JsonDocument heartbeatMsg;
    heartbeatMsg["type"] = "heartbeat_request";
    heartbeatMsg["gateway_id"] = mesh.getNodeId();
    heartbeatMsg["timestamp"] = millis();
    heartbeatMsg["version"] = FW_VERSION;
    heartbeatMsg["optimized"] = true;
    
    String heartbeatStr;
    serializeJson(heartbeatMsg, heartbeatStr);
    
    mesh.sendBroadcast(heartbeatStr);
    stats.totalMessagesSent++;
    
    Serial.printf("Heartbeat sent to %d nodes\n", mesh.getNodeList().size());
}

void checkNodeHealthOptimized() {
    unsigned long currentTime = millis();
    
    for (auto& pair : activeNodes) {
        OptimizedNodeInfo& nodeInfo = pair.second;
        
        if (nodeInfo.isActive && (currentTime - nodeInfo.lastSeen > NODE_TIMEOUT)) {
            nodeInfo.missedHeartbeats++;
            
            if (nodeInfo.missedHeartbeats >= 2) {
                Serial.printf("Node %u timeout (missed: %d)\n", 
                             nodeInfo.nodeId, nodeInfo.missedHeartbeats);
                nodeInfo.isActive = false;
                
                JsonDocument alertMsg;
                alertMsg["type"] = "node_timeout";
                alertMsg["node_id"] = nodeInfo.nodeId;
                alertMsg["gateway_id"] = mesh.getNodeId();
                alertMsg["timestamp"] = currentTime;
                alertMsg["missed_heartbeats"] = nodeInfo.missedHeartbeats;
                alertMsg["device_name"] = nodeInfo.deviceName;
                
                String alertStr;
                serializeJson(alertMsg, alertStr);
                sendToRS232Optimized(alertStr);
            }
        }
    }
}

// =============================================================================
// OPTIMIZED RS232 COMMUNICATION
// =============================================================================
void sendToRS232Optimized(const String& data) {
    Serial.printf("Sending to RS232: %d bytes\n", data.length());
    
    if (data.length() > 1000) {
        const size_t chunkSize = 128;
        for (size_t i = 0; i < data.length(); i += chunkSize) {
            size_t remainingBytes = data.length() - i;
            size_t bytesToSend = (remainingBytes < chunkSize) ? remainingBytes : chunkSize;
            
            String chunk = data.substring(i, i + bytesToSend);
            rs232Serial.print(chunk);
            rs232Serial.flush();
            delay(2);
        }
        rs232Serial.println();
    } else {
        rs232Serial.println(data);
    }
    
    rs232Serial.flush();
    stats.totalMessagesSent++;
}

void updateMeshStatusOptimized() {
    std::list<uint32_t> nodeList = mesh.getNodeList();
    
    JsonDocument statusMsg;
    statusMsg["type"] = "mesh_status";
    statusMsg["gateway_id"] = mesh.getNodeId();
    statusMsg["timestamp"] = millis();
    statusMsg["connected_nodes"] = nodeList.size();
    statusMsg["version"] = FW_VERSION;
    statusMsg["role"] = ROLE_KEY;
    
    statusMsg["total_messages_received"] = stats.totalMessagesReceived;
    statusMsg["total_messages_sent"] = stats.totalMessagesSent;
    statusMsg["total_batches_processed"] = stats.totalBatchesProcessed;
    statusMsg["queued_messages"] = stats.queuedMessages;
    statusMsg["priority_messages"] = stats.totalPriorityMessages;
    statusMsg["average_processing_time"] = stats.averageProcessingTime;
    statusMsg["uptime"] = millis();
    
    JsonObject connMetrics = statusMsg["connection_metrics"].to<JsonObject>();
    connMetrics["total_connections"] = stats.connectionMetrics.totalConnections;
    connMetrics["total_disconnections"] = stats.connectionMetrics.totalDisconnections;
    
    JsonArray nodesArray = statusMsg["active_nodes"].to<JsonArray>();
    for (uint32_t nodeId : nodeList) {
        if (activeNodes.find(nodeId) != activeNodes.end()) {
            JsonObject nodeObj = nodesArray.add<JsonObject>();
            OptimizedNodeInfo& nodeInfo = activeNodes[nodeId];
            
            nodeObj["node_id"] = nodeId;
            nodeObj["last_seen"] = nodeInfo.lastSeen;
            nodeObj["is_active"] = nodeInfo.isActive;
            nodeObj["total_messages"] = nodeInfo.totalMessages;
            nodeObj["device_name"] = nodeInfo.deviceName;
            nodeObj["fw_version"] = nodeInfo.fwVersion;
            nodeObj["is_high_priority"] = nodeInfo.isHighPriority;
        }
    }
    
    String statusStr;
    serializeJson(statusMsg, statusStr);
    sendToRS232Optimized(statusStr);
}

void printOptimizedMeshInfo() {
    Serial.println("\n=== MESH GATEWAY STATUS v2.1.0 ===");
    Serial.printf("Gateway ID: %u | FW: %s | Uptime: %lu seconds\n", 
                  mesh.getNodeId(), FW_VERSION, millis() / 1000);
    Serial.printf("Connected Nodes: %d | Active Tracked: %d\n", 
                  mesh.getNodeList().size(), activeNodes.size());
    
    unsigned long totalQueueSize = messageQueue.size() + priorityQueue.size();
    float queueUtilization = (float)totalQueueSize / MESSAGE_QUEUE_SIZE * 100;
    Serial.printf("Queue: Normal=%d | Priority=%d | Total=%lu/%d (%.1f%%)\n", 
                  messageQueue.size(), priorityQueue.size(), 
                  totalQueueSize, MESSAGE_QUEUE_SIZE, queueUtilization);
    
    if (queueUtilization > CONGESTION_THRESHOLD) {
        Serial.printf("CONGESTION WARNING: Queue %.1f%% full!\n", queueUtilization);
    }
    
    Serial.printf("Messages: RX=%lu | TX=%lu | Dropped=%lu\n", 
                  stats.totalMessagesReceived, stats.totalMessagesSent, stats.droppedMessages);
    Serial.printf("Processing: Batches=%lu | Priority=%lu | Avg=%lums\n", 
                  stats.totalBatchesProcessed, stats.totalPriorityMessages, stats.averageProcessingTime);
    
    Serial.println("Active Nodes:");
    for (const auto& pair : activeNodes) {
        const OptimizedNodeInfo& nodeInfo = pair.second;
        if (nodeInfo.isActive) {
            unsigned long lastSeenSec = (millis() - nodeInfo.lastSeen) / 1000;
            Serial.printf("  Node %u: %s | FW:%s | Msgs=%lu | LastSeen=%lus\n", 
                         nodeInfo.nodeId, 
                         nodeInfo.deviceName.c_str(),
                         nodeInfo.fwVersion.c_str(),
                         nodeInfo.totalMessages,
                         lastSeenSec);
        }
    }
    Serial.println("=====================================\n");
}

void handleRS232ResponseOptimized() {
    if (rs232Serial.available()) {
        String response = rs232Serial.readStringUntil('\n');
        response.trim();

        if (response.length() > 0) {
            Serial.printf("RS232 Response: %s\n", response.c_str());

            // >>> OTA pass-through: RS232 -> Mesh <<<
            if (isOtaJsonLine(response)) {
                // เป็น ota_offer / ota_chunk / ota_end / ota_abort จาก Wi-Fi GW
                forwardOtaToMesh(response);
                return; // ไม่ต้อง parse/ประมวลผลคำสั่งอื่น
            }
            // >>> end OTA pass-through <<<

            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, response);

            if (!error) {
                String responseType = doc["type"] | "";

                if (responseType == "command") {
                    String command = doc["command"] | "";
                    uint32_t targetNode = doc["target_node"] | 0;

                    if (command == "restart_node" && targetNode > 0) {
                        JsonDocument cmdMsg;
                        cmdMsg["type"] = "command";
                        cmdMsg["command"] = "restart";
                        cmdMsg["from_gateway"] = mesh.getNodeId();
                        cmdMsg["timestamp"] = millis();

                        String cmdStr;
                        serializeJson(cmdMsg, cmdStr);
                        mesh.sendSingle(targetNode, cmdStr);

                        Serial.printf("Restart command sent to node %u\n", targetNode);
                    }
                }
            }
        }
    }
}


// =============================================================================
// ENHANCED OLED DISPLAY FUNCTIONS
// =============================================================================
void setupOLED() {
    Serial.println("Setting up OLED display...");
    Wire.begin(I2C_SDA, I2C_SCL);
    
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        Serial.println("SSD1306 allocation failed");
        return;
    }
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    
    display.setCursor(0, 0);
    display.print(">> MESH GATEWAY <<");
    display.drawLine(0, 9, SCREEN_WIDTH, 9, SSD1306_WHITE);
    
    display.setCursor(0, 15);
    display.print("Version   : 2.1.0");
    display.setCursor(0, 24);
    display.print("Type      : Optimized");
    display.setCursor(0, 33);
    display.print("Function  : Mesh Bridge");
    display.setCursor(0, 42);
    display.print("Protocol  : RS232");
    
    display.drawLine(0, 54, SCREEN_WIDTH, 54, SSD1306_WHITE);
    display.setCursor(0, 56);
    display.print("Initializing...");
    display.display();
    
    Serial.println("OLED display initialized");
}

void setupButtons() {
    pinMode(BTN_UP_LEFT, INPUT_PULLUP);
    pinMode(BTN_DOWN_RIGHT, INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);
}

void handleButtons() {
    unsigned long currentTime = millis();
    if (currentTime - buttons.lastDebounce < 50) return;
    
    bool upLeftCurrent = !digitalRead(BTN_UP_LEFT);
    bool downRightCurrent = !digitalRead(BTN_DOWN_RIGHT);
    bool selectCurrent = !digitalRead(BTN_SELECT);
    
    if (upLeftCurrent && !buttons.upLeftPressed) {
        navigateMenu(true, false);
        buttons.lastDebounce = currentTime;
    }
    buttons.upLeftPressed = upLeftCurrent;
    
    if (downRightCurrent && !buttons.downRightPressed) {
        navigateMenu(false, false);
        buttons.lastDebounce = currentTime;
    }
    buttons.downRightPressed = downRightCurrent;
    
    if (selectCurrent && !buttons.selectPressed) {
        navigateMenu(false, true);
        buttons.lastDebounce = currentTime;
    }
    buttons.selectPressed = selectCurrent;
}

void updateDisplay() {
    if (!menu.needsUpdate) return;
    
    display.clearDisplay();
    
    switch (menu.currentMenu) {
        case MENU_MAIN: drawMainMenu(); break;
        case MENU_MESH_INFO: drawMeshInfo(); break;
        case MENU_NODES_INFO: drawNodesInfo(); break;
        case MENU_PERFORMANCE: drawPerformanceInfo(); break;
        case MENU_CONNECTION_QUALITY: drawConnectionQuality(); break;
        default: drawMainMenu(); break;
    }
    
    display.display();
    menu.needsUpdate = false;
}

void drawMainMenu() {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setFont(&TomThumb);
    
    display.setCursor(33, 6);
    display.print(">> MESH GATEWAY <<");
    display.setCursor(1, 61);
    display.print("Connected : ");
    display.setCursor(44, 61);
    std::list<uint32_t> nodeList = mesh.getNodeList();
    display.println(nodeList.size());
    display.setCursor(53, 61);
    display.print("Name : ");
    display.setCursor(75, 61);
    display.println("MeshGateway");
    display.drawLine(0, 10, 128, 10, 1);
    display.drawLine(0, 53, 128, 53, 1);
    
    const char* menuItems[] = {
        "1.Mesh Info",
        "2.Nodes Info",
        "3.Performance", 
        "4.Connection"
    };
    
    int menuYPositions[] = {19, 29, 40, 51};
    
    for (int i = 0; i < 4; i++) {
        display.setCursor(3, menuYPositions[i]);
        if (i == menu.selectedItem) {
            display.println(">");
        }
        display.setCursor(8, menuYPositions[i]);
        display.println(menuItems[i]);
    }
}

void drawMeshInfo() {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setFont(&TomThumb);
    
    display.setCursor(33, 6);
    display.print(">> MESH GATEWAY <<");
    display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);
    
    display.setCursor(0, 16);
    display.printf("Network   : %s", MESH_PREFIX);
    display.setCursor(0, 25);
    display.printf("Port      : %d", MESH_PORT);
    display.setCursor(0, 34);
    display.printf("My ID     : %u", mesh.getNodeId());
    display.setCursor(0, 43);
    display.printf("Version   : %s", FW_VERSION);
    
    std::list<uint32_t> nodeList = mesh.getNodeList();
    display.setCursor(0, 52);
    display.printf("Connected : %d", nodeList.size());
    
    display.drawLine(0, 57, SCREEN_WIDTH, 57, SSD1306_WHITE);
    display.setCursor(0, 61);
    display.print("SELECT: Back");
}

void drawNodesInfo() {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setFont(&TomThumb);
    
    display.setCursor(33, 6);
    display.print(">> MESH GATEWAY <<");
    display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);
    
    display.setCursor(0, 16);
    
    int nodeCount = 0;
    for (const auto& pair : activeNodes) {
        if (nodeCount >= 4) break;
        
        const OptimizedNodeInfo& nodeInfo = pair.second;
        if (nodeInfo.isActive) {
            unsigned long timeSince = (millis() - nodeInfo.lastSeen) / 1000;
            display.setCursor(0, 16 + nodeCount * 9);
            display.printf("ID:%u %s(%lus)", 
                          (unsigned int)(nodeInfo.nodeId % 10000),
                          nodeInfo.isHighPriority ? "!" : " ",
                          timeSince);
            nodeCount++;
        }
    }
    
    if (nodeCount == 0) {
        display.setCursor(0, 16);
        display.print("No active nodes");
    }
    
    display.drawLine(0, 57, SCREEN_WIDTH, 57, SSD1306_WHITE);
    display.setCursor(0, 61);
    display.print("SELECT: Back");
}

void drawPerformanceInfo() {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setFont(&TomThumb);
    
    display.setCursor(33, 6);
    display.print(">> MESH GATEWAY <<");
    display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);
    
    display.setCursor(0, 16);
    display.printf("Batches   : %lu", stats.totalBatchesProcessed);
    display.setCursor(0, 25);
    display.printf("Priority  : %lu", stats.totalPriorityMessages);
    display.setCursor(0, 34);
    display.printf("Queue     : %lu", stats.queuedMessages);
    display.setCursor(0, 43);
    display.printf("Avg Proc  : %lums", stats.averageProcessingTime);
    display.setCursor(0, 52);
    display.printf("Heap      : %d", ESP.getFreeHeap());
    
    display.drawLine(0, 57, SCREEN_WIDTH, 57, SSD1306_WHITE);
    display.setCursor(0, 61);
    display.print("SELECT: Back");
}

void drawConnectionQuality() {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setFont(&TomThumb);
    
    display.setCursor(33, 6);
    display.print(">> MESH GATEWAY <<");
    display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);
    
    display.setCursor(0, 16);
    display.printf("Connects  : %lu", stats.connectionMetrics.totalConnections);
    display.setCursor(0, 25);
    display.printf("Disconn   : %lu", stats.connectionMetrics.totalDisconnections);
    display.setCursor(0, 34);
    display.printf("Reconn    : %lu", stats.connectionMetrics.totalReconnections);
    
    unsigned long totalEvents = stats.connectionMetrics.totalConnections + 
                               stats.connectionMetrics.totalDisconnections;
    display.setCursor(0, 43);
    if (totalEvents > 0) {
        float stability = (float)stats.connectionMetrics.totalConnections / totalEvents * 100;
        display.printf("Stability : %.1f%%", stability);
    } else {
        display.printf("Stability : N/A");
    }
    
    display.drawLine(0, 57, SCREEN_WIDTH, 57, SSD1306_WHITE);
    display.setCursor(0, 61);
    display.print("SELECT: Back");
}

void navigateMenu(bool up, bool select) {
    menu.needsUpdate = true;
    
    if (select) {
        if (menu.currentMenu == MENU_MAIN) {
            switch (menu.selectedItem) {
                case 0: menu.currentMenu = MENU_MESH_INFO; break;
                case 1: menu.currentMenu = MENU_NODES_INFO; break;
                case 2: menu.currentMenu = MENU_PERFORMANCE; break;
                case 3: menu.currentMenu = MENU_CONNECTION_QUALITY; break;
            }
        } else {
            menu.currentMenu = MENU_MAIN;
        }
    } else {
        if (menu.currentMenu == MENU_MAIN) {
            if (up) {
                menu.selectedItem = (menu.selectedItem - 1 + 4) % 4;
            } else {
                menu.selectedItem = (menu.selectedItem + 1) % 4;
            }
        }
    }
}