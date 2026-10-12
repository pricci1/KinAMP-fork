#!/usr/bin/python3
"""Read-only ELF preflight against a PRIVATE dereferenced target library tree.

Usage: check-target-abi.py TARGET_ROOT ELF [ELF ...] > abi.json
Checks closure, ARM hard-float, versioned/unversioned strong imports and loader.
Includes device coreelements/mixersink and envload in each executable closure.
No vendor bytes or symbol export tables are copied to the report.
This is not a substitute for native execution or proof of vendor header ABI.
"""
import json
from pathlib import Path
import re
import subprocess
import sys


def readelf(path, *flags):
    return subprocess.check_output(["readelf", "--wide", *flags, str(path)], text=True)


def symbols(path):
    definitions, imports, weak = set(), set(), set()
    for line in readelf(path, "--dyn-syms").splitlines():
        parts = line.split()
        if len(parts) < 8 or not parts[0].rstrip(":").isdigit():
            continue
        bind, visibility, index, name = parts[4:8]
        if bind not in ("GLOBAL", "WEAK", "UNIQUE") or visibility not in ("DEFAULT", "PROTECTED"):
            continue
        normalized = name.replace("@@", "@")
        if index == "UND":
            (weak if bind == "WEAK" else imports).add(normalized)
        else:
            definitions.add(normalized)
            if "@" not in name or "@@" in name:
                definitions.add(name.split("@")[0])
    return definitions, imports, weak


def main():
    target = Path(sys.argv[1]).resolve()
    search = [target / "mnt/us/KinAMP/libs_hf", target / "lib", target / "usr/lib"]
    nodes = {}

    def inspect(path):
        path = path.resolve()
        if path not in nodes:
            dynamic = readelf(path, "--dynamic")
            nodes[path] = {
                "needed": re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic),
                "rpath": re.findall(r"\((?:RUNPATH|RPATH)\).*\[(.*?)\]", dynamic),
                "symbols": symbols(path),
                "header": readelf(path, "--file-header"),
            }
        return nodes[path]

    def locate(name):
        return next((directory / name for directory in search if (directory / name).is_file()), None)

    reports, failures = [], []
    for argument in sys.argv[2:]:
        binary = Path(argument).resolve()
        program_headers = readelf(binary, "--program-headers")
        match = re.search(r"Requesting program interpreter: (.*?)\]", program_headers)
        interpreter = match.group(1) if match else None
        if interpreter != "/lib/ld-linux-armhf.so.3":
            failures.append(f"{binary.name}: unexpected interpreter {interpreter}")
        roots = [binary, target / "lib/ld-linux-armhf.so.3", target / "usr/lib/libenvload.so",
                 target / "usr/lib/gstreamer-0.10/libgstcoreelements.so",
                 target / "usr/lib/gstreamer-0.10/libgstmixersink.so"]
        closure, queue, missing = {}, roots[:], []
        while queue:
            path = queue.pop(0).resolve()
            if path in closure:
                continue
            node = inspect(path)
            closure[path] = node
            for name in node["needed"]:
                dependency = locate(name)
                if dependency is None:
                    missing.append({"object": path.name, "needed": name})
                else:
                    queue.append(dependency)
        available = set().union(*(node["symbols"][0] for node in closure.values()))
        unresolved, weak_missing, imports = [], [], set()
        for path, node in closure.items():
            required, weak = node["symbols"][1:]
            imports.update(required)
            for name in sorted(required - available):
                unresolved.append({"object": path.name, "symbol": name})
            for name in sorted(weak - available):
                weak_missing.append({"object": path.name, "symbol": name})
            header = node["header"]
            if not all(value in header for value in ("ELF32", "little endian", "Machine:                           ARM", "hard-float ABI")):
                failures.append(f"{path.name}: not ARM ELF32 little-endian hard-float")
        direct = inspect(binary)
        if direct["rpath"]:
            failures.append(f"{binary.name}: RPATH/RUNPATH present")
        failures.extend(f"{binary.name}: missing {item}" for item in missing + unresolved)
        reports.append({
            "binary": binary.name, "bytes": binary.stat().st_size,
            "interpreter": interpreter, "direct_needed": direct["needed"], "rpath": direct["rpath"],
            "attributes": readelf(binary, "--arch-specific").splitlines(),
            "binary_required_versions": sorted({name.split("@", 1)[1] for name in direct["symbols"][1] if "@" in name}),
            "binary_strong_imports": sorted(direct["symbols"][1]),
            "closure": [{"object": path.name, "needed": node["needed"]} for path, node in closure.items()],
            "strong_imports_checked": len(imports), "missing_libraries": missing,
            "unresolved_strong_imports": unresolved, "unresolved_optional_weak_imports": weak_missing,
        })
    print(json.dumps({"result": "FAIL" if failures else "PASS", "scope": "supplied target runtime plus native output plugins; static ELF preflight only",
                      "binaries": reports, "failures": failures}, indent=2))
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
