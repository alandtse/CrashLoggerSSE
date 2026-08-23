#!/usr/bin/env python3
"""
Trigger PdbGen.java in the already-open Ghidra session for each Skyrim runtime, via
GhidrAssistMCP's local MCP server -- no Claude Code involved, just the MCP protocol
directly. Requires Ghidra running with GhidrAssistMCP loaded and the runtime programs
already open and analyzed (see RUNTIMES in package_skyrim_pdbs.py for program_name).

This only regenerates; it does not stage/package/upload -- run package_skyrim_pdbs.py
afterward (or use its --regenerate flag to chain both).

Requires: pip install mcp
"""

import argparse
import asyncio
import os
import re
import sys

from mcp import ClientSession
from mcp.client.sse import sse_client

from package_skyrim_pdbs import RUNTIMES

DEFAULT_MCP_URL = os.environ.get("GHIDRA_MCP_URL", "http://localhost:8080/mcp")
POLL_INTERVAL_SECONDS = 3
SCRIPT_TIMEOUT_SECONDS = 300


def text_of(result):
    return "\n".join(getattr(block, "text", "") for block in result.content)


async def regenerate_one(session, key, cfg):
    program = cfg["program_name"]
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
        return (key, True, f"{program}: {pdb_size.group(1).strip()}")

    return (key, False, f"{program}: timed out after {SCRIPT_TIMEOUT_SECONDS}s (task {task_id})")


async def regenerate(runtime_keys, mcp_url):
    async with sse_client(mcp_url) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            results = []
            for key in runtime_keys:
                cfg = RUNTIMES[key]
                print(f"Regenerating {key} ({cfg['program_name']})...")
                result = await regenerate_one(session, key, cfg)
                results.append(result)
                _, ok, msg = result
                print(f"  [{'OK ' if ok else 'FAIL'}] {msg}")
            return results


def regenerate_runtimes(runtime_keys, mcp_url=DEFAULT_MCP_URL):
    """Callable entry point for other scripts (e.g. package_skyrim_pdbs.py --regenerate).
    Returns the (key, ok, message) results; does not exit the process."""
    ordered_keys = [k for k in RUNTIMES if k in runtime_keys]
    results = asyncio.run(regenerate(ordered_keys, mcp_url))
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
    return p.parse_args()


def main():
    args = parse_args()
    results = regenerate_runtimes(args.runtimes, args.mcp_url)
    if any(not r[1] for r in results):
        sys.exit(1)


if __name__ == "__main__":
    main()
