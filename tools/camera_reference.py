#!/usr/bin/env python3
"""Search and validate Film3DS's source-linked sensor/ISP reference catalog.

Pure stdlib, read-only: no camera commands, writes, networking, or file mutation.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "docs" / "camera-reference"
TABLES = {
    "sources": ("sources.csv", "source_id"),
    "registers": ("registers.csv", "register_id"),
    "experiments": ("experiments.csv", "batch_id"),
    "findings": ("findings.csv", "finding_id"),
}


def load_table(name: str) -> list[dict[str, str]]:
    filename, _ = TABLES[name]
    with (CATALOG / filename).open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames or not all(reader.fieldnames):
            raise ValueError(f"{filename}: invalid CSV header")
        rows = list(reader)
        for number, row in enumerate(rows, 2):
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"{filename}:{number}: invalid column count")
        return rows


def verify(tables: dict[str, list[dict[str, str]]]) -> list[str]:
    errors: list[str] = []
    for name, rows in tables.items():
        key = TABLES[name][1]
        seen: set[str] = set()
        for number, row in enumerate(rows, 2):
            value = row.get(key, "").strip()
            if not value:
                errors.append(f"{name}:{number}: missing {key}")
            if value in seen:
                errors.append(f"{name}:{number}: duplicate {key}: {value}")
            seen.add(value)

    known_sources = {row["source_id"] for row in tables["sources"]}
    for name in ("registers", "findings"):
        for number, row in enumerate(tables[name], 2):
            for source in row.get("source_ids", "").split(";"):
                source = source.strip()
                if source and source not in known_sources:
                    errors.append(f"{name}:{number}: unknown source {source}")
            evidence = row.get("evidence_path", "") or row.get("project_evidence_path", "")
            if evidence and (Path(evidence).is_absolute() or ".." in Path(evidence).parts
                             or not (ROOT / evidence).exists()):
                errors.append(f"{name}:{number}: missing or unsafe evidence path {evidence}")

    for number, row in enumerate(tables["sources"], 2):
        url = row.get("url", "")
        if url and not url.startswith("https://"):
            errors.append(f"sources:{number}: non-HTTPS URL {url}")
        if not url and row.get("access") != "missing":
            errors.append(f"sources:{number}: no URL; must set access=missing")
    return errors


def query(tables: dict[str, list[dict[str, str]]], terms: str, kind: str,
          status: str, limit: int) -> list[dict[str, str]]:
    selected = TABLES if kind == "all" else {kind: TABLES[kind]}
    matches: list[dict[str, str]] = []
    needle = terms.casefold().strip()
    state = status.casefold().strip()
    for name in selected:
        for row in tables[name]:
            haystack = " ".join(row.values()).casefold()
            if needle and needle not in haystack:
                continue
            if state:
                values = (row.get("n3ds_status", ""), row.get("evidence_grade", ""),
                          row.get("access", ""), row.get("report_passed_values", ""))
                if state not in (v.casefold() for v in values):
                    continue
            matches.append({"table": name, **row})
            if len(matches) >= limit:
                return matches
    return matches


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("search", nargs="?", default="", help="address, symbol, finding, phrase")
    p.add_argument("--table", choices=["all", *TABLES], default="all")
    p.add_argument("--status", default="", help="exact N3DS status or evidence grade")
    p.add_argument("--limit", type=int, default=50)
    p.add_argument("--json", action="store_true", help="machine-readable results")
    p.add_argument("--verify", action="store_true", help="check IDs, references, local evidence paths")
    args = p.parse_args()
    if args.limit < 1:
        p.error("--limit must be positive")

    try:
        tables = {name: load_table(name) for name in TABLES}
        if args.verify:
            errors = verify(tables)
            if errors:
                print("\n".join(errors), file=sys.stderr)
                return 1
            print("OK: " + ", ".join(f"{name}={len(rows)}" for name, rows in tables.items()))
            return 0

        matches = query(tables, args.search, args.table, args.status, args.limit)
        if args.json:
            print(json.dumps(matches, ensure_ascii=False, indent=2))
        else:
            for row in matches:
                name = row["table"]
                key = TABLES[name][1]
                short = (row.get("claim") or row.get("symbol") or row.get("title")
                         or row.get("directory_kind") or "").replace("\n", " ")
                addr = row.get("address", "")
                state = row.get("n3ds_status") or row.get("evidence_grade") or row.get("access") or ""
                print(f"{name:<12} {row[key]:<34} {addr:<17} {state:<27} {short[:130]}")
            print(f"{len(matches)} matching rows")
        return 0
    except (OSError, UnicodeError, ValueError, KeyError) as exc:
        print(f"camera_reference: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
