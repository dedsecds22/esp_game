# TENET Mesh — Firmware & Game Specification

**Dokumentumtípus:** végleges fejlesztési cél specifikációja  
**Verzió:** 1.0  
**Projekt:** ESP32-S3 alapú, több eszközös kooperatív hálózati játék  
**Játékosok:** 2–4 fő  
**Kommunikáció:** ESP-NOW, saját alkalmazási protokollal  
**PC-s alkalmazás:** Python / Tkinter  
**Dokumentum státusza:** fejlesztési követelményrendszer — a funkciók megvalósítottságát külön kell ellenőrizni

---

# 1. A projekt célja

A TENET Mesh egy fizikai, kooperatív játék, amelyben a játékosok hat ESP32-S3 eszközből álló hálózatot üzemeltetnek, hibákat javítanak, csomagokat továbbítanak, és közösen teljesítenek időkorlátos küldetéseket.

A játék egyszerre szórakoztató és oktatási célú. A résztvevők gyakorlati tapasztalatot szereznek az elosztott rendszerek, a hálózati kommunikáció, a csomagkézbesítés, a hibakezelés és a megbízhatóság alapjaiban.

A rendszernek három fő része van:

1. **ESP32-S3 firmware:** kezeli a hardvert, a kommunikációt, a játékállapotokat és az eszközök közötti üzenetváltást.
2. **Fizikai kezelőfelület:** két TFT-kijelző és egy digitális HIGH/LOW kimenetű IR-vevő biztosítja a játék közbeni visszajelzést és interakciót.
3. **PC-s alkalmazás:** a játék indítását, konfigurálását, diagnosztikáját, csomagforgalmát, pontozását és eredményeit kezeli.

A fejlesztés elsődleges célja nem egy új rendszer nulláról történő megépítése, hanem a meglévő repository és a működő firmware-ek továbbfejlesztése egy egységes, tesztelhető és dokumentált játékká.

## 1.1. Alapelvek

- A játék 2–4 emberrel játszható, kooperatív módon.
- Minden játékos ugyanazon közös küldetés sikeréért dolgozik.
- A hat fizikai ESP32-S3 eszköz a játék tényleges működésének része.
- Az eszközök közötti kommunikáció ESP-NOW-on, saját alkalmazási protokollal történik.
- Az IR-vevő nem NEC-kódokat dekódol; a firmware a digitális jel időbeli viselkedését elemzi.
- A TFT-kijelzők valós, a firmware által ismert állapotokat mutatnak.
- A PC-s alkalmazás nem találhat ki eszközállapotokat: a kijelzett hálózati adatoknak a ténylegesen megkapott üzeneteken kell alapulniuk.
- A csomagküldési kísérlet nem azonos a sikeres kézbesítéssel.
- A pontozásnak és a győzelmi feltételeknek determinisztikusnak és tesztelhetőnek kell lenniük.
- A tesztmód és a játékmód egymástól elkülönül.
- A meglévő, működő funkciókat nem szabad indokolatlanul lecserélni.

---

# 2. A játék koncepciója

A játékosok egy instabillá vált hálózatot próbálnak helyreállítani. A rendszerben csomagvesztés, kommunikációs hiba, hibás node-állapotok és időkorlátos feladatok jelennek meg.

A csapatnak fel kell ismernie a hibákat, ki kell választania a megfelelő javítási műveleteket, és el kell juttatnia a küldetéshez tartozó adatcsomagokat a kijelölt célállomásra.

A játék alapmódja a **Network Recovery**.

## 2.1. A küldetés célja

A csapat akkor nyer, ha a küldetés időkorlátján belül:

1. kézbesíti az összes kötelező adatcsomagot;
2. helyreállítja az összes kötelező node-ot;
3. eléri a küldetés által előírt stabil hálózati állapotot;
4. a gateway megerősíti a küldetés sikeres befejezését.

A játékosok közös eredményt érnek el. Nincs egyéni kiesés.

## 2.2. Játékosszám és feladatmegosztás

### Két játékos

- **Terepi operátor:** figyeli a TFT-ket és használja az IR-távirányítót.
- **Hálózati operátor:** kezeli a PC-s alkalmazást, figyeli a csomagokat és a hálózat állapotát.

### Három játékos

A harmadik játékos a csomagforgalom és a hibák elemzéséért felel.

### Négy játékos

A negyedik játékos hálózati diagnosztikai és helyreállítási feladatokat kap.

A szerepek nem kötelezőek és nem kötődnek mereven a személyekhez. A játék minden esetben teljesíthető marad 2–4 fővel.

---

# 3. Hardverarchitektúra

A rendszer hat ESP32-S3 eszközből áll. A node-ok azonosítója a firmware-ben és a PC-s alkalmazásban egységesen használt numerikus ID.

| Node ID | Hardver szerepe | Elsődleges feladat |
|---|---|---|
| 0 | TFT-kijelzős node | Globális játékállapot, visszaszámlálás és hálózati összefoglaló |
| 1 | TFT-kijelzős node | Hibák, aktuális feladat és IR-visszajelzés |
| 2 | Önálló IR-vezérlő node | A játékos által kezdeményezett IR-interakciók kezelése, amennyiben ezt a jelenlegi hardveres kiosztás támogatja |
| 3 | Relay node | Üzenetek továbbítása a támogatott hálózati útvonalakon |
| 4 | IR-vevővel és mesh gateway-funkcióval rendelkező node | IR-jelbemenet kezelése és a kapcsolódó játékparancsok továbbítása |
| 5 | USB gateway | PC-kapcsolat, játékállapot-kezelés és a PC-s alkalmazás kiszolgálása |

A táblázat a kívánt logikai szerepeket határozza meg. A tényleges GPIO-kiosztás, a perifériák és a jelenlegi firmware-szerepek ellenőrzése szükséges a megvalósítás előtt.

## 3.1. Node-azonosítás

Minden node-nak egyedi, stabil azonosítóval kell rendelkeznie.

A node ID nem változhat véletlenszerűen egy újraindítás miatt. A PC-s alkalmazásnak azonosító alapján kell nyilvántartania az eszközöket.

Minden node legalább a következő információkat szolgáltassa:

- node ID;
- firmware-verzió;
- aktuális működési állapot;
- kommunikációs állapot;
- utolsó életjelzés időpontja;
- támogatott funkciók, amennyiben ezek lekérdezhetők.

## 3.2. Hardverhiba és játékhiba megkülönböztetése

A játék által generált hiba nem jelentheti automatikusan a fizikai eszköz meghibásodását.

A rendszer különböztesse meg:

- a játékbeli logikai hibát;
- a kommunikációs hibát;
- a fizikai periféria hibáját;
- a firmware belső hibáját;
- a PC-kapcsolat hibáját.

Ez lehetővé teszi, hogy a játékosok ne tévesszék össze a küldetés során keletkezett hibákat a valódi hardverproblémákkal.

---

# 4. Firmware-architektúra

A firmware-t modulárisan kell felépíteni. A játéklogikát, a hálózati kommunikációt, a kijelzőkezelést és az IR-mérést külön felelősségi körként kell kezelni.

A meglévő forráskód szerkezetét nem szükséges teljesen átalakítani, ha már biztosítja ezeket a funkciókat.

## 4.1. Elvárt logikai modulok

### A. Hardware Layer

Feladata:

- GPIO-k kezelése;
- TFT-perifériák inicializálása;
- IR-bemenet olvasása;
- egyéb, ténylegesen meglévő perifériák kezelése.

Ez a réteg nem tartalmazhat küldetéslogikát vagy pontozást.

### B. ESP-NOW Transport Layer

Feladata:

- az ESP-NOW inicializálása;
- üzenetek küldése és fogadása;
- a támogatott peer-ek kezelése;
- kommunikációs hibák jelzése;
- a küldési visszajelzések továbbítása a felsőbb réteg felé.

Az ESP-NOW szállítási visszajelzése nem feltétlenül bizonyítja, hogy a távoli alkalmazás feldolgozta az üzenetet. A logikai kézbesítést külön alkalmazási nyugtával kell igazolni.

### C. Application Protocol Layer

Feladata:

- a saját üzenetformátum kezelése;
- a protokollverzió ellenőrzése;
- a forrás- és célazonosító ellenőrzése;
- a csomagazonosítók kezelése;
- az alkalmazási nyugták feldolgozása;
- a duplikátumok felismerése;
- az érvénytelen üzenetek elutasítása.

### D. Node State Manager

Feladata:

- a node állapotának tárolása;
- az engedélyezett állapotváltások ellenőrzése;
- a hibák és javítások nyilvántartása;
- a kommunikációs állapot elkülönítése a játékállapottól.

### E. Game State Manager

Ahol a hardver és a firmware architektúrája ezt lehetővé teszi, a játék központi állapotát a gateway kezelje.

Feladata:

- a küldetés elindítása;
- a küldetés szüneteltetése és befejezése;
- a játékállapotok kezelése;
- a feladatok és hibák nyilvántartása;
- a győzelmi és vereségi feltételek ellenőrzése;
- a pontozáshoz szükséges események tárolása.

A többi node a saját helyi állapotáért felel, és hitelesített üzenetekben közli a változásokat a gatewayjel.

### F. IR Input Manager

Feladata:

- a digitális IR-jel olvasása;
- az aktív jel időtartamának mérése;
- az impulzuscsoportok felismerése;
- a zajos vagy érvénytelen jel elutasítása;
- a kalibrációs értékek alkalmazása;
- a felismert művelet továbbítása a játéklogikának.

### G. Display Manager

Feladata:

- a TFT-kijelzők tartalmának frissítése;
- a hálózati és játékállapotok megjelenítése;
- az aktuális hiba és feladat kijelzése;
- a képernyők közötti állapotváltás.

A kijelzők frissítése nem akadályozhatja a hálózati kommunikációt vagy az IR-jel mérését.

### H. Diagnostics and Logging

Feladata:

- belső események naplózása;
- kommunikációs hibák jelzése;
- perifériahibák jelentése;
- a PC-s alkalmazás számára továbbítható diagnosztikai adatok biztosítása.

A naplózás nem blokkolhatja tartósan a játékot.

---

# 5. ESP-NOW és az alkalmazási protokoll

## 5.1. Alapkövetelmények

A node-ok közötti kommunikáció ESP-NOW-on keresztül történik.

A saját protokoll célja, hogy az eszközök egyértelműen meg tudják határozni:

- ki küldte az üzenetet;
- kinek szól az üzenet;
- milyen típusú műveletet képvisel;
- melyik küldetéshez tartozik;
- melyik logikai csomagról van szó;
- sikeresen feldolgozta-e a címzett.

A protokoll nem támaszkodhat arra, hogy minden üzenet pontosan egyszer érkezik meg.

## 5.2. Kötelező üzenetkategóriák

A végleges protokoll legalább a következő logikai üzenetkategóriákat támogassa:

| Üzenetkategória | Feladat |
|---|---|
| `HELLO` | Node jelenlétének és képességeinek bejelentése |
| `HEARTBEAT` | Rendszeres életjelzés |
| `STATUS` | Aktuális node-állapot közlése |
| `COMMAND` | Művelet kezdeményezése |
| `COMMAND_RESULT` | Parancs végrehajtásának eredménye |
| `PACKET_DATA` | Játékbeli adatcsomag továbbítása |
| `ACK` | Alkalmazási szintű visszaigazolás |
| `ERROR` | Hiba közlése |
| `GAME_EVENT` | Játékbeli esemény jelentése |
| `CONFIG` | Engedélyezett konfiguráció átadása |
| `DIAGNOSTIC` | Teszt- és diagnosztikai eredmény |
| `PING` / `PONG` | Kapcsolat ellenőrzése |

Ezek logikai üzenettípusok. A tényleges enumnevek és a bináris kódolás a jelenlegi firmware-hez igazítható.

## 5.3. Kötelező üzenetmezők

A protokoll minden üzenete tartalmazzon vagy egyértelműen kapcsoljon hozzá legalább:

- protokollverziót;
- üzenettípust;
- forrásnode-ot;
- célnode-ot vagy célcsoportot;
- üzenetazonosítót;
- munkamenet-azonosítót, ha az üzenet egy küldetéshez tartozik;
- payloadhosszt;
- a payloadot;
- integritásellenőrzési információt, amennyiben azt az alkalmazási formátum előírja.

A csomagformátumnak bele kell férnie az adott ESP-NOW üzenetméret-korlátjába. A túlméretes adatokat csak külön, dokumentált darabolási mechanizmussal szabad kezelni.

## 5.4. Azonosítók

Az üzenetazonosító az egyes protokollüzeneteket különbözteti meg.

A logikai játékcsomag-azonosító a csomagkézbesítési feladatot azonosítja.

Ez a két azonosító nem feltétlenül ugyanaz:

- egyetlen logikai csomag több küldési kísérletet eredményezhet;
- minden küldési kísérlet külön üzenetazonosítót kaphat;
- az összes kísérlet ugyanahhoz a logikai csomagazonosítóhoz tartozik.

## 5.5. Alkalmazási nyugták

Az `ACK` azt jelenti, hogy a címzett az üzenetet az alkalmazási protokoll szintjén fogadta vagy feldolgozta, a nyugta típusától függően.

A küldő nem tekintheti a logikai adatcsomagot kézbesítettnek pusztán azért, mert az ESP-NOW küldési callback sikeres továbbítási eredményt jelzett.

A végleges implementációban külön kell kezelni:

- rádiós küldési eredmény;
- alkalmazási vétel;
- érvényes feldolgozás;
- logikai csomagkézbesítés.

## 5.6. Újraküldés és duplikátumok

Ha egy szükséges alkalmazási nyugta nem érkezik meg a konfigurált időkorláton belül, a küldő újraküldheti az üzenetet.

Az újraküldés:

- korlátozott számú kísérletet használjon;
- dokumentált időkorlátok szerint működjön;
- naplózza az újraküldések számát;
- ne eredményezzen többszörös pontozást.

A fogadó azonosítsa a már feldolgozott logikai csomagokat. Az ismételt kézbesítési kísérlet ne hajtsa végre kétszer ugyanazt a nem idempotens műveletet.

## 5.7. Relay működése

A relay csak azokat az üzeneteket továbbítsa, amelyekre a protokoll szerint jogosult.

A továbbítás során:

- ne módosítsa a logikai csomag tartalmát;
- ne változtassa meg a forrásazonosítót;
- ne hozzon létre új logikai csomagot;
- akadályozza meg a végtelen továbbítási hurkokat;
- alkalmazza a protokoll által meghatározott duplikátum- és hop-kezelést.

A relay által továbbított üzenet eredeti forrása és végső célja maradjon visszakövethető.

## 5.8. Biztonságos parancskezelés

A node csak az érvényes, ismert és az adott célra engedélyezett parancsokat hajtsa végre.

A hibás vagy ismeretlen üzenet:

- ne módosítsa a játékállapotot;
- ne okozzon váratlan újraindítást;
- adjon visszajelzést, ha ez biztonságosan megtehető;
- kerüljön a diagnosztikai naplóba.

---

# 6. A node-ok állapotgépe

A firmware-ben a kommunikációs állapotot és a játékbeli állapotot külön kell kezelni.

## 6.1. Kommunikációs állapotok

- `INITIALIZING`
- `DISCOVERING`
- `ONLINE`
- `DEGRADED`
- `OFFLINE`
- `ERROR`

A pontos átmeneteket időkorlátok és tényleges kommunikációs eredmények határozzák meg.

Az `ONLINE` állapothoz friss, érvényes kommunikációs bizonyíték szükséges. Az `OFFLINE` állapot nem jelent automatikusan játékbeli hibát.

## 6.2. Játékbeli állapotok

- `NORMAL`
- `INVERSE`
- `PARADOX`
- `REPAIRING`

A játékbeli állapotot kizárólag engedélyezett, naplózott esemény módosíthatja.

### NORMAL

A node ellátja a küldetésben előírt feladatát.

### INVERSE

A node működése a játék által előidézett logikai hibában korlátozott. A node továbbra is kommunikálhat, de az érintett műveletek hibás eredményt adhatnak.

### PARADOX

Kritikus játékbeli hiba. A node nem képes ellátni a küldetésben előírt normál feladatát.

### REPAIRING

A javítás folyamatban van. Ez önmagában még nem jelenti a sikeres helyreállítást.

A helyreállítás csak a javítás eredményének megerősítése után történhet meg.

## 6.3. Állapotváltási szabályok

- A `NORMAL` állapotból hiba csak játékesemény vagy hitelesített diagnosztikai esemény alapján keletkezhet.
- A `REPAIRING` állapotból sikeres javítás után `NORMAL` állapot következhet.
- Sikertelen javítás esetén a korábbi hibaállapot marad érvényben.
- Az `OFFLINE` kommunikációs állapot nem törölheti a játékbeli állapotot.
- Újraindítás után a node nem jelentheti automatikusan sikeresnek a korábban megkezdett javítást.
- A gateway a hiányzó vagy ellentmondó állapotadatokat bizonytalanként kezelje, ne sikeres állapotként.

---

# 7. IR-vezérlés — kizárólag HIGH/LOW időmérés

## 7.1. Hardveres korlát

Az IR-vevő a firmware számára digitális HIGH/LOW állapotot biztosít. A játék nem függ NEC-, RC5- vagy más távirányító-protokoll dekódolásától.

A játékos bármilyen kompatibilis infravörös távirányítót használhat, beleértve egy hétköznapi televízió-távirányítót is, ha annak jeleit a meglévő IR-vevő megfelelően érzékeli.

A játék nem a megnyomott gomb azonosítóját vizsgálja. A vezérlés alapja a mért aktív jelminta időtartama.

## 7.2. Jelszint normalizálása

A firmware-ben külön konfigurálható legyen, hogy a fizikai bemenet HIGH vagy LOW szintje számít aktív IR-jelnek.

A konfiguráció alapértelmezett értékét a tényleges szenzor és a jelenlegi firmware ellenőrzése után kell meghatározni.

A további feldolgozás egy egységes logikai jelet használjon:

- `ACTIVE`: a vevő aktív infravörös jelet érzékel;
- `INACTIVE`: nincs aktív jel.

A dokumentum további része az aktív jelmintát nevezi röviden „HIGH időnek”, de a firmware-ben az aktív állapotot a fizikai jelszinttől függetlenül kell értelmezni.

## 7.3. Impulzuscsoportok kezelése

A hagyományos távirányítók gyakran rövid impulzusok és szünetek sorozatával működnek. Ezért egy gombnyomás nem feltétlenül jelent folyamatosan aktív jelet.

A firmware-nek a következő folyamatot kell megvalósítania:

1. Figyeli a digitális bemenetet.
2. Megméri az aktív szakaszok időtartamát.
3. Az egymáshoz közel eső impulzusokat egyetlen jelcsoportba rendezi.
4. Összegzi a jelcsoport aktív időtartamait.
5. A megfelelő szünet vagy maximális mérési idő után lezárja a jelcsoportot.
6. Az összesített aktív idő alapján műveletet választ ki.
7. A felismert műveletet egyszer továbbítja a játéklogikának.

Az aktív idő összegzése nem azonos a teljes eltelt idővel. A szünetek nem számítanak bele az aktív időtartamba.

## 7.4. IR-parancskategóriák

A kezdeti küszöbértékek:

| Kategória | Összesített aktív idő | Tervezett művelet |
|---|---:|---|
| Rövid | 0,10–0,40 s | Állapotlekérdezés vagy megerősítés |
| Közepes | 0,50–1,20 s | Javítási művelet kezdeményezése |
| Hosszú | 1,30–2,50 s | Kritikus művelet vagy speciális mód |
| Érvénytelen | A tartományokon kívül | Nincs művelet |

A fenti küszöbök kezdeti tervezési értékek. Az IR Calibration felület segítségével mérések alapján módosíthatók.

A tartományok közötti hézagok szándékosak. Az ezekbe eső mérések ne indítsanak el automatikusan műveletet.

A túl rövid, túl hosszú, zajos vagy bizonytalan jelcsoportot a firmware utasítsa el.

## 7.5. Egy jelcsoportból egy művelet

Egy IR-jelcsoport legfeljebb egy játékparancsot eredményezhet.

A firmware nem ismételheti meg automatikusan a javítást csak azért, mert a távirányító ismétlődő impulzusokat küldött.

A folyamatos vagy ismétlődő jelek kezeléséhez cooldown és jelcsoport-lezárási szabály szükséges.

A művelet csak akkor tekinthető sikeresnek, ha a játéklogika elfogadta, és az eredmény visszajelzése megtörtént.

## 7.6. IR-parancsok és játékállapot

A rövid, közepes és hosszú kategória csak a parancs jellegét határozza meg. A konkrét művelet a játék aktuális állapotától függhet.

Például a közepes jel a hibás node javítását kezdeményezheti, de csak akkor, ha:

- a küldetés fut;
- van javítható hiba;
- a node a javítás célpontja;
- a parancs az adott helyzetben engedélyezett;
- a játék nincs szüneteltetve vagy lezárva.

A játék nem hajthat végre érvénytelen vagy lehetetlen műveletet.

## 7.7. IR-kalibráció

A PC-s alkalmazásnak külön IR Calibration felületet kell biztosítania.

A felület jelenítse meg:

- a nyers fizikai jelszintet;
- a normalizált aktív/inaktív állapotot;
- az aktív szakaszok időtartamát;
- a szünetek időtartamát;
- a csoportosított aktív időt;
- a felismert parancskategóriát;
- az elutasítás okát.

A felhasználó módosíthassa:

- a bemeneti polaritást;
- az impulzusok összefogásának szünetküszöbét;
- a rövid, közepes és hosszú tartományokat;
- a minimális érvényes jelmintát;
- a maximális mérési időt;
- a cooldown értékét.

A beállításokat a firmware ellenőrizze. Érvénytelen vagy egymást átfedő tartományok nem menthetők.

A konfiguráció alkalmazása előtt a PC-s alkalmazás kérjen megerősítést, és jelenítse meg az új értékeket.

## 7.8. Időmérés

Az IR-jel időtartamát monoton, a rendszeridő-változástól független időforrással kell mérni.

A mérés nem blokkolhatja az ESP-NOW kommunikációt, a TFT-frissítést vagy más kritikus feladatokat.

A megvalósításban kerülni kell a hosszú, blokkoló várakozásokat és a késleltetésalapú, pontatlan mérést.

---

# 8. A TFT-kijelzők követelményei

A két TFT-kijelző külön feladatot lát el.

A kijelzők a valós játékállapotot jelenítik meg, és nem generálnak önálló, a gatewayjel ellentmondó játékállapotokat.

## 8.1. TFT 0 — globális állapot

A TFT 0 a játék központi állapotát mutatja.

### Készenléti képernyő

- `TENET MESH`
- `INITIALIZING`, `CONNECTING` vagy `READY`
- elérhető node-ok száma
- gateway állapota
- rövid diagnosztikai üzenet, ha szükséges

### Játék közben

- küldetés neve;
- hátralévő idő;
- elérhető kötelező node-ok száma;
- sikeresen kézbesített csomagok száma;
- szükséges csomagok teljes száma;
- aktív hibák száma;
- küldetés állapota.

### Példa

```text
TENET MESH
NETWORK RECOVERY

TIME  03:42
NODES 5/5
PACKETS 04/08
ERRORS 02

MISSION ACTIVE
```

A példa szemléltető jellegű. A valós adatok a gatewaytől érkeznek.

### Kör vége

A TFT 0 jelenítse meg:

- `MISSION COMPLETE` vagy `MISSION FAILED`;
- a végső csomagkézbesítési eredményt;
- a pontszámot, ha elérhető;
- a következő körre vonatkozó rövid üzenetet.

## 8.2. TFT 1 — feladat és hibakezelés

A TFT 1 a játékos aktuális feladatát mutatja.

### Hiba nélkül

- aktuális feladat;
- utolsó jelentős esemény;
- a következő művelet rövid leírása.

### Hiba esetén

- érintett node ID;
- hibatípus;
- hiba súlyossága;
- szükséges művelet;
- IR-parancs állapota.

### IR-mérés közben

- `IR SIGNAL DETECTED`;
- mért aktív idő;
- aktuális jelcsoport állapota;
- felismert kategória, ha a mérés lezárult;
- elutasítási üzenet, ha a jel érvénytelen.

### Javítás közben

- `REPAIR IN PROGRESS`;
- javítás célpontja;
- parancs állapota.

### Javítás után

- `NODE RESTORED`, ha a gateway visszaigazolta a sikeres helyreállítást;
- `REPAIR FAILED`, ha a művelet sikertelen volt;
- rövid útmutatás a következő lépéshez.

## 8.3. Közös kijelzőállapotok

A két TFT-nek támogatnia kell a következő logikai állapotokat:

- inicializálás;
- hálózatkeresés;
- készenlét;
- küldetés indítása;
- aktív játék;
- hiba;
- javítás;
- csomagkézbesítés;
- kommunikációs hiba;
- szüneteltetés;
- győzelem;
- vereség.

A két kijelző információi kiegészítik egymást. Nem szükséges minden adatot mindkettőn megjeleníteni.

## 8.4. Kijelzőfrissítés

A kijelzőkezelés:

- ne akadályozza a kommunikációt;
- ne blokkolja az IR-mérést;
- ne okozzon indokolatlan villogást;
- lehetőség szerint csak a megváltozott részeket frissítse;
- hibás vagy hiányzó adat esetén jelezze az ismeretlen állapotot.

A TFT-k nem mutathatnak sikeres kézbesítést vagy javítást addig, amíg azt a játékállapot igazolja.

---

# 9. A játék állapotgépe

A játék állapotát a gatewaynek kell kezelnie, amennyiben a jelenlegi architektúra ezt támogatja.

## 9.1. Állapotok

- `BOOT`
- `DISCOVERY`
- `READY`
- `COUNTDOWN`
- `RUNNING`
- `PAUSED`
- `MISSION_SUCCESS`
- `MISSION_FAILED`
- `DIAGNOSTIC`

## 9.2. Átmenetek

### BOOT → DISCOVERY

A gateway és a firmware inicializálása befejeződött.

### DISCOVERY → READY

Minden kötelező node elérhető, és a rendszer készen áll a küldetésre.

### READY → COUNTDOWN

A játékos vagy a játékmester elindítja a küldetést.

### COUNTDOWN → RUNNING

A visszaszámlálás lejárt, a küldetés ténylegesen elkezdődött.

### RUNNING → PAUSED

A játék szüneteltetése megtörtént.

### PAUSED → RUNNING

A játék folytatódik.

### RUNNING → MISSION_SUCCESS

Minden kötelező győzelmi feltétel teljesült.

### RUNNING → MISSION_FAILED

A küldetés nem teljesíthető, lejárt az idő, vagy a játékos megszakította a küldetést.

### Bármely biztonságosan megszakítható állapot → DIAGNOSTIC

A rendszer diagnosztikai módba kerül, amennyiben a jelenlegi hardveres és firmware-architektúra ezt biztonságosan támogatja.

## 9.3. Állapotváltások hitelessége

A játékállapotot csak engedélyezett parancs vagy hitelesített belső esemény módosíthatja.

A TFT-k és a PC-s alkalmazás csak megjelenítik a játékállapotot, illetve engedélyezett parancsokat kezdeményeznek.

A PC-s alkalmazás nem állíthatja sikeresnek a küldetést pusztán a helyi felület alapján.

---

# 10. A Network Recovery játékmenet

## 10.1. Előkészítés

A játékosok bekapcsolják a hat node-ot, csatlakoztatják a PC-t, és megvárják a hálózat felépülését.

A PC-s alkalmazás ellenőrzi:

- az USB gateway elérhetőségét;
- a node-ok jelenlétét;
- a firmware-verziókat;
- a kötelező perifériákat;
- az IR-bemenet diagnosztikai állapotát;
- az ESP-NOW kommunikációt.

A játék csak akkor indulhat el, ha a kötelező komponensek készen állnak.

## 10.2. Küldetés konfigurálása

A játékmester kiválasztja:

- a nehézségi szintet;
- a küldetési időt;
- a kézbesítendő csomagok számát;
- az engedélyezett hibák típusait;
- a hibák intenzitását.

A konfigurációt a rendszer ellenőrzi, és a küldetés kezdetén rögzíti.

## 10.3. Hibagenerálás

A játék során a rendszer meghatározott időközönként hibákat generálhat.

Lehetséges hibák:

- `INVERSE` node;
- `PARADOX` node;
- csomagvesztés;
- késleltetés;
- kommunikációs időtúllépés;
- ismételt csomag;
- hibás logikai payload;
- átmeneti kapcsolatvesztés.

A hibagenerálás kizárólag játékbeli eseményeket idézhet elő. Nem szabad szándékosan fizikai hardverhibát okoznia vagy a firmware-t újraindítania.

A játék nem generálhat olyan hibát, amely a szabályok alapján helyrehozhatatlanná teszi a küldetést, kivéve, ha az adott nehézségi szint ezt kifejezetten előírja.

## 10.4. Diagnosztika és javítás

A játékosok a kijelzők és a PC-s alkalmazás alapján megkeresik a hibát.

A javítási folyamat:

1. Hiba észlelése.
2. A hibás node vagy csomag azonosítása.
3. Megfelelő művelet kiválasztása.
4. Parancs elküldése.
5. Parancs fogadásának visszaigazolása.
6. Művelet végrehajtása.
7. A javítás eredményének ellenőrzése.
8. A játékállapot frissítése.

Sikertelen javítás esetén a hibaállapot megmarad.

## 10.5. Csomagkézbesítés

A küldetéshez tartozó logikai csomagokat a meghatározott célállomásra kell továbbítani.

A kézbesítés sikeres, ha:

- a csomagazonosító megfelel a küldetésben előírt értéknek;
- a fogadó érvényesnek találja a csomagot;
- a fogadó alkalmazási szintű nyugtát ad;
- a gateway rögzíti a kézbesítést.

A csomagküldési kísérlet nem növelheti automatikusan a kézbesített csomagok számát.

## 10.6. A küldetés lezárása

A küldetés sikeres, ha minden előírt feltétel teljesült a határidőn belül.

A küldetés sikertelen, ha lejárt az idő, a kritikus feltételek már nem teljesíthetők, vagy a játékosok megszakították a játékot.

A lezárás után az eredményeket meg kell őrizni a PC-s alkalmazás számára.

---

# 11. Nehézségi szintek

A kezdeti nehézségi szintek:

| Paraméter | Easy | Normal | Hard |
|---|---:|---:|---:|
| Küldetési idő | 300 s | 240 s | 180 s |
| Tervezett hibaintervallum | 20 s | 12 s | 6 s |
| Szükséges csomagok | 5 | 8 | 12 |
| Újraküldési lehetőségek | Bőséges | Korlátozott | Korlátozott |
| Hibák intenzitása | Alacsony | Közepes | Magas |

Ezek kezdeti célértékek. A tényleges firmware-paramétereket játékpróbák alapján kell véglegesíteni.

A játék konfigurációjában az időket és a darabszámokat központilag kell kezelni, hogy a firmware és a PC-s alkalmazás ne használjon egymásnak ellentmondó értékeket.

---

# 12. Pontozási rendszer

A maximális eredmény 100 pont.

| Kategória | Maximális pont |
|---|---:|
| Csomagkézbesítés | 40 |
| Hálózati stabilitás | 30 |
| Hibakezelés | 20 |
| Időbeli teljesítmény | 10 |

A pontozás kizárólag a gateway által rögzített eseményekre támaszkodhat.

## 12.1. Csomagkézbesítés

A pontok arányosan járnak a sikeresen kézbesített logikai csomagok után.

Az ismételt küldési kísérlet nem számít új kézbesítésnek.

## 12.2. Hálózati stabilitás

A pontok a kötelező node-ok végső állapotától és elérhetőségétől függnek.

## 12.3. Hibakezelés

A pontozás figyelembe veszi a sikeres javításokat, a hibás műveleteket és a felesleges újrapróbálkozásokat.

## 12.4. Időbeli teljesítmény

A pontok a küldetés sikeres és időben történő teljesítését értékelik.

A pontozás részletes képleteit a kód implementálása előtt külön, determinisztikus szabályként kell rögzíteni.

A pontszám nem írhatja felül a győzelmi feltételeket: 100 pont sem jelent teljes győzelmet, ha a kötelező feladatok nem teljesültek.

---

# 13. Hibakezelés és helyreállítás

## 13.1. Node offline

Ha a node nem ad életjelet a konfigurált időkorláton belül, a gateway `OFFLINE` állapotot rögzít.

A node csak a kommunikáció helyreállítása és ellenőrzése után tekinthető újra elérhetőnek.

## 13.2. Csomagvesztés

A csomagvesztés nem jelenti automatikusan a küldetés végét.

A protokoll a beállított újraküldési szabályok szerint próbálkozhat újra.

## 13.3. Duplikátum

A duplikált logikai csomag nem számít új kézbesítésnek, és nem hajthat végre ismét egy már végrehajtott, nem idempotens műveletet.

## 13.4. Érvénytelen parancs

Az érvénytelen parancs nem változtathatja meg a játékállapotot.

A PC-s alkalmazásban és a TFT-n megfelelő hibaüzenet jelenjen meg, ha az adott felület ezt támogatja.

## 13.5. PC-kapcsolat elvesztése

Ha a PC és az USB gateway közötti kapcsolat megszakad:

- a játék ne rögzítsen igazolatlan kézbesítéseket;
- a gateway jelezze a kapcsolatvesztést;
- a PC-s alkalmazás jelezze, hogy az adatok elavultak;
- a rendszer a meghatározott szabályok szerint szüneteltesse vagy zárja le a küldetést.

A játékállapot nem állítható vissza észrevétlenül a PC-s alkalmazás újracsatlakozásakor.

## 13.6. Újraindítás

Az újraindítás nem számíthat sikeres javításnak.

A gatewaynek lehetőség szerint meg kell őriznie a küldetésnaplót és az addig rögzített eredményeket.

A firmware nem állíthatja automatikusan vissza a korábbi játékállapotot olyan módon, amely duplikált kézbesítést vagy hibás pontozást eredményez.

## 13.7. Ismeretlen állapot

Ha a gateway nem tudja igazolni egy node aktuális állapotát, azt ismeretlennek vagy elavultnak kell tekinteni.

Az ismeretlen állapot nem számítható normál állapotnak a győzelem ellenőrzésekor.

---

# 14. PC-s alkalmazás — funkcionális specifikáció

A PC-s alkalmazás Python és Tkinter használatával készül. A meglévő alkalmazást tovább kell fejleszteni, nem szükséges új GUI-technológiára áttérni.

A felületnek egyszerűnek, áttekinthetőnek és diagnosztikára alkalmasnak kell lennie.

## 14.1. Dashboard / Lobby

A főoldal mutassa:

- a hat node állapotát;
- az USB gateway kapcsolatát;
- a firmware-verziókat;
- a legutóbbi életjelek idejét;
- az IR-bemenet állapotát;
- a hardvertesztek eredményét.

Interaktív elemek:

- `Scan / Refresh`;
- `Run Hardware Test`;
- `Calibrate IR`;
- `Start Mission`;
- `Open Diagnostics`.

A `Start Mission` csak akkor engedélyezett, ha minden kötelező komponens készen áll.

## 14.2. Élő hálózati térkép

A felület jelenítse meg a node-okat és a közöttük dokumentált kommunikációs kapcsolatokat.

Minden node-nál látható legyen:

- ID;
- kommunikációs állapot;
- játékbeli állapot;
- utolsó életjelzés;
- hibajelzés.

A kapcsolatok megjelenítése nem jelentheti azt, hogy minden ESP-NOW peer közvetlenül elérhető minden más peer számára. Csak ténylegesen ismert vagy konfigurált kapcsolatot szabad aktívként mutatni.

Interakciók:

- node kiválasztása;
- részletes állapot megnyitása;
- engedélyezett diagnosztikai parancs indítása;
- releváns események megjelenítése.

## 14.3. Packet Monitor

A csomagmonitor a játék hálózati eseményeit követi.

Kötelező mezők:

- időbélyeg;
- üzenetazonosító;
- logikai csomagazonosító, ha van;
- forrásnode;
- célnode;
- üzenettípus;
- státusz;
- újraküldések száma;
- késleltetés, ha mérhető.

A státuszok között szerepeljen:

- `SENT`;
- `ACK`;
- `LOST`;
- `RETRY`;
- `INVALID`;
- `DUPLICATE`;
- `UNKNOWN`.

Interakciók:

- szűrés forrás és cél szerint;
- szűrés üzenettípus szerint;
- szűrés státusz szerint;
- csomag részleteinek megtekintése;
- napló exportálása.

A monitorban a `SENT` és az `ACK` nem használható felcserélhető jelentéssel.

## 14.4. Mission Control

A Mission Control felület biztosítsa:

- a nehézségi szint kiválasztását;
- a küldetési idő beállítását;
- a csomagszám beállítását;
- a hibák típusának és intenzitásának kiválasztását;
- a küldetés indítását;
- a szüneteltetést és folytatást;
- a küldetés megszakítását;
- új kör indítását.

A futó küldetés konfigurációja nem változhat meg észrevétlenül.

A kritikus műveletek, például a megszakítás és az újraindítás, kérjenek megerősítést.

## 14.5. IR Calibration

Az IR-kalibrációs modul támogassa:

- a fizikai HIGH/LOW bemenet megjelenítését;
- a bemeneti polaritás beállítását;
- az aktív szakaszok időmérését;
- a szünetek mérését;
- az impulzusok csoportosítását;
- az összesített aktív idő kijelzését;
- a parancskategória felismerését;
- a küszöbértékek szerkesztését;
- a cooldown beállítását;
- a konfiguráció tesztelését és mentését.

A modulnak egyértelműen jeleznie kell, hogy a távirányító jele:

- nem érkezett meg;
- aktív;
- mérés alatt van;
- felismert parancsot eredményezett;
- érvénytelen vagy bizonytalan volt.

A PC-s alkalmazás ne állítsa, hogy egy IR-művelet sikeres volt, amíg a firmware nem igazolta annak eredményét.

## 14.6. Node Diagnostics

A node diagnosztikai oldalán látható legyen:

- node ID;
- firmware-verzió;
- aktuális kommunikációs állapot;
- játékbeli állapot;
- utolsó életjelzés;
- utolsó hiba;
- támogatott parancsok;
- elérhető tesztek.

A felület csak a firmware által ténylegesen támogatott parancsokat engedélyezze.

## 14.7. Eredmények és naplózás

A kör lezárásakor jelenjen meg:

- küldetés eredménye;
- teljes idő;
- sikeres kézbesítések;
- sikertelen csomagok;
- újraküldések;
- hibás node-ok;
- sikeres javítások;
- végső pontszám;
- események időrendje.

A napló exportálható legyen JSON és CSV formátumban.

A naplónak tartalmaznia kell a munkamenet azonosítóját és a protokollverziót, hogy később is elemezhető legyen.

## 14.8. UI-állapot és adatfrissítés

A PC-s alkalmazás:

- külön kezelje a kommunikációs szálat és a GUI-t;
- ne blokkolja a Tkinter eseménykezelését;
- kezelje az USB-kapcsolat megszakadását;
- jelezze az elavult adatokat;
- ne állítson elő nem igazolt játékállapotokat;
- őrizze meg a már beérkezett események időrendjét.

---

# 15. Tesztelési követelmények

A rendszer csak akkor tekinthető elkészültnek, ha a működését a fizikai eszközökön és a PC-s alkalmazásban is ellenőriztük.

## 15.1. Indítási tesztek

- Mind a hat node inicializálható.
- Minden node a megfelelő ID-val jelentkezik be.
- A gateway felismeri az elérhető eszközöket.
- A PC-s alkalmazás megjeleníti a tényleges állapotokat.
- A TFT-k készenléti állapotba kerülnek.

## 15.2. Kommunikációs tesztek

- A node-ok érvényes üzeneteket küldenek és fogadnak.
- Az alkalmazási nyugták helyesen működnek.
- A sikertelen kézbesítés felismerhető.
- Az újraküldés működik.
- A duplikátumok nem eredményeznek többszörös műveletet.
- A relay nem hoz létre továbbítási hurkot.
- Az offline node állapota helyesen jelenik meg.

## 15.3. IR-tesztek

- A bemeneti polaritás helyesen konfigurálható.
- A rövid impulzusok felismerhetők.
- Az impulzuscsoportok összesített aktív ideje helyesen mérhető.
- A szünetek nem növelik az aktív időt.
- A rövid, közepes és hosszú kategóriák a konfigurált küszöbök szerint működnek.
- Az érvénytelen jelek nem indítanak el parancsot.
- Egy jelcsoportból legfeljebb egy művelet keletkezik.
- A cooldown megakadályozza a nem kívánt ismétlést.
- A mérés nem akadályozza a kommunikációt.

## 15.4. TFT-tesztek

- Minden meghatározott játékállapot megjeleníthető.
- A két kijelző szerepe elkülönül.
- A hiba és a javítás eredménye helyesen jelenik meg.
- A kijelzők nem jeleznek sikeres javítást igazolás nélkül.
- A frissítés nem blokkolja a firmware-t.

## 15.5. PC-alkalmazás tesztjei

- A Lobby megjeleníti az összes node állapotát.
- A Mission Control helyesen konfigurálja a küldetést.
- A Packet Monitor helyesen különbözteti meg a küldést és a kézbesítést.
- Az IR Calibration valós méréseket mutat.
- A Node Diagnostics csak támogatott műveleteket engedélyez.
- A napló exportálása működik.
- A kapcsolat megszakadása felismerhető.
- A GUI nem fagy le a kommunikáció közben.

## 15.6. Teljes játékmenet-teszt

Legalább egy teljes, kétfős és egy teljes, négyfős tesztkört végig kell játszani.

A teszt igazolja:

1. a játék előkészítését;
2. a küldetés indítását;
3. a hibák felismerését;
4. az IR-műveletek feldolgozását;
5. a javításokat;
6. a csomagkézbesítést;
7. a pontozást;
8. a győzelmi és vereségi feltételeket;
9. az eredményképernyőt;
10. a napló exportálását.

---

# 16. Fejlesztési prioritások

A megvalósítás a következő sorrendben történjen.

## P0 — Meglévő működés megőrzése

- A jelenlegi firmware-ek és node-szerepek feltérképezése.
- A működő ESP-NOW kommunikáció megőrzése.
- A PC-s alkalmazás meglévő tesztjeinek megőrzése.
- A tényleges parancsok és üzenettípusok dokumentálása.

**Elfogadási feltétel:** a jelenlegi hardvertesztek továbbra is sikeresen lefutnak.

## P1 — Egységes állapotmodell

- Node-állapotok egységesítése.
- Kommunikációs és játékállapot elkülönítése.
- Gateway állapotkezelésének tisztázása.
- Kijelzőállapotok definiálása.

**Elfogadási feltétel:** a PC-s alkalmazás és a TFT-k ugyanazt a hiteles játékállapotot mutatják.

## P2 — IR-mérés és kalibráció

- Aktív jelszint kezelése.
- Impulzusok csoportosítása.
- Aktív idő mérése.
- Parancskategóriák.
- Cooldown.
- IR-diagnosztika a PC-s alkalmazásban.

**Elfogadási feltétel:** többféle kompatibilis TV-távirányítóval stabilan felismerhetők a beállított időtartam-kategóriák.

## P3 — Teljes Network Recovery játékmód

- Küldetés indítása és lezárása.
- Hibagenerálás.
- Javítási folyamat.
- Csomagkézbesítés.
- Győzelem és vereség.
- Determinisztikus pontozás.

**Elfogadási feltétel:** egy teljes játékkör a fizikai eszközökön végigjátszható.

## P4 — PC-s felület véglegesítése

- Dashboard.
- Élő hálózati térkép.
- Packet Monitor.
- Mission Control.
- IR Calibration.
- Node Diagnostics.
- Eredmények és naplóexport.

**Elfogadási feltétel:** a játékmester a szükséges műveleteket egyértelműen, a konzol használata nélkül el tudja végezni.

## P5 — Játékegyensúly és dokumentáció

- Nehézségi szintek finomhangolása.
- Játékidő és hibaintervallumok tesztelése.
- 2–4 játékosra optimalizálás.
- Végleges játékszabályzat.
- Firmware- és protokolldokumentáció.

**Elfogadási feltétel:** a játék újraindítható, ismételhető és a dokumentáció alapján megtanulható.

---

# 17. A firmware végleges elfogadási feltételei

A TENET Mesh akkor tekinthető az 1.0-s célállapotot elérő rendszernek, ha:

- mind a hat node megbízhatóan inicializálható;
- a node-ok közötti ESP-NOW kommunikáció stabilan működik;
- a saját protokoll képes az üzenetek azonosítására és az alkalmazási nyugták kezelésére;
- a csomagvesztés és az újraküldés helyesen jelenik meg;
- az IR-vevő digitális jeléből stabil, időalapú parancsok állíthatók elő;
- mindkét TFT a meghatározott szerepének megfelelő információkat mutatja;
- a PC-s alkalmazás a valós eszközállapotokat jeleníti meg;
- a teljes Network Recovery játékmód végigjátszható;
- a győzelmi és vereségi feltételek egyértelműen működnek;
- a pontozás reprodukálható;
- a tesztmód továbbra is használható;
- a firmware nem függ NEC-kódok dekódolásától;
- a rendszer hibás vagy ismeretlen állapotban sem jelez hamis sikert;
- a működő hardverek és a már elkészült kommunikációs funkciók a fejlesztés során megmaradnak.

---

# 18. Végső fejlesztési elv

A TENET Mesh célja egy olyan fizikai, kooperatív hálózati játék, amelyben a játékosok valós eszközökön keresztül tapasztalják meg a kommunikáció, a hibakezelés és a csomagkézbesítés alapelveit.

A firmware feladata nem pusztán a kijelzők vezérlése és az üzenetek továbbítása. A rendszernek egyértelmű, tesztelhető és visszakövethető játékállapotot kell fenntartania.

A fejlesztés során a meglévő működő komponenseket kell alapul venni, a hiányzó funkciókat pedig a jelen specifikáció alapján, fokozatosan kell megvalósítani.

**A legfontosabb szabály: a kijelzőn és a PC-s alkalmazásban csak olyan állapot jelenhet meg sikeresként, amelyet a rendszer ténylegesen igazolt.**