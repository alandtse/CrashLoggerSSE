#!/usr/bin/env python3
"""
Trigger PdbGen.java in the already-open Ghidra session for each Skyrim runtime, via
GhidrAssistMCP's local MCP server -- no Claude Code involved, just the MCP protocol
directly. Requires Ghidra running with GhidrAssistMCP loaded and the runtime programs
already open and analyzed (see RUNTIMES in package_skyrim_pdbs.py for program_name).

Skips a runtime whose Program hasn't changed (Ghidra's own modification counter, which
tracks every in-memory edit regardless of save state) since its last successful regen
here, and whose source PDB still exists -- pass --force to regenerate anyway.

This only regenerates; it does not stage/package/upload -- run package_skyrim_pdbs.py
afterward (or use its --regenerate flag to chain both).

Requires: pip install mcp
"""

import argparse
import asyncio
import datetime
import os
import re
import sys

from mcp import ClientSession
from mcp.client.sse import sse_client

from package_skyrim_pdbs import RUNTIMES, load_state, save_state

DEFAULT_MCP_URL = os.environ.get("GHIDRA_MCP_URL", "http://localhost:8080/mcp")
POLL_INTERVAL_SECONDS = 3
SCRIPT_TIMEOUT_SECONDS = 300
# Bounds each individual MCP request -- SCRIPT_TIMEOUT_SECONDS only bounds our own polling
# loop, not a single hung call, which would otherwise block the run indefinitely.
MCP_READ_TIMEOUT_SECONDS = 60


def text_of(result):
    return "\n".join(getattr(block, "text", "") for block in result.content)


async def get_modification_number(session, program):
    result = await session.call_tool("eval_python", {
        "script": "print('MODNUM:' + str(currentProgram.getModificationNumber()))",
        "program_name": program,
        "sync": True,
    })
    match = re.search(r"MODNUM:(\d+)", text_of(result))
    return int(match.group(1)) if match else None


async def regenerate_one(session, key, cfg, state, force):
    program = cfg["program_name"]
    modnum = await get_modification_number(session, program)
    prior = state.get("runtimes", {}).get(key, {})
    if (not force and modnum is not None and prior.get("modification_number") == modnum
            and os.path.isfile(cfg["src_pdb"])):
        return (key, True, f"{program}: unchanged since last regen (mod #{modnum}), skipped")

    run_result = await session.call_tool("scripts", {
        "action": "run",
        "name": "PdbGen.java",
        "program_name": program,
        "timeout_seconds": SCRIPT_TIMEOUT_SECONDS,
    })
    run_text = text_of(run_result)
    match = re.search(r"task submitted:\s*([0-9a-fA-F-]+)", run_text)
    if not match:
        return (key, False, f"could not read task_id from: {run_text[:300]}")
    task_id = match.group(1)

    for _ in range(SCRIPT_TIMEOUT_SECONDS // POLL_INTERVAL_SECONDS):
        await asyncio.sleep(POLL_INTERVAL_SECONDS)
        status_result = await session.call_tool("get_task_status", {"task_id": task_id})
        status_text = text_of(status_result)
        if "Status: RUNNING" in status_text or "Status: PENDING" in status_text:
            continue
        if "exited with code" in status_text or "[PDBGEN] FAILED" in status_text:
            return (key, False, f"{program}: pdbgen failed, see task {task_id}")
        pdb_size = re.search(r"PDB file size:\s*(.+)", status_text)
        if not pdb_size or "not found" in pdb_size.group(1):
            return (key, False, f"{program}: no PDB produced, see task {task_id}")
        state.setdefault("runtimes", {})[key] = {
            "modification_number": modnum,
            "regenerated_at": datetime.datetime.now().isoformat(),
        }
        save_state(state)
        return (key, True, f"{program}: {pdb_size.group(1).strip()}")

    return (key, False, f"{program}: timed out after {SCRIPT_TIMEOUT_SECONDS}s (task {task_id})")


async def regenerate(runtime_keys, mcp_url, force):
    state = load_state()
    async with sse_client(mcp_url) as (read, write):
        async with ClientSession(read, write, read_timeout_seconds=MCP_READ_TIMEOUT_SECONDS) as session:
            await session.initialize()
            results = []
            for key in runtime_keys:
                cfg = RUNTIMES[key]
                print(f"Regenerating {key} ({cfg['program_name']})...")
                result = await regenerate_one(session, key, cfg, state, force)
                results.append(result)
                _, ok, msg = result
                print(f"  [{'OK ' if ok else 'FAIL'}] {msg}")
            return results


def regenerate_runtimes(runtime_keys, mcp_url=DEFAULT_MCP_URL, force=False):
    """Callable entry point for other scripts (e.g. package_skyrim_pdbs.py --regenerate).
    Returns the (key, ok, message) results; does not exit the process."""
    ordered_keys = [k for k in RUNTIMES if k in runtime_keys]
    results = asyncio.run(regenerate(ordered_keys, mcp_url, force))
    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} runtime(s) regenerated.")
    return results


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--runtimes", nargs="+", default=list(RUNTIMES.keys()),
                   choices=list(RUNTIMES.keys()),
                   help="which runtimes to regenerate (default: all)")
    p.add_argument("--mcp-url", default=DEFAULT_MCP_URL,
                   help="GhidrAssistMCP endpoint (default: $GHIDRA_MCP_URL or http://localhost:8080/mcp)")
    p.add_argument("--force", action="store_true",
                   help="regenerate even if the Program hasn't changed since the last successful regen")
    return p.parse_args()


def main():
    args = parse_args()
    results = regenerate_runtimes(args.runtimes, args.mcp_url, args.force)
    if any(not r[1] for r in results):
        sys.exit(1)


if __name__ == "__main__":
    main()
