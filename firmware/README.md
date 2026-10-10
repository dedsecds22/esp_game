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

### ESP32-S3 upload troubleshooting

`Failed to connect to ESP32-S3: No serial data received` means esptool
opened a COM port but did not receive a ROM-bootloader response. The
runtime USB CDC setting does not itself put the chip in download mode.

For the native USB connector, select `USB Mode: Hardware CDC and JTAG`,
`Upload Mode: UART0 / Hardware CDC`, and `USB CDC On Boot: Enabled`. Close
Serial Monitor, select the ESP32-S3's current COM port, and try:

1. Hold the board's `BOOT` button.
2. While holding it, click Upload in Arduino IDE.
3. When the console shows `Connecting...`, release `BOOT`.

If it still does not connect, hold `BOOT`, tap and release `RESET/EN`,
then release `BOOT` and retry Upload. Lower `Upload Speed` to `115200`.
The port can disappear and reappear when the chip changes from application
mode to ROM bootloader mode; reselect the newly appearing ESP32-S3 port if
needed. Make sure the cable carries data and that it is plugged into the
board's native USB connector, not its UART-only connector. Do not select
the Gateway runtime baud rate as the upload speed.

Open the sketch matching the ESP being flashed:

- `TenetGateway/TenetGateway.ino` - USB serial bridge, node ID 5
- `TenetTftNormal/TenetTftNormal.ino` - blue ILI9341, node ID 0
- `TenetTftInverse/TenetTftInverse.ino` - red ILI9341, node ID 1
- `TenetRelay/TenetRelay.ino` - relay, node ID 3
- `TenetIrGate/TenetIrGate.ino` - IR receiver and mesh broadcaster, node ID 4
- `TenetIrRemote/TenetIrRemote.ino` - standalone battery remote, node ID 2
- `TenetIrGateRawTest/TenetIrGateRawTest.ino` - temporary raw sensor test
- `TenetIrRemoteRawTest/TenetIrRemoteRawTest.ino` - temporary 38 kHz LED test

The production IR_GATE uses the measured sensor polarity: GPIO 7 is LOW
when idle and HIGH when IR is detected. It sends one mesh inversion on a
LOW-to-HIGH transition, with the normal 8-second cooldown. It does not
decode a NEC address or command; any IR source visible to the sensor can
trigger it. The remote still emits the NEC-modulated IR frame, which the
simple sensor only treats as detected light.

## Temporary raw IR test

Use these temporary sketches before trying the mesh IR roles. Upload each
to its own ESP32 separately; open Serial Monitor at 115200 baud. They do
not initialize ESP-NOW.

For the receiver, connect VCC/GND and its signal wire to GPIO 7. Open
`TenetIrGateRawTest.ino`; the sketch prints the raw input every second and
prints each HIGH/LOW transition. This device has been verified LOW while
idle and HIGH while receiving. Power the sensor at 3.3 V if supported and
ensure its signal output never exceeds 3.3 V; ESP32-S3 GPIOs are not 5 V
tolerant. Use a level shifter or divider if the sensor outputs 5 V.

For a bare two-lead IR LED, connect GPIO 6 to the LED anode through a
current-limiting resistor and connect its cathode to GND. Do not connect a
bare LED directly across VCC/GND or directly to a GPIO without current
limiting. If the two-lead part is a prebuilt module that expects a supply,
do not connect its VCC directly to GPIO 6; switch its supply with a suitable
transistor instead. Open `TenetIrRemoteRawTest.ino` and send `1` to enable
the 38 kHz carrier, `0` to stop it, `b` for a 200 ms burst, or `a` to
toggle repeating 100 ms bursts once per second. Start `a`, then switch the
Serial Monitor to the receiver's COM port and observe its input. The IR
light is invisible.

This test checks the emitter, sensor output, and wiring only. It does not
send a NEC command or exercise ESP-NOW. A receiver that is only a bare
phototransistor may respond differently from a 38 kHz demodulating module.

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
through a transistor. IR_GATE reads the active-HIGH sensor output on GPIO
7. The remote carrier/code and these pins are configured in `TenetMesh.cpp`.

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

The Gateway is intentionally silent at startup. Open its Serial Monitor at
2,000,000 baud and set the line ending to `Newline` (not `No line ending`).
First send this probe:

```json
{"cmd":"ping"}
```

Expect `{"event":"gateway_ready","node":5}`. The command parser only
processes a command after it receives the terminating newline. Once this
probe works, send the mesh test:

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
Send `{"cmd":"ir_state"}` to query the IR_GATE's input level; the Gateway
returns an `ir_state` event with `level` set to `LOW` or `HIGH`.

Then start Defuse from the PC app's Game Mode. A successful mesh test is
required first. The Gateway chooses an uninfected node at the selected
difficulty interval and sends an encoded paradox event. Both TFTs display
one byte of a shared random session key. Join the blue byte as the high
byte and the red byte as the low byte to form `session_key`. The packet log
shows the infected node ID and encoded error code; calculate
`session_key = error_code XOR node_id` and submit the result as a four-digit
hex repair key. A bad key locks repairs for three seconds. The
Gateway declares victory when the countdown reaches zero with no paradox,
or defeat if a mesh node goes offline for five seconds or a paradox remains
at zero.

For a short transport-only round from Serial Monitor, send:

1. Send `{"cmd":"start","seconds":10}`. Confirm blue counts down from
	10 and red counts up from 0.
2. Send `{"cmd":"invert"}`. Confirm both screens change from NORMAL to
	INVERSE. This tests the control broadcast only; the separate IR test
	checks the handheld-triggered broadcast.

This first playable mode is Defuse. Payload Delivery and scoring remain
future work.

The Gateway accepts newline-terminated JSON commands such as
`{"cmd":"start","seconds":60,"difficulty":"normal"}`, `{"cmd":"repair","target":1,"key":"0x4F2A"}`,
`{"cmd":"invert"}`, `{"cmd":"ir_state"}`, and `{"cmd":"ping"}`. Mesh
packets are output as JSON lines with `src`, `dst`, `type`, `id`, `dir`,
`payload`, and `rssi` fields.

The shared ESP-NOW packet is 25 bytes with an XOR checksum. This firmware
provides the radio bridge, TFT status screen, IR control, relay base, and
initial Defuse mechanics. Payload Delivery and scoring remain future work.