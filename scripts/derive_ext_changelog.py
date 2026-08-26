#!/usr/bin/env python3
"""Derive packaging/reapack/reaper_mcp.ext's @changelog from index.xml's NEWEST <version> block.

WHY THIS IS A SCRIPT AND NOT A SENTENCE IN A RUNBOOK.

A release bumps the version STRING across every sync site. The `.ext` carries TWO fields that
must move together — `@version` and `@changelog` — and a version sweep only moves the first, so
the file ends up announcing the new release while describing the previous one. Our runbook has
said *"derive the block; do not copy it"* since 1.13.2 and the drift happened anyway, more than
once. A prescription is not an instrument.

Run this immediately after prepending a new <version> block, then check that the pair agrees.

Usage:  python3 scripts/derive_ext_changelog.py [--check]
        --check  report what WOULD change and write nothing (exit 1 if it would change)
"""
import io, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT = os.path.join(ROOT, "packaging", "reapack", "reaper_mcp.ext")
IDX = os.path.join(ROOT, "packaging", "reapack", "index.xml")
INDENT = "  "


def newest_block(xml):
    m = re.search(r'<version name="([^"]+)"[^>]*>\s*<changelog>\s*<!\[CDATA\[(.*?)\]\]>',
                  xml, re.S)
    if not m:
        sys.exit("REFUSE: no <version> block with a CDATA changelog in %s" % IDX)
    return m.group(1), m.group(2).strip()


def main(argv):
    check = "--check" in argv
    ver, body = newest_block(io.open(IDX, encoding="utf-8").read())
    ext = io.open(EXT, encoding="utf-8").read()

    m = re.search(r"^@changelog[ \t]*\n(.*?)(?=^@\w|\Z)", ext, re.M | re.S)
    if not m:
        sys.exit("REFUSE: no @changelog section in %s" % EXT)

    derived = "".join(INDENT + l + "\n" if l.strip() else "\n" for l in body.splitlines())
    new_ext = ext[:m.start(1)] + derived + ext[m.end(1):]
    # the version string is the caller's job (the sweep owns it); this only reports on it
    ev = re.search(r"^@version\s+(\S+)\s*$", new_ext, re.M)
    if ev and ev.group(1) != ver:
        print("⚠️  @version is %s and index.xml's newest block is %s — NOT changed here."
              % (ev.group(1), ver))
        print("    The version string is the sweep's job (docs/RELEASING.md); this derives the")
        print("    CHANGELOG only. The pair gate will still report the version mismatch.")

    if new_ext == ext:
        print("unchanged — @changelog already matches the %s block" % ver)
        return 0
    if check:
        print("WOULD REWRITE @changelog from index.xml's %s block" % ver)
        return 1
    io.open(EXT, "w", encoding="utf-8").write(new_ext)
    print("derived @changelog for %s from index.xml (%d lines)" % (ver, len(body.splitlines())))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
