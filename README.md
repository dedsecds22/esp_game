# TENET Mesh

ESP32-S3 alapú, ESP-NOW hálózatra épülő játékprojekt. A műszaki
specifikációk és a játékszabályok a repository gyökerében találhatók.

## Firmware indítása másik gépen

1. Klónozd a repository-t, majd telepítsd az Arduino IDE-t és az
   Espressif ESP32 board package-et.
2. Az Arduino Library Managerből telepítsd az `ArduinoJson`,
   `LovyanGFX` és `IRremoteESP8266` könyvtárakat.
3. Másold a `firmware/Arduino/libraries/TenetMesh` mappát az Arduino
   sketchbook `libraries` mappájába, majd indítsd újra az IDE-t.
4. Nyisd meg a megfelelő sketch-et a
   `firmware/Arduino/sketches` alatt, válaszd ki az ESP32-S3 boardot,
   majd töltsd fel az eszközre.

| Eszköz | Sketch | Node ID |
| --- | --- | ---: |
| Gateway / USB-CDC | `TenetGateway/TenetGateway.ino` | 5 |
| Kék TFT | `TenetTftNormal/TenetTftNormal.ino` | 0 |
| Piros TFT | `TenetTftInverse/TenetTftInverse.ino` | 1 |
| Relay | `TenetRelay/TenetRelay.ino` | 3 |
| IR vevő és mesh gateway | `TenetIrGate/TenetIrGate.ino` | 4 |
| Elemes, önálló IR távirányító | `TenetIrRemote/TenetIrRemote.ino` | 2 |

TFT pin-kiosztás, soros parancsok és eszközbeállítások: [firmware README](firmware/README.md).

## PC control app

The Tkinter Network Recovery controller is under `pc_app`. Create its environment
with `py -3 -m venv pc_app/.venv`, install dependencies using
`pc_app/.venv/Scripts/python.exe -m pip install -r pc_app/requirements.txt`,
then run `pc_app/.venv/Scripts/python.exe pc_app/tenet_control.py`. The app
provides Lobby, Mission Control, Packet Monitor, Test Mode, and IR Calibration; details are in
[pc app README](pc_app/README.md).

## Dokumentáció

- [Műszaki specifikáció](tenet-mesh-spec.md)
- [Játékszabályok és WinForms vezérlő](tenet-mesh-rules-and-winforms.md)
- [Projektjegyzetek](tenet-mesh.md)