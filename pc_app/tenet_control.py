from __future__ import annotations

import json
import queue
import threading
import time
import tkinter as tk
from datetime import datetime
from tkinter import messagebox, ttk
from typing import Any

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


def encode_command(command: str, **values: Any) -> bytes:
    """Encode one newline-delimited command in the Gateway's JSON format."""
    payload = {"cmd": command, **values}
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")


def decode_line(line: bytes | str) -> dict[str, Any] | None:
    """Decode one Gateway JSON line; ignore malformed/non-object lines."""
    try:
        text = line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line
        value = json.loads(text.strip())
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None
    return value if isinstance(value, dict) else None


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
            except Exception as error:  # serial driver exceptions vary by OS
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
        self.root.title("TENET MESH  /  Control Room")
        self.root.geometry("1220x820")
        self.root.minsize(980, 680)
        self.root.configure(bg=COLORS["bg"])

        self.events: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.worker: SerialWorker | None = None
        self.nodes: dict[int, dict[str, Any]] = {
            node_id: {
                "role": NODE_ROLES[node_id],
                "online": node_id == 2,
                "rssi": None,
                "direction": 1,
                "paradox": False,
                "last_seen": None,
            }
            for node_id in NODE_ROLES
        }
        self.nodes[2]["online"] = False
        self.gateway_connected = False
        self.test_results: dict[int, dict[str, Any]] = {}
        self.test_running = False
        self.mesh_ready = False
        self.game_running = False
        self.game_deadline = 0.0
        self.countdown_seconds = tk.IntVar(value=60)
        self.difficulty = tk.StringVar(value="Normal")
        self.target_node = tk.IntVar(value=0)
        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="2000000")
        self.connection_text = tk.StringVar(value="DISCONNECTED")
        self.status_text = tk.StringVar(value="Gatewayhez kapcsolódás szükséges")
        self.ir_state_text = tk.StringVar(value="IR INPUT  /  UNKNOWN")

        self._configure_styles()
        self._build_ui()
        self.refresh_ports()
        self.root.after(80, self._drain_events)
        self.root.after(500, self._refresh_node_states)
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
        self.game_tab = ttk.Frame(self.notebook, style="Panel.TFrame", padding=16)
        self.notebook.add(self.test_tab, text="TEST MODE")
        self.notebook.add(self.game_tab, text="GAME MODE")
        self._build_test_tab()
        self._build_game_tab()

        log_panel = ttk.Frame(self.root, style="Panel.TFrame", padding=12)
        log_panel.grid(row=3, column=0, sticky="nsew", padx=18, pady=(12, 0))
        log_panel.grid_columnconfigure(0, weight=1)
        log_panel.grid_rowconfigure(1, weight=1)
        log_header = ttk.Frame(log_panel, style="Panel.TFrame")
        log_header.grid(row=0, column=0, sticky="ew", pady=(0, 8))
        ttk.Label(log_header, text="PACKET / EVENT STREAM", style="PanelTitle.TLabel").pack(side="left")
        ttk.Button(log_header, text="CLEAR", command=self.clear_log).pack(side="right")
        columns = ("time", "src", "dst", "type", "id", "dir", "rssi", "payload")
        self.packet_tree = ttk.Treeview(log_panel, columns=columns, show="headings", height=8)
        widths = {"time": 88, "src": 42, "dst": 42, "type": 54, "id": 60, "dir": 45, "rssi": 58, "payload": 280}
        for column, label in zip(columns, ("TIME", "SRC", "DST", "TYPE", "ID", "DIR", "RSSI", "PAYLOAD")):
            self.packet_tree.heading(column, text=label)
            self.packet_tree.column(column, width=widths[column], anchor="w", stretch=column == "payload")
        self.packet_tree.grid(row=1, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(log_panel, orient="vertical", command=self.packet_tree.yview)
        scrollbar.grid(row=1, column=1, sticky="ns")
        self.packet_tree.configure(yscrollcommand=scrollbar.set)

        footer = tk.Frame(self.root, bg=COLORS["bg"], padx=20, pady=8)
        footer.grid(row=4, column=0, sticky="ew")
        tk.Label(footer, textvariable=self.status_text, bg=COLORS["bg"], fg=COLORS["muted"], font=("Segoe UI", 9)).pack(side="left")
        tk.Label(footer, text="ESP-NOW  /  CHANNEL 1  /  JSON-LINE", bg=COLORS["bg"], fg=COLORS["muted"], font=("Cascadia Mono", 8)).pack(side="right")

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
        self.connection_text.set("GATEWAY ONLINE" if connected else "DISCONNECTED")
        self.link_led.itemconfigure(self.link_dot, fill=COLORS["lime"] if connected else COLORS["offline"])
        self.connect_button.configure(text="DISCONNECT" if connected else "CONNECT", style="Danger.TButton" if connected else "Primary.TButton")
        if connected:
            self.nodes[5]["online"] = True
            self.nodes[5]["last_seen"] = time.monotonic()
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

    def start_round(self) -> None:
        try:
            seconds = int(self.countdown_seconds.get())
            if not 1 <= seconds <= 3600:
                raise ValueError
        except (ValueError, tk.TclError):
            messagebox.showwarning("Countdown", "1 és 3600 másodperc közötti értéket adj meg.")
            return
        if not self.gateway_connected:
            self._send("start", seconds=seconds)
            return
        if not self.mesh_ready:
            messagebox.showwarning("Mesh preflight", "Előbb futtasd sikeresen a mesh tesztet (4/4 node és TFT init PASS).")
            return
        if not self._send("start", seconds=seconds, difficulty=self.difficulty.get().lower()):
            return
        self.game_running = False
        self.game_clock.configure(text="ARMING ROUND")
        self.status_text.set("Gateway visszaigazolására vár")

    def _tick_game_clock(self) -> None:
        if not self.game_running:
            return
        remaining = max(0, int(self.game_deadline - time.monotonic() + 0.999))
        self.game_clock.configure(text=f"LIVE  /  00:{remaining:02d}")
        if remaining == 0:
            self.game_running = False
            self.game_clock.configure(text="ROUND CHECK  /  00:00")
            self.status_text.set("A Gateway kiértékeli a kör eredményét")
        else:
            self.root.after(200, self._tick_game_clock)

    def invert_time(self) -> None:
        self._send("invert")

    def send_repair(self) -> None:
        key = self.key_entry.get().strip()
        if not key:
            messagebox.showwarning("Repair key", "Add meg a kulcsot a parancs elküldéséhez.")
            return
        self._send("repair", target=self.target_node.get(), key=key)
        self.key_entry.delete(0, "end")

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

    def _handle_line(self, raw_line: bytes) -> None:
        message = decode_line(raw_line)
        if message is None:
            self.status_text.set("Nem JSON Gateway sor érkezett")
            return
        event = message.get("event")
        if event == "gateway_ready":
            self.status_text.set("Gateway válaszolt a pingre")
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
            self.ir_state_text.set(f"IR INPUT  /  {level}")
            self.status_text.set(f"IR_GATE input: {level} ({message.get('rssi', '?')} dBm)")
        elif event == "game_state":
            self._handle_game_state(message)
        elif event is None and "src" in message:
            self._handle_packet(message)
        elif "error" in message:
            self.status_text.set(f"Gateway hiba: {message['error']}")
        self._append_log(message)

    def _handle_game_state(self, event: dict[str, Any]) -> None:
        state = event.get("state")
        node_id = event.get("node")
        if state == "STARTED":
            for node in self.nodes.values():
                node["paradox"] = False
            self.game_running = True
            self.game_deadline = time.monotonic() + int(event.get("seconds", self.countdown_seconds.get()))
            self.status_text.set(f"Kör elindult ({event.get('detail', 'normal')})")
            self._tick_game_clock()
        elif state == "NODE_PARADOX" and isinstance(node_id, int) and node_id in self.nodes:
            self.nodes[node_id]["paradox"] = True
            self.status_text.set(f"PARADOX: N{node_id} / hibakód: {event.get('detail', '--')}")
            self.draw_map()
        elif state == "NODE_REPAIRED" and isinstance(node_id, int) and node_id in self.nodes:
            self.nodes[node_id]["paradox"] = False
            self.status_text.set(f"N{node_id} javítva")
            self.draw_map()
        elif state == "REPAIR_REJECTED":
            self.status_text.set(f"REPAIR elutasítva / 3 mp zár: N{node_id}")
        elif state in ("VICTORY", "DEFEAT"):
            self.game_running = False
            self.game_clock.configure(text="DEFUSED" if state == "VICTORY" else "DEFEAT")
            self.status_text.set(f"{state}: {event.get('detail', '')}")
            if state == "VICTORY":
                for node in self.nodes.values():
                    node["paradox"] = False
            self.draw_map()

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
            direction = packet.get("dir")
            if direction in (-1, 1):
                node["direction"] = direction
            if packet.get("type") == 4:
                node["paradox"] = True
            self.draw_map()
        if packet.get("type") == 3:
            for node in self.nodes.values():
                if node["role"] != "IR_REMOTE":
                    node["direction"] *= -1

    def _append_log(self, message: dict[str, Any]) -> None:
        timestamp = datetime.now().strftime("%H:%M:%S")
        if "src" in message:
            values = (
                timestamp,
                message.get("src", ""),
                message.get("dst", ""),
                message.get("type", ""),
                message.get("id", ""),
                message.get("dir", ""),
                message.get("rssi", ""),
                str(message.get("payload", "")),
            )
        else:
            values = (timestamp, "-", "-", "EVT", "-", "-", "-", json.dumps(message, ensure_ascii=False))
        self.packet_tree.insert("", 0, values=values)
        children = self.packet_tree.get_children()
        for item in children[500:]:
            self.packet_tree.delete(item)

    def _refresh_node_states(self) -> None:
        now = time.monotonic()
        for node_id in MESH_NODE_IDS:
            if node_id == 5:
                self.nodes[node_id]["online"] = self.gateway_connected
                continue
            last_seen = self.nodes[node_id]["last_seen"]
            if last_seen is not None and now - last_seen > 6:
                self.nodes[node_id]["online"] = False
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
        for node_id in (0, 1, 3, 4):
            x, y = points[node_id]
            gx, gy = points[5]
            canvas.create_line(x, y, gx, gy, fill=COLORS["line"], width=1, dash=(4, 5))
        x2, y2 = points[2]
        x4, y4 = points[4]
        canvas.create_line(x2, y2 + 8, x4, y4 - 25, fill="#544d38", width=1, dash=(2, 5))
        canvas.create_text((x2 + x4) // 2 + 18, (y2 + y4) // 2, text="IR", fill=COLORS["yellow"], font=("Cascadia Mono", 8))

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
                status = "PARADOX"
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