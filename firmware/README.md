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

## First hardware test

Flash the matching sketch to each ESP32-S3 individually. The TFTs, relay,
and IR_GATE only need USB temporarily for flashing; after flashing, power
all five mesh nodes at the same time. Connect only the Gateway to the PC
for normal operation. The battery remote does not join ESP-NOW.

Both TFTs show a five-second ILI9341 test screen with labeled red, green,
blue, white, yellow, and cyan bars. Check that the full 320x240 image is
visible, the colors look correct, and all labels are upright. After that,
the blue TFT should show a decreasing countdown and the red TFT an
increasing counter. The Gateway reports display initialization over
ESP-NOW; because this SPI wiring does not read pixels back, the color and
orientation check must be done by looking at both panels.

Open the Gateway Serial Monitor at 2,000,000 baud with newline line
ending. Send:

```json
{"cmd":"test"}
```

Within five seconds, expect a `mesh_test_summary` with `status: PASS`,
`online: 4`, `expected: 4`, and `tft_init: PASS`. Individual
`mesh_test_node` events identify nodes 0, 1, 3, and 4 and include RSSI;
TFT nodes also report display initialization. Any missing node or TFT
init reports FAIL. This checks ESP-NOW request/response connectivity,
not merely periodic beacons.

To test the IR remote through to the Gateway, send
`{"cmd":"test_ir"}` and press the remote button within 15 seconds. Expect
`ir_test` status `PASS` for `IR_REMOTE->IR_GATE->ESP_NOW->GATEWAY`. A
timeout indicates the IR code, receiver wiring, or mesh path needs checking.

Then run the gameplay transport preflight:

1. Send `{"cmd":"start","seconds":10}`. Confirm blue counts down from
	10 and red counts up from 0.
2. Send `{"cmd":"invert"}`. Confirm both screens change from NORMAL to
	INVERSE. This tests the control broadcast only; the separate IR test
	checks the handheld-triggered broadcast.

These are connection and display preflight tests, not a claim that all
game mechanics are complete. Paradox generation/detection, repair-key
validation, countdown victory handling, and scoring are not implemented
yet.

The Gateway accepts newline-terminated JSON commands such as
`{"cmd":"start","seconds":60}`, `{"cmd":"repair","target":1,"key":"0x4F2A"}`,
`{"cmd":"invert"}`, and `{"cmd":"ping"}`. Mesh packets are output as JSON
lines with `src`, `dst`, `type`, `id`, `dir`, `payload`, and `rssi` fields.

The shared ESP-NOW packet is 25 bytes with an XOR checksum. This firmware
provides the radio bridge, TFT status screen, IR control, and relay base;
paradox detection, repair-key verification, and scoring remain future work.