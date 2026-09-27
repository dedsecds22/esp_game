# TENET MESH — Játékszabályzat és C# WinForms Alkalmazás

Ez a dokumentum a `tenet-mesh-spec.md` kiegészítése: pontos
játékszabályokat, győzelmi/vereségi feltételeket, és a C# vezérlő app
**Windows Forms**-alapú teljes felépítését adja.

---

## A. RÉSZLETES JÁTÉKSZABÁLYZAT

### A.1 Szerepek
A játék **2 fős kooperatív** (bővíthető 3 főre), aszimmetrikus
információval:

| Szerep | Helyszín | Amit lát | Amit csinál |
|---|---|---|---|
| **Terepjátékos** | A node-ok mellett fizikailag | 2× TFT kijelző, IR távirányító a kezében | Mozgatja/aktiválja az IR gate-et, felolvassa a TFT-adatokat |
| **Operátor** | PC előtt | C# WinForms app (mesh térkép, sniffer, hack-konzol) | Dekódolja a hibákat, `REPAIR` parancsokat küld vissza |

A két szerep **nem lát mindent** — ez adja a kooperáció kényszerét: a
Terepjátékos fizikailag látja, melyik node villog pirosan, de nem
tudja *miért*; az Operátor látja a technikai részleteket (packet ID-k,
RSSI, XOR-kulcs-töredékek), de nem tudja, melyik node van éppen
paradoxonban a valóságban, amíg a Terepjátékos be nem mondja rádión
/ hangosan.

### A.2 A játék célja
Két győzelmi mód közül választhatsz kampány-indításkor:

**Mód 1 — "Defuse" (idő-bomba hatástalanítás)**
A Kék TFT (Node 0) countdownja 60-ról indul, 0-ig fut. Ha eléri a
0-t **és** a hálózat akkor "sérült" állapotban van (≥1 node
paradoxonban), a csapat veszít. A cél: időben, a megfelelő
pillanatban IR-rel megfordítani az időt úgy, hogy mire lejár, minden
node NORMAL állapotban legyen.

**Mód 2 — "Payload Delivery" (titkos csomag eljuttatása)**
Egy kijelölt `MSG_DATA` csomagnak (a "titkos adat") 001-től kell
eljutnia a Gateway node-ig (Node 5) **folytonosan növekvő** ID-vel.
Ha a csomag egy INVERSE node-on halad át fordítás nélkül, a payload
összekeveredik (byte-reverse), és "korrupttá" válik — ekkor az
Operátornak a hack-konzollal kell újraépítenie/validálnia, mielőtt
továbbengedi.

Alapesetben a **Mód 1**-et javaslom kezdésnek, mert egyszerűbb
felügyelni és rövidebb egy kör (3-5 perc).

### A.3 Kör felépítése (Mód 1 — Defuse)
1. **Setup**: minden node NORMAL, mindkét TFT 60-as countdownon áll,
   0 paradoxon.
2. **Start**: az Operátor a WinForms appból küld egy `START` parancsot
   → a Gateway broadcastol egy `MSG_BOMB_STATE` üzenetet, minden node
   elindítja a helyi countdownt.
3. **Random hibaesemények**: a firmware ~10-15 másodpercenként
   véletlenszerűen egy node-ot "megfertőz" (mesterségesen paradoxon
   állapotba tesz) — ez szimulálja a hálózati instabilitást, és
   kényszeríti a csapatot cselekvésre.
4. **IR aktiválás**: a Terepjátékos eldönti, mikor süti el az IR-t.
   Cooldown: **8 másodperc** (lásd A.5), tehát nem lehet spammelni.
5. **Fordítás hatása**: az adott zóna node-jai átfordulnak. Ha egy
   node éppen paradoxonban volt **és** a fordítás "helyes irányú"
   volt hozzá képest, a paradoxon feloldódik. Ha rossz irányú volt,
   a paradoxon megmarad vagy súlyosbodik (lásd A.4 pontszámítás).
6. **Vég**: countdown eléri a 0-t.
   - **Győzelem**: 0 paradoxonban lévő node.
   - **Vereség**: ≥1 node paradoxonban.
   - **Részleges győzelem** (opcionális, ha pontozol): a hibátlanul
     tartott node-ok aránya alapján csillagozás (1-3 csillag).

### A.4 Pontszámítás / nehézségi skálázás
```
score = (normal_time_ratio × 40)
      + (paradox_free_nodes / total_nodes × 40)
      + (correct_hacks / total_hacks_needed × 20)
```
- `normal_time_ratio`: mennyi ideig volt a hálózat többsége NORMAL
  állapotban a teljes kör alatt (a túl gyakori fordítgatást bünteti).
- Nehézségi szintek a random fertőzés gyakoriságával skálázhatók:
  Easy = 20 mp, Normal = 12 mp, Hard = 6 mp közötti fertőzés.

### A.5 IR gate szabályok (finomítva)
- **Cooldown**: 8 mp trigger között (firmware-ben `COOLDOWN_MS`).
- **Hatótáv/zóna**: alapból minden node egy zónában van (globális
  fordítás); ha bővíted zónákra, minden IR lövés csak a legközelebbi
  2 node-ot fordítja (fizikai IR hatótáv-korlát is természetesen
  segít ezt szimulálni, mivel az IR irányított).
- **"Dupla fordítás" szabály**: ha ugyanazt a zónát kétszer fordítod
  cooldown-on belül (ha hackkel meg lehet kerülni a cooldownt —
  ez lehet egy külön "power-up" a játékban), a node visszaáll
  NORMAL-ba, de +1 paradoxon-pontot kap büntetésként ("időhurok
  túlterhelés").

### A.6 Hack-konzol szabály (paradoxon feloldása távolból)
Amikor egy node paradoxonba kerül:
1. A node egy `MSG_PARADOX` csomagot küld a Gateway-nek, benne egy
   XOR-kódolt hibakóddal: `code = node_id XOR session_key`.
2. A `session_key` **két félre van osztva**: az egyik fele a Kék TFT
   alján jelenik meg, a másik fele a Piros TFT-n (tükrözve!, tehát
   vissza kell fejteni/olvasni fordítva).
3. A Terepjátékos rádión/hangosan bediktálja mindkét felet az
   Operátornak.
4. Az Operátor a WinForms hack-konzoljába beírja a teljes kulcsot →
   `REPAIR` parancs megy vissza a mesh-be → a node kilép PARADOX-ból.
5. **Hibás kulcs**: 3 mp "lezárás" büntetés (a node nem fogad új
   REPAIR-t eddig), hogy ne lehessen brute-force-olni.

### A.7 Csalás elleni / edge-case szabályok
- Ha egy node teljesen elveszti a rádiókapcsolatot (nincs heartbeat 5
  mp-ig), a Gateway "OFFLINE" állapotba teszi a WinForms térképen
  (szürke node), és a kör automatikusan vereség, ha ez a bomba
  lejárta előtt nem áll helyre — ez reális hálózati instabilitást
  szimulál, nem csak dizájn-elem.
- Ha a Terepjátékos kétszer egymás után "rossz pillanatban" fordít
  (azaz a hálózat NORMAL-ból INVERSE-be megy, miközben már 0
  paradoxon volt és semmi indoka nem volt fordítani), nincs
  mechanikai büntetés — ez taktikai hiba, a countdown-vesztés a
  büntetés.

---

## B. C# WINDOWS FORMS ALKALMAZÁS — TELJES FELÉPÍTÉS

### B.1 Miért működik jól WinForms-ban is
A WPF-es javaslat elsősorban a data-binding és az effekt-réteg miatt
volt kényelmes, de a te esetedben (ismered a WinFormst, gyors
prototípus kell) simán megoldható **GDI+ egyéni rajzolással**
(`Graphics` objektum egy `Panel.Paint` eseményén) — ez sőt
*teljesítményben* is jobb lehet, mint a WPF retained-mode rendering,
mert te kontrollálod pontosan mikor rajzol újra.

### B.2 Projekt-struktúra
```
TenetControl.WinForms/
├─ Program.cs
├─ Forms/
│  └─ MainForm.cs / MainForm.Designer.cs
├─ Controls/
│  ├─ MeshMapControl.cs       # egyéni UserControl, GDI+ rajzolás
│  └─ ScanlineOverlay.cs      # opcionális CRT/sci-fi glitch overlay
├─ Services/
│  ├─ SerialService.cs        # SerialPort háttérszálon
│  ├─ MeshState.cs            # node-ok állapota, thread-safe
│  └─ PacketCodec.cs          # JSON <-> objektum konverzió
├─ Models/
│  ├─ NodeInfo.cs
│  └─ PacketEntry.cs
└─ TenetControl.WinForms.csproj
```

### B.3 `NodeInfo.cs` és `PacketEntry.cs`
```csharp
public class NodeInfo
{
    public int Id { get; set; }
    public string Role { get; set; }        // "TFT_NORMAL", "RELAY", stb.
    public int Rssi { get; set; }
    public int TimeDir { get; set; }         // +1 / -1
    public bool IsParadox { get; set; }
    public bool IsOnline { get; set; }
    public DateTime LastSeen { get; set; }
    public PointF MapPosition { get; set; }  // előre fix hexagon-pozíció
}

public class PacketEntry
{
    public int Src { get; set; }
    public int Dst { get; set; }
    public int Type { get; set; }
    public int Id { get; set; }
    public int Dir { get; set; }
    public string Payload { get; set; }
    public int Rssi { get; set; }
    public DateTime Timestamp { get; set; } = DateTime.Now;
}
```

### B.4 `SerialService.cs` — soros vétel háttérszálon
WinForms-nál kulcsfontosságú, hogy a `SerialPort` eseményei **nem** a
UI szálon futnak — mindig `Control.Invoke`/`BeginInvoke`-ot kell
használni a vissza-marsallozáshoz.

```csharp
public class SerialService : IDisposable
{
    private SerialPort _port;
    public event Action<PacketEntry> PacketReceived;
    public event Action<string> RawLineReceived;

    public void Start(string portName, int baud = 2_000_000)
    {
        _port = new SerialPort(portName, baud)
        {
            NewLine = "\n",
            ReadTimeout = 500
        };
        _port.Open();
        var thread = new Thread(ReadLoop) { IsBackground = true };
        thread.Start();
    }

    private void ReadLoop()
    {
        while (_port?.IsOpen == true)
        {
            try
            {
                string line = _port.ReadLine();
                RawLineReceived?.Invoke(line);
                var entry = PacketCodec.Deserialize(line);
                if (entry != null) PacketReceived?.Invoke(entry);
            }
            catch (TimeoutException) { }
            catch (IOException) { break; } // kábel kihúzva
        }
    }

    public void SendCommand(object cmd)
    {
        string json = PacketCodec.SerializeCommand(cmd);
        _port?.WriteLine(json);
    }

    public void Dispose() => _port?.Close();
}
```

### B.5 `MainForm.cs` — UI összekötés
```csharp
public partial class MainForm : Form
{
    private readonly SerialService _serial = new();
    private readonly MeshState _state = new();

    public MainForm()
    {
        InitializeComponent();
        _serial.PacketReceived += OnPacketReceived;
        meshMapControl1.BindState(_state);
    }

    private void OnPacketReceived(PacketEntry entry)
    {
        // vissza a UI szálra
        if (InvokeRequired) { BeginInvoke(new Action(() => OnPacketReceived(entry))); return; }

        _state.UpdateFromPacket(entry);
        AppendToSnifferLog(entry);
        meshMapControl1.Invalidate();   // újrarajzolás triggerelése
    }

    private void AppendToSnifferLog(PacketEntry e)
    {
        string line = $"[{e.Timestamp:HH:mm:ss}] {e.Src}->{e.Dst} " +
                       $"type={e.Type} id={e.Id} dir={e.Dir} rssi={e.Rssi}";
        if (_state.GlobalTimeDirection == -1)
            snifferListBox.Items.Insert(0, line); // inverzben fentről görgetve
        else
            snifferListBox.Items.Add(line);

        if (snifferListBox.Items.Count > 500)
            snifferListBox.Items.RemoveAt(snifferListBox.Items.Count - 1);
    }

    private void btnRepairSubmit_Click(object sender, EventArgs e)
    {
        int target = (int)numTargetNode.Value;
        string key = txtHackKey.Text.Trim();
        _serial.SendCommand(new { cmd = "repair", target, key });
    }
}
```

### B.6 `MeshMapControl.cs` — egyéni GDI+ node-térkép
Ez a legfontosabb vizuális elem. `UserControl`-ból származtatva,
`DoubleBuffered = true` kötelező, különben villog rajzoláskor.

```csharp
public class MeshMapControl : UserControl
{
    private MeshState _state;
    private static readonly Color BgColor = Color.FromArgb(10, 14, 26);
    private static readonly Color NormalColor = Color.FromArgb(0, 240, 255);
    private static readonly Color InverseColor = Color.FromArgb(255, 45, 85);
    private static readonly Color ParadoxColor = Color.FromArgb(255, 214, 10);

    public MeshMapControl()
    {
        DoubleBuffered = true;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint, true);
    }

    public void BindState(MeshState state) => _state = state;

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        g.Clear(BgColor);
        if (_state == null) return;

        // élek (RSSI-alapú vastagsággal) előbb, node-ok felül
        foreach (var edge in _state.GetActiveLinks())
            DrawEdge(g, edge);

        foreach (var node in _state.Nodes)
            DrawNode(g, node);
    }

    private void DrawNode(Graphics g, NodeInfo node)
    {
        Color c = node.IsParadox ? ParadoxColor
                : node.TimeDir < 0 ? InverseColor
                : NormalColor;

        // "glow" effekt: több koncentrikus, csökkenő átlátszóságú kör
        using var glowBrush = new SolidBrush(Color.FromArgb(60, c));
        for (int r = 24; r > 10; r -= 4)
            g.FillEllipse(glowBrush, node.MapPosition.X - r, node.MapPosition.Y - r, r * 2, r * 2);

        using var brush = new SolidBrush(c);
        g.FillEllipse(brush, node.MapPosition.X - 10, node.MapPosition.Y - 10, 20, 20);

        using var textBrush = new SolidBrush(Color.White);
        using var font = new Font("Consolas", 8);
        g.DrawString($"N{node.Id} {node.Rssi}dBm", font, textBrush,
                      node.MapPosition.X - 20, node.MapPosition.Y + 14);
    }

    private void DrawEdge(Graphics g, (NodeInfo a, NodeInfo b, int rssi) edge)
    {
        float thickness = MapRange(edge.rssi, -90, -30, 1f, 5f);
        using var pen = new Pen(Color.FromArgb(120, NormalColor), thickness);
        g.DrawLine(pen, edge.a.MapPosition, edge.b.MapPosition);
    }

    private static float MapRange(float v, float inMin, float inMax, float outMin, float outMax)
        => outMin + (v - inMin) / (inMax - inMin) * (outMax - outMin);
}
```

**Villogás paradoxon esetén**: egy `System.Windows.Forms.Timer` 300
ms-onként `Invalidate()`-ol, és a `DrawNode` a `paradox` node-oknál
egy szinusz-alapú alfa-oszcillációt ad az `Environment.TickCount`
alapján — nem kell külön animációs framework.

### B.7 Sci-fi vizuális réteg WinForms-ban (mivel nincs Tailwind/CSS)
- **Sötét téma globálisan**: `Form.BackColor`, minden `Panel`/`Label`
  explicit színezve (WinForms nem örökli automatikusan sötét témát).
- **Monospace font mindenhol**: `Consolas` vagy `Cascadia Mono` a
  "terminál" hangulathoz.
- **Egyedi `Button` stílus**: `FlatStyle = FlatStyle.Flat`,
  `FlatAppearance.BorderColor` neon szín, hover-eseményen színváltás.
- **Scanline overlay (opcionális)**: egy félig átlátszó PNG csíkmintát
  a form tetején egy `PictureBox`-szal, `BackColor = Transparent` —
  olcsó CRT-hatás.

### B.8 Timer-alapú fő ciklus (countdown UI szinkron)
```csharp
private System.Windows.Forms.Timer _uiTimer;

private void SetupTimer()
{
    _uiTimer = new System.Windows.Forms.Timer { Interval = 250 };
    _uiTimer.Tick += (s, e) =>
    {
        lblCountdown.Text = _state.CurrentCountdown.ToString();
        meshMapControl1.Invalidate(); // folyamatos glow-animáció
        if (_state.CurrentCountdown <= 0) HandleRoundEnd();
    };
    _uiTimer.Start();
}
```

### B.9 Build/csomagolás
- **.NET 8 Windows Forms App** sablon (`dotnet new winforms`).
- NuGet: `System.IO.Ports` (ha .NET 8-cal külön kell hozzáadni —
  Windows-on natívan elérhető, de a csomagreferenciát explicit fel
  kell venni a `.csproj`-ban).
- `Application.SetHighDpiMode(HighDpiMode.SystemAware)` a
  `Program.cs`-ben, hogy a GDI+ rajzolás ne legyen elmosódott nagy
  DPI-jű monitorokon.

---

## C. Következő lépés javaslat
Ha szeretnéd, tudok generálni egy induló `.csproj` + `MainForm`
vázat is konkrét fájlokkal (letölthető formában), amit rögtön meg
tudsz nyitni Visual Studio-ban és tesztelheted a soros kapcsolatot
egy mock adatküldővel, mielőtt a firmware kész lenne.
