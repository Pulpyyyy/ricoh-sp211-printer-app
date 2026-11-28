#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression test of the print encoder (src/lilo2.c). No printer needed.

build/test_encoder writes synthetic pages as print streams. Each stream is
decoded by build/lilo2check, which must find the right page: size, bands,
share of black dots, and the very dots expected. The stream must also be, byte
for byte, the one whose SHA-256 is in tests/data/lilo2.sha256.

    make test          check
    make references    write tests/data/lilo2.sha256 (see tests/data/README.md)
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys

TOP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BUILD = os.path.join(TOP, "build")
OUT = os.path.join(BUILD, "encoder")
REFERENCES = os.path.join(TOP, "tests", "data", "lilo2.sha256")

# paper -> paper code, width and height in dots, bands of 256 lines
PAPERS = {
    "a4": (9, 4864, 6816, 27),
    "letter": (1, 4992, 6400, 25),
}
# page -> lowest and highest plausible share of black dots, in percent
PAGES = {
    "text": (5.0, 20.0),
    "gray": (40.0, 55.0),
    "heavy": (40.0, 55.0),
}
CASES = [f"{paper}-{page}" for paper in PAPERS for page in PAGES]

REPORT = re.compile(
    r"page 1: paper (\d+) tray (\d+) media (\d+), (\d+)x(\d+) dots, "
    r"(\d+)/(\d+) bands, (\d+) lines, black (\d+\.\d\d)%")


class Failure(Exception):
    pass


def read(path):
    with open(path, "rb") as f:
        return f.read()


def black_dots(pbm):
    """Number of black dots in a binary PBM (P4) image."""
    data = pbm.split(b"\n", 2)[2]
    return bin(int.from_bytes(data, "big")).count("1")


def check_page(name):
    """Decodes one stream, checks the page, returns the stream's SHA-256."""
    paper, page = name.split("-")
    code, width, height, bands = PAPERS[paper]
    low, high = PAGES[page]
    base = os.path.join(OUT, name)

    result = subprocess.run(
        [os.path.join(BUILD, "lilo2check"), base + ".prn", base + ".decoded"],
        capture_output=True, text=True)
    report = result.stdout + result.stderr
    if result.returncode or "1 page(s), stream is well formed" not in report:
        raise Failure(f"lilo2check rejects the stream:\n{report}")
    found = REPORT.search(report)
    if not found:
        raise Failure(f"unexpected report from lilo2check:\n{report}")

    got = tuple(int(v) for v in found.groups()[:8])
    want = (code, 2, 0, width, height, bands, bands, height)
    if got != want:
        raise Failure("paper, tray, media, width, height, bands read, bands "
                      f"announced, lines: got {got}, expected {want}")

    expected = read(base + ".pbm")
    share = 100.0 * black_dots(expected) / (width * height)
    if not low <= share <= high:
        raise Failure(f"the test page has {share:.2f}% black dots, "
                      f"expected {low} to {high}%")
    if found.group(9) != f"{share:.2f}":
        raise Failure(f"black {found.group(9)}% decoded, {share:.2f}% expected")
    decoded = base + ".decoded-1.pbm"
    if read(decoded) != expected:
        raise Failure(f"decoded dots differ from the expected ones: compare "
                      f"{decoded} with {base}.pbm")
    os.remove(decoded)
    os.remove(base + ".pbm")

    return hashlib.sha256(read(base + ".prn")).hexdigest()


def read_references():
    """name -> SHA-256, from lines in the format of sha256sum."""
    references = {}
    with open(REFERENCES, encoding="ascii") as f:
        for line in f:
            if line.strip():
                digest, name = line.split()
                references[name.removesuffix(".prn")] = digest
    return references


def reference_lines(digests):
    return "".join(f"{digests[name]}  {name}.prn\n" for name in CASES)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--update", action="store_true",
                        help="write the references instead of checking them")
    args = parser.parse_args()

    os.makedirs(OUT, exist_ok=True)
    subprocess.run([os.path.join(BUILD, "test_encoder"), OUT], check=True)

    digests, failures = {}, 0
    for name in CASES:
        try:
            digests[name] = check_page(name)
        except Failure as e:
            failures += 1
            print(f"FAIL {name}: {e}", file=sys.stderr)
    if failures:
        print(f"encoder: {len(CASES)} pages, {failures} failures; "
              "references neither checked nor written", file=sys.stderr)
        return 1

    if args.update:
        os.makedirs(os.path.dirname(REFERENCES), exist_ok=True)
        with open(REFERENCES, "w", encoding="ascii", newline="\n") as f:
            f.write(reference_lines(digests))
        print(f"encoder: {len(CASES)} references written to "
              f"{os.path.relpath(REFERENCES, TOP)}")
        return 0

    # Until the first references are committed there is nothing to compare
    # with. Once the file exists, every page must be in it and match.
    if not os.path.exists(REFERENCES):
        print(f"encoder: {len(CASES)} pages decode as expected\n"
              "WARNING: byte-for-byte comparison SKIPPED, "
              "tests/data/lilo2.sha256 does not exist.\n"
              "See tests/data/README.md. SHA-256 of this build:\n"
              + reference_lines(digests), end="")
        return 0

    references = read_references()
    for name in CASES:
        if references.get(name) != digests[name]:
            failures += 1
            print(f"FAIL {name}: stream changed\n"
                  f"  reference {references.get(name, '(none)')}\n"
                  f"  this build {digests[name]}  (build/encoder/{name}.prn)",
                  file=sys.stderr)
    for name in sorted(set(references) - set(CASES)):
        failures += 1
        print(f"FAIL {name}: reference without a test page", file=sys.stderr)
    print(f"encoder: {len(CASES)} pages, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
