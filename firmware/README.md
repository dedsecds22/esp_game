# TENET Mesh Arduino firmware

Separate Arduino IDE sketches provide upload targets for the Gateway,
blue TFT, red TFT, relay, receive-only IR_GATE, and battery-powered IR
remote.

## Arduino IDE setup

1. Install the ESP32 board package by Espressif Systems.
2. Install `ArduinoJson`, `LovyanGFX`, and `IRremoteESP8266` from Library
	Manager.
3. Copy `firmware/Arduino/libraries/TenetMesh` into the Arduino sketchbook's
	`libraries` folder, then restart Arduino IDE.
4. Open a sketch under `firmware/Arduino/sketches` and select an ESP32-S3
	board. For the Gateway, enable USB CDC On Boot and select 2,000,000 baud.

Open the sketch matching the ESP being flashed:

- `TenetGateway/TenetGateway.ino` - USB serial bridge, node ID 5
- `TenetTftNormal/TenetTftNormal.ino` - blue ILI9341, node ID 0
- `TenetTftInverse/TenetTftInverse.ino` - red ILI9341, node ID 1
- `TenetRelay/TenetRelay.ino` - relay, node ID 3
- `TenetIrGate/TenetIrGate.ino` - IR receiver and mesh broadcaster, node ID 4
- `TenetIrRemote/TenetIrRemote.ino` - standalone battery remote, node ID 2

## TFT wiring

The TFT sketches use LovyanGFX and the tested ESP32-S3 + ILI9341 setup
from `tft_example.ino`:

| Signal | ESP32-S3 GPIO |
| --- | ---: |
| SCLK | 12 |
| MOSI | 11 |
| MISO | 9 |
| DC | 13 |
| CS | 10 |
| RESET | 14 |

The panel is 240x320 in landscape rotation. Both TFT sketches draw through
a 320x240 sprite. The example's `background.cpp` asset is not needed for
the current status display.

## Other connections

All mesh nodes use Wi-Fi channel 1. The standalone remote uses an
active-low button on GPIO 4 and an IR LED driver on GPIO 6. Drive the LED
through a transistor. IR_GATE receives the 38 kHz signal on GPIO 7. The
NEC code and these pins are configured in `TenetMesh.cpp`.

The Gateway accepts newline-terminated JSON commands such as
`{"cmd":"start","seconds":60}`, `{"cmd":"repair","target":1,"key":"0x4F2A"}`,
`{"cmd":"invert"}`, and `{"cmd":"ping"}`. Mesh packets are output as JSON
lines with `src`, `dst`, `type`, `id`, `dir`, `payload`, and `rssi` fields.

The shared ESP-NOW packet is 25 bytes with an XOR checksum. This firmware
provides the radio bridge, TFT status screen, IR control, and relay base;
paradox detection, repair-key verification, and scoring remain future work.