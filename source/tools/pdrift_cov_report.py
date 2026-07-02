#!/usr/bin/env python3
"""Summarize pdrift_host_probe PDRIFT_COVERAGE output."""
from __future__ import annotations

import sys
from pathlib import Path


def load(path: Path) -> dict[str, list[tuple[int, int]]]:
    sections: dict[str, list[tuple[int, int]]] = {}
    current: list[tuple[int, int]] | None = None
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = []
            sections[line[1:-1]] = current
            continue
        if current is None:
            continue
        parts = line.split()
        if len(parts) >= 2:
            current.append((int(parts[0], 16), int(parts[1], 0)))
    return sections


def ranges(pcs: list[int]) -> list[tuple[int, int, int]]:
    if not pcs:
        return []
    out: list[tuple[int, int, int]] = []
    start = prev = pcs[0]
    count = 1
    for pc in pcs[1:]:
        if pc == prev + 2:
            prev = pc
            count += 1
            continue
        out.append((start, prev, count))
        start = prev = pc
        count = 1
    out.append((start, prev, count))
    return out


def report(name: str, entries: list[tuple[int, int]], top_n: int) -> None:
    total_hits = sum(count for _, count in entries)
    pcs = sorted(pc for pc, _ in entries)
    spans = ranges(pcs)
    hot = sorted(entries, key=lambda item: item[1], reverse=True)[:top_n]
    largest = sorted(spans, key=lambda item: item[2], reverse=True)[: min(12, len(spans))]

    print(f"[{name}] pcs={len(entries)} hits={total_hits} spans={len(spans)}")
    if total_hits:
        cover_90 = 0
        hit_acc = 0
        for _, count in sorted(entries, key=lambda item: item[1], reverse=True):
            cover_90 += 1
            hit_acc += count
            if hit_acc * 100 >= total_hits * 90:
                break
        print(f"  pcs_for_90pct_hits={cover_90}")
    print("  top_pcs:")
    for pc, count in hot:
        pct = (count * 100.0 / total_hits) if total_hits else 0.0
        print(f"    {pc:06x} {count:10d} {pct:6.2f}%")
    print("  largest_contiguous_spans:")
    for start, end, count in largest:
        print(f"    {start:06x}-{end:06x} pcs={count}")


def main(argv: list[str]) -> int:
    path = Path(argv[1]) if len(argv) > 1 else Path("build/pdrift_cov_1500_play.txt")
    top_n = int(argv[2]) if len(argv) > 2 else 20
    sections = load(path)
    if not sections:
        print(f"no coverage sections in {path}", file=sys.stderr)
        return 1
    for name, entries in sections.items():
        report(name, entries, top_n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
