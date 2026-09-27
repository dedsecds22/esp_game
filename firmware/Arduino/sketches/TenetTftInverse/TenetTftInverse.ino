#include <TenetMesh.h>

TenetFirmware firmware(TenetRole::TftInverse, 1);

void setup() {
    firmware.begin();
}

void loop() {
    firmware.update();
}