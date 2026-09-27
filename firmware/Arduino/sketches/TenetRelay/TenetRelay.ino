#include <TenetMesh.h>

TenetFirmware firmware(TenetRole::Relay, 3);

void setup() {
    firmware.begin();
}

void loop() {
    firmware.update();
}