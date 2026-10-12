from __future__ import annotations

import json
import csv
import queue
import threading
import time
import tkinter as tk
from datetime import datetime
from tkinter import filedialog, messagebox, ttk
from typing import Any

from protocol import GatewayEvent, decode_line, encode_command, packet_status
from game_rules import IrConfig, MissionConfig, calculate_score

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


COLORS = {
    "bg": "#101719",
    "panel": "#172225",
    "panel_alt": "#1d2a2d",
    "line": "#304044",
    "text": "#edf4f1",
    "muted": "#91a39f",
    "cyan": "#56d7d0",
    "lime": "#b9e769",
    "red": "#ff746b",
    "yellow": "#f3c969",
    "offline": "#647371",
}

NODE_ROLES = {
    0: "TFT_NORMAL",
    1: "TFT_INVERSE",
    2: "IR_REMOTE",
    3: "RELAY",
    4: "IR_GATE",
    5: "GATEWAY",
}
MESH_NODE_IDS = (0, 1, 3, 4, 5)


class SerialWorker:
    def __init__(self, port: str, baud: int, events: queue.Queue[tuple[str, Any]]):
        self.port_name = port
        self.baud = baud
        self.events = events
        self.serial_port: Any = None
        self.stop_event = threading.Event()
        self.write_lock = threading.Lock()
        self.thread: threading.Thread | None = None

    def start(self) -> None:
        if serial is None:
            raise RuntimeError("A pyserial nincs telepítve. Futtasd: pip install -r requirements.txt")
        self.serial_port = serial.Serial(self.port_name, self.baud, timeout=0.2, write_timeout=1)
        self.thread = threading.Thread(target=self._read_loop, name="tenet-serial", daemon=True)
        self.thread.start()

    def _read_loop(self) -> None:
        while not self.stop_event.is_set():
            try:
                line = self.serial_port.readline()
                if line:
                    self.events.put(("line", line))
            except Exception as error:
                if not self.stop_event.is_set():
                    self.events.put(("error", str(error)))
                break
        self.events.put(("closed", None))

    def send(self, command: str, **values: Any) -> None:
        if self.serial_port is None or not self.serial_port.is_open:
            raise RuntimeError("Nincs nyitott soros kapcsolat.")
        data = encode_command(command, **values)
        with self.write_lock:
            self.serial_port.write(data)
            self.serial_port.flush()

    def close(self) -> None:
        self.stop_event.set()
        if self.serial_port is not None and self.serial_port.is_open:
            self.serial_port.close()


class TenetControlApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("TENET MESH / Network Recovery")
        self.root.geometry("1260x860")
        self.root.minsize(1040, 700)
        self.root.configure(bg=COLORS["bg"])
        self.events: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.worker: SerialWorker | None = None

        self.nodes = {
            node_id: {
                "role": NODE_ROLES[node_id],
                "online": node_id == 2,
                "rssi": None,
                "direction": 1,
                "paradox": False,
                "last_seen": None,
                "communication_state": "UNKNOWN",
                "game_state": "UNKNOWN",
                "firmware": "UNKNOWN",
                "capabilities": [],
                "last_seen_wall": None,
            }
            for node_id in NODE_ROLES
        }
        self.nodes[2]["online"] = False
        self.nodes[2]["communication_state"] = "STANDALONE"
        self.gateway_connected = False
        self.gateway_ready = False
        self.test_results: dict[int, dict[str, Any]] = {}
        self.test_running = False
        self.mesh_ready = False
        self.game_running = False
        self.game_deadline = 0.0
        self.countdown_seconds = tk.IntVar(value=240)
        self.difficulty = tk.StringVar(value="Normal")
        self.required_packets = tk.IntVar(value=8)
        self.fault_interval = tk.IntVar(value=12)
        self.fault_intensity = tk.IntVar(value=2)
        self.mission_id = tk.IntVar(value=(int(time.time()) & 0xFFFF) or 1)
        self.packet_target = tk.IntVar(value=0)
        self.repair_target = tk.IntVar(value=0)
        self.mission_state_text = tk.StringVar(value="DISCOVERY")
        self.mission_progress_text = tk.StringVar(value="PACKETS 0 / 8     ERRORS 0")
        self.ir_active_high = tk.BooleanVar(value=True)
        self.ir_gap_ms = tk.IntVar(value=80)
        self.ir_short_min = tk.IntVar(value=100)
        self.ir_short_max = tk.IntVar(value=400)
        self.ir_medium_min = tk.IntVar(value=500)
        self.ir_medium_max = tk.IntVar(value=1200)
        self.ir_long_min = tk.IntVar(value=1300)
        self.ir_long_max = tk.IntVar(value=2500)
        self.ir_minimum_ms = tk.IntVar(value=50)
        self.ir_maximum_ms = tk.IntVar(value=3000)
        self.ir_cooldown_ms = tk.IntVar(value=800)
        self.ir_measurement_text = tk.StringVar(value="NO MEASUREMENT")
        self.packet_records: list[dict[str, Any]] = []
        self.packet_type_filter = tk.StringVar(value="ALL")
        self.packet_status_filter = tk.StringVar(value="ALL")
        self.packet_node_filter = tk.StringVar(value="ALL")
        self.target_node = tk.IntVar(value=0)
        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="2000000")
        self.connection_text = tk.StringVar(value="DISCONNECTED")
        self.status_text = tk.StringVar(value="Gatewayhez kapcsolódás szükséges")
        self.ir_state_text = tk.StringVar(value="IR INPUT  /  UNKNOWN")
        self.ir_verified_at: float | None = None
        self.ir_config_ready = False
        self.ir_capture_active = False

        self._configure_styles()
        self._build_ui()
        self.refresh_ports()
        self.root.after(80, self._drain_events)
        self.root.after(500, self._refresh_node_states)
        self.root.after(2000, self._send_host_heartbeat)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _configure_styles(self) -> None:
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("TFrame", background=COLORS["bg"])
        style.configure("Panel.TFrame", background=COLORS["panel"])
        style.configure("Alt.TFrame", background=COLORS["panel_alt"])
        style.configure("TLabel", background=COLORS["bg"], foreground=COLORS["text"], font=("Segoe UI", 10))
        style.configure("Muted.TLabel", background=COLORS["panel"], foreground=COLORS["muted"], font=("Segoe UI", 9))
        style.configure("PanelTitle.TLabel", background=COLORS["panel"], foreground=COLORS["text"], font=("Segoe UI Semibold", 12))
        style.configure("Metric.TLabel", background=COLORS["panel"], foreground=COLORS["lime"], font=("Bahnschrift SemiBold", 22))
        style.configure("TButton", background=COLORS["panel_alt"], foreground=COLORS["text"], borderwidth=0, padding=(12, 8), font=("Segoe UI Semibold", 9))
        style.map("TButton", background=[("active", "#2b3b3f"), ("disabled", "#202a2c")], foreground=[("disabled", "#667572")])
        style.configure("Primary.TButton", background=COLORS["lime"], foreground="#172018")
        style.map("Primary.TButton", background=[("active", "#c9f27b")])
        style.configure("Danger.TButton", background="#512f31", foreground="#ffd6d1")
        style.map("Danger.TButton", background=[("active", "#704044")])
        style.configure("TNotebook", background=COLORS["panel"], borderwidth=0)
        style.configure("TNotebook.Tab", background=COLORS["panel_alt"], foreground=COLORS["muted"], padding=(16, 10), font=("Segoe UI Semibold", 10))
        style.map("TNotebook.Tab", background=[("selected", COLORS["panel"])], foreground=[("selected", COLORS["cyan"])])
        style.configure("Treeview", background=COLORS["panel"], fieldbackground=COLORS["panel"], foreground=COLORS["text"], borderwidth=0, rowheight=26, font=("Cascadia Mono", 9))
        style.configure("Treeview.Heading", background=COLORS["panel_alt"], foreground=COLORS["muted"], relief="flat", font=("Segoe UI Semibold", 9))
        style.map("Treeview", background=[("selected", "#285050")])
        style.configure("TCombobox", fieldbackground=COLORS["panel_alt"], background=COLORS["panel_alt"], foreground=COLORS["text"], arrowcolor=COLORS["cyan"])
        style.configure("TSpinbox", fieldbackground=COLORS["panel_alt"], background=COLORS["panel_alt"], foreground=COLORS["text"], arrowsize=12)

    def _build_ui(self) -> None:
        self.root.grid_columnconfigure(0, weight=1)
        self.root.grid_rowconfigure(2, weight=1)
        self.root.grid_rowconfigure(3, weight=1)

        header = tk.Frame(self.root, bg=COLORS["bg"], padx=22, pady=16)
        header.grid(row=0, column=0, sticky="ew")
        header.grid_columnconfigure(0, weight=1)
        title = tk.Frame(header, bg=COLORS["bg"])
        title.grid(row=0, column=0, sticky="w")
        tk.Label(title, text="TENET", bg=COLORS["bg"], fg=COLORS["text"], font=("Bahnschrift SemiBold", 25)).pack(side="left")
        tk.Label(title, text=" / MESH CONTROL", bg=COLORS["bg"], fg=COLORS["cyan"], font=("Cascadia Mono", 12)).pack(side="left", padx=(8, 0), pady=(7, 0))
        tk.Label(header, text="FIELD CONSOLE     BUILD 01", bg=COLORS["bg"], fg=COLORS["muted"], font=("Cascadia Mono", 9)).grid(row=0, column=1, sticky="e")

        connection = tk.Frame(self.root, bg=COLORS["panel_alt"], padx=18, pady=11)
        connection.grid(row=1, column=0, sticky="ew", padx=18, pady=(0, 12))
        connection.grid_columnconfigure(7, weight=1)
        tk.Label(connection, text="SERIAL LINK", bg=COLORS["panel_alt"], fg=COLORS["muted"], font=("Cascadia Mono", 9)).grid(row=0, column=0, padx=(0, 12))
        self.port_combo = ttk.Combobox(connection, textvariable=self.port_var, width=13, state="readonly")
        self.port_combo.grid(row=0, column=1, padx=4)
        ttk.Button(connection, text="↻", width=3, command=self.refresh_ports).grid(row=0, column=2, padx=3)
        self.connect_button = ttk.Button(connection, text="CONNECT", style="Primary.TButton", command=self.toggle_connection)
        self.connect_button.grid(row=0, column=3, padx=(8, 16))
        tk.Label(connection, text="BAUD", bg=COLORS["panel_alt"], fg=COLORS["muted"], font=("Cascadia Mono", 9)).grid(row=0, column=4, padx=(4, 6))
        self.baud_combo = ttk.Combobox(connection, textvariable=self.baud_var, values=("2000000", "115200"), width=10, state="readonly")
        self.baud_combo.grid(row=0, column=5)
        self.link_led = tk.Canvas(connection, width=12, height=12, bg=COLORS["panel_alt"], highlightthickness=0)
        self.link_led.grid(row=0, column=6, padx=(18, 7))
        self.link_dot = self.link_led.create_oval(2, 2, 10, 10, fill=COLORS["offline"], outline="")
        tk.Label(connection, textvariable=self.connection_text, bg=COLORS["panel_alt"], fg=COLORS["muted"], font=("Cascadia Mono", 9)).grid(row=0, column=7, sticky="w")

        body = ttk.Frame(self.root)
        body.grid(row=2, column=0, sticky="nsew", padx=18)
        body.grid_columnconfigure(0, weight=5, uniform="top")
        body.grid_columnconfigure(1, weight=4, uniform="top")
        body.grid_rowconfigure(0, weight=1)

        map_panel = ttk.Frame(body, style="Panel.TFrame", padding=14)
        map_panel.grid(row=0, column=0, sticky="nsew", padx=(0, 7))
        map_panel.grid_columnconfigure(0, weight=1)
        map_panel.grid_rowconfigure(1, weight=1)
        map_head = ttk.Frame(map_panel, style="Panel.TFrame")
        map_head.grid(row=0, column=0, sticky="ew", pady=(0, 8))
        ttk.Label(map_head, text="MESH TOPOLOGY", style="PanelTitle.TLabel").pack(side="left")
        self.online_label = ttk.Label(map_head, text="0 / 5 ONLINE", style="Muted.TLabel")
        self.online_label.pack(side="right")
        self.map_canvas = tk.Canvas(map_panel, bg=COLORS["panel"], highlightthickness=0, height=300)
        self.map_canvas.grid(row=1, column=0, sticky="nsew")
        self.map_canvas.bind("<Configure>", lambda _event: self.draw_map())

        mode_panel = ttk.Frame(body, style="Panel.TFrame", padding=0)
        mode_panel.grid(row=0, column=1, sticky="nsew", padx=(7, 0))
        self.notebook = ttk.Notebook(mode_panel)
        self.notebook.pack(fill="both", expand=True)
        self.test_tab = ttk.Frame(self.notebook, style="Panel.TFrame", padding=16)
        self.lobby_tab = ttk.Frame(self.notebook, style="Panel.TFrame", padding=16)
        self.mission_tab = ttk.Frame(self.notebook, style="Panel.TFrame", padding=16)
        self.ir_tab = ttk.Frame(self.notebook, style="Panel.TFrame", padding=16)
        self.notebook.add(self.lobby_tab, text="LOBBY")
        self.notebook.add(self.mission_tab, text="MISSION")
        self.notebook.add(self.test_tab, text="TEST MODE")
        self.notebook.add(self.ir_tab, text="IR CALIBRATION")
        self._build_lobby_tab()
        self._build_mission_tab()
        self._build_test_tab()
        self._build_ir_tab()

        log_panel = ttk.Frame(self.root, style="Panel.TFrame", padding=12)
        log_panel.grid(row=3, column=0, sticky="nsew", padx=18, pady=(12, 0))
        log_panel.grid_columnconfigure(0, weight=1)
        log_panel.grid_rowconfigure(1, weight=1)
        log_header = ttk.Frame(log_panel, style="Panel.TFrame")
        log_header.grid(row=0, column=0, sticky="ew", pady=(0, 8))
        ttk.Label(log_header, text="PACKET / EVENT STREAM", style="PanelTitle.TLabel").pack(side="left")
        ttk.Combobox(log_header, textvariable=self.packet_node_filter, values=("ALL", "0", "1", "3", "4", "5"), width=5, state="readonly").pack(side="right", padx=(4, 0))
        ttk.Combobox(log_header, textvariable=self.packet_status_filter, values=("ALL", "SENT", "ACK", "LOST", "RETRY", "INVALID", "DUPLICATE", "UNKNOWN"), width=10, state="readonly").pack(side="right", padx=(4, 0))
        ttk.Combobox(log_header, textvariable=self.packet_type_filter, values=("ALL", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16"), width=6, state="readonly").pack(side="right", padx=(4, 0))
        ttk.Button(log_header, text="EXPORT", command=self.export_log).pack(side="right", padx=(5, 0))
        ttk.Button(log_header, text="CLEAR", command=self.clear_log).pack(side="right")
        for variable in (self.packet_type_filter, self.packet_status_filter, self.packet_node_filter):
            variable.trace_add("write", lambda *_args: self._render_packet_log())
        columns = ("time", "msg", "logical", "mission", "src", "dst", "type", "status", "retry", "latency", "rssi", "payload")
        self.packet_tree = ttk.Treeview(log_panel, columns=columns, show="headings", height=8)
        widths = {"time": 70, "msg": 48, "logical": 55, "mission": 55, "src": 34, "dst": 34, "type": 48, "status": 66, "retry": 38, "latency": 55, "rssi": 48, "payload": 170}
        for column, label in zip(columns, ("TIME", "MSG", "PACKET", "MISSION", "SRC", "DST", "TYPE", "STATUS", "RETRY", "LATENCY", "RSSI", "PAYLOAD")):
            self.packet_tree.heading(column, text=label)
            self.packet_tree.column(column, width=widths[column], anchor="w", stretch=column == "payload")
        self.packet_tree.grid(row=1, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(log_panel, orient="vertical", command=self.packet_tree.yview)
        scrollbar.grid(row=1, column=1, sticky="ns")
        horizontal = ttk.Scrollbar(log_panel, orient="horizontal", command=self.packet_tree.xview)
        horizontal.grid(row=2, column=0, sticky="ew")
        self.packet_tree.configure(yscrollcommand=scrollbar.set, xscrollcommand=horizontal.set)

        footer = tk.Frame(self.root, bg=COLORS["bg"], padx=20, pady=8)
        footer.grid(row=4, column=0, sticky="ew")
        tk.Label(footer, textvariable=self.status_text, bg=COLORS["bg"], fg=COLORS["muted"], font=("Segoe UI", 9)).pack(side="left")
        tk.Label(footer, text="ESP-NOW  /  CHANNEL 1  /  JSON-LINE", bg=COLORS["bg"], fg=COLORS["muted"], font=("Cascadia Mono", 8)).pack(side="right")

    def _build_lobby_tab(self) -> None:
        self.lobby_tab.grid_columnconfigure(0, weight=1)
        header = ttk.Frame(self.lobby_tab, style="Panel.TFrame")
        header.grid(row=0, column=0, sticky="ew")
        ttk.Label(header, text="SYSTEM LOBBY", style="PanelTitle.TLabel").pack(side="left")
        ttk.Label(header, textvariable=self.mission_state_text, style="Muted.TLabel").pack(side="right")
        ttk.Label(self.lobby_tab, text="Firmware-reported inventory; IR remote is standalone and has no mesh heartbeat.", style="Muted.TLabel", wraplength=405).grid(row=1, column=0, sticky="w", pady=(5, 10))
        columns = ("node", "role", "comm", "game", "firmware", "last")
        self.node_tree = ttk.Treeview(self.lobby_tab, columns=columns, show="headings", height=7)
        for name, label, width in (("node", "ID", 35), ("role", "ROLE", 105), ("comm", "LINK", 72), ("game", "STATE", 76), ("firmware", "FIRMWARE", 80), ("last", "LAST SEEN", 68)):
            self.node_tree.heading(name, text=label)
            self.node_tree.column(name, width=width, anchor="w")
        for node_id, role in NODE_ROLES.items():
            comm_state = "STANDALONE" if node_id == 2 else "UNKNOWN"
            self.node_tree.insert("", "end", iid=str(node_id), values=(node_id, role, comm_state, "UNKNOWN", "--", "--"))
        self.node_tree.grid(row=2, column=0, sticky="ew", pady=(0, 10))
        buttons = ttk.Frame(self.lobby_tab, style="Panel.TFrame")
        buttons.grid(row=3, column=0, sticky="ew")
        ttk.Button(buttons, text="SCAN / REFRESH", command=self.scan_nodes).pack(side="left", padx=(0, 6))
        ttk.Button(buttons, text="HARDWARE TEST", command=lambda: self.notebook.select(self.test_tab)).pack(side="left", padx=6)
        ttk.Button(buttons, text="CALIBRATE IR", command=lambda: self.notebook.select(self.ir_tab)).pack(side="left", padx=6)
        self.lobby_ready_text = ttk.Label(self.lobby_tab, text="Required mesh nodes: 0 / 5", style="Muted.TLabel")
        self.lobby_ready_text.grid(row=4, column=0, sticky="w", pady=(10, 0))

    def _build_mission_tab(self) -> None:
        self.mission_tab.grid_columnconfigure(0, weight=1)
        ttk.Label(self.mission_tab, text="NETWORK RECOVERY", style="PanelTitle.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(self.mission_tab, text="A gateway visszaigazolásai vezérlik az állapotot; csak ACK-olt adat számít kézbesítettnek.", style="Muted.TLabel", wraplength=405).grid(row=1, column=0, sticky="w", pady=(5, 11))

        self.fault_vars = {fault: tk.BooleanVar(value=fault in ("INVERSE", "PARADOX", "LOSS", "DELAY")) for fault in ("INVERSE", "PARADOX", "LOSS", "DELAY", "DUPLICATE", "INVALID_PAYLOAD")}
        fields = ttk.Frame(self.mission_tab, style="Panel.TFrame")
        fields.grid(row=2, column=0, sticky="ew")
        for col in (1, 3):
            fields.grid_columnconfigure(col, weight=1)
        ttk.Label(fields, text="DIFFICULTY", style="Muted.TLabel").grid(row=0, column=0, sticky="w", pady=3)
        difficulty = ttk.Combobox(fields, values=("Easy", "Normal", "Hard"), textvariable=self.difficulty, state="readonly", width=12)
        difficulty.grid(row=0, column=1, sticky="ew", padx=5)
        difficulty.bind("<<ComboboxSelected>>", self._apply_difficulty_defaults)
        for row, title, variable, low, high in (
            (1, "MISSION ID", self.mission_id, 1, 65535),
            (2, "TIME (SEC)", self.countdown_seconds, 30, 3600),
            (3, "REQUIRED PACKETS", self.required_packets, 1, 100),
            (4, "FAULT INTERVAL (SEC)", self.fault_interval, 2, 300),
            (5, "FAULT INTENSITY", self.fault_intensity, 1, 5),
        ):
            ttk.Label(fields, text=title, style="Muted.TLabel").grid(row=row, column=0, sticky="w", pady=3)
            ttk.Spinbox(fields, from_=low, to=high, textvariable=variable, width=10, justify="right").grid(row=row, column=1, sticky="ew", padx=5, pady=3)
            ttk.Label(fields, text="PACKET TARGET", style="Muted.TLabel").grid(row=6, column=0, sticky="w", pady=3)
            ttk.Combobox(fields, values=(0, 1, 3, 4), textvariable=self.packet_target,
                     width=10, state="readonly").grid(row=6, column=1, sticky="ew", padx=5, pady=3)
        ttk.Label(self.mission_tab, text="ENABLED FAULTS", style="Muted.TLabel").grid(row=3, column=0, sticky="w", pady=(10, 3))
        faults = ttk.Frame(self.mission_tab, style="Panel.TFrame")
        faults.grid(row=4, column=0, sticky="ew")
        for index, fault in enumerate(self.fault_vars):
            ttk.Checkbutton(faults, text=fault.replace("_", " "), variable=self.fault_vars[fault]).grid(row=index // 2, column=index % 2, sticky="w", padx=(0, 12), pady=2)
        controls = ttk.Frame(self.mission_tab, style="Panel.TFrame")
        controls.grid(row=5, column=0, sticky="ew", pady=(12, 0))
        ttk.Button(controls, text="START MISSION", style="Primary.TButton", command=self.start_mission).pack(side="left", padx=(0, 5))
        ttk.Button(controls, text="PAUSE / RESUME", command=self.toggle_pause).pack(side="left", padx=5)
        ttk.Button(controls, text="ABORT", style="Danger.TButton", command=self.abort_mission).pack(side="left", padx=5)
        ttk.Label(self.mission_tab, textvariable=self.mission_progress_text, style="Metric.TLabel").grid(row=6, column=0, sticky="w", pady=(11, 0))
        repair_row = ttk.Frame(self.mission_tab, style="Panel.TFrame")
        repair_row.grid(row=7, column=0, sticky="ew", pady=(8, 0))
        ttk.Label(repair_row, text="REPAIR FAULT NODE", style="Muted.TLabel").pack(side="left")
        ttk.Combobox(repair_row, values=(0, 1, 3, 4), textvariable=self.repair_target, width=5, state="readonly").pack(side="left", padx=8)
        ttk.Button(repair_row, text="SEND REPAIR", command=self.repair_selected_node).pack(side="right")

    def _apply_difficulty_defaults(self, _event: Any = None) -> None:
        try:
            config = MissionConfig.for_difficulty(self.difficulty.get())
        except ValueError:
            return
        self.countdown_seconds.set(config.duration_seconds)
        self.required_packets.set(config.required_packets)
        self.fault_interval.set(config.fault_interval_seconds)

    def _build_ir_tab(self) -> None:
        self.ir_tab.grid_columnconfigure(0, weight=1)
        ttk.Label(self.ir_tab, text="IR CALIBRATION", style="PanelTitle.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(self.ir_tab, text="HIGH/LOW időmérés; a beállítás csak a Gateway jóváhagyása után lép életbe.", style="Muted.TLabel", wraplength=405).grid(row=1, column=0, sticky="w", pady=(5, 10))
        ttk.Label(self.ir_tab, textvariable=self.ir_measurement_text, style="Metric.TLabel").grid(row=2, column=0, sticky="w", pady=(0, 8))
        self.ir_raw_text = tk.StringVar(value="RAW --    ACTIVE --    PULSE -- ms    GAP -- ms    GROUP -- ms    CATEGORY --")
        ttk.Label(self.ir_tab, textvariable=self.ir_raw_text, style="Muted.TLabel", wraplength=410).grid(row=3, column=0, sticky="w", pady=(0, 10))
        ttk.Checkbutton(self.ir_tab, text="Input active HIGH (disable for active LOW)", variable=self.ir_active_high).grid(row=4, column=0, sticky="w", pady=3)
        config = ttk.Frame(self.ir_tab, style="Panel.TFrame")
        config.grid(row=5, column=0, sticky="ew")
        items = (
            ("Pulse group gap ms", self.ir_gap_ms), ("Short min ms", self.ir_short_min), ("Short max ms", self.ir_short_max),
            ("Medium min ms", self.ir_medium_min), ("Medium max ms", self.ir_medium_max), ("Long min ms", self.ir_long_min),
            ("Long max ms", self.ir_long_max), ("Minimum active ms", self.ir_minimum_ms), ("Maximum group ms", self.ir_maximum_ms),
            ("Cooldown ms", self.ir_cooldown_ms),
        )
        for index, (label, variable) in enumerate(items):
            row, column = divmod(index, 2)
            ttk.Label(config, text=label, style="Muted.TLabel").grid(row=row, column=column * 2, sticky="w", pady=2, padx=(0, 3))
            ttk.Spinbox(config, from_=0, to=10000, textvariable=variable, width=7, justify="right").grid(row=row, column=column * 2 + 1, sticky="ew", padx=(0, 10), pady=2)
        controls = ttk.Frame(self.ir_tab, style="Panel.TFrame")
        controls.grid(row=6, column=0, sticky="ew", pady=(10, 3))
        ttk.Button(controls, text="READ INPUT", command=self.query_ir_state).pack(side="left", padx=(0, 5))
        self.ir_capture_button = ttk.Button(controls, text="START CAPTURE", command=self.toggle_ir_capture)
        self.ir_capture_button.pack(side="left", padx=5)
        ttk.Button(self.ir_tab, text="APPLY IR CONFIG", command=self.apply_ir_config).grid(row=7, column=0, sticky="ew", pady=(5, 3))

    def scan_nodes(self) -> None:
        self._send("scan")
        self.status_text.set("Node discovery kérés elküldve")

    def start_mission(self) -> None:
        try:
            config = MissionConfig(
                self.difficulty.get().lower(),
                int(self.countdown_seconds.get()),
                int(self.fault_interval.get()),
                int(self.required_packets.get()),
                tuple(name for name, variable in self.fault_vars.items() if variable.get()),
                int(self.fault_intensity.get()),
            )
            config.validate()
            mission_id = int(self.mission_id.get())
            if not 1 <= mission_id <= 65535:
                raise ValueError("Mission ID must be 1..65535")
        except (ValueError, tk.TclError) as error:
            messagebox.showwarning("Mission configuration", str(error))
            return
        if not self.gateway_connected or not self.gateway_ready:
            messagebox.showwarning("Gateway", "Csatlakozz az USB gatewayhez a küldetés indításához.")
            return
        if not self.mesh_ready:
            messagebox.showwarning("Preflight", "A küldetés előtt futtasd le sikeresen a kötelező hardware/mesh teszteket.")
            self.notebook.select(self.test_tab)
            return
        stale_nodes = [node_id for node_id in (0, 1, 3, 4)
                       if not self.nodes[node_id]["online"] or self.nodes[node_id]["last_seen"] is None
                       or time.monotonic() - self.nodes[node_id]["last_seen"] > 6
                       or self.nodes[node_id]["firmware"] == "UNKNOWN"]
        if stale_nodes:
            messagebox.showwarning("Preflight", f"Elavult/offline node: {', '.join(map(str, stale_nodes))}. Futtasd újra a scan-t.")
            self.notebook.select(self.lobby_tab)
            return
        if self.ir_verified_at is None or time.monotonic() - self.ir_verified_at > 10:
            messagebox.showwarning("IR preflight", "Kérd le most az IR bemenetet az IR Calibration fülön.")
            self.notebook.select(self.ir_tab)
            return
        if not self.ir_config_ready:
            messagebox.showwarning("IR configuration", "Az IR beállítás még nincs visszaigazolva a firmware-től.")
            self.notebook.select(self.ir_tab)
            return
        self.mission_state_text.set("COUNTDOWN REQUESTED")
        self._send("mission_start", mission_id=mission_id, difficulty=config.difficulty,
                   duration_seconds=config.duration_seconds, packet_count=config.required_packets,
                   fault_interval_seconds=config.fault_interval_seconds,
                   packet_target=int(self.packet_target.get()),
                   fault_intensity=config.fault_intensity, fault_types=list(config.enabled_faults))

    def toggle_pause(self) -> None:
        command = "mission_resume" if self.mission_state_text.get() == "PAUSED" else "mission_pause"
        self._send(command)

    def abort_mission(self) -> None:
        if messagebox.askyesno("Abort mission", "Biztosan megszakítod a küldetést?"):
            self._send("mission_abort")

    def repair_selected_node(self) -> None:
        node_id = int(self.repair_target.get())
        node = self.nodes[node_id]
        if node["game_state"] not in {"INVERSE", "PARADOX", "LOSS", "DELAY", "DUPLICATE", "INVALID_PAYLOAD"}:
            messagebox.showwarning("Repair", f"N{node_id} nem jelentett javítható hibát.")
            return
        self._send("repair", target=node_id)

    def apply_ir_config(self) -> None:
        try:
            config = IrConfig(
                self.ir_active_high.get(), int(self.ir_gap_ms.get()),
                int(self.ir_short_min.get()), int(self.ir_short_max.get()),
                int(self.ir_medium_min.get()), int(self.ir_medium_max.get()),
                int(self.ir_long_min.get()), int(self.ir_long_max.get()),
                int(self.ir_minimum_ms.get()), int(self.ir_maximum_ms.get()),
                int(self.ir_cooldown_ms.get()),
            )
            config.validate()
        except (ValueError, tk.TclError) as error:
            messagebox.showwarning("IR configuration", str(error))
            return
        values = {name: getattr(config, name) for name in config.__dataclass_fields__}
        if messagebox.askyesno("Apply IR configuration", json.dumps(values, indent=2)):
            self.ir_config_ready = False
            self._send("ir_config", **values)

    def toggle_ir_capture(self) -> None:
        command = "ir_calibration_stop" if self.ir_capture_active else "ir_calibrate"
        self._send(command)

    def export_log(self) -> None:
        filename = filedialog.asksaveasfilename(defaultextension=".csv", filetypes=(("CSV", "*.csv"), ("JSON", "*.json")))
        if not filename:
            return
        rows = [self.packet_tree.item(item, "values") for item in self.packet_tree.get_children()]
        headings = [self.packet_tree.heading(column, "text") for column in self.packet_tree["columns"]]
        try:
            if filename.lower().endswith(".json"):
                with open(filename, "w", encoding="utf-8") as output:
                    json.dump([dict(zip(headings, row)) for row in rows], output, ensure_ascii=False, indent=2)
            else:
                with open(filename, "w", encoding="utf-8", newline="") as output:
                    writer = csv.writer(output)
                    writer.writerow(headings)
                    writer.writerows(rows)
        except OSError as error:
            messagebox.showerror("Export", str(error))
            return
        self.status_text.set(f"Napló exportálva: {filename}")

    def _build_test_tab(self) -> None:
        self.test_tab.grid_columnconfigure(0, weight=1)
        ttk.Label(self.test_tab, text="LINK DIAGNOSTICS", style="PanelTitle.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(self.test_tab, text="Ellenőrzi a mesh node-ok válaszát, RSSI-jét és a TFT initet.", style="Muted.TLabel", wraplength=390).grid(row=1, column=0, sticky="w", pady=(5, 16))

        self.mesh_test_button = ttk.Button(self.test_tab, text="RUN MESH TEST", style="Primary.TButton", command=self.run_mesh_test)
        self.mesh_test_button.grid(row=2, column=0, sticky="ew", pady=4)
        self.ir_test_button = ttk.Button(self.test_tab, text="RUN IR PATH TEST", command=self.run_ir_test)
        self.ir_test_button.grid(row=3, column=0, sticky="ew", pady=4)
        ir_state_row = ttk.Frame(self.test_tab, style="Panel.TFrame")
        ir_state_row.grid(row=4, column=0, sticky="ew", pady=(8, 0))
        ttk.Button(ir_state_row, text="READ IR INPUT", command=self.query_ir_state).pack(side="left")
        ttk.Label(ir_state_row, textvariable=self.ir_state_text, style="PanelTitle.TLabel").pack(side="right")

        ttk.Label(self.test_tab, text="NODE RESULTS", style="Muted.TLabel").grid(row=5, column=0, sticky="w", pady=(18, 5))
        self.test_tree = ttk.Treeview(self.test_tab, columns=("node", "role", "mesh", "lcd", "rssi"), show="headings", height=5)
        for col, title, width in (("node", "ID", 40), ("role", "ROLE", 120), ("mesh", "MESH", 72), ("lcd", "DISPLAY", 85), ("rssi", "RSSI", 62)):
            self.test_tree.heading(col, text=title)
            self.test_tree.column(col, width=width, anchor="w")
        for node_id in (0, 1, 3, 4):
            self.test_tree.insert("", "end", iid=str(node_id), values=(node_id, NODE_ROLES[node_id], "--", "--" if node_id in (0, 1) else "N/A", "--"))
        self.test_tree.grid(row=6, column=0, sticky="ew")
        self.mesh_result = ttk.Label(self.test_tab, text="MESH  --     TFT INIT  --     IR PATH  --", style="Muted.TLabel")
        self.mesh_result.grid(row=7, column=0, sticky="w", pady=(12, 0))

    def _build_game_tab(self) -> None:
        self.game_tab.grid_columnconfigure(0, weight=1)
        ttk.Label(self.game_tab, text="DEFUSE / GAME CONTROL", style="PanelTitle.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(self.game_tab, text="Defuse kör: a Gateway fertőz node-okat, a terepjátékos bemondja a hibakódot, az operátor a két TFT kulcsfeléből javít.", style="Muted.TLabel", wraplength=395).grid(row=1, column=0, sticky="w", pady=(5, 14))

        timer_row = ttk.Frame(self.game_tab, style="Panel.TFrame")
        timer_row.grid(row=2, column=0, sticky="ew", pady=3)
        ttk.Label(timer_row, text="COUNTDOWN (SEC)", style="Muted.TLabel").pack(side="left")
        self.seconds_spin = ttk.Spinbox(timer_row, from_=1, to=3600, textvariable=self.countdown_seconds, width=7, justify="center")
        self.seconds_spin.pack(side="right")
        difficulty_row = ttk.Frame(self.game_tab, style="Panel.TFrame")
        difficulty_row.grid(row=3, column=0, sticky="ew", pady=(8, 3))
        ttk.Label(difficulty_row, text="DIFFICULTY", style="Muted.TLabel").pack(side="left")
        ttk.Combobox(difficulty_row, values=("Easy", "Normal", "Hard"), textvariable=self.difficulty, width=10, state="readonly").pack(side="right")
        self.start_button = ttk.Button(self.game_tab, text="START ROUND", style="Primary.TButton", command=self.start_round)
        self.start_button.grid(row=4, column=0, sticky="ew", pady=(7, 4))
        ttk.Button(self.game_tab, text="INVERT TIME", command=self.invert_time).grid(row=5, column=0, sticky="ew", pady=4)

        ttk.Separator(self.game_tab).grid(row=6, column=0, sticky="ew", pady=13)
        repair_row = ttk.Frame(self.game_tab, style="Panel.TFrame")
        repair_row.grid(row=7, column=0, sticky="ew")
        ttk.Label(repair_row, text="REPAIR TARGET", style="Muted.TLabel").pack(side="left")
        self.target_combo = ttk.Combobox(repair_row, values=(0, 1, 3, 4), textvariable=self.target_node, width=7, state="readonly")
        self.target_combo.pack(side="right")
        key_row = ttk.Frame(self.game_tab, style="Panel.TFrame")
        key_row.grid(row=8, column=0, sticky="ew", pady=(8, 4))
        ttk.Label(key_row, text="KEY", style="Muted.TLabel").pack(side="left")
        self.key_entry = ttk.Entry(key_row)
        self.key_entry.pack(side="right", fill="x", expand=True, padx=(18, 0))
        self.repair_button = ttk.Button(self.game_tab, text="SEND REPAIR COMMAND", command=self.send_repair)
        self.repair_button.grid(row=9, column=0, sticky="ew", pady=4)

        self.game_clock = ttk.Label(self.game_tab, text="READY  /  00:60", style="Metric.TLabel")
        self.game_clock.grid(row=10, column=0, sticky="w", pady=(15, 0))
        ttk.Label(self.game_tab, text="A Gateway kezeli a fertőzést és köreredményt; csak helyes session key javít.", style="Muted.TLabel", wraplength=395).grid(row=11, column=0, sticky="w", pady=(2, 0))

    def refresh_ports(self) -> None:
        ports = [] if list_ports is None else [port.device for port in list_ports.comports()]
        self.port_combo["values"] = ports
        if self.port_var.get() not in ports:
            self.port_var.set(ports[0] if ports else "")
        if not ports:
            self.status_text.set("Nem található soros port")

    def toggle_connection(self) -> None:
        if self.worker is not None:
            self.worker.close()
            self.worker = None
            self._set_connected(False)
            return
        if not self.port_var.get():
            messagebox.showwarning("Soros port", "Válaszd ki a Gateway COM portját.")
            return
        try:
            worker = SerialWorker(self.port_var.get(), int(self.baud_var.get()), self.events)
            worker.start()
        except Exception as error:
            messagebox.showerror("Kapcsolódási hiba", str(error))
            return
        self.worker = worker
        self._set_connected(True)
        self.status_text.set(f"Kapcsolódva: {self.port_var.get()} @ {self.baud_var.get()}")
        self._send("ping")

    def _set_connected(self, connected: bool) -> None:
        self.gateway_connected = connected
        if not connected:
            self.mesh_ready = False
            self.gateway_ready = False
        link_text = "DISCONNECTED" if not connected else "GATEWAY ONLINE" if self.gateway_ready else "WAITING FOR GATEWAY"
        self.connection_text.set(link_text)
        self.link_led.itemconfigure(self.link_dot, fill=COLORS["lime"] if self.gateway_ready else COLORS["yellow"] if connected else COLORS["offline"])
        self.connect_button.configure(text="DISCONNECT" if connected else "CONNECT", style="Danger.TButton" if connected else "Primary.TButton")
        self.nodes[5]["online"] = self.gateway_ready
        self.nodes[5]["communication_state"] = "ONLINE" if self.gateway_ready else "DISCOVERING"
        if self.gateway_ready:
            self.nodes[5]["last_seen"] = time.monotonic()
            self.nodes[5]["last_seen_wall"] = datetime.now()
        self._update_lobby_node(5)
        self.draw_map()

    def _send(self, command: str, **values: Any) -> bool:
        if self.worker is None:
            messagebox.showwarning("Nincs kapcsolat", "Előbb csatlakozz a Gatewayhez.")
            return False
        try:
            self.worker.send(command, **values)
            self.status_text.set(f"Elküldve: {command}")
            return True
        except Exception as error:
            self.status_text.set(f"Küldési hiba: {error}")
            messagebox.showerror("Soros küldési hiba", str(error))
            return False

    def run_mesh_test(self) -> None:
        self.mesh_ready = False
        self.test_results.clear()
        self.test_running = True
        for node_id in (0, 1, 3, 4):
            self.test_tree.item(str(node_id), values=(node_id, NODE_ROLES[node_id], "WAIT", "WAIT" if node_id in (0, 1) else "N/A", "--"))
        self.mesh_result.configure(text="MESH TEST RUNNING  /  5 SEC")
        if not self._send("test"):
            self.test_running = False

    def run_ir_test(self) -> None:
        self._send("test_ir")
        self.mesh_result.configure(text="IR TEST WAITING  /  PRESS REMOTE")

    def query_ir_state(self) -> None:
        self.ir_state_text.set("IR INPUT  /  WAITING")
        self._send("ir_state")

    def _drain_events(self) -> None:
        while True:
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "line":
                self._handle_line(value)
            elif kind == "error":
                self.status_text.set(f"Soros hiba: {value}")
                self._set_connected(False)
                self.worker = None
            elif kind == "closed" and self.worker is not None and self.worker.stop_event.is_set():
                self._set_connected(False)
        self.root.after(80, self._drain_events)

    def _send_host_heartbeat(self) -> None:
        if self.gateway_connected:
            self._send("host_heartbeat")
        self.root.after(2000, self._send_host_heartbeat)

    def _handle_line(self, raw_line: bytes) -> None:
        message = decode_line(raw_line)
        if message is None:
            self.status_text.set("Nem JSON Gateway sor érkezett")
            return
        event = message.get("event")
        if event == "gateway_ready":
            self.gateway_ready = True
            self.connection_text.set("GATEWAY ONLINE")
            self.link_led.itemconfigure(self.link_dot, fill=COLORS["lime"])
            self.nodes[5]["online"] = True
            self.nodes[5]["communication_state"] = "ONLINE"
            self.nodes[5]["firmware"] = message.get("firmware", "UNKNOWN")
            self.nodes[5]["last_seen"] = time.monotonic()
            self.nodes[5]["last_seen_wall"] = datetime.now()
            self._update_lobby_node(5)
            self.draw_map()
            self.status_text.set("Gateway válaszolt a pingre")
        elif event == GatewayEvent.HELLO:
            node_id = message.get("node")
            if isinstance(node_id, int) and node_id in self.nodes:
                node = self.nodes[node_id]
                node["firmware"] = message.get("firmware", "UNKNOWN")
                node["communication_state"] = "ONLINE"
                node["online"] = True
                node["last_seen"] = time.monotonic()
                node["last_seen_wall"] = datetime.now()
                self._update_lobby_node(node_id)
                self.draw_map()
        elif event == GatewayEvent.NODE_STATUS:
            node_id = message.get("node")
            if isinstance(node_id, int) and node_id in self.nodes:
                node = self.nodes[node_id]
                node["communication_state"] = message.get("communication_state", "UNKNOWN")
                node["game_state"] = message.get("game_state", "UNKNOWN")
                node["online"] = node["communication_state"] == "ONLINE"
                node["rssi"] = message.get("rssi")
                node["last_seen"] = time.monotonic()
                node["last_seen_wall"] = datetime.now()
                node["paradox"] = node["game_state"] == "PARADOX"
                self._update_lobby_node(node_id)
                self.draw_map()
        elif event == GatewayEvent.PACKET:
            self._handle_packet(message)
        elif event == GatewayEvent.PACKET_RESULT:
            self._handle_packet_result(message)
        elif event == GatewayEvent.MISSION_STATE:
            self._handle_mission_state(message)
        elif event == GatewayEvent.MISSION_RESULT:
            self._handle_mission_result(message)
        elif event == "ir_config_result":
            status = message.get("status", "UNKNOWN")
            self.ir_config_ready = status == "OK"
            self.status_text.set(f"IR config: {status}")
            messagebox.showinfo("IR calibration", f"Gateway result: {status}")
        elif event == "ir_calibration":
            status = message.get("status", "UNKNOWN")
            self.ir_capture_active = status in ("CAPTURE_STARTED", "IR_CAPTURE_READY")
            self.ir_capture_button.configure(text="STOP CAPTURE" if self.ir_capture_active else "START CAPTURE")
            self.status_text.set(f"IR capture: {status}")
        elif event == "ir_action_result":
            self.status_text.set(f"IR {message.get('category', 'UNKNOWN')}: {message.get('status', 'UNKNOWN')}")
        elif event == GatewayEvent.IR_SAMPLE:
            self._handle_ir_sample(message)
        elif event == "mesh_test_node":
            node_id = message.get("node")
            if isinstance(node_id, int):
                self.test_results[node_id] = message
                self.nodes[node_id]["online"] = message.get("status") == "PASS"
                self.nodes[node_id]["rssi"] = message.get("rssi")
                self.nodes[node_id]["last_seen"] = time.monotonic()
                self._update_test_row(node_id, message)
                self.draw_map()
        elif event == "mesh_test_summary":
            self.test_running = False
            self.mesh_ready = message.get("status") == "PASS" and message.get("tft_init") == "PASS"
            self.mesh_result.configure(text=f"MESH  {message.get('status', '--')}     TFT INIT  {message.get('tft_init', '--')}     IR PATH  SEE TEST")
            self.status_text.set(f"Mesh teszt: {message.get('online', 0)} / {message.get('expected', 4)} node online")
        elif event == "ir_test":
            status = message.get("status", "--")
            self.mesh_result.configure(text=f"IR PATH  {status}")
            self.status_text.set(f"IR útvonal teszt: {status}")
        elif event == "ir_state":
            level = message.get("level", "UNKNOWN")
            self.ir_verified_at = time.monotonic()
            self.ir_state_text.set(f"IR INPUT  /  {level}")
            self.status_text.set(f"IR_GATE input: {level} ({message.get('rssi', '?')} dBm)")
        elif "error" in message:
            self.status_text.set(f"Gateway hiba: {message['error']}")
        self._append_log(message)

    def _update_lobby_node(self, node_id: int) -> None:
        node = self.nodes[node_id]
        seen_at = node.get("last_seen_wall")
        last_seen = seen_at.strftime("%H:%M:%S") if seen_at is not None else "--"
        if str(node_id) in self.node_tree.get_children(""):
            self.node_tree.item(str(node_id), values=(
                node_id, node["role"], node["communication_state"], node["game_state"],
                node["firmware"], last_seen,
            ))
        online = sum(self.nodes[item]["online"] for item in (0, 1, 3, 4, 5))
        self.lobby_ready_text.configure(text=f"Mesh nodes online: {online} / 5    IR remote: standalone")

    def _handle_packet_result(self, event: dict[str, Any]) -> None:
        delivery = event.get("delivery_status", "UNKNOWN")
        if delivery == "ACK":
            self.status_text.set(f"Packet {event.get('logical_packet_id')} kézbesítve / ACK")
        elif delivery in ("LOST", "INVALID", "DUPLICATE"):
            self.status_text.set(f"Packet eredmény: {delivery}")

    def _handle_mission_state(self, event: dict[str, Any]) -> None:
        state = event.get("state", "UNKNOWN")
        event_type = event.get("event_type", state)
        self.mission_state_text.set(state)
        delivered = event.get("delivered_packets", 0)
        required = event.get("required_packets", self.required_packets.get())
        faults = event.get("active_faults", 0)
        remaining = event.get("remaining_seconds")
        time_text = "" if remaining is None else f"     TIME {int(remaining) // 60:02}:{int(remaining) % 60:02}"
        self.mission_progress_text.set(f"PACKETS {delivered} / {required}     ERRORS {faults}{time_text}")
        self.status_text.set(f"Mission {event.get('mission_id', '--')}: {event_type} / {event.get('detail', '')}")
        node_id = event.get("node")
        if isinstance(node_id, int) and node_id in self.nodes:
            node = self.nodes[node_id]
            if event.get("node_state") is not None:
                node["game_state"] = event["node_state"]
            elif event_type == "NODE_FAULT":
                node["game_state"] = event.get("detail", "PARADOX")
            elif event_type == "NODE_RESTORED":
                node["game_state"] = "NORMAL"
            node["paradox"] = node["game_state"] in {"INVERSE", "PARADOX", "LOSS", "DELAY", "DUPLICATE", "INVALID_PAYLOAD"}
            self._update_lobby_node(node_id)
        if event_type == "ERROR":
            self.status_text.set(f"Mission start rejected: {event.get('detail', 'UNKNOWN')}")

    def _handle_mission_result(self, result: dict[str, Any]) -> None:
        state = result.get("state", "MISSION_FAILED")
        self.mission_state_text.set(state)
        self.mission_progress_text.set(
            f"PACKETS {result.get('delivered_packets', 0)} / {result.get('required_packets', 0)}"
            f"     SCORE {result.get('score', 0)} / 100"
        )
        self.status_text.set(
            f"{state}: {result.get('reason', '')} / score {result.get('score', 0)}"
            f" / time {result.get('elapsed_seconds', 0)}s"
            f" / failed {result.get('failed_packets', 0)} / retries {result.get('retries', 0)}"
            f" / repairs {result.get('faults_repaired', 0)}/{result.get('faults_total', 0)}"
        )

    def _handle_ir_sample(self, event: dict[str, Any]) -> None:
        self.ir_raw_text.set(
            f"RAW {event.get('raw_level', '--')}    ACTIVE {event.get('active', '--')}    "
            f"PULSE {event.get('pulse_ms', '--')} ms    GAP {event.get('gap_ms', '--')} ms    "
            f"GROUP {event.get('group_active_ms', '--')} ms    CATEGORY {event.get('category', '--')}"
        )
        self.ir_measurement_text.set(event.get("reason", event.get("category", "IR SAMPLE")))
        self.status_text.set(f"IR: {event.get('category', 'UNKNOWN')} / {event.get('reason', '')}")

    def _update_test_row(self, node_id: int, result: dict[str, Any]) -> None:
        if str(node_id) not in self.test_tree.get_children(""):
            return
        self.test_tree.item(str(node_id), values=(
            node_id,
            result.get("role", NODE_ROLES.get(node_id, "?")),
            result.get("status", "--"),
            result.get("display", "N/A"),
            result.get("rssi", "--"),
        ))

    def _handle_packet(self, packet: dict[str, Any]) -> None:
        node_id = packet.get("src")
        if isinstance(node_id, int) and node_id in self.nodes:
            node = self.nodes[node_id]
            node["online"] = True
            node["rssi"] = packet.get("rssi")
            node["last_seen"] = time.monotonic()
            node["last_seen_wall"] = datetime.now()
            node["communication_state"] = "ONLINE"
            direction = packet.get("dir")
            if direction in (-1, 1):
                node["direction"] = direction
            if packet.get("type") == 4:
                node["paradox"] = True
            self.draw_map()

    def _append_log(self, message: dict[str, Any]) -> None:
        timestamp = datetime.now().strftime("%H:%M:%S")
        record = dict(message)
        record["timestamp"] = timestamp
        record["delivery_status"] = packet_status(record)
        self.packet_records.insert(0, record)
        del self.packet_records[500:]
        self._render_packet_log()

    def _render_packet_log(self) -> None:
        if not hasattr(self, "packet_tree"):
            return
        self.packet_tree.delete(*self.packet_tree.get_children())
        type_filter = self.packet_type_filter.get()
        status_filter = self.packet_status_filter.get()
        node_filter = self.packet_node_filter.get()
        for record in self.packet_records:
            if type_filter != "ALL" and str(record.get("type", "")) != type_filter:
                continue
            if status_filter != "ALL" and record.get("delivery_status") != status_filter:
                continue
            if node_filter != "ALL" and node_filter not in (str(record.get("src", "")), str(record.get("dst", ""))):
                continue
            payload = record.get("payload", record.get("detail", ""))
            if not isinstance(payload, str):
                payload = json.dumps(payload, ensure_ascii=False)
            values = (
                record.get("timestamp", ""), record.get("id", ""),
                record.get("logical_packet_id", ""), record.get("mission_id", ""),
                record.get("src", ""), record.get("dst", ""), record.get("type", record.get("event", "")),
                record.get("delivery_status", "UNKNOWN"), record.get("retry_count", ""),
                record.get("latency_ms", ""), record.get("rssi", ""), payload,
            )
            self.packet_tree.insert("", "end", values=values)

    def _refresh_node_states(self) -> None:
        now = time.monotonic()
        for node_id in MESH_NODE_IDS:
            if node_id == 5:
                self.nodes[node_id]["online"] = self.gateway_ready
                self.nodes[node_id]["communication_state"] = "ONLINE" if self.gateway_ready else "OFFLINE"
                self._update_lobby_node(node_id)
                continue
            last_seen = self.nodes[node_id]["last_seen"]
            if last_seen is not None and now - last_seen > 6:
                self.nodes[node_id]["online"] = False
                self.nodes[node_id]["communication_state"] = "OFFLINE"
                self._update_lobby_node(node_id)
        self.draw_map()
        self.root.after(500, self._refresh_node_states)

    def draw_map(self) -> None:
        if not hasattr(self, "map_canvas"):
            return
        canvas = self.map_canvas
        canvas.delete("all")
        width = max(canvas.winfo_width(), 300)
        height = max(canvas.winfo_height(), 240)
        positions = {
            0: (0.21, 0.25),
            1: (0.79, 0.25),
            2: (0.50, 0.13),
            3: (0.21, 0.75),
            4: (0.79, 0.75),
            5: (0.50, 0.50),
        }
        points = {node_id: (int(x * width), int(y * height)) for node_id, (x, y) in positions.items()}
        x2, y2 = points[2]
        x4, y4 = points[4]
        canvas.create_line(x2, y2 + 8, x4, y4 - 25, fill="#544d38", width=1, dash=(2, 5))
        canvas.create_text((x2 + x4) // 2 + 18, (y2 + y4) // 2, text="IR", fill=COLORS["yellow"], font=("Cascadia Mono", 8))
        canvas.create_text(width // 2, height - 10, text="ESP-NOW FLOOD  /  DIRECT PEERS NOT ASSUMED", fill=COLORS["muted"], font=("Cascadia Mono", 7))

        online_count = sum(bool(self.nodes[node_id]["online"]) for node_id in MESH_NODE_IDS)
        if hasattr(self, "online_label"):
            self.online_label.configure(text=f"{online_count} / 5 ONLINE")
        for node_id, (x, y) in points.items():
            node = self.nodes[node_id]
            if node_id == 2:
                color = COLORS["yellow"]
                status = "IR TX / STANDALONE"
            elif node.get("paradox"):
                color = COLORS["red"]
                status = node["game_state"]
            elif node["online"]:
                color = COLORS["lime"] if node["direction"] > 0 else COLORS["red"]
                status = "ONLINE"
            else:
                color = COLORS["offline"]
                status = "OFFLINE"
            radius = 17 if node_id == 5 else (13 if node_id != 2 else 9)
            canvas.create_oval(x - radius - 5, y - radius - 5, x + radius + 5, y + radius + 5,
                               outline=color if node["online"] else COLORS["line"], width=1)
            canvas.create_oval(x - radius, y - radius, x + radius, y + radius,
                               fill=COLORS["panel_alt"], outline=color, width=2)
            canvas.create_text(x, y, text=str(node_id), fill=color, font=("Cascadia Mono", 10, "bold"))
            canvas.create_text(x, y + radius + 17, text=node["role"], fill=COLORS["text"], font=("Cascadia Mono", 8))
            details = status
            if node_id in MESH_NODE_IDS and node["rssi"] is not None and node_id != 5:
                details += f"  {node['rssi']} dBm"
            canvas.create_text(x, y + radius + 31, text=details, fill=COLORS["muted"], font=("Segoe UI", 8))

    def clear_log(self) -> None:
        self.packet_records.clear()
        for item in self.packet_tree.get_children():
            self.packet_tree.delete(item)

    def close(self) -> None:
        if self.worker is not None:
            self.worker.close()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    if serial is None:
        root.withdraw()
        messagebox.showerror(
            "Dependency missing",
            "A pyserial nincs telepítve. A projekt mappájában futtasd:\n\n"
            "python -m pip install -r requirements.txt",
        )
        root.destroy()
        return
    TenetControlApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()