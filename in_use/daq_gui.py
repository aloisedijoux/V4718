#!/usr/bin/env python3
"""
daq_gui.py -- Simple Tkinter control panel for the V1290N TDC DAQ.

Thin GUI wrapper around the existing command-line tools in this folder
(module_reset, enable_channels*, config_trigger_matching,
set_continuous_storage, enable_trigger_subtraction, clear_buffer,
decode_status, read_channel_pattern, software_trigger,
read_output_buffer_blt, live_histogram_multi.py, live_sipm_matrix.py). It does not talk to the
hardware directly -- it just runs those binaries with the arguments picked
in the form and shows their output in a log panel, so anything documented
in howTo.md / procedure.md still applies.

Run:
    cd in_use
    python3 daq_gui.py

Requires: python3 with tkinter (stdlib) and, for the live histogram
display, matplotlib (same requirement as live_histogram_multi.py).
"""
import os
import sys
import signal
import subprocess
import threading
import queue
import time
from datetime import datetime

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(BASE_DIR, "gui_logs")

if BASE_DIR not in sys.path:
    sys.path.insert(0, BASE_DIR)
import sipm_channel_map
import channel_list


def bin_path(name):
    return os.path.join(BASE_DIR, name)


class DaqGui:
    MODULE_N = "V1290N (16 ch)"
    MODULE_A = "V1290A (32 ch)"
    MODULE_BOTH = "Both (V1290N + V1290A)"
    MAX_CHANNELS = {MODULE_N: 16, MODULE_A: 32}


    def __init__(self, root):
        self.root = root
        root.title("V1290N TDC DAQ - Control Panel")
        root.geometry("1180x760")
        root.minsize(980, 620)
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.log_queue = queue.Queue()
        self.session_log_fh = None
        self.errlog_fh = None
        self.csv_fh = None
        self.acq_proc1 = None  # read_output_buffer_blt
        self.acq_live_procs = []  # live_histogram_multi.py / live_sipm_matrix.py (0, 1, or both, run simultaneously)

        self._open_session_log()
        self._build_ui()
        self.poll_log_queue()
        self.check_acquisition()

    # ------------------------------------------------------------------ #
    # Logging plumbing
    # ------------------------------------------------------------------ #
    def _open_session_log(self):
        os.makedirs(LOG_DIR, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = os.path.join(LOG_DIR, f"daq_gui_{ts}.txt")
        self.session_log_path = path
        try:
            self.session_log_fh = open(path, "a", buffering=1)
        except OSError:
            self.session_log_fh = None

    def log(self, msg):
        self.log_queue.put(msg)

    def poll_log_queue(self):
        drained = False
        while True:
            try:
                msg = self.log_queue.get_nowait()
            except queue.Empty:
                break
            drained = True
            now_ns = time.time_ns()
            sec, nsec = divmod(now_ns, 1_000_000_000)
            ts = datetime.fromtimestamp(sec).strftime("%H:%M:%S") + f".{nsec:09d}"
            line = f"[{ts}] {msg}"
            self.log_text.insert(tk.END, line + "\n")
            if self.session_log_fh:
                try:
                    self.session_log_fh.write(line + "\n")
                except OSError:
                    pass
        if drained and self.autoscroll_var.get():
            self.log_text.see(tk.END)
        self.root.after(150, self.poll_log_queue)

    # ------------------------------------------------------------------ #
    # UI layout
    # ------------------------------------------------------------------ #
    def _build_ui(self):
        outer = ttk.Panedwindow(self.root, orient=tk.HORIZONTAL)
        outer.pack(fill=tk.BOTH, expand=True)

        left_container = ttk.Frame(outer)
        right = ttk.Frame(outer, padding=8)
        outer.add(left_container, weight=0)
        outer.add(right, weight=1)

        left = self._build_scrollable_left(left_container)

        self._build_connection(left)
        self._build_config(left)
        self._build_verify(left)
        self._build_acquisition(left)
        self._build_log(right)
        self._on_module_type_change()

    def _build_scrollable_left(self, parent):
        """Wrap the left control panel (connection/config/verify/acquisition, stacked
        vertically) in a canvas + scrollbar so every section -- including the Start/Stop
        acquisition buttons at the bottom -- stays reachable even when the window is
        shorter than the panel's natural height."""
        canvas = tk.Canvas(parent, borderwidth=0, highlightthickness=0)
        vscroll = ttk.Scrollbar(parent, orient=tk.VERTICAL, command=canvas.yview)
        canvas.configure(yscrollcommand=vscroll.set)
        vscroll.pack(side=tk.RIGHT, fill=tk.Y)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        inner = ttk.Frame(canvas, padding=8)
        inner_id = canvas.create_window((0, 0), window=inner, anchor="nw")

        def sync_scrollregion(_event=None):
            canvas.configure(scrollregion=canvas.bbox("all"))
            canvas.configure(width=inner.winfo_reqwidth())
        inner.bind("<Configure>", sync_scrollregion)

        def on_canvas_configure(event):
            if event.width > inner.winfo_reqwidth():
                canvas.itemconfigure(inner_id, width=event.width)
        canvas.bind("<Configure>", on_canvas_configure)

        def on_mousewheel(event):
            if event.num == 5 or event.delta < 0:
                canvas.yview_scroll(1, "units")
            elif event.num == 4 or event.delta > 0:
                canvas.yview_scroll(-1, "units")

        def bind_mousewheel(_event):
            canvas.bind_all("<MouseWheel>", on_mousewheel)
            canvas.bind_all("<Button-4>", on_mousewheel)
            canvas.bind_all("<Button-5>", on_mousewheel)

        def unbind_mousewheel(_event):
            canvas.unbind_all("<MouseWheel>")
            canvas.unbind_all("<Button-4>")
            canvas.unbind_all("<Button-5>")

        canvas.bind("<Enter>", bind_mousewheel)
        canvas.bind("<Leave>", unbind_mousewheel)

        return inner

    def _build_connection(self, parent):
        f = ttk.LabelFrame(parent, text="Connection", padding=8)
        f.pack(fill=tk.X, pady=(0, 8))

        self.pid_var = tk.StringVar(value="192.168.1.254")
        self.base_var = tk.StringVar(value="0x03E00000")
        ttk.Label(f, text="PID (USB) or IP address (ETH):").grid(row=0, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.pid_var, width=16).grid(row=0, column=1, sticky="w", padx=4)
        ttk.Label(f, text="Base address (hex):").grid(row=1, column=0, sticky="w")
        ttk.Entry(f, textvariable=self.base_var, width=16).grid(row=1, column=1, sticky="w", padx=4)
        ttk.Label(f, text="(numeric = USB PID e.g. 64324; anything else = ETH IP e.g. 192.168.1.254)",
                  font=("TkDefaultFont", 8)).grid(row=2, column=0, columnspan=2, sticky="w")

        ttk.Label(f, text="Module model:").grid(row=3, column=0, sticky="w", pady=(6, 0))
        self.module_type_var = tk.StringVar(value=self.MODULE_N)
        module_combo = ttk.Combobox(f, textvariable=self.module_type_var, state="readonly", width=24,
                                     values=[self.MODULE_N, self.MODULE_A, self.MODULE_BOTH])
        module_combo.grid(row=3, column=1, sticky="w", padx=4, pady=(6, 0))
        module_combo.bind("<<ComboboxSelected>>", lambda e: self._on_module_type_change())

        self.base2_label = ttk.Label(f, text="Base address 2 (hex, V1290A):")
        self.base_var2 = tk.StringVar(value="0x03000000")
        self.base2_entry = ttk.Entry(f, textvariable=self.base_var2, width=16)
        self.base2_label.grid(row=4, column=0, sticky="w")
        self.base2_entry.grid(row=4, column=1, sticky="w", padx=4)

        self.channels_hint_var = tk.StringVar(value="")
        ttk.Label(f, textvariable=self.channels_hint_var,
                  font=("TkDefaultFont", 8)).grid(row=5, column=0, columnspan=2, sticky="w")
        self._on_module_type_change()

    def _build_config(self, parent):
        f = ttk.LabelFrame(parent, text="Module configuration", padding=8)
        f.pack(fill=tk.X, pady=(0, 8))

        self.channels_label_var = tk.StringVar(value="Channels (comma-sep, blank = all 16):")
        ttk.Label(f, textvariable=self.channels_label_var).grid(row=0, column=0, columnspan=2, sticky="w")
        self.channels_var = tk.StringVar(value="")
        ttk.Entry(f, textvariable=self.channels_var, width=24).grid(row=1, column=0, columnspan=2, sticky="we", pady=(0, 6))

        self.mode_var = tk.StringVar(value="trigger")
        ttk.Radiobutton(f, text="Trigger matching", value="trigger", variable=self.mode_var,
                         command=self._on_mode_change).grid(row=2, column=0, sticky="w")
        ttk.Radiobutton(f, text="Continuous storage", value="continuous", variable=self.mode_var,
                         command=self._on_mode_change).grid(row=2, column=1, sticky="w")

        tm = ttk.Frame(f)
        tm.grid(row=3, column=0, columnspan=2, sticky="we", pady=4)
        self.width_var = tk.StringVar(value="2000")
        self.tw_offset_var = tk.StringVar(value="-2000")
        self.smargin_var = tk.StringVar(value="8")
        self.rmargin_var = tk.StringVar(value="4")
        self._tm_entries = []
        for i, (label, var) in enumerate([
            ("Window width", self.width_var),
            ("Window offset", self.tw_offset_var),
            ("Search margin", self.smargin_var),
            ("Reject margin", self.rmargin_var),
        ]):
            ttk.Label(tm, text=label + ":").grid(row=i, column=0, sticky="w")
            e = ttk.Entry(tm, textvariable=var, width=10)
            e.grid(row=i, column=1, sticky="w", padx=4)
            self._tm_entries.append(e)
        ttk.Label(f, text="(units: 25 ns ticks, per config_trigger_matching.c)",
                  font=("TkDefaultFont", 8)).grid(row=4, column=0, columnspan=2, sticky="w")

        self.subtraction_var = tk.BooleanVar(value=True)
        self._sub_check = ttk.Checkbutton(f, text="Enable trigger-time subtraction (SUB_TRG)",
                                           variable=self.subtraction_var)
        self._sub_check.grid(row=5, column=0, columnspan=2, sticky="w", pady=(4, 6))

        self.tdc_header_off_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="Disable TDC Header/Trailer (CONFIRMED BROKEN on this module, 05/10 -- do not use)",
                         variable=self.tdc_header_off_var).grid(
            row=6, column=0, columnspan=2, sticky="w", pady=(0, 6))

        btns = ttk.Frame(f)
        btns.grid(row=7, column=0, columnspan=2, sticky="we")
        ttk.Button(btns, text="Reset module", command=self.on_reset_module).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(btns, text="Apply configuration", command=self.on_apply_config).pack(side=tk.LEFT, padx=4)

    def _on_mode_change(self):
        state = "normal" if self.mode_var.get() == "trigger" else "disabled"
        for e in self._tm_entries:
            e.configure(state=state)
        self._sub_check.configure(state=state)

    # ------------------------------------------------------------------ #
    # Module model (V1290N / V1290A / both) plumbing
    # ------------------------------------------------------------------ #
    def _on_module_type_change(self):
        both = self.module_type_var.get() == self.MODULE_BOTH
        state = "normal" if both else "disabled"
        self.base2_label.configure(state=state)
        self.base2_entry.configure(state=state)
        if hasattr(self, "acq_target_combo"):
            if both:
                self.acq_target_label.grid()
                self.acq_target_combo.grid()
            else:
                self.acq_target_label.grid_remove()
                self.acq_target_combo.grid_remove()

        mtype = self.module_type_var.get()
        if mtype == self.MODULE_N:
            blank_hint = "blank = all 16"
        elif mtype == self.MODULE_A:
            blank_hint = "blank = all 32"
        else:
            blank_hint = "blank = all (16 on the N, 32 on the A)"
        if hasattr(self, "channels_label_var"):
            self.channels_label_var.set(f"Channels (comma-sep, ranges ok e.g. 0-5,16, {blank_hint}):")
        self.channels_hint_var.set(
            f"Selected model: {mtype}"
            + (" -- channel numbers are validated per module." if mtype == self.MODULE_BOTH else "")
        )

    def _targets(self):
        """Return the list of (module_label, base_address_str, max_channels) to act on,
        given the selected module model. Everything else in the GUI still only knows how
        to talk to one base address per call, so 'Both' simply repeats each action for
        both base addresses."""
        base = self.base_var.get().strip()
        mtype = self.module_type_var.get()
        if mtype == self.MODULE_A:
            return [("V1290A", base, self.MAX_CHANNELS[self.MODULE_A])]
        if mtype == self.MODULE_BOTH:
            base2 = self.base_var2.get().strip()
            return [
                ("V1290N", base, self.MAX_CHANNELS[self.MODULE_N]),
                ("V1290A", base2, self.MAX_CHANNELS[self.MODULE_A]),
            ]
        return [("V1290N", base, self.MAX_CHANNELS[self.MODULE_N])]

    def _acq_target(self):
        """(module_label, base_address_str) the acquisition reads from. The reader handles a
        single output buffer, so with 'Both' the user picks which module."""
        targets = self._targets()
        if len(targets) == 2:
            wanted = self.acq_target_var.get()
            for label, base, _ in targets:
                if label == wanted:
                    return label, base
        return targets[0][0], targets[0][1]

    def _window_ticks(self):
        """(width, offset) of the trigger window as ints (25 ns ticks), or None if invalid."""
        try:
            width = int(self.width_var.get().strip() or "2000")
            offset = int(self.tw_offset_var.get().strip() or "-2000")
        except ValueError:
            return None
        return width, offset

    def _build_verify(self, parent):
        f = ttk.LabelFrame(parent, text="Verify / utilities", padding=8)
        f.pack(fill=tk.X, pady=(0, 8))

        row1 = ttk.Frame(f)
        row1.pack(fill=tk.X)
        ttk.Button(row1, text="Decode status", command=self.on_decode_status).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(row1, text="Read channel pattern", command=self.on_read_pattern).pack(side=tk.LEFT, padx=4)
        ttk.Button(row1, text="Clear buffer", command=self.on_clear_buffer).pack(side=tk.LEFT, padx=4)

        row2 = ttk.Frame(f)
        row2.pack(fill=tk.X, pady=(6, 0))
        ttk.Label(row2, text="Software trigger  n=").pack(side=tk.LEFT)
        self.sw_n_var = tk.StringVar(value="1")
        ttk.Entry(row2, textvariable=self.sw_n_var, width=5).pack(side=tk.LEFT, padx=(2, 8))
        ttk.Label(row2, text="delay(us)=").pack(side=tk.LEFT)
        self.sw_delay_var = tk.StringVar(value="0")
        ttk.Entry(row2, textvariable=self.sw_delay_var, width=6).pack(side=tk.LEFT, padx=(2, 8))
        ttk.Button(row2, text="Fire", command=self.on_software_trigger).pack(side=tk.LEFT)

    def _build_acquisition(self, parent):
        f = ttk.LabelFrame(parent, text="Acquisition", padding=8)
        f.pack(fill=tk.X, pady=(0, 8))

        ttk.Label(f, text="Output folder:").grid(row=0, column=0, sticky="w")
        self.outdir_var = tk.StringVar(value=BASE_DIR)
        ttk.Entry(f, textvariable=self.outdir_var, width=28).grid(row=0, column=1, sticky="we", padx=4)
        ttk.Button(f, text="Browse...", command=self.on_browse_outdir).grid(row=0, column=2, sticky="w")

        ttk.Label(f, text="Run name:").grid(row=1, column=0, sticky="w")
        self.runname_var = tk.StringVar(value="run")
        ttk.Entry(f, textvariable=self.runname_var, width=28).grid(row=1, column=1, sticky="we", padx=4, pady=(4, 0))

        ttk.Label(f, text="BLT words per read:").grid(row=2, column=0, sticky="w")
        self.blt_var = tk.StringVar(value="4096")
        ttk.Entry(f, textvariable=self.blt_var, width=10).grid(row=2, column=1, sticky="w", padx=4, pady=(4, 0))

        ttk.Label(f, text="Stream offset (ns, -s):").grid(row=3, column=0, sticky="w")
        self.offset_var = tk.StringVar(value="-2000")
        ttk.Entry(f, textvariable=self.offset_var, width=10).grid(row=3, column=1, sticky="w", padx=4, pady=(4, 0))
        ttk.Label(f, text="(match trigger window offset above; ignored in continuous storage)",
                  font=("TkDefaultFont", 8)).grid(row=4, column=0, columnspan=3, sticky="w")

        self.acq_target_label = ttk.Label(f, text="Acquire from:")
        self.acq_target_var = tk.StringVar(value="V1290N")
        self.acq_target_combo = ttk.Combobox(f, textvariable=self.acq_target_var, state="readonly", width=8,
                                              values=["V1290N", "V1290A"])
        self.acq_target_label.grid(row=2, column=2, sticky="e", padx=(8, 2), pady=(4, 0))
        self.acq_target_combo.grid(row=2, column=3, sticky="w", pady=(4, 0))
        self._on_module_type_change()

        self.save_binary_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(f, text="Save raw binary (<run>.bin, replayable with decode_binary.py)",
                         variable=self.save_binary_var).grid(row=5, column=0, columnspan=3, sticky="w", pady=(6, 0))

        ttk.Label(f, text="Live view:").grid(row=6, column=0, sticky="w", pady=(6, 0))
        live_row = ttk.Frame(f)
        live_row.grid(row=6, column=1, columnspan=2, sticky="w", padx=4, pady=(6, 0))
        self.live_hist_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(live_row, text="Histogram", variable=self.live_hist_var).pack(side=tk.LEFT)
        self.live_matrix_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(live_row, text="SiPM matrix", variable=self.live_matrix_var,
                         command=self._on_live_view_change).pack(side=tk.LEFT, padx=(12, 0))

        self.sipm_row = ttk.Frame(f)
        self.sipm_row.grid(row=7, column=0, columnspan=3, sticky="we", pady=(4, 0))
        ttk.Label(self.sipm_row, text="C bars (blank=all C5-C13):").pack(side=tk.LEFT)
        self.sipm_c_var = tk.StringVar(value="")
        ttk.Entry(self.sipm_row, textvariable=self.sipm_c_var, width=14).pack(side=tk.LEFT, padx=(2, 10))
        ttk.Label(self.sipm_row, text="D bars (blank=all D4-D10):").pack(side=tk.LEFT)
        self.sipm_d_var = tk.StringVar(value="")
        ttk.Entry(self.sipm_row, textvariable=self.sipm_d_var, width=14).pack(side=tk.LEFT, padx=(2, 0))

        self.sipm_hint = ttk.Label(f, text="(fixed module #6/#7 cabling -- each bar sums both ends; see sipm_channel_map.py)",
                                    font=("TkDefaultFont", 8))
        self.sipm_hint.grid(row=8, column=0, columnspan=3, sticky="w")
        self._on_live_view_change()

        btns = ttk.Frame(f)
        btns.grid(row=9, column=0, columnspan=3, sticky="we", pady=(8, 0))
        self.start_btn = ttk.Button(btns, text="Start acquisition", command=self.on_start_acquisition)
        self.start_btn.pack(side=tk.LEFT, padx=(0, 4))
        self.stop_btn = ttk.Button(btns, text="Stop acquisition", command=self.on_stop_acquisition, state="disabled")
        self.stop_btn.pack(side=tk.LEFT, padx=4)
        ttk.Button(btns, text="Quick read (single pass)", command=self.on_quick_read).pack(side=tk.LEFT, padx=4)

        self.status_var = tk.StringVar(value="STOPPED")
        self.status_label = ttk.Label(f, textvariable=self.status_var, foreground="#a33")
        self.status_label.grid(row=10, column=0, columnspan=3, sticky="w", pady=(6, 0))

        f.columnconfigure(1, weight=1)

    def _on_live_view_change(self):
        if self.live_matrix_var.get():
            self.sipm_row.grid()
            self.sipm_hint.grid()
        else:
            self.sipm_row.grid_remove()
            self.sipm_hint.grid_remove()

    def _build_log(self, parent):
        f = ttk.LabelFrame(parent, text="Log", padding=8)
        f.pack(fill=tk.BOTH, expand=True)

        top = ttk.Frame(f)
        top.pack(fill=tk.X)
        ttk.Label(top, text=f"Session log file: {self.session_log_path}",
                  font=("TkDefaultFont", 8)).pack(side=tk.LEFT)
        self.autoscroll_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="Autoscroll", variable=self.autoscroll_var).pack(side=tk.RIGHT)
        ttk.Button(top, text="Clear", command=self.on_clear_log).pack(side=tk.RIGHT, padx=4)

        text_frame = ttk.Frame(f)
        text_frame.pack(fill=tk.BOTH, expand=True, pady=(4, 0))
        yscroll = ttk.Scrollbar(text_frame, orient=tk.VERTICAL)
        self.log_text = tk.Text(text_frame, wrap="none", height=30, yscrollcommand=yscroll.set,
                                 background="#111", foreground="#ddd", insertbackground="#ddd")
        yscroll.config(command=self.log_text.yview)
        yscroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.log(f"Ready. Working directory: {BASE_DIR}")

    def on_clear_log(self):
        self.log_text.delete("1.0", tk.END)

    # ------------------------------------------------------------------ #
    # Helpers to run the CLI tools
    # ------------------------------------------------------------------ #
    def _check_exe(self, path):
        if not os.path.isfile(path):
            self.log(f"ERROR: {os.path.basename(path)} not found in {BASE_DIR}. Compile it first (see howTo.md).")
            return False
        if not os.access(path, os.X_OK):
            self.log(f"ERROR: {os.path.basename(path)} is not executable (chmod +x {os.path.basename(path)}).")
            return False
        return True

    def run_sequence(self, cmds, tag=""):
        """Run a list of argv lists sequentially in a background thread, stop on first failure."""
        def worker():
            for args in cmds:
                if not self._check_exe(args[0]):
                    self.log(f"[{tag}] aborted.")
                    return
                self.log("$ " + " ".join(args))
                try:
                    proc = subprocess.run(args, cwd=BASE_DIR, capture_output=True, text=True, timeout=30)
                except Exception as e:
                    self.log(f"[{tag}] ERROR launching {args[0]}: {e}")
                    self.log(f"[{tag}] aborted.")
                    return
                for line in (proc.stdout or "").splitlines():
                    self.log(line)
                for line in (proc.stderr or "").splitlines():
                    self.log(line)
                if proc.returncode != 0:
                    self.log(f"[{tag}] FAILED: {os.path.basename(args[0])} exited with code {proc.returncode}")
                    self.log(f"[{tag}] aborted.")
                    return
            self.log(f"[{tag}] done.")
        threading.Thread(target=worker, daemon=True).start()

    def run_single(self, args, tag):
        self.run_sequence([args], tag=tag)

    # ------------------------------------------------------------------ #
    # Button callbacks -- configuration
    # ------------------------------------------------------------------ #
    def _refuse_if_acquiring(self, what):
        if self.acq_proc1 is not None:
            messagebox.showwarning(what, f"{what} would clear the module's data and reprogram it "
                                          "while an acquisition is running. Stop the acquisition first.")
            return True
        return False

    def on_reset_module(self):
        if self._refuse_if_acquiring("Reset module"):
            return
        if not messagebox.askyesno("Reset module",
                                    "This resets the module to power-on defaults "
                                    "(clears channels and trigger config). Continue?"):
            return
        pid = self.pid_var.get().strip()
        cmds = [[bin_path("module_reset"), pid, base] for _, base, _ in self._targets()]
        self.run_sequence(cmds, tag="Reset module")

    def on_apply_config(self):
        if self._refuse_if_acquiring("Apply configuration"):
            return
        if self.mode_var.get() == "trigger":
            win = self._window_ticks()
            if win is None or not all(v.get().strip().lstrip("-").isdigit()
                                       for v in (self.smargin_var, self.rmargin_var)):
                messagebox.showerror("Apply configuration",
                                      "Window width/offset and margins must be integers (25 ns ticks).")
                return
            if not -2048 <= win[1] <= 2047 or not 1 <= win[0] <= 4095:
                messagebox.showerror("Apply configuration",
                                      "Out of range (12-bit fields): width 1..4095, offset -2048..2047 ticks "
                                      "(1 tick = 25 ns).")
                return
        pid = self.pid_var.get().strip()
        channels = self.channels_var.get().strip()
        try:
            requested_chans = channel_list.parse_channel_list(channels) if channels else []
        except ValueError as e:
            messagebox.showerror("Apply configuration",
                                  f"Invalid channel list ({e}). Examples: 0,1,2,3 or 0-5,16.")
            return

        cmds = []
        for label, base, max_ch in self._targets():
            if requested_chans:
                in_range = [str(c) for c in requested_chans if 0 <= c < max_ch]
                skipped = [c for c in requested_chans if not (0 <= c < max_ch)]
                if skipped:
                    self.log(f"NOTE [{label}]: skipping channel(s) {', '.join(str(c) for c in skipped)} "
                              f"(out of range 0-{max_ch - 1} for this module).")
                if in_range:
                    cmds.append([bin_path("enable_channels_set"), pid, base] + in_range)
            else:
                cmds.append([bin_path("enable_channels"), pid, base])

            if self.tdc_header_off_var.get():
                cmds.append([bin_path("set_tdc_header"), pid, base, "off"])

            if self.mode_var.get() == "trigger":
                width = self.width_var.get().strip() or "2000"
                offset = self.tw_offset_var.get().strip() or "-2000"
                smargin = self.smargin_var.get().strip() or "8"
                rmargin = self.rmargin_var.get().strip() or "4"
                cmds.append([bin_path("config_trigger_matching"), pid, base, width, offset, smargin, rmargin])
                if self.subtraction_var.get():
                    cmds.append([bin_path("enable_trigger_subtraction"), pid, base])
            else:
                cmds.append([bin_path("set_continuous_storage"), pid, base])

        self.run_sequence(cmds, tag="Apply configuration")

    # ------------------------------------------------------------------ #
    # Button callbacks -- verify / utilities
    # ------------------------------------------------------------------ #
    def on_decode_status(self):
        pid = self.pid_var.get().strip()
        cmds = [[bin_path("decode_status"), pid, base] for _, base, _ in self._targets()]
        self.run_sequence(cmds, tag="Decode status")

    def on_read_pattern(self):
        pid = self.pid_var.get().strip()
        cmds = [[bin_path("read_channel_pattern"), pid, base] for _, base, _ in self._targets()]
        self.run_sequence(cmds, tag="Read channel pattern")

    def on_clear_buffer(self):
        pid = self.pid_var.get().strip()
        cmds = [[bin_path("clear_buffer"), pid, base] for _, base, _ in self._targets()]
        self.run_sequence(cmds, tag="Clear buffer")

    def on_software_trigger(self):
        pid = self.pid_var.get().strip()
        n = self.sw_n_var.get().strip() or "1"
        delay = self.sw_delay_var.get().strip() or "0"
        cmds = [[bin_path("software_trigger"), pid, base, n, delay] for _, base, _ in self._targets()]
        self.run_sequence(cmds, tag="Software trigger")

    # ------------------------------------------------------------------ #
    # Button callbacks -- acquisition
    # ------------------------------------------------------------------ #
    def on_browse_outdir(self):
        d = filedialog.askdirectory(initialdir=self.outdir_var.get() or BASE_DIR)
        if d:
            self.outdir_var.set(d)

    def _unique_run_name(self, outdir, run_name):
        """If any output file for run_name already exists in outdir (a sign the user forgot
        to change the run name / output folder since the previous run), return a fresh
        run_name (run_name_2, run_name_3, ...) instead of silently overwriting it."""
        def files_exist(name):
            candidates = (
                os.path.join(outdir, f"{name}.bin"),
                os.path.join(outdir, f"{name}_stream.csv"),
                os.path.join(outdir, f"{name}_stderr.log"),
            )
            return any(os.path.exists(p) for p in candidates)

        if not files_exist(run_name):
            return run_name
        n = 2
        while files_exist(f"{run_name}_{n}"):
            n += 1
        return f"{run_name}_{n}"

    def on_quick_read(self):
        pid, base = self.pid_var.get().strip(), self.base_var.get().strip()
        blt = self.blt_var.get().strip() or "4096"
        self.run_single([bin_path("read_output_buffer_blt"), pid, base, blt, "-v"], tag="Quick read")

    def _stream_reader(self, stream, prefix):
        """Read a text stream line-by-line in a thread and push it to the log queue."""
        try:
            for line in iter(stream.readline, ""):
                if line == "":
                    break
                self.log(f"[{prefix}] {line.rstrip()}")
        except (ValueError, OSError):
            pass

    def on_start_acquisition(self):
        if self.acq_proc1 is not None:
            messagebox.showinfo("Acquisition", "Acquisition is already running.")
            return

        pid = self.pid_var.get().strip()
        acq_label, base = self._acq_target()
        blt = self.blt_var.get().strip() or "4096"
        offset = self.offset_var.get().strip()
        if offset == "?":
            messagebox.showerror("Acquisition", "Window offset is not an integer -- cannot derive the stream offset.")
            return
        hist_env = None
        if self.mode_var.get() == "trigger":
            width, _tw_offset = self._window_ticks()
            if "HISTM_RANGE_NS" not in os.environ:
                try:
                    offset_ns = int(offset)
                except ValueError:
                    offset_ns = None
                if offset_ns is not None:
                    hist_env = dict(os.environ)
                    hist_env["HISTM_RANGE_NS"] = f"{offset_ns},{offset_ns + width * 25}"
        outdir = self.outdir_var.get().strip() or BASE_DIR
        run_name = self.runname_var.get().strip() or "run"
        want_hist = self.live_hist_var.get()
        want_matrix = self.live_matrix_var.get()

        sipm_env = None
        if want_matrix:
            c_bars = [b.strip().upper() for b in self.sipm_c_var.get().split(",") if b.strip()]
            d_bars = [b.strip().upper() for b in self.sipm_d_var.get().split(",") if b.strip()]
            unknown = [b for b in c_bars if b not in sipm_channel_map.C_BAR_ORDER] + \
                      [b for b in d_bars if b not in sipm_channel_map.D_BAR_ORDER]
            if unknown:
                messagebox.showerror("Acquisition",
                                      f"Unknown bar(s) {unknown}. C bars: {sipm_channel_map.C_BAR_ORDER}, "
                                      f"D bars: {sipm_channel_map.D_BAR_ORDER}.")
                return
            sipm_env = dict(os.environ)
            if c_bars:
                sipm_env["SIPM_C_BARS"] = ",".join(c_bars)
            if d_bars:
                sipm_env["SIPM_D_BARS"] = ",".join(d_bars)

        if self.module_type_var.get() == self.MODULE_BOTH:
            self.log(f"NOTE: 'Both' selected -- acquisition reads a single output buffer: {acq_label} "
                      f"at {base}.")

        exe = bin_path("read_output_buffer_blt")
        if not self._check_exe(exe):
            return

        try:
            os.makedirs(outdir, exist_ok=True)
        except OSError as e:
            messagebox.showerror("Acquisition", f"Cannot create output folder: {e}")
            return

        unique_name = self._unique_run_name(outdir, run_name)
        if unique_name != run_name:
            self.log(f"NOTE: output files for run '{run_name}' already exist in {outdir} -- "
                      f"looks like the run name / output folder wasn't changed since the last run. "
                      f"Auto-renamed this run to '{unique_name}' so nothing gets overwritten.")
            run_name = unique_name
            self.runname_var.set(run_name)

        errlog_path = os.path.join(outdir, f"{run_name}_stderr.log")
        try:
            self.errlog_fh = open(errlog_path, "a", buffering=1)
        except OSError as e:
            messagebox.showerror("Acquisition", f"Cannot open {errlog_path}: {e}")
            return

        args = [exe, pid, base, blt, "-c", "-s", offset]
        if self.save_binary_var.get():
            binfile = os.path.join(outdir, f"{run_name}.bin")
            args += ["-b", binfile]

        use_live = want_hist or want_matrix
        self.csv_fh = None
        if not use_live:
            csv_path = os.path.join(outdir, f"{run_name}_stream.csv")
            try:
                self.csv_fh = open(csv_path, "w", buffering=1)
            except OSError as e:
                messagebox.showerror("Acquisition", f"Cannot open {csv_path}: {e}")
                self.errlog_fh.close()
                self.errlog_fh = None
                return
            self.log(f"Stream CSV (trigger#,channel,delay_ns): {csv_path}")

        self.log("$ " + " ".join(args))
        try:
            self.acq_proc1 = subprocess.Popen(
                args, cwd=BASE_DIR,
                stdout=(subprocess.PIPE if use_live else self.csv_fh),
                stderr=subprocess.PIPE, text=True, bufsize=1,
            )
        except Exception as e:
            self.log(f"ERROR starting acquisition: {e}")
            self._close_acq_files()
            return
        threading.Thread(target=self._stream_reader, args=(self.acq_proc1.stderr, "acq"), daemon=True).start()

        self.acq_live_procs = []
        if use_live:
            wanted = []
            if want_hist:
                wanted.append(("live_histogram_multi.py", hist_env, "hist"))
            if want_matrix:
                wanted.append(("live_sipm_matrix.py", sipm_env, "matrix"))

            for script_name, env, tag in wanted:
                live_script = bin_path(script_name)
                if not os.path.isfile(live_script):
                    self.log(f"WARNING: {script_name} not found, skipping live display.")
                    continue
                try:
                    proc = subprocess.Popen(
                        [sys.executable, live_script], cwd=BASE_DIR,
                        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE, text=True, bufsize=1,
                        env=env,
                    )
                except Exception as e:
                    self.log(f"WARNING: could not start live view ({script_name}): {e}")
                    continue
                self.acq_live_procs.append(proc)
                threading.Thread(target=self._stream_reader, args=(proc.stderr, tag), daemon=True).start()

            if self.acq_live_procs:
                # The GUI fans the single acquisition stream out to every live viewer's
                # stdin itself (instead of handing proc1.stdout's fd to one child directly),
                # so histogram + SiPM matrix (or any future live view) can run at once.
                threading.Thread(target=self._relay_stream,
                                  args=(self.acq_proc1.stdout, list(self.acq_live_procs)),
                                  daemon=True).start()
            else:
                # No viewer actually started (e.g. missing scripts) -- still drain proc1's
                # stdout so it doesn't block on a full pipe with nobody reading it.
                threading.Thread(target=self._drain_stream, args=(self.acq_proc1.stdout,), daemon=True).start()

        self.log(f"[Acquisition] started (PID {self.acq_proc1.pid}).")
        self._set_acq_running(True)

    def _relay_stream(self, src_stream, dest_procs):
        """Read text lines from src_stream and fan them out to the stdin of every
        process in dest_procs, so several live viewers can consume the same
        acquisition stream at once. Drops a destination once its stdin breaks
        (e.g. the viewer window was closed) instead of stopping the relay."""
        try:
            for line in src_stream:
                for proc in list(dest_procs):
                    try:
                        proc.stdin.write(line)
                        proc.stdin.flush()
                    except (BrokenPipeError, ValueError, OSError):
                        dest_procs.remove(proc)
        except (ValueError, OSError):
            pass
        finally:
            for proc in dest_procs:
                try:
                    proc.stdin.close()
                except Exception:
                    pass

    def _drain_stream(self, stream):
        try:
            for _line in stream:
                pass
        except (ValueError, OSError):
            pass

    def on_stop_acquisition(self):
        if self.acq_proc1 is None:
            return
        self.log("[Acquisition] stopping (SIGINT)...")
        try:
            self.acq_proc1.send_signal(signal.SIGINT)
        except Exception as e:
            self.log(f"[Acquisition] error sending SIGINT: {e}")

        def waiter():
            try:
                self.acq_proc1.wait(timeout=10)
            except Exception:
                try:
                    self.acq_proc1.kill()
                except Exception:
                    pass
            for proc in self.acq_live_procs:
                try:
                    proc.wait(timeout=5)
                except Exception:
                    try:
                        proc.terminate()
                    except Exception:
                        pass
            self.log("[Acquisition] stopped.")
            self.root.after(0, self._cleanup_acquisition)
        threading.Thread(target=waiter, daemon=True).start()

    def check_acquisition(self):
        if self.acq_proc1 is not None and self.acq_proc1.poll() is not None:
            self.log(f"[Acquisition] process exited on its own (code {self.acq_proc1.returncode}).")
            self._cleanup_acquisition()
        self.root.after(500, self.check_acquisition)

    def _close_acq_files(self):
        for fh in (self.errlog_fh, self.csv_fh):
            if fh:
                try:
                    fh.close()
                except OSError:
                    pass
        self.errlog_fh = None
        self.csv_fh = None

    def _cleanup_acquisition(self):
        self.acq_proc1 = None
        self.acq_live_procs = []
        self._close_acq_files()
        self._set_acq_running(False)

    def _set_acq_running(self, running):
        if running:
            self.status_var.set("RUNNING")
            self.status_label.configure(foreground="#2a2")
            self.start_btn.configure(state="disabled")
            self.stop_btn.configure(state="normal")
        else:
            self.status_var.set("STOPPED")
            self.status_label.configure(foreground="#a33")
            self.start_btn.configure(state="normal")
            self.stop_btn.configure(state="disabled")

    # ------------------------------------------------------------------ #
    def on_close(self):
        if self.acq_proc1 is not None:
            if not messagebox.askyesno("Quit", "Acquisition is still running. Stop it and quit?"):
                return
            self.on_stop_acquisition()
        if self.session_log_fh:
            try:
                self.session_log_fh.close()
            except OSError:
                pass
        self.root.destroy()


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except tk.TclError:
        pass
    DaqGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()
