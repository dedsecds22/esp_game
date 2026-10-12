#define LGFX_USE_V1

#include "TenetMesh.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/gpio.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

#include <ArduinoJson.h>
#include <IRsend.h>
#include <LovyanGFX.hpp>

#include <stddef.h>
#include <string.h>

namespace {

constexpr uint8_t kMagic = 0xAA;
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kWifiChannel = 1;
constexpr uint8_t kBroadcastId = 0xFF;
constexpr uint8_t kMaxTtl = 4;
constexpr uint8_t kMsgHeartbeat = 0x01;
constexpr uint8_t kMsgData = 0x02;
constexpr uint8_t kMsgCommand = 0x03;
constexpr uint8_t kMsgInvertTime = 0x03;
constexpr uint8_t kMsgParadox = 0x04;
constexpr uint8_t kMsgRepair = 0x05;
constexpr uint8_t kMsgBombState = 0x06;
constexpr uint8_t kMsgHello = 0x07;
constexpr uint8_t kMsgStatus = 0x08;
constexpr uint8_t kMsgCommandResult = 0x09;
constexpr uint8_t kMsgAck = 0x0A;
constexpr uint8_t kMsgError = 0x0B;
constexpr uint8_t kMsgGameEvent = 0x0C;
constexpr uint8_t kMsgConfig = 0x0D;
constexpr uint8_t kMsgDiagnostic = 0x0E;
constexpr uint8_t kMsgPing = 0x0F;
constexpr uint8_t kMsgPong = 0x10;
constexpr char kFirmwareVersion[] = "1.0.0";
constexpr uint32_t kHeartbeatMs = 2000;
constexpr uint32_t kIrCooldownMs = 8000;
constexpr uint32_t kMeshTestTimeoutMs = 5000;
constexpr uint32_t kMeshTestRetryMs = 500;
constexpr uint32_t kIrTestTimeoutMs = 15000;
constexpr uint32_t kTftVisualTestMs = 5000;
constexpr uint32_t kIrNecCode = 0x00FF18E7;
constexpr uint32_t kRepairLockMs = 3000;
constexpr uint32_t kOfflineTimeoutMs = 5000;
constexpr uint32_t kRoundStartGraceMs = 5000;
constexpr uint32_t kMissionCountdownMs = 3000;
constexpr uint32_t kPacketAckTimeoutMs = 1200;
constexpr uint8_t kMaxPacketRetries = 3;
constexpr uint32_t kMissionConfigAckTimeoutMs = 1200;
constexpr uint8_t kMissionConfigMaxRetries = 3;
constexpr uint32_t kHostHeartbeatTimeoutMs = 5000;
constexpr uint8_t kIrTxPin = 6;
constexpr uint8_t kIrRxPin = 7;
constexpr uint8_t kRemoteButtonPin = 4;
constexpr uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint16_t kIrEdgeBufferSize = 128;

struct IrEdgeEvent {
    uint32_t timestampUs;
    uint8_t rawLevel;
};

volatile IrEdgeEvent irEdges[kIrEdgeBufferSize]{};
volatile uint16_t irEdgeHead = 0;
volatile uint16_t irEdgeTail = 0;
volatile uint16_t irEdgeOverflowCount = 0;

void IRAM_ATTR captureIrEdge() {
    const uint16_t head = irEdgeHead;
    const uint16_t next = (head + 1) % kIrEdgeBufferSize;
    if (next == irEdgeTail) {
        ++irEdgeOverflowCount;
        return;
    }
    irEdges[head].timestampUs = micros();
    irEdges[head].rawLevel = gpio_get_level(static_cast<gpio_num_t>(kIrRxPin));
    irEdgeHead = next;
}

#pragma pack(push, 1)
struct TenetPacket {
    uint8_t magic;
    uint8_t protocolVersion;
    uint8_t msgType;
    uint8_t srcNode;
    uint8_t dstNode;
    uint8_t ttl;
    int8_t timeDir;
    uint16_t messageId;
    uint16_t missionId;
    uint16_t logicalPacketId;
    uint8_t payloadLength;
    char payload[80];
    uint8_t checksum;
};
#pragma pack(pop)

static_assert(sizeof(TenetPacket) == 95, "TENET v1 wire packet must be 95 bytes");

uint8_t packetChecksum(const TenetPacket &packet) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&packet);
    uint8_t checksum = 0;
    for (size_t index = 0; index < offsetof(TenetPacket, checksum); ++index) {
        checksum ^= bytes[index];
    }
    return checksum;
}

bool packetValid(const TenetPacket &packet) {
    return packet.magic == kMagic && packet.protocolVersion == kProtocolVersion &&
           packet.payloadLength <= sizeof(packet.payload) &&
           packet.checksum == packetChecksum(packet);
}

void finalizePacket(TenetPacket &packet) {
    packet.magic = kMagic;
    packet.protocolVersion = kProtocolVersion;
    packet.payloadLength = strnlen(packet.payload, sizeof(packet.payload));
    packet.checksum = packetChecksum(packet);
}

TenetPacket makePacket(uint8_t source, uint8_t destination, uint8_t type,
                       int8_t direction, uint16_t id, const char *payload) {
    TenetPacket packet{};
    packet.magic = kMagic;
    packet.protocolVersion = kProtocolVersion;
    packet.messageId = id;
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
    uint16_t missionId;
    uint16_t messageId;
};

} // namespace

struct TenetFirmware::Impl {
    TenetRole role;
    uint8_t nodeId;
    QueueHandle_t receiveQueue = nullptr;
    uint16_t nextPacketId = 1;
    uint16_t missionId = 0;
    int8_t timeDirection = 1;
    uint32_t lastHeartbeatAt = 0;
    uint32_t lastIrTriggerAt = 0;
    uint32_t lastDisplayAt = 0;
    uint32_t tftVisualTestUntil = 0;
    uint32_t bombRemainingSeconds = 60;
    uint32_t bombStartedAt = 0;
    uint32_t frozenElapsedSeconds = 0;
    uint32_t lastButtonChangeAt = 0;
    uint32_t meshTestStartedAt = 0;
    uint32_t meshTestLastPingAt = 0;
    uint32_t irTestStartedAt = 0;
    uint32_t nextInfectionAt = 0;
    uint32_t roundDeadline = 0;
    uint32_t roundStartedAt = 0;
    uint32_t lastRoundBroadcastAt = 0;
    uint32_t repairLockUntil = 0;
    uint32_t lastNodeSeenAt[6]{};
    uint32_t missionDeadline = 0;
    uint32_t missionCountdownDeadline = 0;
    uint32_t missionConfigSentAt = 0;
    uint32_t lastHostHeartbeatAt = 0;
    uint32_t missionPausedAt = 0;
    uint32_t missionDurationSeconds = 240;
    uint16_t missionConfigMessageId = 0;
    uint8_t missionConfigAckMask = 0;
    uint8_t missionConfigRetryCount = 0;
    uint32_t nextMissionFaultAt = 0;
    uint32_t nextMissionPacketAt = 0;
    uint32_t lastMissionUiAt = 0;
    uint32_t packetSentAt = 0;
    uint32_t lastHudBroadcastAt = 0;
    bool bombRunning = false;
    bool irHasTriggered = false;
    bool displayReady = false;
    bool meshTestActive = false;
    bool irTestActive = false;
    bool irTestTriggered = false;
    bool previousButtonState = HIGH;
    int lastIrInputState = LOW;
    bool roundActive = false;
    bool packetPending = false;
    bool currentMissionFaults[6]{};
    bool faultRequestPending[6]{};
    bool nodeRepairPending[6]{};
    bool nodeDeliveredLogicalPackets[101]{};
    bool nodeHasReportedHello = false;
    bool repairInProgress = false;
    uint32_t repairCompleteAt = 0;
    uint16_t repairCommandMessageId = 0;
    uint16_t delayedLogicalPacketId = 0;
    uint32_t delayedAckAt = 0;
    bool delayedAckPending = false;
    bool deliveredLogicalPackets[101]{};
    uint8_t logicalPacketAttempts[101]{};
    TenetPacket pendingPacket{};
    uint16_t pendingLogicalId = 0;
    uint8_t packetRetryCount = 0;
    uint8_t packetFaultCount = 0;
    uint8_t missionFaultsRepaired = 0;
    uint8_t missionFaultTotal = 0;
    uint8_t requiredPackets = 8;
    uint8_t deliveredPackets = 0;
    uint8_t requiredNodeCount = 5;
    uint8_t missionFaultIntensity = 2;
    uint32_t missionFaultIntervalMs = 12000;
    uint32_t totalPacketRetries = 0;
    uint8_t lostLogicalPackets = 0;
    uint8_t invalidLogicalPackets = 0;
    uint8_t enabledFaultMask = 0x03;
    const char *missionState = "DISCOVERY";
    bool missionPaused = false;
    uint8_t missionPacketTarget = 0;
    uint8_t requiredPacketCount = 8;
    uint8_t deliveredPacketCount = 0;
    uint8_t pendingRetryCount = 0;
    uint16_t nextLogicalPacketId = 1;
    uint16_t pendingLogicalPacketId = 0;
    uint8_t pendingDestination = 0xFF;
    uint32_t pendingPacketSentAt = 0;
    uint32_t totalRetryCount = 0;
    uint8_t currentMissionFaultMask = 0;
    uint8_t nodeGameState[6]{};
    bool irActiveHigh = true;
    bool irGroupOpen = false;
    bool irPulseOpen = false;
    bool irLastActive = false;
    bool irHasAction = false;
    bool irCalibrationMode = false;
    int irLastRaw = LOW;
    uint32_t irPulseStartedAtUs = 0;
    uint32_t irGroupStartedAtUs = 0;
    uint32_t irLastInactiveAtUs = 0;
    uint32_t irLastActionAt = 0;
    uint32_t irGroupActiveUs = 0;
    uint32_t irLastGapUs = 0;
    uint16_t irGroupGapMs = 80;
    uint16_t irShortMinMs = 100;
    uint16_t irShortMaxMs = 400;
    uint16_t irMediumMinMs = 500;
    uint16_t irMediumMaxMs = 1200;
    uint16_t irLongMinMs = 1300;
    uint16_t irLongMaxMs = 2500;
    uint16_t irMinimumActiveMs = 50;
    uint16_t irMaximumGroupMs = 3000;
    uint16_t irCooldownMs = 800;
    uint16_t sessionKey = 0;
    uint8_t paradoxMask = 0;
    uint32_t infectionIntervalMs = 12000;
    int8_t roundOutcome = 0;
    bool paradox = false;
    bool hasSessionKey = false;
    char tftMissionState[24] = "READY";
    uint32_t tftMissionTimeSeconds = 0;
    uint8_t tftDeliveredPackets = 0;
    uint8_t tftRequiredPackets = 0;
    uint8_t tftActiveFaults = 0;
    uint8_t tftOnlineNodes = 0;
    char tftLatestFault[32] = "NO ACTIVE FAULT";
    uint8_t meshTestReceivedMask = 0;
    uint8_t meshTestDisplayMask = 0;
    int8_t meshTestRssi[6] = {-127, -127, -127, -127, -127, -127};
    size_t serialLineLength = 0;
    char serialLine[384]{};
    SeenPacket seenPackets[32]{};
    uint8_t seenPacketCursor = 0;
    LGFX_ESP32S3_ILI9341 display;
    LGFX_Sprite canvas;
    IRsend irTransmitter;

    static Impl *active;

    Impl(TenetRole selectedRole, uint8_t selectedNodeId)
                : role(selectedRole), nodeId(selectedNodeId), canvas(&display),
                    irTransmitter(kIrTxPin) {}

    bool wasSeen(const TenetPacket &packet) {
        for (const SeenPacket &seen : seenPackets) {
            if (seen.source == packet.srcNode && seen.type == packet.msgType &&
                seen.missionId == packet.missionId && seen.messageId == packet.messageId) {
                return true;
            }
        }
        seenPackets[seenPacketCursor] = {
            packet.srcNode, packet.msgType, packet.missionId, packet.messageId};
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

    void sendLocalPacket(uint8_t type, uint8_t destination, const char *payload,
                         uint16_t logicalPacketId = 0) {
        TenetPacket packet = makePacket(nodeId, destination, type, timeDirection,
                                        nextPacketId++, payload);
        packet.missionId = missionId;
        packet.logicalPacketId = logicalPacketId;
        sendPacket(packet);
    }

    void emitGameState(const char *state, const char *detail = nullptr, int nodeId = -1) {
        StaticJsonDocument<192> document;
        document["event"] = "game_state";
        document["state"] = state;
        if (detail != nullptr) {
            document["detail"] = detail;
        }
        if (nodeId >= 0) {
            document["node"] = nodeId;
        }
        if (strcmp(state, "STARTED") == 0) {
            document["seconds"] = (roundDeadline - roundStartedAt) / 1000UL;
        }
        document["paradox_mask"] = paradoxMask;
        serializeJson(document, Serial);
        Serial.println();
    }

    void emitMissionState(const char *state, const char *detail = nullptr, int nodeId = -1) {
        StaticJsonDocument<320> document;
        document["event"] = "mission_state";
        document["protocol_version"] = kProtocolVersion;
        document["mission_id"] = missionId;
        document["state"] = missionState;
        document["event_type"] = state;
        document["delivered_packets"] = deliveredPackets;
        document["required_packets"] = requiredPackets;
        document["active_faults"] = packetFaultCount;
        document["total_retries"] = totalPacketRetries;
        document["online_nodes"] = requiredNodesOnline() ? requiredNodeCount : 0;
        if (strcmp(missionState, "RUNNING") == 0) {
            document["remaining_seconds"] = missionDeadline > millis()
                ? (missionDeadline - millis() + 999) / 1000 : 0;
        } else if (strcmp(missionState, "COUNTDOWN") == 0) {
            document["remaining_seconds"] = missionCountdownDeadline > millis()
                ? (missionCountdownDeadline - millis() + 999) / 1000 : 0;
        }
        if (detail != nullptr) {
            document["detail"] = detail;
        }
        if (nodeId >= 0) {
            document["node"] = nodeId;
            const uint8_t state = nodeGameState[nodeId];
            document["node_state"] = state == 0 ? "NORMAL"
                : state == 1 ? "INVERSE"
                : state == 2 ? "PARADOX"
                : state == 3 ? "REPAIRING"
                : state == 4 ? "LOSS"
                : state == 5 ? "DELAY"
                : state == 6 ? "DUPLICATE" : "INVALID_PAYLOAD";
        }
        serializeJson(document, Serial);
        Serial.println();
        char displayEvent[72];
        snprintf(displayEvent, sizeof(displayEvent), "MISSION_STATE:%s:%s",
             missionState, detail == nullptr ? "" : detail);
        sendLocalPacket(kMsgGameEvent, kBroadcastId, displayEvent);
    }

    void emitPacketResult(uint16_t logicalId, const char *status, uint8_t retries,
                          uint32_t latencyMs, uint8_t source, uint8_t destination) {
        StaticJsonDocument<256> document;
        document["event"] = "packet_result";
        document["protocol_version"] = kProtocolVersion;
        document["mission_id"] = missionId;
        document["message_id"] = pendingPacket.messageId;
        document["logical_packet_id"] = logicalId;
        document["src"] = source;
        document["dst"] = destination;
        document["type"] = "PACKET_DATA";
        document["delivery_status"] = status;
        document["retry_count"] = retries;
        document["latency_ms"] = latencyMs;
        serializeJson(document, Serial);
        Serial.println();
    }

    bool requiredNodesOnline() const {
        return onlineRequiredNodeCount() == requiredNodeCount;
    }

    uint8_t onlineRequiredNodeCount() const {
        static constexpr uint8_t required[] = {0, 1, 3, 4};
        const uint32_t now = millis();
        uint8_t online = 1;
        for (uint8_t node : required) {
            if (lastNodeSeenAt[node] != 0 && now - lastNodeSeenAt[node] <= kOfflineTimeoutMs) {
                ++online;
            }
        }
        return online;
    }

    void startMission(JsonDocument &document) {
        const uint16_t requestedId = document["mission_id"] | static_cast<uint16_t>(missionId + 1);
        const char *difficulty = document["difficulty"] | "normal";
        uint32_t duration = document["duration_seconds"] | (document["seconds"] | 240);
        const uint8_t packetCount = document["packet_count"] | 8;
        const uint32_t faultInterval = document["fault_interval_seconds"] | 12;
        const uint8_t intensity = document["fault_intensity"] | 2;
        if (requestedId == 0 || duration < 30 || duration > 3600 || packetCount == 0 ||
            packetCount > 100 || faultInterval < 2 || faultInterval > 300 ||
            intensity < 1 || intensity > 5) {
            emitMissionState("ERROR", "INVALID_CONFIG");
            return;
        }
        if (roundActive || strcmp(missionState, "RUNNING") == 0 ||
            strcmp(missionState, "PAUSED") == 0 || strcmp(missionState, "COUNTDOWN") == 0) {
            emitMissionState("ERROR", "MISSION_ALREADY_ACTIVE");
            return;
        }
        if (!requiredNodesOnline()) {
            emitMissionState("ERROR", "REQUIRED_NODE_OFFLINE");
            return;
        }
        missionId = requestedId;
        requiredPackets = packetCount;
        missionDurationSeconds = duration;
        missionFaultIntervalMs = faultInterval * 1000UL;
        missionFaultIntensity = intensity;
        deliveredPackets = 0;
        packetFaultCount = 0;
        missionFaultsRepaired = 0;
        missionFaultTotal = 0;
        totalPacketRetries = 0;
        lostLogicalPackets = 0;
        invalidLogicalPackets = 0;
        pendingLogicalId = 0;
        packetRetryCount = 0;
        packetPending = false;
        missionPaused = false;
        missionConfigAckMask = 0;
        missionConfigRetryCount = 0;
        memset(currentMissionFaults, 0, sizeof(currentMissionFaults));
        memset(faultRequestPending, 0, sizeof(faultRequestPending));
        memset(nodeRepairPending, 0, sizeof(nodeRepairPending));
        memset(deliveredLogicalPackets, 0, sizeof(deliveredLogicalPackets));
        memset(nodeDeliveredLogicalPackets, 0, sizeof(nodeDeliveredLogicalPackets));
        memset(logicalPacketAttempts, 0, sizeof(logicalPacketAttempts));
        nextLogicalPacketId = 1;
        memset(logicalPacketAttempts, 0, sizeof(logicalPacketAttempts));
        nextLogicalPacketId = 1;
        uint8_t target = 0;
        if (document["packet_target"].is<uint8_t>()) {
            target = document["packet_target"].as<uint8_t>();
        }
        if (target != 0 && target != 1 && target != 3 && target != 4) {
            emitMissionState("ERROR", "INVALID_PACKET_TARGET");
            return;
        }
        missionPacketTarget = target;
        enabledFaultMask = 0;
        JsonArray faults = document["fault_types"].as<JsonArray>();
        for (JsonVariant fault : faults) {
            const char *name = fault | "";
            if (strcmp(name, "INVERSE") == 0) enabledFaultMask |= 0x01;
            else if (strcmp(name, "PARADOX") == 0) enabledFaultMask |= 0x02;
            else if (strcmp(name, "LOSS") == 0) enabledFaultMask |= 0x04;
            else if (strcmp(name, "DELAY") == 0) enabledFaultMask |= 0x08;
            else if (strcmp(name, "DUPLICATE") == 0) enabledFaultMask |= 0x10;
            else if (strcmp(name, "INVALID_PAYLOAD") == 0) enabledFaultMask |= 0x20;
        }
        if (enabledFaultMask == 0) {
            emitMissionState("ERROR", "NO_FAULT_TYPES");
            return;
        }
        if (strcmp(difficulty, "easy") == 0) {
            missionFaultIntensity = min<uint8_t>(missionFaultIntensity, 2);
        } else if (strcmp(difficulty, "hard") == 0) {
            missionFaultIntensity = max<uint8_t>(missionFaultIntensity, 4);
        } else if (strcmp(difficulty, "normal") != 0) {
            emitMissionState("ERROR", "INVALID_DIFFICULTY");
            return;
        }
        randomSeed(esp_random());
        roundStartedAt = millis();
        lastHostHeartbeatAt = roundStartedAt;
        roundActive = true;
        missionState = "CONFIGURING";
        sendMissionConfig();
        emitMissionState("CONFIGURING", difficulty);
    }

    void sendMissionConfig() {
        char payload[48];
        snprintf(payload, sizeof(payload), "MISSION_CONFIG:%lu,%u",
                 static_cast<unsigned long>(missionDurationSeconds), requiredPackets);
        sendLocalPacket(kMsgConfig, kBroadcastId, payload);
        missionConfigMessageId = nextPacketId - 1;
        missionConfigSentAt = millis();
    }

    void beginMissionCountdown() {
        if (missionConfigAckMask != 0x1B || strcmp(missionState, "CONFIGURING") != 0) return;
        missionState = "COUNTDOWN";
        missionCountdownDeadline = millis() + kMissionCountdownMs;
        sendLocalPacket(kMsgGameEvent, kBroadcastId, "MISSION_COUNTDOWN");
        emitMissionState("COUNTDOWN", "ALL_NODES_CONFIGURED");
    }

    void generateMissionFault() {
        static constexpr uint8_t targets[] = {0, 1, 3, 4};
        uint8_t eligible[sizeof(targets)]{};
        uint8_t eligibleCount = 0;
        for (uint8_t node : targets) {
            if (!currentMissionFaults[node] && !faultRequestPending[node] && lastNodeSeenAt[node] != 0 &&
                millis() - lastNodeSeenAt[node] <= kOfflineTimeoutMs) {
                eligible[eligibleCount++] = node;
            }
        }
        if (eligibleCount == 0) {
            emitMissionState("DIAGNOSTIC", "NO_ONLINE_FAULT_TARGET");
            return;
        }
        const uint8_t target = eligible[random(eligibleCount)];
        uint8_t availableTypes[6]{};
        uint8_t typeCount = 0;
        for (uint8_t bit = 0; bit < 6; ++bit) {
            if ((enabledFaultMask & (1U << bit)) != 0) {
                availableTypes[typeCount++] = bit;
            }
        }
        const uint8_t typeBit = availableTypes[random(typeCount)];
        const char *fault = "PARADOX";
        if (typeBit == 0) fault = "INVERSE";
        else if (typeBit == 2) fault = "LOSS";
        else if (typeBit == 3) fault = "DELAY";
        else if (typeBit == 4) fault = "DUPLICATE";
        else if (typeBit == 5) fault = "INVALID_PAYLOAD";
        faultRequestPending[target] = true;
        ++missionFaultTotal;
        char payload[32];
        snprintf(payload, sizeof(payload), "FAULT:%s", fault);
        sendLocalPacket(kMsgGameEvent, target, payload);
        snprintf(payload, sizeof(payload), "TASK:FAULT:%u:%s", target, fault);
        sendLocalPacket(kMsgGameEvent, 1, payload);
        emitMissionState("NODE_FAULT", fault, target);
    }

    void finishMission(bool success, const char *reason) {
        roundActive = false;
        missionPaused = false;
        packetPending = false;
        missionState = success ? "MISSION_SUCCESS" : "MISSION_FAILED";
        sendLocalPacket(kMsgGameEvent, kBroadcastId, success ? "MISSION_SUCCESS" : "MISSION_FAILED");
        const uint32_t duration = missionDurationSeconds * 1000UL;
        const uint32_t remaining = missionDeadline > millis() ? missionDeadline - millis() : 0;
        const uint8_t onlineNodes = onlineRequiredNodeCount();
        const uint8_t deliveryScore = requiredPackets == 0 ? 0
            : (40U * min<uint8_t>(deliveredPackets, requiredPackets)) / requiredPackets;
        const uint8_t stabilityScore = (30U * onlineNodes) / requiredNodeCount;
        const uint8_t recoveryScore = missionFaultTotal == 0 ? 20
            : (20U * min<uint8_t>(missionFaultsRepaired, missionFaultTotal)) / missionFaultTotal;
        const uint8_t timeScore = duration == 0 ? 0
            : (10U * min<uint32_t>(remaining, duration)) / duration;
        StaticJsonDocument<384> result;
        result["event"] = "mission_result";
        result["protocol_version"] = kProtocolVersion;
        result["mission_id"] = missionId;
        result["state"] = success ? "MISSION_SUCCESS" : "MISSION_FAILED";
        result["delivered_packets"] = deliveredPackets;
        result["required_packets"] = requiredPackets;
        result["failed_packets"] = lostLogicalPackets + invalidLogicalPackets;
        result["retries"] = totalPacketRetries;
        result["online_nodes"] = onlineNodes;
        result["faults_repaired"] = missionFaultsRepaired;
        result["faults_total"] = missionFaultTotal;
        JsonArray faultedNodes = result["faulted_nodes"].to<JsonArray>();
        uint8_t activeFaultCount = 0;
        for (uint8_t id = 0; id < 6; ++id) {
            if (currentMissionFaults[id]) {
                faultedNodes.add(id);
                ++activeFaultCount;
            }
        }
        result["active_faults"] = activeFaultCount;
        result["elapsed_seconds"] = (millis() - roundStartedAt) / 1000;
        result["score"] = deliveryScore + stabilityScore + recoveryScore + timeScore;
        result["score_delivery"] = deliveryScore;
        result["score_stability"] = stabilityScore;
        result["score_recovery"] = recoveryScore;
        result["score_time"] = timeScore;
        result["reason"] = reason;
        serializeJson(result, Serial);
        Serial.println();
        emitMissionState(success ? "MISSION_SUCCESS" : "MISSION_FAILED", reason);
    }

    void sendNextDataPacket() {
        if (packetPending || deliveredPackets >= requiredPackets) {
            return;
        }
        uint16_t logicalId = 0;
        for (uint16_t candidate = 1; candidate <= requiredPackets; ++candidate) {
            if (!deliveredLogicalPackets[candidate] &&
                logicalPacketAttempts[candidate] <= kMaxPacketRetries) {
                logicalId = candidate;
                break;
            }
        }
        if (logicalId == 0) {
            return;
            return;
        }
        char payload[32];
        snprintf(payload, sizeof(payload), "PACKET:%04u", logicalId);
        pendingPacket = makePacket(nodeId, missionPacketTarget, kMsgData,
                                   timeDirection, nextPacketId++, payload);
        pendingPacket.missionId = missionId;
        pendingPacket.logicalPacketId = logicalId;
        packetPending = true;
        packetRetryCount = logicalPacketAttempts[logicalId];
        pendingLogicalId = logicalId;
        packetSentAt = millis();
        ++logicalPacketAttempts[logicalId];
        if (!sendPacket(pendingPacket)) {
            emitPacketResult(logicalId, "LOST", 0, 0, nodeId, missionPacketTarget);
        } else {
            emitPacketResult(logicalId, "SENT", 0, 0, nodeId, missionPacketTarget);
        }
    }

    void updatePendingPacket() {
        if (!packetPending || millis() - packetSentAt < kPacketAckTimeoutMs) {
            return;
        }
        const uint32_t elapsed = millis() - packetSentAt;
        if (packetRetryCount >= kMaxPacketRetries) {
            ++lostLogicalPackets;
            emitPacketResult(pendingLogicalId, "LOST", packetRetryCount, elapsed,
                             nodeId, missionPacketTarget);
            packetPending = false;
            return;
        }
        ++packetRetryCount;
        ++totalPacketRetries;
        pendingPacket.messageId = nextPacketId++;
        packetSentAt = millis();
        ++logicalPacketAttempts[pendingLogicalId];
        sendPacket(pendingPacket);
        emitPacketResult(pendingLogicalId, "RETRY", packetRetryCount, elapsed,
                         nodeId, missionPacketTarget);
    }

    void updateMission() {
        const uint32_t now = millis();
        if (strcmp(missionState, "CONFIGURING") == 0) {
            if (missionConfigAckMask == 0x1B) {
                beginMissionCountdown();
            } else if (now - missionConfigSentAt >= kMissionConfigAckTimeoutMs) {
                if (missionConfigRetryCount >= kMissionConfigMaxRetries) {
                    roundActive = false;
                    missionState = "READY";
                    emitMissionState("ERROR", "CONFIG_ACK_TIMEOUT");
                } else {
                    ++missionConfigRetryCount;
                    sendMissionConfig();
                    emitMissionState("CONFIG_RETRY", "CONFIG_ACK_PENDING");
                }
            }
            return;
        }
        if (strcmp(missionState, "COUNTDOWN") == 0 && now >= missionCountdownDeadline) {
            missionState = "RUNNING";
            missionDeadline = millis() + missionDurationSeconds * 1000UL;
            nextMissionPacketAt = millis();
            nextMissionFaultAt = millis() + missionFaultIntervalMs;
            sendLocalPacket(kMsgGameEvent, kBroadcastId, "MISSION_RUNNING");
            emitMissionState("RUNNING", "COUNTDOWN_COMPLETE");
        } else if (strcmp(missionState, "COUNTDOWN") == 0 && now - lastMissionUiAt >= 1000) {
            lastMissionUiAt = now;
            emitMissionState("COUNTDOWN", "TICK");
        }
        if (strcmp(missionState, "RUNNING") != 0) {
            return;
        }
        if (now - lastHostHeartbeatAt > kHostHeartbeatTimeoutMs) {
            missionPaused = true;
            missionPausedAt = now;
            missionState = "PAUSED";
            emitMissionState("PAUSED", "PC_LINK_LOST");
            return;
        }
        if (missionPaused) return;
        static constexpr uint8_t required[] = {0, 1, 3, 4};
        for (uint8_t node : required) {
            if (lastNodeSeenAt[node] == 0 || now - lastNodeSeenAt[node] > kOfflineTimeoutMs) {
                finishMission(false, "REQUIRED_NODE_OFFLINE");
                return;
            }
        }
        if (deliveredPackets >= requiredPackets && packetFaultCount == 0 && requiredNodesOnline()) {
            finishMission(true, "ALL_REQUIREMENTS_MET");
            return;
        }
        updatePendingPacket();
        if (now >= missionDeadline) {
            const bool success = deliveredPackets >= requiredPackets &&
                                 packetFaultCount == 0 && requiredNodesOnline();
            finishMission(success, success ? "ALL_REQUIREMENTS_MET" : "MISSION_TIMEOUT_OR_FAULTS");
            return;
        }
        if (now >= nextMissionPacketAt && !packetPending && deliveredPackets < requiredPackets) {
            sendNextDataPacket();
            nextMissionPacketAt = now + 2500;
        }
        if (now >= nextMissionFaultAt && packetFaultCount < missionFaultIntensity) {
            generateMissionFault();
            nextMissionFaultAt = now + missionFaultIntervalMs;
        }
        if (now - lastHudBroadcastAt >= 1000) {
            lastHudBroadcastAt = now;
            uint8_t online = 0;
            for (uint8_t id : {static_cast<uint8_t>(0), static_cast<uint8_t>(1),
                               static_cast<uint8_t>(3), static_cast<uint8_t>(4),
                               static_cast<uint8_t>(5)}) {
                if (id == 5 || (lastNodeSeenAt[id] != 0 && now - lastNodeSeenAt[id] <= kOfflineTimeoutMs)) {
                    ++online;
                }
            }
            const uint32_t remaining = missionDeadline > now ? (missionDeadline - now + 999) / 1000 : 0;
            char hud[64];
            snprintf(hud, sizeof(hud), "HUD:%lu,%u,%u,%u,%u", static_cast<unsigned long>(remaining),
                     deliveredPackets, requiredPackets, packetFaultCount, online);
            sendLocalPacket(kMsgStatus, kBroadcastId, hud);
            emitMissionState("RUNNING", "TICK");
        }
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
        irTestTriggered = false;
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
        memcpy(payload, frame.packet.payload, frame.packet.payloadLength);
        document["event"] = "packet";
        document["protocol_version"] = frame.packet.protocolVersion;
        document["src"] = frame.packet.srcNode;
        document["dst"] = frame.packet.dstNode;
        document["type"] = frame.packet.msgType;
        document["protocol_version"] = frame.packet.protocolVersion;
        document["id"] = frame.packet.messageId;
        document["mission_id"] = frame.packet.missionId;
        document["logical_packet_id"] = frame.packet.logicalPacketId;
        document["delivery_status"] = "UNKNOWN";
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
        if (strcmp(command, "host_heartbeat") == 0) {
            lastHostHeartbeatAt = millis();
        } else if (strcmp(command, "mission_start") == 0) {
            startMission(document);
        } else if (strcmp(command, "mission_pause") == 0) {
            if (strcmp(missionState, "RUNNING") == 0) {
                missionPaused = true;
                missionPausedAt = millis();
                missionState = "PAUSED";
                emitMissionState("PAUSED");
            }
        } else if (strcmp(command, "mission_resume") == 0) {
            if (strcmp(missionState, "PAUSED") == 0) {
                const uint32_t pauseDuration = millis() - missionPausedAt;
                missionDeadline += pauseDuration;
                nextMissionFaultAt += pauseDuration;
                nextMissionPacketAt += pauseDuration;
                missionPaused = false;
                missionState = "RUNNING";
                emitMissionState("RUNNING", "RESUMED");
            }
        } else if (strcmp(command, "mission_abort") == 0) {
            if (roundActive) {
                finishMission(false, "ABORTED_BY_HOST");
            }
        } else if (strcmp(command, "scan") == 0) {
            sendLocalPacket(kMsgHello, kBroadcastId, "HELLO_REQUEST");
            emitMissionState("DISCOVERY", "SCAN_REQUESTED");
        } else if (strcmp(command, "ir_config") == 0) {
            char payload[80];
            const bool activeHigh = document["active_high"] | true;
            const uint16_t groupGap = document["group_gap_ms"] | 80;
            const uint16_t shortMin = document["short_min_ms"] | 100;
            const uint16_t shortMax = document["short_max_ms"] | 400;
            const uint16_t mediumMin = document["medium_min_ms"] | 500;
            const uint16_t mediumMax = document["medium_max_ms"] | 1200;
            const uint16_t longMin = document["long_min_ms"] | 1300;
            const uint16_t longMax = document["long_max_ms"] | 2500;
            const uint16_t minActive = document["minimum_active_ms"] | 50;
            const uint16_t maxGroup = document["maximum_group_ms"] | 3000;
            const uint16_t cooldown = document["cooldown_ms"] | 800;
            snprintf(payload, sizeof(payload), "IRCFG:%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                     activeHigh ? 1 : 0, groupGap, shortMin, shortMax, mediumMin,
                     mediumMax, longMin, longMax, minActive, maxGroup, cooldown);
            sendLocalPacket(kMsgConfig, 4, payload);
        } else if (strcmp(command, "start") == 0) {
            startMission(document);
        } else if (strcmp(command, "repair") == 0) {
            const uint8_t target = document["target"] | 0xFF;
            if (strcmp(missionState, "RUNNING") != 0 || target >= 6 ||
                !currentMissionFaults[target]) {
                StaticJsonDocument<160> result;
                result["event"] = "packet_result";
                result["delivery_status"] = "INVALID";
                result["detail"] = "NODE_NOT_REPAIRABLE";
                result["node"] = target;
                serializeJson(result, Serial);
                Serial.println();
            } else {
                sendLocalPacket(kMsgRepair, target, "REPAIR");
            }
        } else if (strcmp(command, "invert") == 0) {
            sendLocalPacket(kMsgInvertTime, kBroadcastId, "INVERT");
        } else if (strcmp(command, "test") == 0) {
            startMeshTest();
        } else if (strcmp(command, "test_ir") == 0) {
            startIrTest();
        } else if (strcmp(command, "ir_state") == 0) {
            sendLocalPacket(kMsgHeartbeat, kBroadcastId, "IR_STATE?");
        } else if (strcmp(command, "ir_calibrate") == 0) {
            sendLocalPacket(kMsgDiagnostic, 4, "IR_CALIBRATE");
            StaticJsonDocument<128> event;
            event["event"] = "ir_calibration";
            event["status"] = "CAPTURE_STARTED";
            serializeJson(event, Serial);
            Serial.println();
        } else if (strcmp(command, "ir_calibration_stop") == 0) {
            sendLocalPacket(kMsgDiagnostic, 4, "IR_CAL_STOP");
        } else if (strcmp(command, "ping") == 0) {
            Serial.println("{\"event\":\"gateway_ready\",\"node\":5,\"firmware\":\"1.0.0\",\"protocol_version\":1}");
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
            if (packet.srcNode < 6 && packet.srcNode != 5) {
                lastNodeSeenAt[packet.srcNode] = millis();
            }
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
            if (packet.msgType == kMsgHello) {
                StaticJsonDocument<192> document;
                document["event"] = "hello";
                document["protocol_version"] = packet.protocolVersion;
                document["node"] = packet.srcNode;
                document["firmware"] = packet.payload;
                document["rssi"] = frame.rssi;
                serializeJson(document, Serial);
                Serial.println();
                if (missionState != "READY" && missionState != "COUNTDOWN" &&
                    missionState != "RUNNING" && requiredNodesOnline()) {
                    missionState = "READY";
                    emitMissionState("READY", "REQUIRED_NODES_ONLINE");
                }
            }
            if (packet.msgType == kMsgStatus && packet.srcNode < 6) {
                StaticJsonDocument<192> document;
                document["event"] = "node_status";
                document["node"] = packet.srcNode;
                document["communication_state"] = "ONLINE";
                document["game_state"] = packet.payload;
                document["last_seen_ms"] = millis();
                document["rssi"] = frame.rssi;
                serializeJson(document, Serial);
                Serial.println();
                const char *reportedState = packet.payload;
                const uint8_t previousState = nodeGameState[packet.srcNode];
                if (strcmp(reportedState, "NORMAL") == 0) {
                    nodeGameState[packet.srcNode] = 0;
                } else if (strcmp(reportedState, "INVERSE") == 0) {
                    nodeGameState[packet.srcNode] = 1;
                } else if (strcmp(reportedState, "PARADOX") == 0) {
                    nodeGameState[packet.srcNode] = 2;
                } else if (strcmp(reportedState, "REPAIRING") == 0) {
                    nodeGameState[packet.srcNode] = 3;
                    if (currentMissionFaults[packet.srcNode]) {
                        nodeRepairPending[packet.srcNode] = true;
                        char task[32];
                        snprintf(task, sizeof(task), "TASK:REPAIRING:%u", packet.srcNode);
                        sendLocalPacket(kMsgGameEvent, 1, task);
                        emitMissionState("REPAIRING", "NODE_ACKNOWLEDGED", packet.srcNode);
                    }
                } else if (strcmp(reportedState, "LOSS") == 0) {
                    nodeGameState[packet.srcNode] = 4;
                } else if (strcmp(reportedState, "DELAY") == 0) {
                    nodeGameState[packet.srcNode] = 5;
                } else if (strcmp(reportedState, "DUPLICATE") == 0) {
                    nodeGameState[packet.srcNode] = 6;
                } else if (strcmp(reportedState, "INVALID_PAYLOAD") == 0) {
                    nodeGameState[packet.srcNode] = 7;
                }
                if (faultRequestPending[packet.srcNode] && nodeGameState[packet.srcNode] != 0) {
                    faultRequestPending[packet.srcNode] = false;
                    currentMissionFaults[packet.srcNode] = true;
                    ++packetFaultCount;
                    emitMissionState("NODE_FAULT", reportedState, packet.srcNode);
                } else if (currentMissionFaults[packet.srcNode] &&
                           nodeRepairPending[packet.srcNode] && nodeGameState[packet.srcNode] == 0) {
                    currentMissionFaults[packet.srcNode] = false;
                    nodeRepairPending[packet.srcNode] = false;
                    if (packetFaultCount > 0) --packetFaultCount;
                    ++missionFaultsRepaired;
                    for (uint16_t logicalId = 1; logicalId <= requiredPackets; ++logicalId) {
                        if (!deliveredLogicalPackets[logicalId]) logicalPacketAttempts[logicalId] = 0;
                    }
                    char task[32];
                    snprintf(task, sizeof(task), "TASK:RESTORED:%u", packet.srcNode);
                    sendLocalPacket(kMsgGameEvent, 1, task);
                    emitMissionState("NODE_RESTORED", "STATUS_CONFIRMED", packet.srcNode);
                } else if (previousState == 2 && nodeGameState[packet.srcNode] == 0) {
                    emitMissionState("STATE_UNCONFIRMED", "REBOOT_OR_UNEXPECTED_RESET", packet.srcNode);
                }
            }
            if (packet.msgType == kMsgAck && packet.srcNode == missionPacketTarget &&
                packet.missionId == missionId && packet.logicalPacketId > 0 &&
                packet.logicalPacketId <= 100 && deliveredLogicalPackets[packet.logicalPacketId]) {
                emitPacketResult(packet.logicalPacketId, "DUPLICATE", 0, 0,
                                 nodeId, missionPacketTarget);
            } else if (packet.msgType == kMsgAck && packet.srcNode == missionPacketTarget &&
                packet.missionId == missionId &&
                packet.logicalPacketId == pendingLogicalId && packetPending) {
                const uint16_t logicalId = packet.logicalPacketId;
                const uint32_t latency = millis() - packetSentAt;
                packetPending = false;
                if (logicalId > 0 && logicalId <= 100 && !deliveredLogicalPackets[logicalId]) {
                    deliveredLogicalPackets[logicalId] = true;
                    ++deliveredPackets;
                    emitPacketResult(logicalId, "ACK", packetRetryCount, latency,
                                     nodeId, missionPacketTarget);
                    emitMissionState("PACKET_ACK", "APPLICATION_ACK");
                } else {
                    emitPacketResult(logicalId, "DUPLICATE", packetRetryCount, latency,
                                     nodeId, missionPacketTarget);
                }
            }
            if (packet.msgType == kMsgError && packet.logicalPacketId == pendingLogicalId && packetPending) {
                packetPending = false;
                ++invalidLogicalPackets;
                emitPacketResult(pendingLogicalId, "INVALID", packetRetryCount,
                                 millis() - packetSentAt, nodeId, packet.srcNode);
                emitMissionState("PACKET_REJECTED", "REPAIR_AND_RETRY", packet.srcNode);
            }
            if (packet.msgType == kMsgCommandResult && packet.srcNode < 6) {
                if (strcmp(packet.payload, "CONFIG_OK") == 0 &&
                    packet.logicalPacketId == missionConfigMessageId && packet.missionId == missionId) {
                    missionConfigAckMask |= 1U << packet.srcNode;
                    emitMissionState("CONFIG_ACK", nodeRoleName(packet.srcNode), packet.srcNode);
                    beginMissionCountdown();
                }
                StaticJsonDocument<160> document;
                document["event"] = "packet_result";
                document["mission_id"] = missionId;
                document["logical_packet_id"] = packet.logicalPacketId;
                document["src"] = nodeId;
                document["dst"] = packet.srcNode;
                document["type"] = "COMMAND_RESULT";
                document["delivery_status"] = strcmp(packet.payload, "REPAIR_OK") == 0 ? "ACK" : "INVALID";
                document["detail"] = packet.payload;
                serializeJson(document, Serial);
                Serial.println();
            }
            if (packet.msgType == kMsgHeartbeat && packet.srcNode == 4 &&
                strncmp(packet.payload, "IR_STATE:", 9) == 0) {
                StaticJsonDocument<128> document;
                document["event"] = "ir_state";
                document["level"] = packet.payload + 9;
                document["raw"] = strcmp(packet.payload + 9, "HIGH") == 0 ? 1 : 0;
                document["rssi"] = frame.rssi;
                serializeJson(document, Serial);
                Serial.println();
            }
            if (packet.msgType == kMsgDiagnostic && packet.srcNode == 4 &&
                strncmp(packet.payload, "IR_SAMPLE:", 10) == 0) {
                sendLocalPacket(kMsgGameEvent, 1, packet.payload);
                int raw = 0;
                char active[12]{};
                unsigned pulse = 0, gap = 0, group = 0;
                char category[16]{}, reason[20]{};
                if (sscanf(packet.payload + 10, "%d,%11[^,],%u,%u,%u,%15[^,],%19s",
                           &raw, active, &pulse, &gap, &group, category, reason) == 7) {
                    StaticJsonDocument<256> document;
                    document["event"] = "ir_sample";
                    document["protocol_version"] = packet.protocolVersion;
                    document["node"] = 4;
                    document["raw_level"] = raw ? "HIGH" : "LOW";
                    document["active"] = strcmp(active, "ACTIVE") == 0;
                    document["pulse_ms"] = pulse;
                    document["pulse_us"] = pulse;
                    document["pulse_ms"] = pulse / 1000.0;
                    document["gap_us"] = gap;
                    document["gap_ms"] = gap / 1000.0;
                    document["group_active_us"] = group;
                    document["group_active_ms"] = group / 1000.0;
                    document["category"] = category;
                    document["reason"] = reason;
                    document["rssi"] = frame.rssi;
                    serializeJson(document, Serial);
                    Serial.println();
                }
            }
            if (packet.msgType == kMsgCommandResult && packet.srcNode == 4 &&
                strncmp(packet.payload, "IR_CONFIG_", 10) == 0) {
                StaticJsonDocument<128> document;
                document["event"] = "ir_config_result";
                document["status"] = packet.payload + 10;
                serializeJson(document, Serial);
                Serial.println();
            }
            if (packet.msgType == kMsgCommandResult && packet.srcNode == 4 &&
                (strcmp(packet.payload, "IR_CAPTURE_READY") == 0 ||
                 strcmp(packet.payload, "IR_CAPTURE_STOPPED") == 0)) {
                StaticJsonDocument<128> document;
                document["event"] = "ir_calibration";
                document["status"] = packet.payload;
                serializeJson(document, Serial);
                Serial.println();
            }
            if (packet.msgType == kMsgDiagnostic && packet.srcNode == 4 &&
                strncmp(packet.payload, "IR_REJECT:", 10) == 0) {
                sendLocalPacket(kMsgGameEvent, 1, packet.payload);
            }
            if (packet.msgType == kMsgCommand && packet.srcNode == 4 &&
                strncmp(packet.payload, "IR_ACTION:", 10) == 0) {
                const char *category = packet.payload + 10;
                if (irTestActive) {
                    finishIrTest(true);
                    return;
                }
                bool accepted = false;
                if (strcmp(category, "SHORT") == 0) {
                    if (strcmp(missionState, "RUNNING") == 0) {
                        sendLocalPacket(kMsgPing, kBroadcastId, "STATE_QUERY");
                        accepted = true;
                    }
                } else if (strcmp(category, "MEDIUM") == 0 && missionState == "RUNNING") {
                    for (uint8_t target = 0; target < 6; ++target) {
                        if (currentMissionFaults[target] && !nodeRepairPending[target]) {
                            sendLocalPacket(kMsgRepair, target, "REPAIR");
                            accepted = true;
                            break;
                        }
                    }
                } else if (strcmp(category, "LONG") == 0 && missionState == "RUNNING") {
                    missionPaused = true;
                    missionPausedAt = millis();
                    missionState = "PAUSED";
                    emitMissionState("PAUSED", "IR_LONG");
                    accepted = true;
                } else if (strcmp(category, "LONG") == 0 && missionState == "PAUSED") {
                    const uint32_t pauseDuration = millis() - missionPausedAt;
                    missionDeadline += pauseDuration;
                    nextMissionFaultAt += pauseDuration;
                    nextMissionPacketAt += pauseDuration;
                    missionPaused = false;
                    missionState = "RUNNING";
                    emitMissionState("RUNNING", "IR_LONG_RESUME");
                    accepted = true;
                }
                StaticJsonDocument<128> document;
                document["event"] = "ir_action_result";
                document["category"] = category;
                document["status"] = accepted ? "ACCEPTED" : "REJECTED";
                document["mission_id"] = missionId;
                serializeJson(document, Serial);
                Serial.println();
            }
            if (packet.msgType == kMsgInvertTime && packet.srcNode == 4 &&
                strcmp(packet.payload, "IR_INVERT") == 0 && irTestActive) {
                finishIrTest(true);
            }
            if (packet.srcNode < 6 && packet.msgType == kMsgParadox) {
                paradoxMask |= 1U << packet.srcNode;
                emitGameState("NODE_PARADOX", packet.payload, packet.srcNode);
            }
            if (packet.srcNode < 6 && packet.msgType == kMsgHeartbeat &&
                strcmp(packet.payload, "REPAIRED") == 0) {
                paradoxMask &= ~(1U << packet.srcNode);
                emitGameState("NODE_REPAIRED", nodeRoleName(packet.srcNode), packet.srcNode);
            }
            if (packet.srcNode < 6 && packet.msgType == kMsgHeartbeat &&
                strcmp(packet.payload, "REPAIR_LOCKED") == 0) {
                emitGameState("REPAIR_REJECTED", nodeRoleName(packet.srcNode), packet.srcNode);
            }
        }
        if (packet.dstNode == nodeId || packet.dstNode == kBroadcastId) {
            if (packet.msgType == kMsgInvertTime) {
                timeDirection = -timeDirection;
                if (nodeGameState[nodeId] == 0) nodeGameState[nodeId] = 1;
                else if (nodeGameState[nodeId] == 1) nodeGameState[nodeId] = 0;
                const char *state = nodeGameState[nodeId] == 0 ? "NORMAL"
                    : nodeGameState[nodeId] == 1 ? "INVERSE"
                    : nodeGameState[nodeId] == 2 ? "PARADOX"
                    : nodeGameState[nodeId] == 3 ? "REPAIRING"
                    : nodeGameState[nodeId] == 4 ? "LOSS"
                    : nodeGameState[nodeId] == 5 ? "DELAY"
                    : nodeGameState[nodeId] == 6 ? "DUPLICATE" : "INVALID_PAYLOAD";
                sendLocalPacket(kMsgStatus, 5, state);
            } else if (packet.msgType == kMsgHello &&
                       strcmp(packet.payload, "HELLO_REQUEST") == 0 && role != TenetRole::IrRemote) {
                char hello[48];
                const char *roleName = role == TenetRole::TftNormal ? "TFT_NORMAL"
                    : role == TenetRole::TftInverse ? "TFT_INVERSE"
                    : role == TenetRole::IrGate ? "IR_GATE"
                    : role == TenetRole::Relay ? "RELAY" : "UNKNOWN";
                snprintf(hello, sizeof(hello), "FW:1.0.0:%s", roleName);
                sendLocalPacket(kMsgHello, 5, hello);
            } else if (packet.msgType == kMsgConfig &&
                       strncmp(packet.payload, "MISSION_CONFIG:", 15) == 0 && role != TenetRole::Gateway) {
                missionId = packet.missionId;
                nodeGameState[nodeId] = 0;
                paradox = false;
                repairInProgress = false;
                memset(nodeDeliveredLogicalPackets, 0, sizeof(nodeDeliveredLogicalPackets));
                unsigned duration = 0;
                unsigned packetCount = 0;
                sscanf(packet.payload + 15, "%u,%u", &duration, &packetCount);
                if (role == TenetRole::TftNormal || role == TenetRole::TftInverse) {
                    tftMissionTimeSeconds = duration;
                    tftRequiredPackets = packetCount;
                    tftDeliveredPackets = 0;
                    tftActiveFaults = 0;
                    tftOnlineNodes = 0;
                    strncpy(tftMissionState, "COUNTDOWN", sizeof(tftMissionState) - 1);
                }
                sendLocalPacket(kMsgCommandResult, 5, "CONFIG_OK", packet.messageId);
                sendLocalPacket(kMsgStatus, 5, "NORMAL");
            } else if (packet.msgType == kMsgConfig && role == TenetRole::IrGate &&
                       strncmp(packet.payload, "IRCFG:", 6) == 0) {
                sendLocalPacket(kMsgCommandResult, 5,
                                applyIrConfig(packet.payload) ? "IR_CONFIG_OK" : "IR_CONFIG_INVALID");
                 } else if (packet.msgType == kMsgDiagnostic && role == TenetRole::IrGate &&
                      strcmp(packet.payload, "IR_CALIBRATE") == 0) {
                  irCalibrationMode = true;
                  sendLocalPacket(kMsgCommandResult, 5, "IR_CAPTURE_READY");
                     } else if (packet.msgType == kMsgDiagnostic && role == TenetRole::IrGate &&
                          strcmp(packet.payload, "IR_CAL_STOP") == 0) {
                      irCalibrationMode = false;
                      sendLocalPacket(kMsgCommandResult, 5, "IR_CAPTURE_STOPPED");
            } else if (packet.msgType == kMsgGameEvent &&
                       strncmp(packet.payload, "MISSION_STATE:", 14) == 0 &&
                       (role == TenetRole::TftNormal || role == TenetRole::TftInverse)) {
                const char *state = packet.payload + 14;
                const char *separator = strchr(state, ':');
                const size_t stateLength = separator == nullptr
                    ? strlen(state) : static_cast<size_t>(separator - state);
                const size_t copyLength = min(stateLength, sizeof(tftMissionState) - 1);
                memcpy(tftMissionState, state, copyLength);
                tftMissionState[copyLength] = '\0';
                if (strcmp(tftMissionState, "MISSION_SUCCESS") == 0 ||
                    strcmp(tftMissionState, "MISSION_FAILED") == 0) {
                    tftLatestFault[0] = '\0';
                }
            } else if (packet.msgType == kMsgGameEvent &&
                       strncmp(packet.payload, "MISSION_", 8) == 0 &&
                       (role == TenetRole::TftNormal || role == TenetRole::TftInverse)) {
                strncpy(tftMissionState, packet.payload, sizeof(tftMissionState) - 1);
                tftMissionState[sizeof(tftMissionState) - 1] = '\0';
            } else if (packet.msgType == kMsgStatus &&
                       strncmp(packet.payload, "HUD:", 4) == 0 &&
                       (role == TenetRole::TftNormal || role == TenetRole::TftInverse)) {
                unsigned remaining = 0, delivered = 0, required = 0, faults = 0, online = 0;
                if (sscanf(packet.payload + 4, "%u,%u,%u,%u,%u", &remaining, &delivered,
                           &required, &faults, &online) == 5) {
                    tftMissionTimeSeconds = remaining;
                    tftDeliveredPackets = delivered;
                    tftRequiredPackets = required;
                    tftActiveFaults = faults;
                    tftOnlineNodes = online;
                }
            } else if (packet.msgType == kMsgGameEvent &&
                       strncmp(packet.payload, "FAULT:", 6) == 0 && packet.dstNode == nodeId &&
                       (role == TenetRole::TftNormal || role == TenetRole::TftInverse)) {
                strncpy(tftLatestFault, packet.payload + 6, sizeof(tftLatestFault) - 1);
                tftLatestFault[sizeof(tftLatestFault) - 1] = '\0';
                 } else if (packet.msgType == kMsgGameEvent && packet.dstNode == nodeId &&
                      strncmp(packet.payload, "TASK:", 5) == 0 && role == TenetRole::TftInverse) {
                  strncpy(tftLatestFault, packet.payload + 5, sizeof(tftLatestFault) - 1);
                  tftLatestFault[sizeof(tftLatestFault) - 1] = '\0';
                  strncpy(tftMissionState, "FIELD TASK", sizeof(tftMissionState) - 1);
                 } else if (packet.msgType == kMsgGameEvent && packet.dstNode == nodeId &&
                      (strncmp(packet.payload, "IR_SAMPLE:", 10) == 0 ||
                       strncmp(packet.payload, "IR_REJECT:", 10) == 0) &&
                      role == TenetRole::TftInverse) {
                  strncpy(tftLatestFault, packet.payload, sizeof(tftLatestFault) - 1);
                  tftLatestFault[sizeof(tftLatestFault) - 1] = '\0';
                  strncpy(tftMissionState, "IR SIGNAL", sizeof(tftMissionState) - 1);
            } else if (packet.msgType == kMsgGameEvent && packet.dstNode == nodeId &&
                       strncmp(packet.payload, "FAULT:", 6) == 0) {
                const char *fault = packet.payload + 6;
                static constexpr const char *faultNames[] = {
                    "INVERSE", "PARADOX", "LOSS", "DELAY", "DUPLICATE", "INVALID_PAYLOAD"};
                static constexpr uint8_t faultStates[] = {1, 2, 4, 5, 6, 7};
                uint8_t selectedState = 2;
                for (uint8_t index = 0; index < 6; ++index) {
                    if (strcmp(fault, faultNames[index]) == 0) {
                        selectedState = faultStates[index];
                        break;
                    }
                }
                nodeGameState[nodeId] = selectedState;
                paradox = selectedState == 2;
                const char *status = selectedState == 1 ? "INVERSE"
                    : selectedState == 2 ? "PARADOX"
                    : selectedState == 4 ? "LOSS"
                    : selectedState == 5 ? "DELAY"
                    : selectedState == 6 ? "DUPLICATE" : "INVALID_PAYLOAD";
                sendLocalPacket(kMsgStatus, 5, status);
            } else if (packet.msgType == kMsgData && packet.dstNode == nodeId) {
                const uint16_t logicalId = packet.logicalPacketId;
                if (packet.missionId != missionId || logicalId == 0 || logicalId > 100) {
                    sendLocalPacket(kMsgError, 5, "PACKET_REJECTED", logicalId);
                } else if (nodeGameState[nodeId] == 4) {
                    return;
                } else if (nodeGameState[nodeId] == 2 || nodeGameState[nodeId] == 7 ||
                           nodeGameState[nodeId] == 1) {
                    char errorPayload[24];
                    snprintf(errorPayload, sizeof(errorPayload), "INVALID:%u", logicalId);
                    sendLocalPacket(kMsgError, 5, errorPayload, logicalId);
                } else {
                    const bool duplicate = nodeDeliveredLogicalPackets[logicalId];
                    nodeDeliveredLogicalPackets[logicalId] = true;
                    if (!duplicate && nodeGameState[nodeId] == 5) {
                        delayedLogicalPacketId = logicalId;
                        delayedAckAt = millis() + 1800;
                        delayedAckPending = true;
                    } else {
                        sendLocalPacket(kMsgAck, 5, duplicate ? "ACK:DUPLICATE" : "ACK:PROCESSED", logicalId);
                        if (nodeGameState[nodeId] == 6) {
                            sendLocalPacket(kMsgAck, 5, "ACK:DUPLICATE", logicalId);
                        }
                    }
                }
            } else if (packet.msgType == kMsgRepair && packet.dstNode == nodeId) {
                if (nodeGameState[nodeId] != 0 && nodeGameState[nodeId] != 3 && !repairInProgress) {
                    nodeGameState[nodeId] = 3;
                    repairInProgress = true;
                    repairCompleteAt = millis() + 500;
                    repairCommandMessageId = packet.messageId;
                    sendLocalPacket(kMsgStatus, 5, "REPAIRING");
                } else {
                    sendLocalPacket(kMsgCommandResult, 5, "REPAIR_INVALID", packet.messageId);
                }
            } else if (packet.msgType == kMsgPing && strcmp(packet.payload, "STATE_QUERY") == 0) {
                const char *state = nodeGameState[nodeId] == 3 ? "REPAIRING"
                    : nodeGameState[nodeId] == 2 ? "PARADOX"
                    : nodeGameState[nodeId] == 1 ? "INVERSE" : "NORMAL";
                sendLocalPacket(kMsgStatus, 5, state);
            } else if (packet.msgType == kMsgHeartbeat &&
                       strcmp(packet.payload, "TEST_PING") == 0) {
                const char *response = "PONG:OK";
                if (role == TenetRole::TftNormal || role == TenetRole::TftInverse) {
                    response = displayReady ? "PONG:LCD_OK" : "PONG:LCD_FAIL";
                }
                sendLocalPacket(kMsgHeartbeat, 5, response);
            } else if (packet.msgType == kMsgHeartbeat && role == TenetRole::IrGate &&
                       strcmp(packet.payload, "IR_STATE?") == 0) {
                sendLocalPacket(kMsgHeartbeat, 5,
                                digitalRead(kIrRxPin) == HIGH ? "IR_STATE:HIGH" : "IR_STATE:LOW");
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
        const char *state = nodeGameState[nodeId] == 0 ? "NORMAL"
            : nodeGameState[nodeId] == 1 ? "INVERSE"
            : nodeGameState[nodeId] == 2 ? "PARADOX"
            : nodeGameState[nodeId] == 3 ? "REPAIRING"
            : nodeGameState[nodeId] == 4 ? "LOSS"
            : nodeGameState[nodeId] == 5 ? "DELAY"
            : nodeGameState[nodeId] == 6 ? "DUPLICATE" : "INVALID_PAYLOAD";
        sendLocalPacket(kMsgStatus, 5, state);
    }

    void drawDisplay() {
        const uint32_t now = millis();
        if (now - lastDisplayAt < 250) return;
        lastDisplayAt = now;
        if (now < tftVisualTestUntil) {
            canvas.fillScreen(TFT_BLACK);
            canvas.setTextColor(TFT_WHITE);
            canvas.setFont(&fonts::Font4);
            canvas.drawCenterString("ILI9341 DISPLAY TEST", 160, 22);
            canvas.setTextColor(TFT_CYAN);
            canvas.setFont(&fonts::Font2);
            canvas.drawCenterString(role == TenetRole::TftNormal ? "NODE 0 / BLUE" : "NODE 1 / RED", 160, 54);
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

        canvas.fillScreen(TFT_BLACK);
        const bool globalDisplay = role == TenetRole::TftNormal;
        canvas.setTextColor(globalDisplay ? TFT_CYAN : TFT_RED);
        canvas.setFont(&fonts::Font4);
        canvas.drawCenterString(globalDisplay ? "TENET MESH" : "FIELD TASK", 160, 12);
        canvas.setTextColor(TFT_WHITE);
        canvas.setFont(&fonts::Font2);
        canvas.drawCenterString(globalDisplay ? "NETWORK RECOVERY" : tftMissionState, 160, 37);

        if (globalDisplay) {
            char timeText[12];
            snprintf(timeText, sizeof(timeText), "%02lu:%02lu",
                     static_cast<unsigned long>(tftMissionTimeSeconds / 60),
                     static_cast<unsigned long>(tftMissionTimeSeconds % 60));
            canvas.setTextColor(TFT_WHITE);
            canvas.setFont(&fonts::Font8);
            canvas.drawCenterString(timeText, 160, 82);
            canvas.setFont(&fonts::Font4);
            char nodesText[20];
            snprintf(nodesText, sizeof(nodesText), "NODES %u/5", tftOnlineNodes);
            canvas.drawString(nodesText, 18, 139);
            char packetsText[24];
            snprintf(packetsText, sizeof(packetsText), "PACKETS %02u/%02u", tftDeliveredPackets, tftRequiredPackets);
            canvas.drawString(packetsText, 18, 169);
            char faultsText[20];
            snprintf(faultsText, sizeof(faultsText), "ERRORS %u", tftActiveFaults);
            canvas.drawString(faultsText, 18, 199);
        } else {
            canvas.setTextColor(tftLatestFault[0] == '\0' ? TFT_GREEN : TFT_YELLOW);
            canvas.setFont(&fonts::Font4);
            canvas.drawCenterString(tftLatestFault[0] == '\0' ? "NO ACTIVE FAULT" : tftLatestFault, 160, 82);
            canvas.setTextColor(TFT_WHITE);
            canvas.setFont(&fonts::Font2);
            canvas.drawCenterString("Short: confirm", 160, 130);
            canvas.drawCenterString("Medium: repair", 160, 158);
            canvas.drawCenterString("Long: pause / resume", 160, 186);
        }
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
        if (role != TenetRole::Gateway) {
            char hello[24];
            const char *roleName = role == TenetRole::TftNormal ? "TFT_NORMAL"
                : role == TenetRole::TftInverse ? "TFT_INVERSE"
                : role == TenetRole::IrGate ? "IR_GATE"
                : role == TenetRole::Relay ? "RELAY" : "IR_REMOTE";
            snprintf(hello, sizeof(hello), "FW:%s:%s", kFirmwareVersion, roleName);
            sendLocalPacket(kMsgHello, 5, hello);
        }
        if (role == TenetRole::IrGate) {
            pinMode(kIrRxPin, INPUT);
            irLastRaw = digitalRead(kIrRxPin);
            irLastActive = irActiveHigh ? irLastRaw == HIGH : irLastRaw == LOW;
            irEdgeHead = 0;
            irEdgeTail = 0;
            attachInterrupt(digitalPinToInterrupt(kIrRxPin), captureIrEdge, CHANGE);
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

    bool popIrEdge(IrEdgeEvent &edge) {
        const uint16_t tail = irEdgeTail;
        if (tail == irEdgeHead) return false;
        edge.timestampUs = irEdges[tail].timestampUs;
        edge.rawLevel = irEdges[tail].rawLevel;
        irEdgeTail = (tail + 1) % kIrEdgeBufferSize;
        return true;
    }

    void updateIrGate() {
        if (role != TenetRole::IrGate) return;
        IrEdgeEvent edge{};
        while (popIrEdge(edge)) {
            const bool active = irActiveHigh ? edge.rawLevel == HIGH : edge.rawLevel == LOW;
            irLastRaw = edge.rawLevel;
            if (active && !irLastActive) {
                if (!irGroupOpen) {
                    irGroupOpen = true;
                    irGroupStartedAtUs = edge.timestampUs;
                    irGroupActiveUs = 0;
                    irLastGapUs = 0;
                    emitIrSample("ACTIVE", "GROUP_START");
                } else {
                    irLastGapUs = edge.timestampUs - irLastInactiveAtUs;
                }
                irPulseOpen = true;
                irPulseStartedAtUs = edge.timestampUs;
            } else if (!active && irLastActive && irPulseOpen) {
                const uint32_t pulseUs = edge.timestampUs - irPulseStartedAtUs;
                irGroupActiveUs += pulseUs;
                irLastInactiveAtUs = edge.timestampUs;
                irPulseOpen = false;
                emitIrSample("INACTIVE", "PULSE_END", pulseUs);
            }
            irLastActive = active;
        }
        if (!irGroupOpen) return;
        const uint32_t nowUs = micros();
        if (nowUs - irGroupStartedAtUs >= static_cast<uint32_t>(irMaximumGroupMs) * 1000UL) {
            finalizeIrGroup("MAX_DURATION");
        } else if (!irLastActive && !irPulseOpen &&
                   nowUs - irLastInactiveAtUs >= static_cast<uint32_t>(irGroupGapMs) * 1000UL) {
            finalizeIrGroup("GROUP_GAP");
        }
    }

    void updateNodeRepair() {
        const uint32_t now = millis();
        if (repairInProgress && now >= repairCompleteAt) {
            repairInProgress = false;
            paradox = false;
            nodeGameState[nodeId] = 0;
            sendLocalPacket(kMsgStatus, 5, "NORMAL");
            sendLocalPacket(kMsgCommandResult, 5, "REPAIR_OK", repairCommandMessageId);
        }
        if (delayedAckPending && now >= delayedAckAt) {
            delayedAckPending = false;
            sendLocalPacket(kMsgAck, 5, "ACK:DELAYED", delayedLogicalPacketId);
        }
    }

    const char *classifyIrGroup(uint32_t activeMs) const {
        if (activeMs < irMinimumActiveMs || activeMs > irMaximumGroupMs) return "INVALID";
        if (activeMs >= irShortMinMs && activeMs <= irShortMaxMs) return "SHORT";
        if (activeMs >= irMediumMinMs && activeMs <= irMediumMaxMs) return "MEDIUM";
        if (activeMs >= irLongMinMs && activeMs <= irLongMaxMs) return "LONG";
        return "INVALID";
    }

    void emitIrSample(const char *activeState, const char *reason, uint32_t pulseUs = 0) {
        if (role != TenetRole::IrGate || !irCalibrationMode) return;
        const char *category = irGroupOpen ? classifyIrGroup(irGroupActiveUs / 1000) : "UNKNOWN";
        char payload[80];
        snprintf(payload, sizeof(payload), "IR_SAMPLE:%d,%s,%lu,%lu,%lu,%s,%s",
                 irLastRaw == HIGH ? 1 : 0, activeState,
                 static_cast<unsigned long>(pulseUs),
                 static_cast<unsigned long>(irLastGapUs),
                 static_cast<unsigned long>(irGroupActiveUs), category, reason);
        sendLocalPacket(kMsgDiagnostic, 5, payload);
    }

    bool applyIrConfig(const char *payload) {
        unsigned activeHigh = 0, groupGap = 0, shortMin = 0, shortMax = 0;
        unsigned mediumMin = 0, mediumMax = 0, longMin = 0, longMax = 0;
        unsigned minimumActive = 0, maximumGroup = 0, cooldown = 0;
        const int parsed = sscanf(payload, "IRCFG:%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                                  &activeHigh, &groupGap, &shortMin, &shortMax,
                                  &mediumMin, &mediumMax, &longMin, &longMax,
                                  &minimumActive, &maximumGroup, &cooldown);
        if (parsed != 11 || activeHigh > 1 || groupGap < 10 || groupGap > 1000 ||
            minimumActive == 0 || minimumActive >= shortMin ||
            shortMin > shortMax || shortMax >= mediumMin || mediumMin > mediumMax ||
            mediumMax >= longMin || longMin > longMax || maximumGroup <= longMax ||
            maximumGroup > 10000 || cooldown > 10000) {
            return false;
        }
        irActiveHigh = activeHigh != 0;
        irGroupGapMs = groupGap;
        irShortMinMs = shortMin;
        irShortMaxMs = shortMax;
        irMediumMinMs = mediumMin;
        irMediumMaxMs = mediumMax;
        irLongMinMs = longMin;
        irLongMaxMs = longMax;
        irMinimumActiveMs = minimumActive;
        irMaximumGroupMs = maximumGroup;
        irCooldownMs = cooldown;
        return true;
    }

    void finalizeIrGroup(const char *reason) {
        if (!irGroupOpen) return;
        const uint32_t nowUs = micros();
        if (irPulseOpen) {
            irGroupActiveUs += nowUs - irPulseStartedAtUs;
            irPulseOpen = false;
        }
        const uint32_t activeMs = irGroupActiveUs / 1000;
        const char *category = classifyIrGroup(activeMs);
        emitIrSample("INACTIVE", reason, 0);
        irGroupOpen = false;
        const uint32_t nowMs = nowUs / 1000;
        if (category == "INVALID" || (irHasAction && nowMs - irLastActionAt < irCooldownMs)) {
            sendLocalPacket(kMsgDiagnostic, 5,
                            category == "INVALID" ? "IR_REJECT:INVALID_RANGE" : "IR_REJECT:COOLDOWN");
            return;
        }
        irLastActionAt = nowMs;
        irHasAction = true;
        if (strcmp(category, "SHORT") == 0) {
            sendLocalPacket(kMsgCommand, 5, "IR_ACTION:SHORT");
        } else if (strcmp(category, "MEDIUM") == 0) {
            sendLocalPacket(kMsgCommand, 5, "IR_ACTION:MEDIUM");
        } else if (strcmp(category, "LONG") == 0) {
            sendLocalPacket(kMsgCommand, 5, "IR_ACTION:LONG");
        }
    }

    void update() {
        if (role == TenetRole::IrRemote) {
            updateRemote();
            return;
        }
        if (role == TenetRole::Gateway) {
            updateGatewaySerial();
            updateMission();
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
        updateNodeRepair();
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