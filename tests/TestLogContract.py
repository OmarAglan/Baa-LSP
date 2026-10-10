#!/usr/bin/env python3
"""Bind every structured log call site to the baa-lsp-log-v1 event catalogue.

The catalogue is a closed set: a call site with an uncatalogued event, a
catalogued event with no call site, or any difference in component, severity,
message, or data keys fails the test. Events, components, and messages must be
string literals, so no path or source text can reach those fields.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

SEVERITY_BY_TYPE = {1: "error", 2: "warning", 3: "info"}
EXPECTED_FIELDS = [
    "schema_version", "sequence", "severity", "component", "event",
    "message", "data",
]
SEGMENT = re.compile(r"[a-z]([a-z0-9-]*[a-z0-9])?")
DATA_KEY = re.compile(r"[a-z][a-z0-9_]*")
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
DATA_ENTRY = re.compile(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*([^{}]*?)\s*\}')


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def call_arguments(source: str, start: int) -> list[str]:
    """Split the arguments of the call whose '(' is at `start`."""
    arguments: list[str] = []
    depth = 0
    current: list[str] = []
    index = start
    while index < len(source):
        char = source[index]
        if char == '"':
            match = STRING.match(source, index)
            if not match:
                fail("unterminated string literal in a sendLogEvent call")
            current.append(match.group(0))
            index = match.end()
            continue
        if char in "({[":
            depth += 1
            if depth > 1:
                current.append(char)
        elif char in ")}]":
            depth -= 1
            if depth == 0:
                arguments.append("".join(current).strip())
                return arguments
            current.append(char)
        elif char == "," and depth == 1:
            arguments.append("".join(current).strip())
            current = []
        else:
            current.append(char)
        index += 1
    fail("unterminated sendLogEvent call")
    return arguments


def literal(argument: str, what: str) -> str:
    """Return the value of one or more adjacent string literals."""
    parts = STRING.findall(argument)
    if not parts or STRING.sub("", argument).strip():
        fail(f"{what} must be a string literal, found: {argument}")
    return "".join(parts)


def call_sites(source: str) -> list[dict]:
    sites = []
    for match in re.finditer(r"(?<![:\w])sendLogEvent\s*\(", source):
        arguments = call_arguments(source, match.end() - 1)
        if len(arguments) < 3 or len(arguments) > 5:
            fail(f"unexpected sendLogEvent argument count: {arguments}")
        log_type = 1
        if len(arguments) > 3:
            if not arguments[3].isdigit():
                fail(f"log type must be an integer literal: {arguments[3]}")
            log_type = int(arguments[3])
        if log_type not in SEVERITY_BY_TYPE:
            fail(f"log type {log_type} has no catalogued severity")
        data = {}
        if len(arguments) > 4:
            entries = DATA_ENTRY.findall(arguments[4])
            residue = DATA_ENTRY.sub("", arguments[4]).replace(",", "").strip()
            if not entries or residue not in {"{}", ""}:
                fail(f"data must be a literal key/value list: {arguments[4]}")
            for key, value in entries:
                if '"' in value:
                    fail(f"data value for '{key}' must not be a string")
                data[key] = "integer"
        sites.append({
            "event": literal(arguments[0], "event"),
            "component": literal(arguments[1], "component"),
            "severity": SEVERITY_BY_TYPE[log_type],
            "message": literal(arguments[2], "message"),
            "data": data,
        })
    return sites


def stable_identifier(value: str, limits: dict) -> bool:
    if not value or len(value) > limits["identifier_max_chars"]:
        return False
    return all(
        len(segment) <= limits["identifier_segment_max_chars"]
        and SEGMENT.fullmatch(segment)
        for segment in value.split(".")
    )


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: TestLogContract.py SERVER_SOURCE CATALOGUE")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    catalogue = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))

    schema = catalogue["schema"]
    if f'StructuredLogSchema = "{schema}"' not in source:
        fail(f"the server does not declare schema {schema}")
    if catalogue["fields"] != EXPECTED_FIELDS:
        fail("catalogue fields differ from the seven baa/logEvent fields")
    for field in EXPECTED_FIELDS:
        if f'{{"{field}",' not in source:
            fail(f"the server does not send field '{field}'")
    if catalogue["telemetry"] is not False or "telemetry/event" in source:
        fail("baa-lsp-log-v1 must stay telemetry-free")

    limits = catalogue["limits"]
    events = catalogue["events"]
    by_event = {entry["event"]: entry for entry in events}
    if len(by_event) != len(events):
        fail("the catalogue lists an event more than once")

    for entry in events:
        event = entry["event"]
        if set(entry) != {"event", "component", "severity", "message", "data"}:
            fail(f"{event}: unexpected catalogue keys")
        if not stable_identifier(event, limits) or \
                not stable_identifier(entry["component"], limits):
            fail(f"{event}: event or component is not a stable identifier")
        if entry["component"] not in catalogue["components"] or \
                not event.startswith(entry["component"] + "."):
            fail(f"{event}: event does not belong to its component")
        if entry["severity"] not in catalogue["severities"]:
            fail(f"{event}: unknown severity {entry['severity']}")
        message = entry["message"]
        units = len(message.encode("utf-16-le")) // 2
        if not message or units > limits["message_max_utf16_units"]:
            fail(f"{event}: message is empty or too long")
        if len(entry["data"]) > limits["data_max_entries"]:
            fail(f"{event}: too many data entries")
        for key, kind in entry["data"].items():
            if not DATA_KEY.fullmatch(key) or \
                    len(key) > limits["data_key_max_chars"]:
                fail(f"{event}: data key '{key}' is not stable")
            if kind != "integer":
                fail(f"{event}: data key '{key}' has unsupported type {kind}")

    sites = call_sites(source)
    if not sites:
        fail("no sendLogEvent call sites were found")
    seen: set[str] = set()
    for site in sites:
        event = site["event"]
        if event in seen:
            fail(f"{event}: emitted from more than one call site")
        seen.add(event)
        if event not in by_event:
            fail(f"{event}: emitted by the server but not catalogued")
        if site != by_event[event]:
            fail(f"{event}: call site {site} differs from catalogue "
                 f"{by_event[event]}")
    missing = sorted(set(by_event) - seen)
    if missing:
        fail(f"catalogued events with no call site: {missing}")

    print(f"baa-lsp-log-v1: {len(sites)} call sites match the catalogue")


if __name__ == "__main__":
    main()
