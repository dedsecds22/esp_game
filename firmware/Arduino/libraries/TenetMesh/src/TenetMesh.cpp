#define LGFX_USE_V1

#include "TenetMesh.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

#include <ArduinoJson.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <LovyanGFX.hpp>

#include <stddef.h>
#include <string.h>

namespace {

constexpr uint8_t kMagic = 0xAA;
constexpr uint8_t kWifiChannel = 1;
constexpr uint8_t kBroadcastId = 0xFF;
constexpr uint8_t kMaxTtl = 4;
constexpr uint8_t kMsgHeartbeat = 0x01;
constexpr uint8_t kMsgData = 0x02;
constexpr uint8_t kMsgInvertTime = 0x03;
constexpr uint8_t kMsgParadox = 0x04;
constexpr uint8_t kMsgRepair = 0x05;
constexpr uint8_t kMsgBombState = 0x06;
constexpr uint32_t kHeartbeatMs = 2000;
constexpr uint32_t kIrCooldownMs = 8000;
constexpr uint32_t kMeshTestTimeoutMs = 5000;
constexpr uint32_t kMeshTestRetryMs = 500;
constexpr uint32_t kIrTestTimeoutMs = 15000;
constexpr uint32_t kTftVisualTestMs = 5000;
constexpr uint32_t kIrNecCode = 0x00FF18E7;
constexpr uint8_t kIrTxPin = 6;
constexpr uint8_t kIrRxPin = 7;
constexpr uint8_t kRemoteButtonPin = 4;
constexpr uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#pragma pack(push, 1)
struct TenetPacket {
    uint8_t magic;
    uint16_t packetId;
    int8_t timeDir;
    uint8_t srcNode;
    uint8_t dstNode;
    uint8_t ttl;
    uint8_t msgType;
    char payload[16];
    uint8_t checksum;
};
#pragma pack(pop)

static_assert(sizeof(TenetPacket) == 25, "TENET wire packet must be 25 bytes");

uint8_t packetChecksum(const TenetPacket &packet) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&packet);
    uint8_t checksum = 0;
    for (size_t index = 0; index < offsetof(TenetPacket, checksum); ++index) {
        checksum ^= bytes[index];
    }
    return checksum;
}

bool packetValid(const TenetPacket &packet) {
    return packet.magic == kMagic && packet.checksum == packetChecksum(packet);
}

void finalizePacket(TenetPacket &packet) {
    packet.magic = kMagic;
    packet.checksum = packetChecksum(packet);
}

TenetPacket makePacket(uint8_t source, uint8_t destination, uint8_t type,
                       int8_t direction, uint16_t id, const char *payload) {
    TenetPacket packet{};
    packet.magic = kMagic;
    packet.packetId = id;
    packet.timeDir = direction;
    packet.srcNode = source;
    packet.dstNode = destination;
    packet.ttl = kMaxTtl;
    packet.msgType = type;
    if (payload != nullptr) {
        strncpy(packet.payload, payload, sizeof(packet.payload) - 1);
    }
    finalizePacket(packet);
    return packet;
}

class LGFX_ESP32S3_ILI9341 : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 panel_;
    lgfx::Bus_SPI bus_;

public:
    LGFX_ESP32S3_ILI9341() {
        auto busConfig = bus_.config();
        busConfig.spi_host = SPI2_HOST;
        busConfig.spi_mode = 0;
        busConfig.freq_write = 40000000;
        busConfig.freq_read = 16000000;
        busConfig.pin_sclk = 12;
        busConfig.pin_mosi = 11;
        busConfig.pin_miso = 9;
        busConfig.pin_dc = 13;
        bus_.config(busConfig);
        panel_.setBus(&bus_);

        auto panelConfig = panel_.config();
        panelConfig.pin_cs = 10;
        panelConfig.pin_rst = 14;
        panelConfig.pin_busy = -1;
        panelConfig.panel_width = 240;
        panelConfig.panel_height = 320;
        panelConfig.offset_x = 0;
        panelConfig.offset_y = 0;
        panelConfig.offset_rotation = 0;
        panelConfig.dummy_read_pixel = 8;
        panelConfig.readable = false;
        panelConfig.invert = false;
        panelConfig.rgb_order = false;
        panelConfig.dlen_16bit = false;
        panelConfig.bus_shared = false;
        panel_.config(panelConfig);
        setPanel(&panel_);
    }
};

struct ReceivedFrame {
    TenetPacket packet;
    int8_t rssi;
};

struct SeenPacket {
    uint8_t source;
    uint8_t type;
    uint16_t id;
};

} // namespace

struct TenetFirmware::Impl {
    TenetRole role;
    uint8_t nodeId;
    QueueHandle_t receiveQueue = nullptr;
    uint16_t nextPacketId = 1;
    int8_t timeDirection = 1;
    uint32_t lastHeartbeatAt = 0;
    uint32_t lastIrTriggerAt = 0;
    uint32_t lastDisplayAt = 0;
    uint32_t tftVisualTestUntil = 0;
    uint32_t bombRemainingSeconds = 60;
    uint32_t bombStartedAt = 0;
    uint32_t lastButtonChangeAt = 0;
    uint32_t meshTestStartedAt = 0;
    uint32_t meshTestLastPingAt = 0;
    uint32_t irTestStartedAt = 0;
    bool bombRunning = false;
    bool irHasTriggered = false;
    bool displayReady = false;
    bool meshTestActive = false;
    bool irTestActive = false;
    bool previousButtonState = HIGH;
    uint8_t meshTestReceivedMask = 0;
    uint8_t meshTestDisplayMask = 0;
    int8_t meshTestRssi[6] = {-127, -127, -127, -127, -127, -127};
    size_t serialLineLength = 0;
    char serialLine[384]{};
    SeenPacket seenPackets[32]{};
    uint8_t seenPacketCursor = 0;
    LGFX_ESP32S3_ILI9341 display;
    LGFX_Sprite canvas;
    IRrecv irReceiver;
    IRsend irTransmitter;
    decode_results irResults{};

    static Impl *active;

    Impl(TenetRole selectedRole, uint8_t selectedNodeId)
        : role(selectedRole), nodeId(selectedNodeId), canvas(&display),
          irReceiver(kIrRxPin), irTransmitter(kIrTxPin) {}

    bool wasSeen(const TenetPacket &packet) {
        for (const SeenPacket &seen : seenPackets) {
            if (seen.source == packet.srcNode && seen.type == packet.msgType &&
                seen.id == packet.packetId) {
                return true;
            }
        }
        seenPackets[seenPacketCursor] = {packet.srcNode, packet.msgType, packet.packetId};
        seenPacketCursor = (seenPacketCursor + 1) % 32;
        return false;
    }

    static void queueFrame(const uint8_t *data, int length, int8_t rssi) {
        if (active == nullptr || active->receiveQueue == nullptr ||
            length != static_cast<int>(sizeof(TenetPacket))) {
            return;
        }
        ReceivedFrame frame{};
        memcpy(&frame.packet, data, sizeof(frame.packet));
        frame.rssi = rssi;
        xQueueSend(active->receiveQueue, &frame, 0);
    }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    static void onDataReceived(const esp_now_recv_info_t *info,
                               const uint8_t *data, int length) {
        const int8_t rssi = info != nullptr && info->rx_ctrl != nullptr
            ? info->rx_ctrl->rssi : -127;
        queueFrame(data, length, rssi);
    }
#else
    static void onDataReceived(const uint8_t *, const uint8_t *data, int length) {
        queueFrame(data, length, -127);
    }
#endif

    bool addBroadcastPeer() {
        if (esp_now_is_peer_exist(kBroadcastMac)) {
            return true;
        }
        esp_now_peer_info_t peer{};
        memcpy(peer.peer_addr, kBroadcastMac, sizeof(kBroadcastMac));
        peer.channel = kWifiChannel;
        peer.encrypt = false;
        return esp_now_add_peer(&peer) == ESP_OK;
    }

    bool sendPacket(TenetPacket packet) {
        finalizePacket(packet);
        wasSeen(packet);
        return esp_now_send(kBroadcastMac,
                            reinterpret_cast<const uint8_t *>(&packet),
                            sizeof(packet)) == ESP_OK;
    }

    void sendLocalPacket(uint8_t type, uint8_t destination, const char *payload) {
        sendPacket(makePacket(nodeId, destination, type, timeDirection,
                              nextPacketId++, payload));
    }

    void startBomb(uint32_t seconds) {
        if (seconds == 0 || seconds > 3600) {
            seconds = 60;
        }
        char payload[16];
        snprintf(payload, sizeof(payload), "START:%lu", static_cast<unsigned long>(seconds));
        sendLocalPacket(kMsgBombState, kBroadcastId, payload);
    }

    void startMeshTest() {
        meshTestActive = true;
        meshTestReceivedMask = 0;
        meshTestDisplayMask = 0;
        memset(meshTestRssi, -127, sizeof(meshTestRssi));
        meshTestStartedAt = millis();
        meshTestLastPingAt = 0;
        Serial.println("{\"event\":\"mesh_test\",\"status\":\"started\",\"expected\":4}");
    }

    void startIrTest() {
        irTestActive = true;
        irTestStartedAt = millis();
        Serial.println("{\"event\":\"ir_test\",\"status\":\"waiting\",\"timeout_ms\":15000}");
    }

    const char *nodeRoleName(uint8_t id) const {
        switch (id) {
            case 0: return "TFT_NORMAL";
            case 1: return "TFT_INVERSE";
            case 3: return "RELAY";
            case 4: return "IR_GATE";
            default: return "UNKNOWN";
        }
    }

    void finishMeshTest() {
        static constexpr uint8_t expectedNodes[] = {0, 1, 3, 4};
        uint8_t onlineCount = 0;
        for (uint8_t id : expectedNodes) {
            const bool online = (meshTestReceivedMask & (1U << id)) != 0;
            onlineCount += online ? 1 : 0;
            StaticJsonDocument<192> document;
            document["event"] = "mesh_test_node";
            document["node"] = id;
            document["role"] = nodeRoleName(id);
            document["status"] = online ? "PASS" : "FAIL";
            document["rssi"] = online ? meshTestRssi[id] : -127;
            if (id == 0 || id == 1) {
                document["display"] = (meshTestDisplayMask & (1U << id)) != 0
                    ? "INIT_PASS" : "INIT_FAIL";
            }
            serializeJson(document, Serial);
            Serial.println();
        }
        StaticJsonDocument<128> summary;
        summary["event"] = "mesh_test_summary";
        summary["status"] = onlineCount == sizeof(expectedNodes) ? "PASS" : "FAIL";
        summary["online"] = onlineCount;
        summary["expected"] = sizeof(expectedNodes);
        summary["tft_init"] = (meshTestDisplayMask & 0x03) == 0x03 ? "PASS" : "FAIL";
        serializeJson(summary, Serial);
        Serial.println();
        meshTestActive = false;
    }

    void finishIrTest(bool passed) {
        StaticJsonDocument<128> document;
        document["event"] = "ir_test";
        document["status"] = passed ? "PASS" : "FAIL";
        document["path"] = "IR_REMOTE->IR_GATE->ESP_NOW->GATEWAY";
        serializeJson(document, Serial);
        Serial.println();
        irTestActive = false;
    }

    void emitPacketJson(const ReceivedFrame &frame) {
        StaticJsonDocument<256> document;
        char payload[sizeof(frame.packet.payload) + 1]{};
        memcpy(payload, frame.packet.payload, sizeof(frame.packet.payload));
        document["src"] = frame.packet.srcNode;
        document["dst"] = frame.packet.dstNode;
        document["type"] = frame.packet.msgType;
        document["id"] = frame.packet.packetId;
        document["dir"] = frame.packet.timeDir;
        document["payload"] = payload;
        document["rssi"] = frame.rssi;
        serializeJson(document, Serial);
        Serial.println();
    }

    void processSerialCommand(const char *line) {
        StaticJsonDocument<256> document;
        if (deserializeJson(document, line)) {
            Serial.println("{\"error\":\"invalid_json\"}");
            return;
        }
        const char *command = document["cmd"] | "";
        if (strcmp(command, "start") == 0) {
            startBomb(document["seconds"] | 60);
        } else if (strcmp(command, "repair") == 0) {
            const uint8_t target = document["target"] | kBroadcastId;
            const char *key = document["key"] | "";
            sendLocalPacket(kMsgRepair, target, key);
        } else if (strcmp(command, "invert") == 0) {
            sendLocalPacket(kMsgInvertTime, kBroadcastId, "INVERT");
        } else if (strcmp(command, "test") == 0) {
            startMeshTest();
        } else if (strcmp(command, "test_ir") == 0) {
            startIrTest();
        } else if (strcmp(command, "ping") == 0) {
            Serial.println("{\"event\":\"gateway_ready\",\"node\":5}");
        } else {
            Serial.println("{\"error\":\"unknown_command\"}");
        }
    }

    void processIncomingFrame(const ReceivedFrame &frame) {
        const TenetPacket &packet = frame.packet;
        if (!packetValid(packet)) {
            return;
        }
        if (role == TenetRole::Gateway) {
            emitPacketJson(frame);
            if (packet.msgType == kMsgHeartbeat &&
                strncmp(packet.payload, "PONG:", 5) == 0 &&
                packet.srcNode < 6 && meshTestActive) {
                meshTestReceivedMask |= 1U << packet.srcNode;
                meshTestRssi[packet.srcNode] = frame.rssi;
                if ((packet.srcNode == 0 || packet.srcNode == 1) &&
                    strcmp(packet.payload, "PONG:LCD_OK") == 0) {
                    meshTestDisplayMask |= 1U << packet.srcNode;
                }
            }
            if (packet.msgType == kMsgInvertTime && packet.srcNode == 4 &&
                strcmp(packet.payload, "IR_INVERT") == 0 && irTestActive) {
                finishIrTest(true);
            }
        }
        if (packet.dstNode == nodeId || packet.dstNode == kBroadcastId) {
            if (packet.msgType == kMsgInvertTime) {
                if (role == TenetRole::TftNormal || role == TenetRole::TftInverse) {
                    timeDirection = -timeDirection;
                }
            } else if (packet.msgType == kMsgBombState &&
                       strncmp(packet.payload, "START:", 6) == 0 &&
                       (role == TenetRole::TftNormal || role == TenetRole::TftInverse)) {
                bombRemainingSeconds = strtoul(packet.payload + 6, nullptr, 10);
                bombStartedAt = millis();
                bombRunning = true;
            } else if (packet.msgType == kMsgHeartbeat &&
                       strcmp(packet.payload, "TEST_PING") == 0) {
                const char *response = "PONG:OK";
                if (role == TenetRole::TftNormal || role == TenetRole::TftInverse) {
                    response = displayReady ? "PONG:LCD_OK" : "PONG:LCD_FAIL";
                }
                sendLocalPacket(kMsgHeartbeat, 5, response);
            }
        }
    }

    void sendHeartbeatIfDue() {
        if (role == TenetRole::Gateway || role == TenetRole::IrRemote) {
            return;
        }
        const uint32_t now = millis();
        if (now - lastHeartbeatAt < kHeartbeatMs) {
            return;
        }
        lastHeartbeatAt = now;
        const char *roleName = "RELAY";
        if (role == TenetRole::TftNormal) {
            roleName = "TFT_NORMAL";
        } else if (role == TenetRole::TftInverse) {
            roleName = "TFT_INVERSE";
        } else if (role == TenetRole::IrGate) {
            roleName = "IR_GATE_RX";
        }
        sendLocalPacket(kMsgHeartbeat, 5, roleName);
    }

    void drawDisplay() {
        const uint32_t now = millis();
        if (now - lastDisplayAt < 250) {
            return;
        }
        lastDisplayAt = now;
        if (now < tftVisualTestUntil) {
            canvas.fillScreen(TFT_BLACK);
            canvas.setTextColor(TFT_WHITE);
            canvas.setFont(&fonts::Font4);
            canvas.drawCenterString("ILI9341 DISPLAY TEST", 160, 22);
            canvas.setTextColor(TFT_CYAN);
            canvas.setFont(&fonts::Font2);
            canvas.drawCenterString(role == TenetRole::TftNormal ? "NODE 0 / BLUE" : "NODE 1 / RED",
                                    160, 54);
            const uint16_t colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_YELLOW, TFT_CYAN};
            for (int index = 0; index < 6; ++index) {
                const int x = 20 + (index % 3) * 96;
                const int y = 88 + (index / 3) * 58;
                canvas.fillRect(x, y, 84, 44, colors[index]);
                canvas.drawRect(x, y, 84, 44, TFT_DARKGREY);
            }
            canvas.setTextColor(TFT_WHITE);
            canvas.drawCenterString("COLOR  ORIENTATION  SPRITE", 160, 218);
            canvas.pushSprite(0, 0);
            return;
        }

        uint32_t elapsed = bombRunning ? (now - bombStartedAt) / 1000 : 0;
        if (elapsed > bombRemainingSeconds) {
            elapsed = bombRemainingSeconds;
        }
        const uint32_t value = role == TenetRole::TftNormal
            ? bombRemainingSeconds - elapsed : elapsed;

        canvas.fillScreen(TFT_BLACK);
        canvas.setTextColor(role == TenetRole::TftNormal ? TFT_CYAN : TFT_RED);
        canvas.setFont(&fonts::Font4);
        canvas.drawCenterString(role == TenetRole::TftNormal ? "TENET / BLUE" : "TENET / RED",
                                160, 28);
        canvas.setTextColor(TFT_WHITE);
        canvas.setFont(&fonts::Font8);
        canvas.drawCenterString(String(value), 160, 112);
        canvas.setTextColor(timeDirection < 0 ? TFT_RED : TFT_GREEN);
        canvas.setFont(&fonts::Font4);
        canvas.drawCenterString(timeDirection < 0 ? "INVERSE" : "NORMAL", 160, 182);
        canvas.setTextColor(TFT_YELLOW);
        canvas.setFont(&fonts::Font2);
        canvas.drawCenterString(bombRunning ? "BOMB ACTIVE" : "WAITING FOR START",
                                160, 218);
        canvas.pushSprite(0, 0);
    }

    void setupRadio() {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        esp_wifi_set_channel(kWifiChannel, WIFI_SECOND_CHAN_NONE);
        if (esp_now_init() != ESP_OK) {
            if (role == TenetRole::Gateway) {
                Serial.println("{\"error\":\"esp_now_init_failed\"}");
            }
            return;
        }
        active = this;
        receiveQueue = xQueueCreate(role == TenetRole::Gateway ? 32 : 16,
                                    sizeof(ReceivedFrame));
        addBroadcastPeer();
        esp_now_register_recv_cb(onDataReceived);
    }

    void begin() {
        if (role == TenetRole::IrRemote) {
            pinMode(kRemoteButtonPin, INPUT_PULLUP);
            irTransmitter.begin();
            return;
        }
        if (role == TenetRole::Gateway) {
            Serial.begin(2000000);
        }
        setupRadio();
        if (role == TenetRole::IrGate) {
            irReceiver.enableIRIn();
        } else if (role == TenetRole::TftNormal || role == TenetRole::TftInverse) {
            displayReady = display.init();
            display.setRotation(1);
            displayReady = canvas.createSprite(320, 240) != nullptr && displayReady;
            if (displayReady) {
                canvas.setSwapBytes(true);
                tftVisualTestUntil = millis() + kTftVisualTestMs;
            }
        }
    }

    void updateRemote() {
        const bool buttonState = digitalRead(kRemoteButtonPin);
        if (buttonState != previousButtonState) {
            lastButtonChangeAt = millis();
            previousButtonState = buttonState;
        }
        if (buttonState == LOW && millis() - lastButtonChangeAt >= 30) {
            irTransmitter.sendNEC(kIrNecCode, 32);
            while (digitalRead(kRemoteButtonPin) == LOW) {
                delay(10);
            }
            previousButtonState = HIGH;
        }
    }

    void updateGatewaySerial() {
        while (Serial.available() > 0) {
            const char character = static_cast<char>(Serial.read());
            if (character == '\n') {
                serialLine[serialLineLength] = '\0';
                processSerialCommand(serialLine);
                serialLineLength = 0;
            } else if (character != '\r' && serialLineLength < sizeof(serialLine) - 1) {
                serialLine[serialLineLength++] = character;
            } else if (serialLineLength >= sizeof(serialLine) - 1) {
                serialLineLength = 0;
            }
        }
    }

    void updateIrGate() {
        if (!irReceiver.decode(&irResults)) {
            return;
        }
        const uint32_t now = millis();
        if (irResults.value == kIrNecCode &&
            (!irHasTriggered || now - lastIrTriggerAt >= kIrCooldownMs)) {
            lastIrTriggerAt = now;
            irHasTriggered = true;
            sendLocalPacket(kMsgInvertTime, kBroadcastId, "IR_INVERT");
        }
        irReceiver.resume();
    }

    void update() {
        if (role == TenetRole::IrRemote) {
            updateRemote();
            return;
        }
        if (role == TenetRole::Gateway) {
            updateGatewaySerial();
            const uint32_t now = millis();
            if (meshTestActive) {
                if (now - meshTestLastPingAt >= kMeshTestRetryMs) {
                    meshTestLastPingAt = now;
                    sendLocalPacket(kMsgHeartbeat, kBroadcastId, "TEST_PING");
                }
                if (now - meshTestStartedAt >= kMeshTestTimeoutMs) {
                    finishMeshTest();
                }
            }
            if (irTestActive && now - irTestStartedAt >= kIrTestTimeoutMs) {
                finishIrTest(false);
            }
        }
        if (receiveQueue != nullptr) {
            ReceivedFrame frame{};
            while (xQueueReceive(receiveQueue, &frame, 0) == pdTRUE) {
                if (!packetValid(frame.packet) || wasSeen(frame.packet)) {
                    continue;
                }
                processIncomingFrame(frame);
                if (role == TenetRole::Relay && frame.packet.ttl > 0 &&
                    frame.packet.dstNode != nodeId) {
                    --frame.packet.ttl;
                    sendPacket(frame.packet);
                }
            }
        }
        if (role == TenetRole::IrGate) {
            updateIrGate();
        }
        sendHeartbeatIfDue();
        if (role == TenetRole::TftNormal || role == TenetRole::TftInverse) {
            drawDisplay();
        }
        delay(1);
    }
};

TenetFirmware::Impl *TenetFirmware::Impl::active = nullptr;

TenetFirmware::TenetFirmware(TenetRole role, uint8_t nodeId)
    : impl_(new Impl(role, nodeId)) {}

TenetFirmware::~TenetFirmware() {
    delete impl_;
}

void TenetFirmware::begin() {
    impl_->begin();
}

void TenetFirmware::update() {
    impl_->update();
}