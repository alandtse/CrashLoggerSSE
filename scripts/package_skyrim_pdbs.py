#!/usr/bin/env python3
"""
Package the Ghidra-generated Skyrim runtime PDBs as Nexus optional-file archives
for CrashLogger (https://www.nexusmods.com/skyrimspecialedition/mods/59818).

CrashLogger resolves vanilla SkyrimSE.exe / SkyrimVR.exe call-stack frames by name
using these PDBs. DIA (PdbHandler.cpp -> loadDataForExe(filename, sPluginPath)) finds
the PDB in Data/SKSE/Plugins/ and validates it against the binary by GUID/age, so the
in-archive file is always named after the runtime's exe (both flat SE+AE ship as
SkyrimSE.pdb; only one Skyrim version is installed at a time, GUID picks the right one).

Per runtime:
  1. locate the freshest source PDB (Ghidra/pdbgen output),
  2. stage it under SKSE/Plugins/<consumer-name>.pdb (mod-manager-relative),
  3. 7z it to an archive named for the Nexus display name.

Output archives go to <out>/ (default: ./pdb_artifacts). Upload them manually to the
mod's Optional Files; the Nexus version is the date (YYYY.MM.DD).

NOTE: regenerating the PDBs from Ghidra (to capture the latest RE) is a separate step
run inside the open Ghidra session (PdbGen script per program), since it needs the live,
analyzed project. This script packages whatever PDBs are currently on disk; pass
--require-fresh <days> to refuse stale PDBs so you don't ship an outdated symbol set.
"""

import argparse
import datetime
import os
import shutil
import subprocess
import sys

# Default 7-Zip location on Windows; override with --sevenzip.
DEFAULT_7Z = r"C:\Program Files\7-Zip\7z.exe"

# Runtime table. Edit paths here if a game install moves.
#   key            : short id used on the CLI (--runtimes se ae vr)
#   src_pdb        : where Ghidra/pdbgen writes this runtime's PDB
#   consumer_name  : filename DIA looks for inside Data/SKSE/Plugins/ (the exe's basename + .pdb)
#   nexus_name     : Nexus optional-file display name (archive is named to match)
# IMPORTANT: PdbGen writes the .pdb next to each program's IMPORT path, which is the
# game ROOT dir (where the .exe lives), NOT Data/SKSE/Plugins/. The Plugins copies are
# the previously-deployed PDBs and go stale; always source from the root (PdbGen output).
RUNTIMES = {
    "se": {
        "src_pdb": r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\SkyrimSE.pdb",
        "consumer_name": "SkyrimSE.pdb",
        "nexus_name": "SkyrimSE 1.5.97.0",
    },
    "ae": {
        # AE imports from SkyrimSE.1170.exe -> PdbGen writes SkyrimSE.1170.pdb in the game root.
        "src_pdb": r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\SkyrimSE.1170.pdb",
        "consumer_name": "SkyrimSE.pdb",  # same as SE: both runtimes execute SkyrimSE.exe; GUID disambiguates
        "nexus_name": "SkyrimSE 1.6.1170.0",
    },
    "vr": {
        "src_pdb": r"E:\SteamLibrary\steamapps\common\SkyrimVR\SkyrimVR.pdb",
        "consumer_name": "SkyrimVR.pdb",
        "nexus_name": "SkyrimVR PDB",
    },
}

PLUGINS_REL = os.path.join("SKSE", "Plugins")  # mod-manager-relative install path


def human_size(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0


def find_7z(explicit):
    for cand in (explicit, DEFAULT_7Z, shutil.which("7z"), shutil.which("7za")):
        if cand and os.path.isfile(cand):
            return cand
    sys.exit("error: 7z not found. Install 7-Zip or pass --sevenzip <path>.")


def package_runtime(key, cfg, out_dir, sevenzip, require_fresh):
    src = cfg["src_pdb"]
    if not os.path.isfile(src):
        return (key, False, f"source PDB missing: {src}")

    mtime = os.path.getmtime(src)
    age_days = (datetime.datetime.now().timestamp() - mtime) / 86400.0
    gen_date = datetime.datetime.fromtimestamp(mtime)
    if require_fresh is not None and age_days > require_fresh:
        return (key, False,
                f"PDB is {age_days:.1f} days old (> --require-fresh {require_fresh}); "
                f"regenerate in Ghidra first. {src}")

    # Stage: out/<key>/SKSE/Plugins/<consumer_name>.pdb
    stage = os.path.join(out_dir, key)
    plugin_dir = os.path.join(stage, PLUGINS_REL)
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(plugin_dir, exist_ok=True)
    staged_pdb = os.path.join(plugin_dir, cfg["consumer_name"])
    shutil.copy2(src, staged_pdb)

    # Archive: out/<nexus_name>.7z containing SKSE/Plugins/<consumer_name>.pdb
    archive = os.path.join(out_dir, cfg["nexus_name"] + ".7z")
    if os.path.isfile(archive):
        os.remove(archive)
    # add from the stage root so the archive contains SKSE/Plugins/... (no extra leading dir)
    proc = subprocess.run(
        [sevenzip, "a", "-t7z", "-mx=9", archive, PLUGINS_REL],
        cwd=stage, capture_output=True, text=True)
    if proc.returncode != 0:
        return (key, False, f"7z failed: {proc.stdout}\n{proc.stderr}")

    asize = os.path.getsize(archive)
    return (key, True,
            f"{cfg['nexus_name']}.7z  ({human_size(asize)})  "
            f"<- {cfg['consumer_name']} from {gen_date:%Y-%m-%d %H:%M} "
            f"(version: {gen_date:%Y.%m.%d})")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--runtimes", nargs="+", default=list(RUNTIMES.keys()),
                   choices=list(RUNTIMES.keys()),
                   help="which runtimes to package (default: all)")
    p.add_argument("--out", default=os.path.join(os.getcwd(), "pdb_artifacts"),
                   help="output directory for the .7z archives")
    p.add_argument("--sevenzip", default=None, help="path to 7z.exe")
    p.add_argument("--require-fresh", type=float, default=None, metavar="DAYS",
                   help="fail if any source PDB is older than DAYS (guards against shipping stale symbols)")
    return p.parse_args()


def main():
    args = parse_args()
    sevenzip = find_7z(args.sevenzip)
    # Absolute-ize: package_runtime runs 7z with cwd=<stage>, so a relative archive path would be
    # resolved against the stage dir (nested) and the later getsize would miss it.
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)

    print(f"Packaging Skyrim PDBs -> {args.out}")
    print(f"Using 7z: {sevenzip}\n")

    results = [package_runtime(k, RUNTIMES[k], args.out, sevenzip, args.require_fresh)
               for k in args.runtimes]

    ok = [r for r in results if r[1]]
    bad = [r for r in results if not r[1]]
    for key, success, msg in results:
        print(f"  [{'OK ' if success else 'FAIL'}] {key}: {msg}")

    print(f"\n{len(ok)}/{len(results)} archives built in {args.out}")
    if ok:
        print("Upload these to the mod's Optional Files (version = date shown above):")
        for key, _, _ in ok:
            print(f"    {RUNTIMES[key]['nexus_name']}.7z")
    if bad:
        sys.exit(1)


if __name__ == "__main__":
    main()
