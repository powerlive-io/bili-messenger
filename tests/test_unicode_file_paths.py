"""Use OS-native Unicode paths as an independent oracle for file imports and exports."""
import csv
from pathlib import Path
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp) / "中文目录" / "六月 福利"
    directory.mkdir(parents=True)
    (directory / "选择名单.txt").write_text("舰长 123 小林\n", encoding="utf-8")
    (directory / "拖入名单.csv").write_text("提督,456,小周\n", encoding="utf-8-sig")
    (directory / "兑换码.txt").write_text("六月-001\n", encoding="utf-8")
    for binary in (sys.argv[1], sys.argv[2]):
        subprocess.run([binary, "--unicode-files"], cwd=tmp, check=True)

    rows = [["舰长", str(100000 + i), f"用户{i:03}"] for i in range(1, 104)]
    for stem in ("12345-2026-01-15", "舰长名单"):
        txt = (directory / f"{stem}.txt").read_text(encoding="utf-8")
        assert txt.splitlines() == ["舰队总数：103", *(" ".join(row) for row in rows)]
        with (directory / f"{stem}.csv").open(encoding="utf-8-sig", newline="") as f:
            assert list(csv.reader(f)) == rows

    with (directory / "已发送记录.csv").open(encoding="utf-8-sig", newline="") as f:
        header, row = csv.reader(f)
        assert header == ["航海等级", "UID", "用户名", "上舰时间", "发送时间", "兑换码"]
        assert row[:4] == ["舰长", "123", "小林", ""] and row[4] and row[5] == "六月-001"
