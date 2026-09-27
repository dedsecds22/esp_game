# TENET MESH — Részletes Technikai Specifikáció

Öt ESP-NOW mesh node-ból és egy külön akkumulátoros IR távirányítóból
álló, összesen 6 ESP32-S3 eszközt használó "idő-inverziós" játék,
2× TFT kijelzővel és PC-s C# taktikai vezérlővel.

---

## 0. Rendszer-architektúra áttekintés

```
                    ┌─────────────────────────┐
                    │   PC (C# WPF app)        │
                    │   - Mesh térkép           │
                    │   - Packet sniffer        │
                    │   - Hack-interfész        │
                    └───────────┬───────────────┘
                          USB Serial (2 Mbaud)
                                │
                    ┌───────────┴───────────────┐
                    │  Node F: GATEWAY (ESP32-S3)│
                    │  mesh ↔ soros híd           │
                    └───────────┬───────────────┘
                                │ ESP-NOW (2.4 GHz, broadcast+unicast)
           ┌───────────┬───────────┼───────────┬───────────┐
           │           │           │           │           │
         Node 0 TFT1 Node 1 TFT2 Node 3 RELAY Node 4 IR_GATE
         (kék TFT)    (piros TFT)  (mesh)      (IR vevő + mesh)
                                    ▲
              Node 2 IR_REMOTE ── IR jel ──────┘
              (akkumulátoros, mesh-en kívül)
```

Node-szerepek fix táblában (`node_id` 0–5). Az öt mesh node közös
firmware-kódbázisból, a `NODE_ROLE` define-nal fordul; a 2-es ID-jű
IR_REMOTE külön, mesh-en kívüli firmware-cél.

| ID | Szerep         | HW extra              |
|----|----------------|------------------------|
| 0  | TFT_NORMAL     | TFT #1 (kék)            |
| 1  | TFT_INVERSE    | TFT #2 (piros)          |
| 2  | IR_REMOTE      | Elemes, önálló IR adó; nem mesh node |
| 3  | RELAY          | —                       |
| 4  | IR_GATE        | IR vevő + ESP-NOW        |
| 5  | GATEWAY        | USB-CDC a PC felé       |

---

## 1. Kommunikációs réteg: ESP-NOW

### Miért ESP-NOW és nem painlessMesh / ESP-MESH?
- **Connectionless**: nincs handshake/route-building késleltetés — ez
  kritikus, mert az `INVERT_TIME` broadcastnak *egyszerre* kell
  megérkeznie minden node-hoz, hogy a játékmenet szinkron maradjon.
- **Alacsony latency**: tipikusan 1–2 ms egy hopban, WiFi AP nélkül.
- **Broadcast natív támogatás**: `esp_now_send()` a broadcast MAC-re
  (`FF:FF:FF:FF:FF:FF`) minden regisztrált peer-hez eljuttatja a
  csomagot — ez lesz az időfordítás trigger csatornája.
- Hátránya: nincs automatikus multi-hop routing, azt neked kell
  megoldanod (lásd 1.3).

### 1.1 Inicializálás (minden node-on azonos)
```c
WiFi.mode(WIFI_STA);
esp_now_init();
esp_now_register_recv_cb(onDataRecv);
esp_now_register_send_cb(onDataSent);

// minden ismert node MAC-jét fel kell venni peer-ként
esp_now_peer_info_t peer = {};
memcpy(peer.peer_addr, known_mac[i], 6);
peer.channel = 1;
peer.encrypt = false; // első verzióban; lásd 1.4 titkosítás
esp_now_add_peer(&peer);
```

### 1.2 Csatorna-menedzsment
Mind a 6 node-nak **ugyanazon a WiFi csatornán** kell lennie (pl.
csatorna 1, fixen beégetve), mert ESP-NOW nem csatornát vált
automatikusan. Ha router is van a közelben, válassz szabad csatornát,
és kösd ki explicit `esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)`
hívással minden boot-nál — az S3 gyakran "beleragad" egy korábbi AP
csatornájába.

### 1.3 Multi-hop routing (a Relay node-ok szerepe)
Az IR_GATE és a Gateway lehet, hogy nincs közvetlen rádiós
hatótávolságban a TFT node-okkal (attól függően, hogyan rendezed el
fizikailag a szobában/asztalon — ez a játék "terepasztal" jellege).
Egyszerű **flood-routing** elég erre a léptékre:

```c
struct RouteEntry { uint8_t packet_id_low8; uint8_t src; uint32_t ts; };
RouteEntry seen_cache[32]; // körkörös buffer, duplikátum-szűrés

void onDataRecv(...) {
  if (isDuplicate(packet)) return;           // ne loopoljon vissza
  markSeen(packet);
  processLocally(packet);                     // saját logika (TFT, IR)
  if (packet.dst_node != my_id && packet.ttl > 0) {
    packet.ttl--;
    esp_now_send(NULL, (uint8_t*)&packet, sizeof(packet)); // re-broadcast
  }
}
```
`ttl` mezőt érdemes hozzáadni a packet structhoz (max 3–4 hop egy 6
node-os hálóban bőven elég), hogy elkerüld a broadcast-vihart.

### 1.4 Titkosítás / hitelesítés (opcionális, 2. körben)
Az ESP-NOW támogat CTR+CBC-MAC (CCMP) titkosítást `peer.encrypt=true`
+ LMK/PMK kulcsokkal. Első verzióban hagyd nyitva (egyszerűbb
debuggolni a C# sniffer oldalán), de ha a "Hekk-Interfész" játékelemet
komolyan veszed, egy második fázisban bekapcsolhatod — így a
játékosnak tényleg fel kell törnie valamit, nem csak UI-szinten
szimulálod.

---

## 2. Packet formátum és az idő-inverzió mechanikája

### 2.1 Struct (véglegesített)
```c
#pragma pack(push, 1)
typedef struct {
  uint8_t  magic;         // 0xAA — gyors validáció
  uint16_t packet_id;     // normál: monoton nő; inverz: monoton csökken
  int8_t   time_dir;      // +1 = normál idősík, -1 = inverz idősík
  uint8_t  src_node;
  uint8_t  dst_node;
  uint8_t  ttl;           // routing hop-limit
  uint8_t  msg_type;      // lásd 2.4
  char     payload[16];   // szabad payload, invertáláskor byte-reverse
  uint8_t  checksum;      // XOR minden előző byte-ra
} tenet_packet_t;
#pragma pack(pop)
// sizeof() = 25 byte — kényelmesen fér ESP-NOW max 250 byte-os keretbe
```

### 2.2 Az invertálás algoritmusa
Amikor egy csomag áthalad egy zóna-határon (IR gate triggerel, vagy
egy node saját maga van INVERSE állapotban és onnan küld):

```c
void invertPacket(tenet_packet_t *p) {
  p->time_dir *= -1;
  // payload byte-reverse: "HELLO\0..." -> "\0...OLLEH"
  int len = strnlen(p->payload, 16);
  for (int i = 0; i < len / 2; i++) {
    char tmp = p->payload[i];
    p->payload[i] = p->payload[len - 1 - i];
    p->payload[len - 1 - i] = tmp;
  }
  // packet_id iránya is megfordul: innentől csökkenő sorozatot várunk
  p->packet_id = REFERENCE_MAX_ID - p->packet_id; // tükrözés egy fix ponthoz
  recalcChecksum(p);
}
```
A `packet_id` tükrözését egy közös `REFERENCE_MAX_ID` konstanshoz
kötöd (pl. 9999), így mindkét irányból konzisztensen visszaszámolható,
hogy melyik "eredeti" packetről van szó — ez kell a C# app packet
sniffer korrelációjához is.

### 2.3 Paradoxon-detektálás (a hiba-állapot)
Minden node tartja a saját `my_time_dir` állapotát. Ha egy beérkező
csomag `time_dir`-je **nem egyezik** a fogadó node aktuális
állapotával:

```c
if (incoming.time_dir != my_time_dir) {
  triggerParadoxEvent(incoming); // TFT piros villogás + hibahang
  sendToGateway(PARADOX_EVENT, incoming.src_node);
  return; // a csomagot eldobjuk, nem dolgozzuk fel
}
```
Ez adja a játék fő feszültségét: a játékosnak *időzítenie* kell az IR
gate használatát, mert rosszkor fordítva egy csomó node paradoxonba
fagy, amíg vissza nem fordítod őket.

### 2.4 `msg_type` enum
```c
enum {
  MSG_HEARTBEAT   = 0x01, // RSSI + local_time riport a Gateway felé
  MSG_DATA        = 0x02, // játékbeli hasznos adat (a "titkos csomag")
  MSG_INVERT_TIME = 0x03, // IR gate broadcastja
  MSG_PARADOX     = 0x04, // hiba-riport
  MSG_REPAIR      = 0x05, // C# app -> mesh, játékos javító parancsa
  MSG_BOMB_STATE  = 0x06, // countdown szinkron a két TFT között
};
```

---

## 3. Node állapotgép (minden node-on fut)

```
        ┌────────────┐   INVERT_TIME broadcast   ┌────────────┐
        │   NORMAL    │ ─────────────────────────►│  INVERSE    │
        │ time_dir=+1 │◄───────────────────────────│ time_dir=-1 │
        └────────────┘   INVERT_TIME broadcast   └────────────┘
             │                                          │
             │ paradox packet érkezik                   │ paradox packet érkezik
             ▼                                          ▼
        ┌────────────────────────────────────────────────┐
        │              PARADOX (frozen)                    │
        │  - nem küld/fogad DATA-t                          │
        │  - villog, MSG_PARADOX-ot spamel a Gateway-nek     │
        │  - kilépés: MSG_REPAIR érkezik a helyes kulccsal    │
        └────────────────────────────────────────────────┘
```

Zóna-alapú viselkedés: nem kell, hogy *minden* node egyszerre
forduljon. Definiálhatsz egy `zone_id`-t node-onként, és az IR gate
csak a saját zónájának node-jait fordítja meg — ez taktikailag
érdekesebb (a "Kék" és "Piros" TFT ilyenkor tényleg két külön
idősíkot mutat egyidejűleg, nem csak egy globális kapcsolót).

---

## 4. A 2 TFT kijelző

### 4.1 Hardver / library
LovyanGFX ajánlott TFT_eSPI helyett az S3-on, mert DMA-s SPI átvitelt
és rugalmasabb pin-configot ad, és jobban kezeli a paralel/SPI
kijelző-variánsokat kézi configgal. Ha ST7789/ILI9341 a kijelződ
(a leggyakoribb 240×320-as modulok), mindkét lib megfelelő, de
LovyanGFX-nél a canvas-flip (tükrözés) trükk (lásd 4.3) egyszerűbb.

### 4.2 TFT #1 — Kék/Normál csapat
- Felső sáv: hálózat health (node-onkénti zöld/piros pötty + RSSI dBm)
- Középső: nagy, monoton **csökkenő** countdown (`60..59..58..`)
- Alsó: utolsó 3 DATA packet ID-je normál sorrendben, balról jobbra

### 4.3 TFT #2 — Piros/Inverz csapat
- A countdown **növekszik** (`0..1..2..`)
- A szöveg vizuálisan tükrözött — két opció:
  1. **Canvas-flip**: mindent egy `LGFX_Sprite`-ra rajzolsz, majd
     `pushRotateZoom(..., 180°)` vagy X-tengely mentén flip-elt
     `pushSprite`-tal írod ki — GPU-szerű trükk, de van pár ms
     overhead soronként.
  2. **Karakter-szintű reverse** (olcsóbb): a string-et megfordítod
     (`strrev`) és jobbról-balra pozícionálva írod ki, plusz invertált
     szín (fekete alapon vörös helyett vörös alapon fekete) — ez adja
     az "üveg másik oldaláról nézve" hatást, és semmivel sem drágább,
     mint egy normál `drawString`.
- Ajánlott: a 2-es opcióval indulj, látványos és egyszerű; a
  canvas-flip-et hagyd később, ha marad idő.

### 4.4 Frissítési logika
A TFT-k **nem** a saját lokális számlálójukat rajzolják ki
egyenesen — a `MSG_BOMB_STATE` csomagot a Gateway (vagy egy kijelölt
"master" node) küldi szét ~1 Hz-cel, hogy a két kijelző garantáltan
szinkronban maradjon még hálózati jitter esetén is. Lokális
interpolációval (a beérkezés között simán számolsz tovább) elkerülöd
a vizuális szaggatást.

---

## 5. Az IR Gate (Node 4 — "Turnstile")

### 5.1 IR_REMOTE (Node ID 2, önálló távirányító)
- Az IR_REMOTE külön, akkumulátoros ESP32-S3 eszköz, nem csatlakozik
  az ESP-NOW meshhez és nem továbbít packeteket.
- Egy nyomógomb megnyomásakor NEC kódot küld 940 nm-es IR LED-del.
  Az IR LED-et tranzisztor hajtsa meg; ne közvetlenül a GPIO-ról
  tápláld.
- A protokoll/kód legyen az IR_GATE-rel közös konfigurációs konstans.

### 5.2 IR_GATE (Node ID 4, csak IR-vevő)
- TSOP38238 vagy hasonló 38 kHz demodulátoros vevőmodul (VCC/GND/OUT).
- Az IR_GATE nem tartalmaz IR adót. A helyes távirányító-kód
  vételekor 8 másodperces cooldown után `MSG_INVERT_TIME` packetet
  broadcastol az ESP-NOW hálózaton.

### 5.3 Library és protokoll
**IRremoteESP8266** NEC protokollal használható mindkét eszközön.
Az adó a gombnyomásra küld, az IR_GATE ellenőrzi a kódot és a cooldown-t.

```c
#include <IRsend.h>
IRsend irsend(IR_TX_PIN);
```

Az IR_GATE vevőoldali példája:
```c
#include <IRrecv.h>
IRrecv irrecv(IR_RX_PIN);
decode_results results;

void loop() {
  if (irrecv.decode(&results)) {
    if (results.value == INVERT_CODE && millis() - lastTrigger >= COOLDOWN_MS) {
      broadcastInvertTime();
      lastTrigger = millis();
    }
    irrecv.resume();
  }
}
```

### 5.4 A broadcast tartalma
```c
void broadcastInvertTime() {
  tenet_packet_t p = {};
  p.magic = 0xAA;
  p.msg_type = MSG_INVERT_TIME;
  p.src_node = MY_ID;
  p.dst_node = 0xFF; // broadcast marker
  p.ttl = 4;
  p.time_dir = 0; // jelzésértékű, minden node maga XOR-olja a sajátját
  recalcChecksum(&p);
  esp_now_send(broadcast_mac, (uint8_t*)&p, sizeof(p));
}
```
Minden node ugyanazt a broadcastot veszi, de csak a saját
`zone_id`-jéhez tartozó node-ok hajtják végre a váltást (lásd 3.
szakasz) — ezt a zóna-táblát vagy fixen beégeted, vagy a broadcast
payload-jában küldesz egy zone-bitmaszkot, ha dinamikusan akarod
variálni játékonként.

---

## 6. Gateway node (Node 5 — mesh ↔ soros híd)

### 6.1 Soros beállítás
Az ESP32-S3 natív USB-CDC-je (`Serial` objektum, ha a board USB-OTG
módban van konfigurálva, nem a hagyományos UART-on keresztüli
USB-soros konverterrel) simán hozza a 2 Mbaud-ot extra hardver
nélkül. Arduino IDE-ben: **Tools → USB CDC On Boot: Enabled**.

```c
void setup() {
  Serial.begin(2000000); // 2 Mbaud
}
```

### 6.2 Mesh → PC irány
Minden beérkező ESP-NOW csomagot a Gateway egyszerű, sorvégjeles
JSON-né alakít (ember és C# `System.Text.Json` által is könnyen
kezelhető), és `Serial.println()`-nel köpi ki:

```json
{"src":2,"dst":5,"type":2,"id":142,"dir":1,"payload":"DATA_OK","rssi":-47}
```

### 6.3 PC → Mesh irány (hack-interfész visszacsatolás)
A C# app soros porton egy egysoros JSON parancsot küld (pl.
`{"cmd":"repair","target":1,"key":"0x4F2A"}`), a Gateway ezt
`MSG_REPAIR` ESP-NOW packetté alakítja és a célzott node-nak (vagy
broadcastban) elküldi.

### 6.4 Miért nem küldi mindenki közvetlenül a PC-t?
Mert csak egy node-nak van USB kábele a gyakorlatban (a "terepasztal"
elrendezésben a többi akkuról/powerbankről megy) — a Gateway node
tehát egyben a mesh egyik fizikai relay pontja is.

---

## 7. C# alkalmazás (WPF, .NET 8)

### 7.1 Projekt-struktúra
```
TenetControl/
├─ App.xaml
├─ MainWindow.xaml           # fő taktikai UI
├─ Services/
│  ├─ SerialService.cs       # háttérszálon olvas, esemény-alapú
│  ├─ MeshStateService.cs    # node-ok, RSSI-gráf, idősík-állapot
│  └─ PacketLog.cs           # ObservableCollection<PacketEntry>
├─ Models/
│  └─ PacketEntry.cs
└─ Views/
   ├─ MeshMapControl.xaml    # node-gráf rajzolás
   └─ HackConsole.xaml       # parancsbeviteli konzol
```

### 7.2 Soros vétel — ne blokkold a UI szálat
```csharp
public class SerialService {
    private SerialPort _port;
    public event Action<PacketEntry> PacketReceived;

    public void Start(string portName) {
        _port = new SerialPort(portName, 2_000_000);
        _port.Open();
        Task.Run(ReadLoop); // dedikált háttérszál, nem async/await a porton
    }

    private void ReadLoop() {
        while (_port.IsOpen) {
            try {
                string line = _port.ReadLine();
                var entry = JsonSerializer.Deserialize<PacketEntry>(line);
                PacketReceived?.Invoke(entry);
            } catch (TimeoutException) { /* várható, folytatjuk */ }
        }
    }
}
```
A UI-frissítést mindig `Dispatcher.Invoke`-on keresztül told vissza a
fő szálra, mert az `ObservableCollection` nem thread-safe.

### 7.3 Mesh térkép (neon sci-fi stílus)
- `Canvas` + kézzel pozícionált node-körök (6 fix pont, pl.
  hexagonális elrendezés), vagy ha később dinamikus RSSI-alapú
  layoutot akarsz, egy egyszerű force-directed graph (pl.
  `GraphSharp` NuGet csomag).
- Élek vastagsága/átlátszósága RSSI-vel arányos:
  `strokeThickness = Map(rssi, -90, -30, 1, 6)`.
- Színpaletta: háttér `#0A0E1A`, normál node `#00F0FF` (cián), inverz
  node `#FF2D55` (neon piros), paradox node villogó sárga `#FFD60A`.
- Glow-effekthez `DropShadowEffect` `BlurRadius=15-20` a node
  `Ellipse`-eken — olcsó és jól néz ki WPF-ben natívan, nem kell
  külön shader.

### 7.4 Packet sniffer / debugger
```csharp
if (currentTimeDirection == -1) {
    PacketLog.Insert(0, entry); // invertben: fentről lefelé "visszafelé görgetés"
} else {
    PacketLog.Add(entry); // normálban: lentről felfelé, ahogy jönnek
}
```
Virtualizált `ListView` (`VirtualizingStackPanel`) kell, ha a log
gyorsan nő, hogy ne akadjon a UI.

### 7.5 Hack-interfész
Egy konkrét, játszható rejtvény-ötlet: a `MSG_PARADOX` eseményben a
Gateway elküld egy XOR-kulccsal kódolt hibakódot
(`payload = original_id XOR secret_key`), amit a C# app **nem** fejt
vissza automatikusan — a képernyőn megjelenik a kódolt hex érték és
2-3 elárult XOR-kulcs-jelölt a "terepen" lévő TFT-ken (a Kék/Piros
kijelzőn külön-külön fél kulcs látszik). A játékosnak a két felet
össze kell raknia és be kell írnia a hack konzolba:

```csharp
private void OnRepairSubmit(int targetNode, string keyHex) {
    var cmd = new { cmd = "repair", target = targetNode, key = keyHex };
    _serial.Write(JsonSerializer.Serialize(cmd) + "\n");
}
```

---

## 8. Fejlesztési sorrend (javasolt mérföldkövek)

1. **Alap ESP-NOW kommunikáció** 2 node között (heartbeat, RSSI log
   soros monitoron) — igazold, hogy a rádiós réteg stabil.
2. **Packet struct + checksum** implementálása, unicast küldés-vétel
   teszt.
3. **Flood-routing** bekapcsolása 3+ node-dal, duplikátum-szűrés
   tesztelése.
4. **IR gate**: NEC dekódolás + `INVERT_TIME` broadcast, egyelőre csak
   soros logban látszódjon az állapotváltás.
5. **TFT-k** alap kiírása (statikus szöveg, majd countdown).
6. **Node állapotgép** + paradoxon-detektálás bekötése.
7. **Gateway ↔ C# soros protokoll**, JSON parse mindkét irányba.
8. **C# UI**: előbb csak nyers log, utána a mesh-térkép, legvégén a
   hack-konzol és a vizuális polish (neon, glow, animációk).

---

Ha kéred, tudok mellé csinálni egy konkrét pin-kiosztási táblát is
(melyik ESP32-S3 GPIO megy a TFT-hez SPI-n, melyik az IR-hez), vagy
egy induló Arduino IDE sketch-vázat a közös firmware-hez.
