"""Allow standalone native/browser runners to import repository build helpers."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
for folder in ("build", "lib", "experimental"):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / folder))
