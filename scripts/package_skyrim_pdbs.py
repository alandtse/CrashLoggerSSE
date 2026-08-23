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
(.github/scripts/build-fomod-package.py). See build_root() for the threshold-ordering
mechanics; RUNTIMES below must stay ordered highest game_version first.

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

Pass --upload --nexus-file-id <id> to push the built archive straight to Nexus as a new
file version (Nexus's public v3 API, no CI involved) instead of uploading it by hand.
The target file_id must already exist on the mod page -- Nexus's API can only add a
version to an existing file entry, not create a new one; create it once via the website.

Requires: pip install pyfomod requests
"""

import argparse
import datetime
import os
import shutil
import subprocess
import sys
import time

import pyfomod
import requests

# Default 7-Zip location on Windows; override with --sevenzip.
DEFAULT_7Z = r"C:\Program Files\7-Zip\7z.exe"

# Runtime table. Edit paths here if a game install moves.
#   key            : short id used on the CLI (--runtimes se ae ae17 vr)
#   src_pdb        : where Ghidra/pdbgen writes this runtime's PDB
#   consumer_name  : filename DIA looks for inside Data/SKSE/Plugins/ (the exe's basename + .pdb)
#   display_name   : shown in the FOMOD option list and used in the version string
#   game_version   : gameDependency threshold -- MUST stay ordered highest-to-lowest below
#   program_name   : Ghidra project path passed to scripts.run (regenerate_pdbs.py)
# Source from the game root (PdbGen's output dir), not Data/SKSE/Plugins/ -- those are
# stale prior deploys. "se" (1.5.97) has no distinct source file; verify the right build
# is behind the plain SkyrimSE.exe import before packaging.
RUNTIMES = {
    "ae17": {
        "src_pdb": r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\SkyrimSE.1.7.99.pdb",
        "consumer_name": "SkyrimSE.pdb",
        "display_name": "SkyrimSE 1.7.99.0",
        "game_version": "1.7.99",
        "program_name": "/SkyrimSE.1.7.99.exe",
    },
    "ae": {
        # AE imports from SkyrimSE.1170.exe -> PdbGen writes SkyrimSE.1170.pdb in the game root.
        "src_pdb": r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\SkyrimSE.1170.pdb",
        "consumer_name": "SkyrimSE.pdb",  # same as SE: both runtimes execute SkyrimSE.exe; GUID disambiguates
        "display_name": "SkyrimSE 1.6.1170.0",
        "game_version": "1.6.1170",
        "program_name": "/SkyrimSE.1170.exe",
    },
    "se": {
        # Always the unsuffixed SkyrimSE.pdb -- the plain SkyrimSE.exe import.
        "src_pdb": r"E:\SteamLibrary\steamapps\common\Skyrim Special Edition\SkyrimSE.pdb",
        "consumer_name": "SkyrimSE.pdb",
        "display_name": "SkyrimSE 1.5.97.0",
        "game_version": "1.5.97",
        "program_name": "/SkyrimSE.exe",
    },
    "vr": {
        "src_pdb": r"E:\SteamLibrary\steamapps\common\SkyrimVR\SkyrimVR.pdb",
        "consumer_name": "SkyrimVR.pdb",
        "display_name": "SkyrimVR 1.4.15.0",
        "game_version": "1.4.15",
        "program_name": "/SkyrimVR.exe",
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
NEXUS_MOD_ID = "59818"  # CrashLogger's mod page
MOD_WEBSITE = f"https://www.nexusmods.com/skyrimspecialedition/mods/{NEXUS_MOD_ID}"
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
        # Patterns are evaluated top-to-bottom, first match wins. Iterate RUNTIMES, not
        # `available` -- a missing runtime's threshold must still intercept, or a
        # higher-version install falls through and gets recommended a mismatched PDB.
        for other_key, other_cfg in RUNTIMES.items():
            conditions = pyfomod.Conditions()
            conditions[None] = other_cfg["game_version"]
            is_self = other_key == key
            option_type[conditions] = pyfomod.OptionType.RECOMMENDED if is_self else pyfomod.OptionType.OPTIONAL
        option.type = option_type

        group.append(option)

    page.append(group)
    root.pages.append(page)
    return root


NEXUS_API_BASE = "https://api.nexusmods.com/v3"


def upload_to_nexus(archive_path, file_id, api_key, version, display_name,
                     mod_id=None, changelog=None, category="optional",
                     archive_existing=False):
    """Push archive_path to Nexus as a new version of file_id via the public v3 API --
    the same multipart-upload + finalise + create-version flow as Nexus-Mods/upload-action,
    reimplemented directly so it runs on the Ghidra machine without a CI dependency."""
    session = requests.Session()
    session.headers.update({"apikey": api_key, "User-Agent": "package_skyrim_pdbs.py"})

    size = os.path.getsize(archive_path)
    filename = os.path.basename(archive_path)

    resp = session.post(f"{NEXUS_API_BASE}/uploads/multipart",
                         json={"filename": filename, "size_bytes": str(size)})
    resp.raise_for_status()
    upload = resp.json()["data"]
    upload_id = upload["id"]
    part_urls = upload["part_presigned_urls"]
    part_size = upload["part_size_bytes"]
    complete_url = upload["complete_presigned_url"]
    print(f"  multipart upload {upload_id}: {len(part_urls)} part(s) x {human_size(part_size)}")

    parts = []
    with open(archive_path, "rb") as f:
        for i, part_url in enumerate(part_urls, start=1):
            chunk = f.read(part_size)
            part_resp = requests.put(part_url, data=chunk,
                                      headers={"Content-Type": "application/octet-stream",
                                               "Content-Length": str(len(chunk))})
            part_resp.raise_for_status()
            etag = part_resp.headers["ETag"].strip('"')
            parts.append((i, etag))
            print(f"  uploaded part {i}/{len(part_urls)}")

    complete_xml = "<CompleteMultipartUpload>\n" + "\n".join(
        f"  <Part>\n    <PartNumber>{n}</PartNumber>\n    <ETag>{tag}</ETag>\n  </Part>" for n, tag in parts
    ) + "\n</CompleteMultipartUpload>"
    complete_resp = requests.post(complete_url, data=complete_xml,
                                   headers={"Content-Type": "application/xml"})
    complete_resp.raise_for_status()

    finalise_resp = session.post(f"{NEXUS_API_BASE}/uploads/{upload_id}/finalise")
    finalise_resp.raise_for_status()

    for attempt in range(60):
        state_resp = session.get(f"{NEXUS_API_BASE}/uploads/{upload_id}")
        state_resp.raise_for_status()
        state = state_resp.json()["data"]["state"]
        if state == "available":
            break
        time.sleep(min(2 * 1.5 ** attempt, 30))
    else:
        raise TimeoutError(f"upload {upload_id} did not become available in time")

    version_resp = session.post(f"{NEXUS_API_BASE}/mod-files/{file_id}/versions", json={
        "upload_id": upload_id,
        "name": display_name,
        "version": version,
        "file_category": category,
        "archive_existing_file": archive_existing,
    })
    version_resp.raise_for_status()
    version_id = version_resp.json()["data"]["version"]["id"]
    print(f"  created file version {version_id} on file {file_id}")

    if changelog and mod_id:
        changelog_resp = session.post(f"{NEXUS_API_BASE}/mods/{mod_id}/changelogs",
                                       json={"version": version, "changelog": changelog})
        changelog_resp.raise_for_status()
        print("  changelog entry added")

    return version_id


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
    p.add_argument("--regenerate", action="store_true",
                   help="regenerate PDBs in the open Ghidra session first (via regenerate_pdbs.py/GhidrAssistMCP)")
    p.add_argument("--upload", action="store_true",
                   help="upload the built archive to Nexus as a new file version (requires --nexus-file-id)")
    p.add_argument("--nexus-file-id", default=None,
                   help="Nexus file_id to add a version to (must already exist -- create it once via the website)")
    p.add_argument("--nexus-api-key", default=os.environ.get("NEXUS_API_KEY"),
                   help="Nexus API key (default: $NEXUS_API_KEY)")
    p.add_argument("--nexus-mod-id", default=NEXUS_MOD_ID, help="Nexus mod_id, for --changelog")
    p.add_argument("--nexus-category", default="optional", help="Nexus file_category for the upload")
    p.add_argument("--changelog", default=None, help="changelog text to attach (requires --nexus-mod-id)")
    return p.parse_args()


def main():
    args = parse_args()
    if args.upload:
        if not args.nexus_file_id:
            sys.exit("error: --upload requires --nexus-file-id")
        if not args.nexus_api_key:
            sys.exit("error: --upload requires --nexus-api-key or $NEXUS_API_KEY")
        if args.changelog and not args.nexus_mod_id:
            sys.exit("error: --changelog requires --nexus-mod-id")

    if args.regenerate:
        import regenerate_pdbs
        print(f"Regenerating {' '.join(args.runtimes)} via GhidrAssistMCP...\n")
        regenerate_pdbs.regenerate_runtimes(args.runtimes)
        print()

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

    if args.upload:
        if bad:
            sys.exit("error: refusing to upload an archive missing runtimes -- fix the FAIL lines above first")
        print(f"\nUploading to Nexus file {args.nexus_file_id} (mod {args.nexus_mod_id})...")
        upload_to_nexus(archive, args.nexus_file_id, args.nexus_api_key, version,
                         display_name=archive_name, mod_id=args.nexus_mod_id,
                         changelog=args.changelog, category=args.nexus_category)
        print("Uploaded.")
    else:
        print(f"\nUpload {archive} to the mod's files (version: {version}), "
              f"or re-run with --upload --nexus-file-id <id>.")

    if bad:
        sys.exit(1)


if __name__ == "__main__":
    main()
