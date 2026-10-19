#!/usr/bin/env python3
"""Compare a patched op1r checkout with its base commit, using a temporary build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BASE = "358d7e6d0460318e1108784fe14acfbfd3850ab4"
FILES = ("clouds_dsp.c", "clouds_dsp.h", "clouds_tables.h")
PLUGIN = "synth_sdk/plugins/clouds"


def run(args):
    result = subprocess.run(args, capture_output=True)
    if result.returncode:
        sys.stderr.buffer.write(result.stderr)
        result.check_returncode()
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tree", nargs="?", type=Path, default=Path.cwd(),
                        help="op1r checkout root (default: current directory)")
    parser.add_argument("--base", default=BASE, help="unoptimized base commit")
    args = parser.parse_args()
    tree = args.tree.resolve()
    base = args.base
    compiler = os.environ.get("CC", "cc")
    results = []
    with tempfile.TemporaryDirectory(prefix="clouds-exact-") as tmp:
        work = Path(tmp)
        for variant in ("base", "patched"):
            directory = work / variant
            directory.mkdir()
            for name in FILES:
                data = (run(["git", "-C", str(tree), "show", f"{base}:{PLUGIN}/{name}"])
                        if variant == "base" else (tree / PLUGIN / name).read_bytes())
                (directory / name).write_bytes(data)
        for configuration, flags in (
            ("op1", ["-DCLOUDS_SR=44100", "-DCLOUDS_MAX_GRAINS=24",
                     "-DCLOUDS_LINEAR_READ", "-DCLOUDS_SKIP_IDLE"]),
            ("reference", ["-DCLOUDS_SR=32000", "-DCLOUDS_MAX_GRAINS=40"]),
        ):
            for variant in ("base", "patched"):
                directory = work / variant
                binary = directory / configuration
                command = [compiler, "-O2", "-g", "-fsanitize=address,undefined",
                           "-fno-sanitize-recover=all", "-DCLOUDS_CHECK", *flags,
                           f'-DDSP_FILE="{directory / "clouds_dsp.c"}"',
                           str(Path(__file__).with_name("clouds_exact_render.c")), "-o", str(binary)]
                # Define the inherited phasor wrap in the baseline; the patch defines it in C.
                if variant == "base":
                    command.insert(1, "-fwrapv")
                run(command)
                for scenario in ("dynamic", "rails", "edge"):
                    run([str(binary), str(directory / f"{configuration}-{scenario}.raw"), scenario])
            for scenario in ("dynamic", "rails", "edge"):
                name = f"{configuration}-{scenario}.raw"
                before = (work / "base" / name).read_bytes()
                after = (work / "patched" / name).read_bytes()
                if not before or before != after:
                    raise RuntimeError(f"nonempty exact comparison failed: {name}")
                results.append({"configuration": configuration, "scenario": scenario,
                                "bytes": len(after), "sha256": hashlib.sha256(after).hexdigest()})
    print(json.dumps({"base": base, "pass": True, "comparisons": results}, indent=2))


if __name__ == "__main__":
    main()
