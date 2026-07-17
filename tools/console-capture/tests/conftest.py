"""Test path setup for the console-capture harness.

Makes three trees importable without installing anything:
  * the console-capture dir  -> ``import console_capture``
  * the n3ds-mcp ``tests``    -> ``import sim_input, sim_ntr`` (the simulators)
  * the n3ds-mcp ``src``      -> ``import n3ds_mcp`` (also handled by the harness's
                                 own bootstrap, inserted here too so the sims and
                                 the harness resolve the SAME package)

Run these tests with the n3ds-mcp venv Python (pytest + Pillow + mcp + n3ds_mcp),
e.g.  E:\\GitHub\\3ds-mcp\\.venv\\Scripts\\python.exe -m pytest tools/console-capture/tests
The n3ds-mcp checkout is found via N3DS_MCP_ROOT or the conventional sibling
``../3ds-mcp`` next to this repo.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

_TESTS_DIR = Path(__file__).resolve().parent
_TOOL_DIR = _TESTS_DIR.parent                       # tools/console-capture
_REPO_ROOT = _TOOL_DIR.parents[1]                   # caesar repo root


def _n3ds_root() -> Path:
    env = os.environ.get("N3DS_MCP_ROOT")
    if env:
        return Path(env)
    return _REPO_ROOT.parent / "3ds-mcp"            # E:\GitHub\3ds-mcp


_N3DS = _n3ds_root()

for p in (_TOOL_DIR, _N3DS / "src", _N3DS / "tests"):
    sp = str(p)
    if p.exists() and sp not in sys.path:
        sys.path.insert(0, sp)
