"""Fetch nwnnsscomp.exe and the TSL nwscript.nss into tools/nwnnsscomp/.

Kept as a download step rather than committing the binary: nwnnsscomp is a
third-party tool with a long, tangled lineage and it is not ours to redistribute.

    python tools/fetch_compiler.py
"""

import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.join(HERE, "nwnnsscomp")

# KotOR Scripting Tool bundles the K2 build of the compiler and the matching header.
BASE = "https://raw.githubusercontent.com/KobaltBlu/KotOR-Scripting-Tool/master/NWN%20Script/k2/"
FILES = ["nwnnsscomp.exe", "nwscript.nss"]


def main():
    os.makedirs(DEST, exist_ok=True)
    for name in FILES:
        url = BASE + urllib.parse.quote(name)
        out = os.path.join(DEST, name)
        if os.path.exists(out):
            print("already present: %s (%d bytes)" % (name, os.path.getsize(out)))
            continue
        print("downloading %s ..." % name)
        with urllib.request.urlopen(url, timeout=120) as resp, open(out, "wb") as fh:
            fh.write(resp.read())
        print("  -> %s (%d bytes)" % (out, os.path.getsize(out)))

    print("\nSource: https://github.com/KobaltBlu/KotOR-Scripting-Tool (NWN Script/k2)")
    print("Both files are third-party; see that project for their licensing.")


if __name__ == "__main__":
    import urllib.parse  # noqa: E402  (kept next to its only use)
    sys.exit(main())
