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

The IR_GATE defaults to the measured sensor polarity (LOW idle, HIGH active)
and captures edges with a GPIO interrupt. It groups pulses by the calibrated
gap, sums active time in microseconds, then classifies SHORT/MEDIUM/LONG or
rejects the group. It does not decode NEC/RC5 button codes. The remote still
emits a modulated IR signal, but any visible compatible IR remote may be
used. IR actions are accepted only when the Gateway permits them.

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

Then start Network Recovery from Mission Control in the PC app. The Gateway
validates the mission configuration, sends it to nodes 0, 1, 3, and 4, and
waits for each node's application-level `CONFIG_OK`. Only then does the
three-second countdown begin. Easy/Normal/Hard defaults are 300/240/180 s,
20/12/6 s fault intervals, and 5/8/12 required logical packets.

During RUNNING, node faults are injected as game-state changes, not physical
hardware failures. A data packet increments delivery progress only after
its destination processes it and returns an application `ACK`. Retries use
new transport message IDs and retain the logical packet ID. A repair is
accepted only for a node currently reported faulty; the UI reports success
only after `REPAIRING` followed by a node `NORMAL` status and command result.
An offline required node or mission deadline produces `MISSION_FAILED`.
The Gateway reports the final score components and mission result.

IR Calibration uses the measured LOW-idle/HIGH-active sensor by default.
`ir_calibrate` captures pulse groups; `ir_config` applies the validated
polarity, gap, duration bands, max group, and cooldown. `ir_state` remains an
on-demand raw LOW/HIGH query. SHORT performs a status query, MEDIUM requests
repair of an active fault, and LONG pauses/resumes; the Gateway returns an
explicit accepted/rejected result.

The Gateway accepts newline-terminated JSON commands such as
`{"cmd":"mission_start","mission_id":123,"difficulty":"normal","duration_seconds":240,"packet_count":8,"packet_target":0,"fault_interval_seconds":12,"fault_intensity":2,"fault_types":["PARADOX","LOSS"]}`, `{"cmd":"mission_pause"}`, `{"cmd":"mission_resume"}`, `{"cmd":"mission_abort"}`, `{"cmd":"repair","target":1}`, `{"cmd":"ir_calibrate"}`, `{"cmd":"ir_config","active_high":true}`, `{"cmd":"ir_state"}`, and `{"cmd":"ping"}`. A running PC app sends `host_heartbeat` every two seconds; after five seconds without it, the Gateway pauses the mission. Resume is explicit.

The shared ESP-NOW frame is 95 bytes with an XOR checksum and is below the
250-byte ESP-NOW limit. It has a 15-byte header and an 80-byte payload
field. Mission setup requires application-level CONFIG_OK
from all four mesh nodes. Logical PACKET_DATA is only counted after a
destination ACK; retries keep the logical ID and use new message IDs. The
firmware implements the initial Network Recovery loop, but physical
delivery/loss/retry, 2/4-player, IR-calibration, and complete mission-result
tests remain required before claiming spec 1.0 acceptance.