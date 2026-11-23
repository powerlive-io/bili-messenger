"""Verify the save-dialog actions write to the chosen Unicode paths, not mojibake names."""
import csv
from pathlib import Path
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp) / "中文目录"
    directory.mkdir()
    subprocess.run([sys.argv[1], "--export"], cwd=tmp, check=True)

    rows = [["舰长", str(100000 + i), f"用户{i:03}"] for i in range(1, 104)]
    for stem in ("12345-2026-01-15", "舰长名单"):
        txt = (directory / f"{stem}.txt").read_text(encoding="utf-8")
        assert txt.splitlines() == ["舰队总数：103", *(" ".join(row) for row in rows)]
        with (directory / f"{stem}.csv").open(encoding="utf-8-sig", newline="") as f:
            assert list(csv.reader(f)) == rows
