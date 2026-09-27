#include <TenetMesh.h>

TenetFirmware firmware(TenetRole::IrGate, 4);

void setup() {
    firmware.begin();
}

void loop() {
    firmware.update();
}