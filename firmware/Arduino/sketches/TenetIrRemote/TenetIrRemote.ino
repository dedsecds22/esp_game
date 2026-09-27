#include <TenetMesh.h>

TenetFirmware firmware(TenetRole::IrRemote, 2);

void setup() {
    firmware.begin();
}

void loop() {
    firmware.update();
}