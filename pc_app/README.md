# TENET Mesh Control

Tkinter desktop controller for the ESP32 Gateway. Test Mode runs mesh,
IR-path, and IR input-level diagnostics. Game Mode runs the Defuse round:
countdown, difficulty-scaled infections, shared TFT key fragments,
target-side repair validation, and Gateway-reported victory/defeat. Serial
IO runs on a background thread and hands parsed JSON lines to the UI.

## Run

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe tenet_control.py
```

Select the Gateway COM port and `2000000` baud, then connect. The app sends
`{"cmd":"ping"}` and expects a `gateway_ready` event. Test Mode sends
`{"cmd":"test"}`, `{"cmd":"test_ir"}`, or `{"cmd":"ir_state"}`. The last
command returns the IR_GATE's digital LOW/HIGH input over ESP-NOW.

Run the mesh test first. Game Mode starts only after all four mesh nodes
and both TFT initializations pass. Easy, Normal, and Hard use 20, 12, and
6 second infection intervals. A correct session key repairs the selected
node; a wrong key locks that node for 3 seconds. The Gateway reports
victory at countdown zero if no paradox remains, and defeat for an offline
node or an unresolved paradox.

Payload Delivery and scoring are not implemented in this initial playable
mode.

## Protocol tests

Run the serial JSON codec tests without hardware:

```powershell
py -3 -m unittest discover -s . -p "test_*.py"
```