constexpr uint8_t IR_LED_PIN = 6;
constexpr uint32_t IR_CARRIER_HZ = 38000;
constexpr uint8_t IR_PWM_RESOLUTION_BITS = 8;
constexpr uint8_t IR_CARRIER_DUTY = 128;
constexpr uint32_t IR_BURST_MS = 200;
constexpr uint32_t AUTO_BURST_PERIOD_MS = 1000;
constexpr uint32_t AUTO_BURST_ON_MS = 100;
constexpr uint32_t STATUS_INTERVAL_MS = 3000;

bool carrierReady = false;
bool carrierOn = false;
bool autoBurstEnabled = false;
uint32_t lastStatusAt = 0;
uint32_t nextAutoBurstAt = 0;

void stopCarrier() {
    ledcWrite(IR_LED_PIN, 0);
    carrierOn = false;
}

void startCarrier() {
    if (!carrierReady) {
        Serial.println("ERROR: LEDC carrier setup failed");
        return;
    }
    ledcWrite(IR_LED_PIN, IR_CARRIER_DUTY);
    carrierOn = true;
}

void setup() {
    Serial.begin(115200);
    carrierReady = ledcAttach(IR_LED_PIN, IR_CARRIER_HZ, IR_PWM_RESOLUTION_BITS);
    stopCarrier();
    Serial.println("IR_REMOTE raw emitter test");
    Serial.println("Send 1=38kHz ON, 0=OFF, b=200ms burst.");
    Serial.println("Use a series resistor or transistor driver for a bare IR LED.");
    Serial.printf("Carrier setup: %s\n", carrierReady ? "OK" : "FAILED");
}

void loop() {
    while (Serial.available() > 0) {
        const char command = static_cast<char>(Serial.read());
        if (command == '1') {
            autoBurstEnabled = false;
            startCarrier();
            Serial.println("IR carrier ON");
        } else if (command == '0') {
            autoBurstEnabled = false;
            stopCarrier();
            Serial.println("IR carrier OFF");
        } else if (command == 'b' || command == 'B') {
            autoBurstEnabled = false;
            startCarrier();
            delay(IR_BURST_MS);
            stopCarrier();
            Serial.println("IR carrier burst sent");
        } else if (command == 'a' || command == 'A') {
            autoBurstEnabled = !autoBurstEnabled;
            nextAutoBurstAt = millis();
            stopCarrier();
            Serial.printf("Automatic bursts %s\n", autoBurstEnabled ? "ON" : "OFF");
        }
    }

    if (autoBurstEnabled && millis() >= nextAutoBurstAt) {
        startCarrier();
        delay(AUTO_BURST_ON_MS);
        stopCarrier();
        nextAutoBurstAt = millis() + AUTO_BURST_PERIOD_MS - AUTO_BURST_ON_MS;
    }

    if (millis() - lastStatusAt >= STATUS_INTERVAL_MS) {
        lastStatusAt = millis();
        Serial.printf("IR carrier state: %s\n", carrierOn ? "ON" : "OFF");
    }
}