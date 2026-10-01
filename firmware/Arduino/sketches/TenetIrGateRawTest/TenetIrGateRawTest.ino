constexpr uint8_t IR_RECEIVER_PIN = 7;
constexpr uint32_t REPORT_INTERVAL_MS = 1000;

int lastReportedState = -1;
uint32_t lastReportAt = 0;

void setup() {
    Serial.begin(115200);
    pinMode(IR_RECEIVER_PIN, INPUT_PULLUP);
    Serial.println("IR_GATE raw input test");
    Serial.println("Expected common demodulator output: HIGH idle, LOW while receiving.");
}

void loop() {
    const int state = digitalRead(IR_RECEIVER_PIN);
    const uint32_t now = millis();

    if (state != lastReportedState) {
        lastReportedState = state;
        Serial.printf("IR_OUT changed: %s (%d)\n", state == HIGH ? "HIGH" : "LOW", state);
    }

    if (now - lastReportAt >= REPORT_INTERVAL_MS) {
        lastReportAt = now;
        Serial.printf("IR_OUT sample: %s (%d)\n", state == HIGH ? "HIGH" : "LOW", state);
    }

    delay(5);
}