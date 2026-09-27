#include <TenetMesh.h>

TenetFirmware firmware(TenetRole::Gateway, 5);

void setup() {
    firmware.begin();
}

void loop() {
    firmware.update();
}