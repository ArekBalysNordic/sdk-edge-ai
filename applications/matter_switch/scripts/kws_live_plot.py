#!/usr/bin/env python3
"""
Live plot of Matter switch KWS / wakeword probabilities from UART logs.

Expects Zephyr LOG_INF lines such as:
  I: 0 with probability 0.996094
  I: kws probs [
  I: 0 (LIGHT_OFF) with probability 0.119522
  ...
  I: ]

Serial defaults: 1000000 8N1 (match the board UART terminal).

Uses matplotlib only (no tkinter). Press 'c' in the plot window to clear history.
"""

from __future__ import annotations

import argparse
import importlib
import queue
import re
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Deque, Dict, List, Optional

import matplotlib

# NCS / minimal Python builds often lack _tkinter; pick a GUI backend that exists.
_BACKEND_CANDIDATES = (
    "Qt5Agg",
    "QtAgg",
    "GTK4Agg",
    "GTK3Agg",
    "TkAgg",
    "WXAgg",
    "MacOSX",
)


def _setup_matplotlib_backend(requested: Optional[str]) -> str:
    if requested:
        matplotlib.use(requested, force=True)
        return matplotlib.get_backend()

    for name in _BACKEND_CANDIDATES:
        if name == "TkAgg":
            try:
                importlib.import_module("_tkinter")
            except ImportError:
                continue
        try:
            matplotlib.use(name, force=True)
            return matplotlib.get_backend()
        except Exception:
            continue

    raise RuntimeError(
        "No matplotlib GUI backend available. Install one of:\n"
        "  sudo apt install python3-pyqt5   # Qt5Agg\n"
        "  sudo apt install python3-gi-cairo gir1.2-gtk-4.0  # GTK4Agg\n"
        "  sudo apt install python3-tk      # TkAgg\n"
        "Or pass --backend <name> if you know what works on this system."
    )


WAKEWORD_RE = re.compile(r"^I:\s*(\d+)\s+with probability\s+([\d.]+)\s*$")
KWS_LINE_RE = re.compile(
    r"^I:\s*(\d+)\s+\(([^)]+)\)\s+with probability\s+([\d.]+)\s*$"
)

DEFAULT_CLASSES = [
    "LIGHT_OFF",
    "LIGHT_ON",
    "LIGHT_SWITCH",
    "OTHER",
    "SCENE_FOUR",
    "SCENE_ONE",
    "SCENE_THREE",
    "SCENE_TWO",
    "SILENCE",
]

PLOT_CLASSES = [
    "LIGHT_OFF",
    "LIGHT_ON",
    "LIGHT_SWITCH",
    "OTHER",
    "SCENE_ONE",
    "SILENCE",
]


@dataclass
class ProbFrame:
    mode: str  # "wakeword" | "kws"
    probs: Dict[str, float]
    top_class: str
    top_prob: float
    t: float = field(default_factory=time.monotonic)


@dataclass
class SerialParser:
    in_kws_block: bool = False
    kws_partial: Dict[str, float] = field(default_factory=dict)

    def feed_line(self, line: str) -> List[ProbFrame]:
        line = line.strip()
        if not line:
            return []

        frames: List[ProbFrame] = []

        if "wakeword detected" in line:
            frames.append(self._status_frame("KWS active"))
            return frames
        if "Waiting for wakeword" in line:
            frames.append(self._status_frame("Waiting for wakeword"))
            return frames

        if "kws probs [" in line:
            self.in_kws_block = True
            self.kws_partial = {}
            return frames

        if self.in_kws_block:
            m = KWS_LINE_RE.match(line)
            if m:
                _idx, name, prob_s = m.groups()
                self.kws_partial[name] = float(prob_s)
                return frames
            if line.endswith("]"):
                self.in_kws_block = False
                if self.kws_partial:
                    frames.append(self._frame_from_probs("kws", self.kws_partial))
                self.kws_partial = {}
            return frames

        m = WAKEWORD_RE.match(line)
        if m:
            cls_idx, prob_s = m.groups()
            prob = float(prob_s)
            frames.append(
                ProbFrame(
                    mode="wakeword",
                    probs={"wakeword": prob},
                    top_class=f"class_{cls_idx}",
                    top_prob=prob,
                )
            )
        return frames

    @staticmethod
    def _frame_from_probs(mode: str, probs: Dict[str, float]) -> ProbFrame:
        top_name = max(probs, key=probs.get)
        return ProbFrame(
            mode=mode,
            probs=dict(probs),
            top_class=top_name,
            top_prob=probs[top_name],
        )

    @staticmethod
    def _status_frame(status: str) -> ProbFrame:
        return ProbFrame(
            mode="status",
            probs={},
            top_class=status,
            top_prob=0.0,
        )


def serial_reader(
    port: str,
    baud: int,
    out_q: queue.Queue,
    stop_evt: threading.Event,
) -> None:
    import serial

    parser = SerialParser()
    try:
        ser = serial.Serial(port, baudrate=baud, timeout=0.1)
    except serial.SerialException as exc:
        out_q.put(("error", str(exc)))
        return

    out_q.put(("info", f"Opened {port} @ {baud} 8N1"))
    ser.reset_input_buffer()

    while not stop_evt.is_set():
        try:
            raw = ser.readline()
        except serial.SerialException as exc:
            out_q.put(("error", str(exc)))
            break
        if not raw:
            continue
        try:
            line = raw.decode("utf-8", errors="replace")
        except Exception:
            continue
        for frame in parser.feed_line(line):
            out_q.put(("frame", frame))

    ser.close()


class LivePlotApp:
    def __init__(self, port: str, baud: int, history: int) -> None:
        import matplotlib.pyplot as plt
        from matplotlib.animation import FuncAnimation

        self.plt = plt
        self.port = port
        self.baud = baud
        self.history = history

        self.data_q: queue.Queue = queue.Queue()
        self.stop_evt = threading.Event()
        self.history_frames: Deque[ProbFrame] = deque(maxlen=history)
        self.class_names: List[str] = list(DEFAULT_CLASSES)
        self.status_text = "Connecting..."
        self.last_mode = "—"

        self.fig, (self.ax_ts, self.ax_bar) = plt.subplots(2, 1, figsize=(11, 7))
        self.fig.canvas.manager.set_window_title("KWS live plot")
        self.fig.suptitle(self._title(), fontsize=10)
        self.fig.tight_layout(pad=2.8)

        self.fig.canvas.mpl_connect("key_press_event", self._on_key)
        self.fig.canvas.mpl_connect("close_event", self._on_close)

        self.anim = FuncAnimation(
            self.fig,
            self._on_timer,
            interval=50,
            blit=False,
            cache_frame_data=False,
        )

        self.reader = threading.Thread(
            target=serial_reader,
            args=(self.port, self.baud, self.data_q, self.stop_evt),
            daemon=True,
        )
        self.reader.start()

    def _title(self) -> str:
        return f"{self.port} @ {self.baud} | {self.status_text}  (press c = clear)"

    def clear_history(self) -> None:
        self.history_frames.clear()

    def _on_key(self, event) -> None:
        if event.key in ("c", "C"):
            self.clear_history()

    def _on_close(self, _event) -> None:
        self.stop_evt.set()

    def _drain_queue(self) -> None:
        while True:
            try:
                kind, payload = self.data_q.get_nowait()
            except queue.Empty:
                break
            if kind == "error":
                self.status_text = f"Serial error: {payload}"
            elif kind == "info":
                self.status_text = payload
            elif kind == "frame":
                frame: ProbFrame = payload
                if frame.mode == "status":
                    self.status_text = frame.top_class
                else:
                    self.last_mode = frame.mode
                    if frame.mode == "kws":
                        for name in frame.probs:
                            if name not in self.class_names:
                                self.class_names.append(name)
                    self.history_frames.append(frame)
                    self.status_text = (
                        f"{self.last_mode} | top: {frame.top_class} "
                        f"({frame.top_prob:.3f}) | n={len(self.history_frames)}"
                    )

    def _on_timer(self, _frame_idx: int) -> None:
        self._drain_queue()
        self.fig.suptitle(self._title(), fontsize=10)
        self._draw_timeseries()
        self._draw_bars()

    def _draw_timeseries(self) -> None:
        self.ax_ts.clear()
        self.ax_ts.set_ylim(0, 1.05)
        self.ax_ts.set_ylabel("probability")
        self.ax_ts.set_xlabel("samples (most recent → right)")
        self.ax_ts.grid(True, alpha=0.3)

        kws_frames = [f for f in self.history_frames if f.mode == "kws"]
        ww_frames = [f for f in self.history_frames if f.mode == "wakeword"]

        if kws_frames:
            names = [c for c in PLOT_CLASSES if any(c in f.probs for f in kws_frames)]
            if not names:
                names = sorted(
                    {n for f in kws_frames for n in f.probs},
                    key=lambda n: DEFAULT_CLASSES.index(n)
                    if n in DEFAULT_CLASSES
                    else 999,
                )
            x = list(range(len(kws_frames)))
            for name in names:
                ys = [f.probs.get(name, 0.0) for f in kws_frames]
                self.ax_ts.plot(x, ys, label=name, linewidth=1.5)
            self.ax_ts.legend(loc="upper left", fontsize=8, ncol=2)
            self.ax_ts.set_title("KWS class probabilities (history)")
        elif ww_frames:
            x = list(range(len(ww_frames)))
            ys = [f.top_prob for f in ww_frames]
            self.ax_ts.plot(
                x, ys, label="wakeword prob", color="tab:orange", linewidth=2
            )
            self.ax_ts.legend(loc="upper left", fontsize=9)
            self.ax_ts.set_title("Wakeword probability (history)")
        else:
            self.ax_ts.set_title("Waiting for probability logs…")

    def _draw_bars(self) -> None:
        self.ax_bar.clear()
        self.ax_bar.set_ylim(0, 1.05)
        self.ax_bar.set_ylabel("probability")
        self.ax_bar.grid(True, axis="y", alpha=0.3)

        latest: Optional[ProbFrame] = None
        for f in reversed(self.history_frames):
            if f.mode in ("kws", "wakeword"):
                latest = f
                break

        if latest is None:
            self.ax_bar.set_title("Latest frame (none yet)")
            return

        if latest.mode == "wakeword":
            labels = ["wakeword"]
            values = [latest.top_prob]
            colors = ["tab:orange"]
            self.ax_bar.set_title(
                f"Latest wakeword — {latest.top_class} ({latest.top_prob:.3f})"
            )
        else:
            labels = [c for c in DEFAULT_CLASSES if c in latest.probs] or list(
                latest.probs.keys()
            )
            values = [latest.probs.get(c, 0.0) for c in labels]
            colors = [
                "tab:red" if c == latest.top_class else "tab:blue" for c in labels
            ]
            self.ax_bar.set_title(
                f"Latest KWS — top: {latest.top_class} ({latest.top_prob:.3f})"
            )

        self.ax_bar.bar(labels, values, color=colors)
        self.ax_bar.tick_params(axis="x", rotation=45, labelsize=8)

    def run(self) -> None:
        self.plt.show()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Live KWS / wakeword probability plotter for matter_switch UART logs."
    )
    parser.add_argument(
        "port",
        nargs="?",
        default="/dev/ttyACM0",
        help="Serial port (default: /dev/ttyACM0)",
    )
    parser.add_argument(
        "-b",
        "--baud",
        type=int,
        default=1_000_000,
        help="Baud rate (default: 1000000)",
    )
    parser.add_argument(
        "-n",
        "--history",
        type=int,
        default=300,
        help="Number of frames kept in the rolling history (default: 300)",
    )
    parser.add_argument(
        "--backend",
        default=None,
        help="matplotlib backend (default: auto, skips Tk if _tkinter missing)",
    )
    args = parser.parse_args()

    try:
        backend = _setup_matplotlib_backend(args.backend)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 1

    print(f"matplotlib backend: {backend}", file=sys.stderr)

    try:
        app = LivePlotApp(args.port, args.baud, args.history)
        app.run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
