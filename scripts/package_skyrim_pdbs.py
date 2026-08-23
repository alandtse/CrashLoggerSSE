#!/usr/bin/env python3
"""
Package the Ghidra-generated Skyrim runtime PDBs as a single FOMOD-wrapped
archive for CrashLogger (https://www.nexusmods.com/skyrimspecialedition/mods/59818).

CrashLogger resolves vanilla SkyrimSE.exe / SkyrimVR.exe call-stack frames by name
using these PDBs. DIA (PdbHandler.cpp -> loadDataForExe(filename, sPluginPath)) finds
the PDB in Data/SKSE/Plugins/ and validates it against the binary by GUID/age, so the
in-archive file is always named after the runtime's exe (SE/AE/SE-1.7.99 all ship as
SkyrimSE.pdb; only one Skyrim version is installed at a time, GUID picks the right one).

Rather than shipping 4 separate archives and making the player pick the right one by
hand, this builds ONE archive with a FOMOD installer: a single install step offers all
4 runtime PDBs as mutually-exclusive options, gameDependency-conditioned on the
installed Skyrim version so the mod manager pre-selects the matching one automatically
-- same mechanism as our sister repo open-shaders uses for its shader-cache picker
(.github/scripts/build-fomod-package.py), and same reasoning: gameDependency has no
comparison operator or negation, only "installed game version >= X", so each option's
pattern list checks every runtime's threshold ordered highest-to-lowest game_version
first so the first (most specific) match wins, rather than a lower threshold also
matching and overriding it. RUNTIMES below must stay ordered highest game_version first
for this to hold; a runtime on a wholly different game (VR) still composes correctly
here since its threshold (1.4.15) is numerically lower than every SE/AE threshold, so an
SE/AE install always intercepts before ever reaching VR's pattern, and a VR install
(never >= 1.5.97) falls through to VR's own pattern.

Per runtime:
  1. locate the freshest source PDB (Ghidra/pdbgen output),
  2. stage it under SKSE/Plugins/<consumer-name>.pdb (mod-manager-relative),
  3. build FOMOD XML via pyfomod,
  4. 7z the whole staged tree into one archive.

Output archive goes to <out>/ (default: ./pdb_artifacts). Upload it manually to the
mod's files; the Nexus version is the date (YYYY.MM.DD).

NOTE: regenerating the PDBs from Ghidra (to capture the latest RE) is a separate step
run inside the open Ghidra session (PdbGen script per program), since it needs the live,
analyzed project. This script packages whatever PDBs are currently on disk; pass
--require-fresh <days> to refuse stale PDBs so you don't ship an outdated symbol set.

Requires: pip install pyfomod
"""

import argparse
import datetime
import os
import shutil
import subprocess
import sys

import pyfomod

# Default 7-Zip location on Windows; override with --sevenzip.
DEFAULT_7Z = r"C:\Program Files\7-Zip\7z.exe"

# Runtime table. Edit paths here if a game install moves.
#   key            : short id used on the CLI (--runtimes se ae se17 vr)
#   src_pdb        : where Ghidra/pdbgen writes this runtime's PDB
#   consumer_name  : filename DIA looks for inside Data/SKSE/Plugins/ (the exe's basename + .pdb)
#   display_name   : shown in the FOMOD option list and used in the version string
#   game_version   : gameDependency threshold -- MUST stay ordered highest-to-lowest below
#                    (see module docstring for why)
# IMPORTANT: PdbGen writes the .pdb next to each program's IMPORT path, which is the
# game ROOT dir (where the .exe lives), NOT Data/SKSE/Plugins/. The Plugins copies are
# the previously-deployed PDBs and go stale; always source from the root (PdbGen output).
#
# Each runtime is imported into Ghidra from its own distinctly-named exe copy so its
# PdbGen output doesn't collide with another runtime's -- except 1.5.97, which is always
# SkyrimSE.pdb (unsuffixed): that's the plain SkyrimSE.exe import. If the wrong build is
# behind that path when this runs, that's a Ghidra-side problem to fix before packaging,
# not something this script tries to detect -- it only checks whether the file exists.
RUNTIMES = {
    "se17": {
        "src_pdb": r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\SkyrimSE.1.7.99.pdb",
        "consumer_name": "SkyrimSE.pdb",
        "display_name": "SkyrimSE 1.7.99.0",
        "game_version": "1.7.99",
    },
    "ae": {
        # AE imports from SkyrimSE.1170.exe -> PdbGen writes SkyrimSE.1170.pdb in the game root.
        "src_pdb": r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\SkyrimSE.1170.pdb",
        "consumer_name": "SkyrimSE.pdb",  # same as SE: both runtimes execute SkyrimSE.exe; GUID disambiguates
        "display_name": "SkyrimSE 1.6.1170.0",
        "game_version": "1.6.1170",
    },
    "se": {
        # Always the unsuffixed SkyrimSE.pdb -- the plain SkyrimSE.exe import.
        "src_pdb": r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\SkyrimSE.pdb",
        "consumer_name": "SkyrimSE.pdb",
        "display_name": "SkyrimSE 1.5.97.0",
        "game_version": "1.5.97",
    },
    "vr": {
        "src_pdb": r"E:\SteamLibrary\steamapps\common\SkyrimVR\SkyrimVR.pdb",
        "consumer_name": "SkyrimVR.pdb",
        "display_name": "SkyrimVR 1.4.15.0",
        "game_version": "1.4.15",
    },
}

PLUGINS_REL = os.path.join("SKSE", "Plugins")  # local filesystem staging path (OS-native separators)
PLUGINS_REL_POSIX = "SKSE/Plugins"  # FOMOD source/destination strings always use forward slashes

MOD_NAME = "CrashLogger PDBs"
MOD_AUTHOR = "CrashLogger Contributors"
MOD_DESCRIPTION = (
    "Optional PDB symbol files for CrashLogger, letting it resolve vanilla "
    "SkyrimSE.exe/SkyrimVR.exe call-stack frames by name instead of +offset. "
    "Pick the variant matching your installed game version; the mod manager "
    "pre-selects it automatically where it can detect your game version."
)
MOD_WEBSITE = "https://www.nexusmods.com/skyrimspecialedition/mods/59818"
STEP_PAGE_NAME = "Runtime PDB"
STEP_GROUP_NAME = "Symbol file for your installed Skyrim version"


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


def stage_runtime(key, cfg, stage_dir, require_fresh):
    src = cfg["src_pdb"]
    if not os.path.isfile(src):
        return (key, False, f"source PDB missing: {src}", None)

    mtime = os.path.getmtime(src)
    age_days = (datetime.datetime.now().timestamp() - mtime) / 86400.0
    gen_date = datetime.datetime.fromtimestamp(mtime)
    if require_fresh is not None and age_days > require_fresh:
        return (key, False,
                f"PDB is {age_days:.1f} days old (> --require-fresh {require_fresh}); "
                f"regenerate in Ghidra first. {src}", None)

    plugin_dir = os.path.join(stage_dir, PLUGINS_REL)
    os.makedirs(plugin_dir, exist_ok=True)
    staged_pdb = os.path.join(plugin_dir, cfg["consumer_name"])
    shutil.copy2(src, staged_pdb)

    asize = os.path.getsize(staged_pdb)
    return (key, True,
            f"{cfg['display_name']}  ({human_size(asize)})  "
            f"<- {os.path.basename(src)} from {gen_date:%Y-%m-%d %H:%M}", gen_date)


def build_root(version, available):
    """available: list of (key, cfg) in RUNTIMES iteration order (highest game_version first)."""
    root = pyfomod.Root()
    root.name = f"{MOD_NAME} {version}"
    root.author = MOD_AUTHOR
    root.version = version
    root.description = MOD_DESCRIPTION
    root.website = MOD_WEBSITE

    if not available:
        return root

    page = pyfomod.Page()
    page.name = STEP_PAGE_NAME

    group = pyfomod.Group()
    group.name = STEP_GROUP_NAME
    # ATMOSTONE (not EXACTLYONE): skipping is valid -- these are optional symbol files,
    # CrashLogger works fine (just less readable stack traces) without any of them.
    group.type = pyfomod.GroupType.ATMOSTONE

    for key, cfg in available:
        option = pyfomod.Option()
        option.name = cfg["display_name"]
        option.description = f"Symbols for {cfg['display_name']}."
        option.files[f"{key}/{PLUGINS_REL_POSIX}/"] = PLUGINS_REL_POSIX

        option_type = pyfomod.Type()
        option_type.default = pyfomod.OptionType.OPTIONAL
        # All available runtimes, not just this one: each pattern is a single
        # gameDependency threshold, evaluated top-to-bottom by the mod manager --
        # first match wins. Listing them highest-game_version-first here (mirroring
        # RUNTIMES' own order) means a higher runtime's threshold always intercepts
        # before a lower one's, so only the actual best match ends up Recommended.
        for other_key, other_cfg in available:
            conditions = pyfomod.Conditions()
            conditions[None] = other_cfg["game_version"]
            is_self = other_key == key
            option_type[conditions] = pyfomod.OptionType.RECOMMENDED if is_self else pyfomod.OptionType.OPTIONAL
        option.type = option_type

        group.append(option)

    page.append(group)
    root.pages.append(page)
    return root


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--runtimes", nargs="+", default=list(RUNTIMES.keys()),
                   choices=list(RUNTIMES.keys()),
                   help="which runtimes to include (default: all)")
    p.add_argument("--out", default=os.path.join(os.getcwd(), "pdb_artifacts"),
                   help="output directory for the staged tree and archive")
    p.add_argument("--sevenzip", default=None, help="path to 7z.exe")
    p.add_argument("--require-fresh", type=float, default=None, metavar="DAYS",
                   help="fail if any source PDB is older than DAYS (guards against shipping stale symbols)")
    p.add_argument("--version", default=None,
                   help="FOMOD version string (default: today's date, YYYY.MM.DD)")
    return p.parse_args()


def main():
    args = parse_args()
    sevenzip = find_7z(args.sevenzip)
    args.out = os.path.abspath(args.out)
    version = args.version or datetime.date.today().strftime("%Y.%m.%d")

    stage_root = os.path.join(args.out, "staged")
    if os.path.isdir(stage_root):
        shutil.rmtree(stage_root)
    os.makedirs(stage_root, exist_ok=True)

    print(f"Staging Skyrim PDBs -> {stage_root}")
    print(f"Using 7z: {sevenzip}\n")

    # Preserve RUNTIMES' declared order (highest game_version first) -- required for
    # the gameDependency interception logic in build_root().
    ordered_keys = [k for k in RUNTIMES if k in args.runtimes]
    results = [stage_runtime(k, RUNTIMES[k], os.path.join(stage_root, k), args.require_fresh)
               for k in ordered_keys]

    ok = [r for r in results if r[1]]
    bad = [r for r in results if not r[1]]
    for key, success, msg, _ in results:
        print(f"  [{'OK ' if success else 'FAIL'}] {key}: {msg}")

    if not ok:
        print("\nno runtimes staged; nothing to package.")
        sys.exit(1)

    available = [(key, RUNTIMES[key]) for key, success, _, _ in results if success]
    root = build_root(version, available)
    errors = root.validate()
    if errors:
        print("error: generated ModuleConfig.xml failed validation:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        sys.exit(1)

    pyfomod.write(root, stage_root)

    archive_name = f"CrashLogger-PDBs-{version}.zip"
    archive = os.path.join(args.out, archive_name)
    if os.path.isfile(archive):
        os.remove(archive)
    proc = subprocess.run(
        [sevenzip, "a", "-tzip", "-mx=9", archive, "."],
        cwd=stage_root, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"error: 7z failed: {proc.stdout}\n{proc.stderr}", file=sys.stderr)
        sys.exit(1)

    asize = os.path.getsize(archive)
    print(f"\n{len(ok)}/{len(results)} runtime(s) packaged into {archive_name} ({human_size(asize)})")
    if bad:
        print("Missing/stale runtimes were skipped (see FAIL lines above); "
              "regenerate them in Ghidra and re-run to include them.")
    print(f"\nUpload {archive} to the mod's files (version: {version}).")
    if bad:
        sys.exit(1)


if __name__ == "__main__":
    main()
