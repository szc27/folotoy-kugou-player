#!/usr/bin/env python3
"""Fail when UI source text uses a code point the embedded fonts cannot draw.

The firmware ships one subset font per UI size (assets/fonts/app_font_*.c) plus a
runtime coverage table (assets/fonts/app_font_coverage.c). A UTF-8 string literal
that is not in that subset renders as a placeholder box on the panel, and a
successful build does not prove the glyph exists. This check decodes every string
literal under main/ into code points and compares them with the inventory that
tools/gen_cjk_font.py wrote, so the static gate catches the problem instead of the
panel.

Usage: python3 tools/check_font_coverage.py [--inventory PATH] [--source-dir PATH]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_INVENTORY = ROOT / "assets" / "fonts" / "app_font_inventory.txt"
DEFAULT_SOURCE_DIR = ROOT / "main"

# Whitespace and control code points are never rendered as glyphs.
IGNORED = {0x09, 0x0A, 0x0D}


def load_inventory(path: Path) -> set[int]:
    if not path.exists():
        raise SystemExit(f"inventory not found: {path}\n"
                         f"run tools/gen_cjk_font.py first")
    covered: set[int] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        token = line.split()[0]
        if not token.startswith("U+"):
            raise SystemExit(f"{path.name}: unexpected line {line!r}")
        covered.add(int(token[2:], 16))
    if not covered:
        raise SystemExit(f"{path.name}: inventory is empty")
    return covered


def decode_c_string(raw: str, path: Path, line_no: int) -> bytes:
    """Turn the body of a C string literal into the bytes the compiler emits."""
    out = bytearray()
    i = 0
    length = len(raw)
    while i < length:
        ch = raw[i]
        if ch != "\\":
            out.extend(ch.encode("utf-8"))
            i += 1
            continue
        i += 1
        if i >= length:
            raise SystemExit(f"{path}:{line_no}: trailing backslash in string literal")
        esc = raw[i]
        i += 1
        simple = {
            "n": b"\n", "t": b"\t", "r": b"\r", "0": b"\0", "a": b"\a",
            "b": b"\b", "f": b"\f", "v": b"\v", "\\": b"\\", '"': b'"', "'": b"'",
            "?": b"?",
        }
        if esc in simple:
            out.extend(simple[esc])
        elif esc == "x":
            digits = ""
            while i < length and len(digits) < 2 and raw[i] in "0123456789abcdefABCDEF":
                digits += raw[i]
                i += 1
            if not digits:
                raise SystemExit(f"{path}:{line_no}: \\x without hex digits")
            out.append(int(digits, 16))
        elif esc in "01234567":
            digits = esc
            while i < length and len(digits) < 3 and raw[i] in "01234567":
                digits += raw[i]
                i += 1
            out.append(int(digits, 8) & 0xFF)
        elif esc == "u":
            digits = raw[i:i + 4]
            i += 4
            out.extend(chr(int(digits, 16)).encode("utf-8"))
        elif esc == "U":
            digits = raw[i:i + 8]
            i += 8
            out.extend(chr(int(digits, 16)).encode("utf-8"))
        else:
            # Unknown escape: keep the character, the compiler will complain if invalid.
            out.extend(esc.encode("utf-8"))
    return bytes(out)


def iter_string_literals(text: str):
    """Yield (line_number, body) for each double-quoted literal, skipping comments."""
    i = 0
    line = 1
    length = len(text)
    while i < length:
        ch = text[i]
        if ch == "\n":
            line += 1
            i += 1
            continue
        if ch == "/" and i + 1 < length:
            nxt = text[i + 1]
            if nxt == "/":
                while i < length and text[i] != "\n":
                    i += 1
                continue
            if nxt == "*":
                i += 2
                while i + 1 < length and not (text[i] == "*" and text[i + 1] == "/"):
                    if text[i] == "\n":
                        line += 1
                    i += 1
                i += 2
                continue
        if ch == "'":
            # Character constant: copy verbatim, honouring escapes.
            i += 1
            while i < length and text[i] != "'":
                if text[i] == "\\":
                    i += 1
                if i < length and text[i] == "\n":
                    line += 1
                i += 1
            i += 1
            continue
        if ch == '"':
            start_line = line
            i += 1
            start = i
            while i < length and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                if i < length and text[i] == "\n":
                    line += 1
                    break
                i += 1
            yield start_line, text[start:i]
            i += 1
            continue
        i += 1


def scan_source(path: Path, covered: set[int]):
    text = path.read_text(encoding="utf-8", errors="replace")
    findings = []
    for line_no, body in iter_string_literals(text):
        try:
            raw = decode_c_string(body, path, line_no)
        except (ValueError, SystemExit):
            continue
        if not any(byte >= 0x80 for byte in raw):
            continue
        try:
            decoded = raw.decode("utf-8")
        except UnicodeDecodeError:
            findings.append((line_no, body, ["<invalid UTF-8>"]))
            continue
        missing = sorted({ord(c) for c in decoded
                          if ord(c) not in IGNORED and ord(c) not in covered})
        if missing:
            findings.append((line_no, body, missing))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE_DIR)
    args = parser.parse_args()

    covered = load_inventory(args.inventory)
    sources = sorted(list(args.source_dir.glob("*.c")) + list(args.source_dir.glob("*.h")))
    if not sources:
        raise SystemExit(f"no sources found under {args.source_dir}")

    failures = 0
    for path in sources:
        for line_no, body, missing in scan_source(path, covered):
            failures += 1
            chars = " ".join(f"U+{cp:04X}({chr(cp) if cp < 0xD800 else '?'})" for cp in missing)
            rel = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
            print(f"{rel}:{line_no}: not in font subset: {chars}", file=sys.stderr)
            print(f"    literal: \"{body}\"", file=sys.stderr)

    if failures:
        print(f"\nFont coverage: FAIL ({failures} string literal(s) use missing glyphs). "
              f"Add the characters to tools/gen_cjk_font.py's inventory and regenerate, "
              f"or change the UI text.", file=sys.stderr)
        return 1

    print(f"Font coverage: PASS ({len(sources)} files, {len(covered)} code points available)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
