#!/usr/bin/env python3
"""Materialize our overlay on the exact upstream SDK, without editing upstream."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PIN = json.loads((ROOT / "upstream.json").read_text())


def run(*args, cwd=None, capture=False):
    return subprocess.run(args, cwd=cwd, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=Path,
                        help="Use an existing pristine checkout at the pinned commit")
    args = parser.parse_args()
    cache = (args.upstream or ROOT / ".cache/muse-gadget-sdk").resolve()
    if not cache.exists():
        if args.upstream:
            parser.error("upstream directory does not exist")
        cache.mkdir(parents=True)
        run("git", "init", str(cache))
        run("git", "remote", "add", "origin", PIN["repository"], cwd=cache)
        run("git", "fetch", "--depth", "1", "origin", PIN["commit"], cwd=cache)
        run("git", "checkout", "--detach", "FETCH_HEAD", cwd=cache)
    head = run("git", "rev-parse", "HEAD", cwd=cache, capture=True).strip()
    if head != PIN["commit"]:
        parser.error("upstream HEAD differs from upstream.json; refusing to patch")
    if run("git", "status", "--porcelain", cwd=cache, capture=True).strip():
        parser.error("upstream checkout must be pristine")
    target = ROOT / ".build/sdk"
    marker = target / ".generated-by-muse-g2"
    if target.exists():
        if not marker.exists():
            parser.error("existing .build/sdk is not a generated tree")
        shutil.rmtree(target)
    target.mkdir(parents=True)
    marker.write_text(PIN["commit"] + "\n")
    # SDK host tests also inspect the sibling Linux/mobile protocol clients.
    for item in cache.iterdir():
        if item.name == ".git":
            continue
        if item.is_dir():
            shutil.copytree(item, target / item.name)
        else:
            shutil.copy2(item, target / item.name)
    # Each overlay publishes a manifest of local-source -> upstream-destination.
    manifests = sorted((ROOT / "firmware").rglob("overlay.json"))
    if not manifests:
        parser.error("firmware overlay manifests are missing")
    for manifest_path in manifests:
        manifest = json.loads(manifest_path.read_text())
        for source, destination in manifest.get("files", {}).items():
            src = (manifest_path.parent / source).resolve()
            dst = (target / destination).resolve()
            if not src.is_relative_to(ROOT / "firmware") or not dst.is_relative_to(target):
                parser.error("overlay path escapes its tree")
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        for patch in manifest.get("patches", []):
            run("git", "apply", "--check", str(manifest_path.parent / patch), cwd=target)
            run("git", "apply", str(manifest_path.parent / patch), cwd=target)
    lock = ROOT / "firmware/dependencies.lock"
    if lock.exists():
        shutil.copy2(lock, target / "esp32/dependencies.lock")
    print(f"Prepared {target / 'esp32'} at {head}")


if __name__ == "__main__":
    main()
