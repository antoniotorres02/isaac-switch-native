#!/usr/bin/env python3
"""Isaac Switch Builder: run with no arguments for the window, or see --help."""
import os
import sys
from pathlib import Path

# The windowed Windows build has no console: print() must still not fail. And a
# console or a pipe that cannot show ✔ shows a ? instead of stopping the program.
for name in ("stdout", "stderr"):
    stream = getattr(sys, name)
    if stream is None:
        setattr(sys, name, open(os.devnull, "w"))
    elif hasattr(stream, "reconfigure"):
        stream.reconfigure(errors="replace")

sys.path.insert(0, str(Path(__file__).resolve().parent))

from isb.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
