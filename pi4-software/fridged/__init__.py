"""fridged — the one process that owns the fridge database.

Reads the RP2040 over the serial link, answers the questions the board asks
(inventory, payment links), writes everything to SQLite, and serves a small HTTP
API. Grafana reads the database directly, read-only.

Design and reasoning: ../../dashboard.md
Order of work:        ../../dashboard-plan.md

    python -m fridged --port sim --db fridge.db        simulated board
    python -m fridged --port COM7 --db fridge.db       real board

WHY THERE IS A sys.path LINE BELOW
----------------------------------
The wire codec lives in `pi4-software/tools/protocol.py` and is deliberately a
plain top-level module rather than part of a package: it is run directly
(`python protocol.py` self-tests it against the C++ header) and imported flatly
by `fake_pi.py`. Turning it into `tools.protocol` so this package could import
it normally would break both of those.

Copying it in here would be worse still — the note at the top of protocol.py
exists because a second copy of the codec is a bug waiting to happen, and a
third would be the same bug. So: one file, found by adding its directory to the
path, once, here.
"""

import sys
from pathlib import Path

_TOOLS_DIR = Path(__file__).resolve().parent.parent / "tools"
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))
