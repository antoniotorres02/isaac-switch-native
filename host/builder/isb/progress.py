"""Turns build_all.py's JSON events into what a person wants to see: which
stage, how far along, and roughly how long is left."""
from __future__ import annotations

import time

# Seconds each stage took on the 16-thread machine GUIDE.md was measured on.
EXPECTED = {"prepare": 5, "functions": 2, "tables": 175, "starts": 21, "lift": 240,
            "assets": 120, "nro": 240, "nsp": 60, "sdcard": 5}
ORDER = list(EXPECTED)
EXPORT_EXPECTED = 90


class Progress:
    def __init__(self, export: bool = False, clock=time.monotonic, nsp: bool = True):
        self.clock = clock
        self.names = [n for n in ORDER if nsp or n != "nsp"] + (["export"] if export else [])
        self.expected = dict(EXPECTED, export=EXPORT_EXPECTED)
        self.state = {n: "pending" for n in self.names}
        self.took: dict[str, float] = {}
        self.started = clock()
        self.stage_started: float | None = None
        self.current: str | None = None
        self.last_log = ""
        self.problems: list[str] = []
        self.failure: dict | None = None
        self.result: dict | None = None
        self.stopped = False

    def feed(self, event: dict) -> None:
        kind = event.get("event")
        if kind == "stage":
            name = event.get("name")
            if name not in self.state:
                return
            self.state[name] = event.get("state", "pending")
            if self.state[name] == "running":
                self.current, self.stage_started, self.last_log = name, self.clock(), ""
            elif self.state[name] == "done":
                self.took[name] = float(event.get("seconds", 0))
        elif kind == "log":
            self.last_log = event.get("line", "")
        elif kind == "problems":
            self.problems = list(event.get("problems", []))
        elif kind == "failed":
            self.failure = event
            if self.current:
                self.state[self.current] = "failed"
        elif kind == "exporting" and "export" in self.state:
            self.state["export"] = "running"
            self.current, self.stage_started, self.last_log = "export", self.clock(), ""
        elif kind == "exported" and "export" in self.state:
            self.state["export"] = "done"
        elif kind == "stopped":
            self.stopped = True
            if self.current and self.state.get(self.current) == "running":
                self.state[self.current] = "pending"
        elif kind == "done":
            self.result = event
            self.current = None

    def remaining(self) -> list[str]:
        return [n for n in self.names if self.state[n] in ("pending", "running", "failed")]

    def fraction(self) -> float:
        """Done work over all work, weighted by the expected time of each stage.
        Skipped stages (done in an earlier run) do not count either way."""
        todo = [n for n in self.names if self.state[n] != "skipped"]
        total = sum(self.expected[n] for n in todo) or 1
        done = sum(self.expected[n] for n in todo if self.state[n] == "done")
        if self.current and self.state.get(self.current) == "running" and self.stage_started is not None:
            exp = self.expected[self.current]
            done += min(exp * 0.95, self.clock() - self.stage_started)
        return max(0.0, min(1.0, done / total))

    def eta(self) -> float | None:
        """Seconds left, or None before anything has run long enough to say."""
        if self.result:
            return 0.0
        left = 0.0
        for n in self.remaining():
            exp = self.expected[n]
            if n == self.current and self.stage_started is not None:
                left += max(exp * 0.05, exp - (self.clock() - self.stage_started))
            else:
                left += exp
        return left

    def elapsed(self) -> float:
        return self.clock() - self.started
