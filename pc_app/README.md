# TENET Mesh Control

Tkinter desktop controller for the ESP32 Gateway. Lobby, Mission Control,
Packet Monitor, Test Mode, and IR Calibration all use Gateway-reported
state. The serial reader runs on a background thread and hands newline JSON
events to the Tkinter thread.

## Run

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe tenet_control.py
```

Select the Gateway COM port and `2000000` baud, then connect. The app retries
`ping` until it receives `gateway_ready`; it sends a host heartbeat every
two seconds while connected. A lost PC link pauses the mission at the
Gateway; reconnecting does not resume it automatically. Test Mode sends
`{"cmd":"test"}`, `{"cmd":"test_ir"}`, or `{"cmd":"ir_state"}`.

Run Scan, the mesh/TFT test, and an IR input check before starting. Mission
Control supports the spec's Easy/Normal/Hard defaults (300/240/180 seconds,
20/12/6 second faults, and 5/8/12 logical packets), selected fault types,
packet target, pause/resume, abort, and targeted repair. Delivery counts
advance only on destination application ACK. The Gateway reports mission
success/failure and deterministic score components.

Mission start is blocked until the Gateway handshake, four required mesh
nodes, TFT initialization, a fresh IR input query, and an acknowledged IR
configuration are ready. The Gateway then waits for CONFIG_OK from all
required nodes before its three-second countdown. Retries use a new message
ID but retain the same logical packet ID; duplicate logical delivery is
never scored twice.

IR Calibration configures polarity, active pulse group thresholds, gap,
maximum group time, and cooldown. It shows raw input plus measured pulse,
gap, group active time, category, and rejection reason. Settings are not
considered applied until the IR_GATE reports `IR_CONFIG_OK`.

The repository is not yet the full 1.0 acceptance state: physical multi-node
mission runs, packet retry/fault matrix testing, JSON+CSV export against
real session results, diagnostics command discovery, and full 2/4-player
playtesting are still required. The current protocol uses a fixed discovery
inventory for node IDs 0, 1, 3, and 4; node 2 is a standalone IR remote.

## Protocol tests

Run the serial JSON codec tests without hardware:

```powershell
py -3 -m unittest discover -s . -p "test_*.py"
```