#!/usr/bin/env python3
"""Extract key FHE-PCA metrics from experiment logs into CSV."""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path


PATTERNS = {
    "d": re.compile(r"FHE-PCA \[bootstrap\] \(d=(\d+),"),
    "m_iter": re.compile(r"m_iter=(\d+)"),
    "newton": re.compile(r"Newton=(\d+)"),
    "keygen_ms": re.compile(r"CryptoContext \+ KeyGen 耗时:\s*([0-9.]+) ms"),
    "depth": re.compile(r"multiplicativeDepth =\s*(\d+)"),
    "slots": re.compile(r"numSlots =\s*(\d+)"),
    "trace": re.compile(r"trace\(C\) =\s*([0-9.eE+-]+)"),
    "server_ms": re.compile(r"Server 耗时:\s*([0-9.]+) ms"),
    "bootstrap": re.compile(r"Bootstrap 次数:\s*(\d+)\s+Bootstrap 累计耗时:\s*([0-9.]+) ms"),
    "lambda1": re.compile(r"#1\s+HE:\s*([0-9.eE+-]+)\s+真实:\s*([0-9.eE+-]+)\s+误差:\s*([0-9.]+)%.*cos_sim=([0-9.eE+-]+)"),
    "r2v": re.compile(r"R²\(V\).*=\s*([0-9.eE+-]+)"),
    "r2x_enc": re.compile(r"R²\(X\) 重建评分（密文 K=\d+）:\s*([0-9.eE+-]+)"),
    "r2x_true": re.compile(r"R²\(X\) 重建评分（明文 K=\d+）:\s*([0-9.eE+-]+)"),
    "r2x_gap": re.compile(r"R²\(X\) 差距 =\s*([0-9.eE+-]+)"),
    "evr": re.compile(r"累计方差解释率.*=\s*([0-9.eE+-]+)"),
}


def first(pattern: re.Pattern[str], text: str, group: int = 1) -> str:
    match = pattern.search(text)
    return match.group(group) if match else ""


def parse_log(path: Path) -> dict[str, str]:
    text = path.read_text(errors="replace")
    row = {
        "log_file": str(path),
        "d": first(PATTERNS["d"], text),
        "m_iter": first(PATTERNS["m_iter"], text),
        "newton": first(PATTERNS["newton"], text),
        "multiplicative_depth": first(PATTERNS["depth"], text),
        "num_slots": first(PATTERNS["slots"], text),
        "keygen_ms": first(PATTERNS["keygen_ms"], text),
        "trace": first(PATTERNS["trace"], text),
        "server_ms": first(PATTERNS["server_ms"], text),
        "bootstrap_count": "",
        "bootstrap_ms": "",
        "lambda1_he": "",
        "lambda1_true": "",
        "lambda1_error_pct": "",
        "lambda1_cos_sim": "",
        "r2v": first(PATTERNS["r2v"], text),
        "r2x_enc": first(PATTERNS["r2x_enc"], text),
        "r2x_true": first(PATTERNS["r2x_true"], text),
        "r2x_gap": first(PATTERNS["r2x_gap"], text),
        "explained_variance_ratio": first(PATTERNS["evr"], text),
        "status": "ok" if "=== 完成 ===" in text else "incomplete",
    }

    if match := PATTERNS["bootstrap"].search(text):
        row["bootstrap_count"] = match.group(1)
        row["bootstrap_ms"] = match.group(2)
    if match := PATTERNS["lambda1"].search(text):
        row["lambda1_he"] = match.group(1)
        row["lambda1_true"] = match.group(2)
        row["lambda1_error_pct"] = match.group(3)
        row["lambda1_cos_sim"] = match.group(4)
    return row


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("experiments/logs")
    logs = sorted(root.glob("*.log"))
    fieldnames = [
        "log_file",
        "status",
        "d",
        "m_iter",
        "newton",
        "multiplicative_depth",
        "num_slots",
        "keygen_ms",
        "server_ms",
        "bootstrap_count",
        "bootstrap_ms",
        "lambda1_he",
        "lambda1_true",
        "lambda1_error_pct",
        "lambda1_cos_sim",
        "r2v",
        "r2x_enc",
        "r2x_true",
        "r2x_gap",
        "explained_variance_ratio",
        "trace",
    ]
    writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
    writer.writeheader()
    for log in logs:
        writer.writerow(parse_log(log))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
