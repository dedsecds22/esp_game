#include <TenetMesh.h>

TenetFirmware firmware(TenetRole::TftNormal, 0);

void setup() {
    firmware.begin();
}

void loop() {
    firmware.update();
}