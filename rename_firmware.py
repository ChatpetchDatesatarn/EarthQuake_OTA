# -*- coding: utf-8 -*-
"""
rename_firmware.py  (Enhanced)
--------------------------------
Post-build script for PlatformIO + Standalone packer

- After each successful build:
  * Copy .pio/build/<env>/firmware.bin -> dist/<env>/<env>_v<FW_VERSION>.bin
                                       -> dist/<env>/<env>_latest.bin
  * Generate SHA256 files (<file>.sha256) with hex string (single line)
  * Generate dist/<env>/manifest.json
  * Update dist/index.json (summary of all envs)

- If executed directly (python rename_firmware.py):
  * Sweep .pio/build/*/firmware.bin for all envs and package them.

Works with SCons >= 4 / PlatformIO latest on Windows/macOS/Linux.
"""

from __future__ import annotations
import os, re, json, hashlib, shutil, time, sys
from pathlib import Path

# ---------------- Utilities ----------------
def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def _sanitize(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", str(s))

def _get_define(env, name: str, default: str = "") -> str:
    """Get C/C++ define (e.g., FW_VERSION) from CPPDEFINES. Normalize quotes/backslashes."""
    try:
        defines = env.get("CPPDEFINES", [])
        for d in defines:
            val = None
            if isinstance(d, (list, tuple)) and len(d) >= 2 and d[0] == name:
                val = d[1]
                if isinstance(val, (list, tuple)) and len(val):
                    val = val[0]
            elif d == name:
                # macro without value
                return default

            if val is not None:
                val = str(val)
                # 🔧 สำคัญ: ล้าง backslash และ quote
                val = val.replace('\\"', '"').replace("\\", "")
                val = val.strip().strip('"').strip("'")
                return val
    except Exception:
        pass
    return default

def _write_text(p: Path, s: str):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(s, encoding="utf-8")

def _now_str():
    return time.strftime("%Y-%m-%d %H:%M:%S")

# ---------------- Core packer ----------------
def package_one(projdir: Path, env_name: str, fw_bin: Path, fw_version: str) -> dict:
    """Package one environment output into dist/<env>."""
    env_name = _sanitize(env_name)
    fw_version = _sanitize(fw_version) if fw_version else time.strftime("%Y%m%d-%H%M%S")

    dist_dir = projdir / "dist" / env_name
    dist_dir.mkdir(parents=True, exist_ok=True)

    ver_name = f"{env_name}_v{fw_version}.bin"
    latest_name = f"{env_name}_latest.bin"
    ver_bin = dist_dir / ver_name
    latest_bin = dist_dir / latest_name

    shutil.copy2(fw_bin, ver_bin)
    shutil.copy2(fw_bin, latest_bin)

    sha_ver = _sha256(ver_bin)
    sha_lat = _sha256(latest_bin)

    _write_text(dist_dir / f"{ver_name}.sha256", sha_ver + "\n")
    _write_text(dist_dir / f"{latest_name}.sha256", sha_lat + "\n")

    manifest = {
        "env": env_name,
        "version": fw_version,
        "path_versioned": str(ver_bin.relative_to(projdir)).replace("\\", "/"),
        "path_latest": str(latest_bin.relative_to(projdir)).replace("\\", "/"),
        "sha256": {"versioned": sha_ver, "latest": sha_lat},
        "built_at": _now_str(),
        "size_bytes": fw_bin.stat().st_size,
    }
    _write_text(dist_dir / "manifest.json", json.dumps(manifest, indent=2))

    print(f"\n[rename] ✅ Packaged {env_name}")
    print(f"  Version     : {fw_version}")
    print(f"  Source bin  : {fw_bin}")
    print(f"  Output dir  : {dist_dir}")
    print(f"  SHA256 (ver): {sha_ver[:16]}...")
    print(f"  SHA256 (lat): {sha_lat[:16]}...")
    return manifest

def update_index(projdir: Path):
    """Aggregate manifests into dist/index.json."""
    dist_root = projdir / "dist"
    items = []
    if dist_root.exists():
        for manifest in dist_root.glob("*/manifest.json"):
            try:
                items.append(json.loads(manifest.read_text(encoding="utf-8")))
            except Exception:
                pass
    idx = {
        "generated_at": _now_str(),
        "artifacts": items,
    }
    _write_text(dist_root / "index.json", json.dumps(idx, indent=2))
    print(f"[rename] ℹ️  index.json updated ({len(items)} envs)")

# ---------------- SCons / PlatformIO hook ----------------
DefaultEnvironment = None
try:
    from SCons.Script import DefaultEnvironment as _DE  # type: ignore
    DefaultEnvironment = _DE
except Exception:
    pass

def _post_action(target, source, env):
    """
    Called by SCons after building firmware.bin
    target[0] should be .pio/build/<env>/firmware.bin
    """
    try:
        pioenv = env.subst("${PIOENV}")
        projdir = Path(env.subst("${PROJECT_DIR}")).resolve()
        bin_path = Path(str(target[0]))
        if not bin_path.exists():
            print("[rename] ❌ firmware.bin not found:", bin_path)
            return

        fw_ver = _get_define(env, "FW_VERSION", "")
        if not fw_ver:
            fw_ver = time.strftime("%Y%m%d-%H%M%S")

        print(f"[rename] ▶ pack env={pioenv}  ver={fw_ver}")
        package_one(projdir, pioenv, bin_path, fw_ver)
        update_index(projdir)

    except Exception as e:
        print("[rename] ❌ ERROR in post_action:", repr(e))

# Register SCons hook(s)
if DefaultEnvironment is not None:
    env = DefaultEnvironment()
    try:
        print(f"[rename] hook register for env: {env.subst('${PIOENV}')}")
    except Exception:
        print("[rename] hook register (no env name)")

    # Hook multiple aliases to be robust across PlatformIO versions
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _post_action)
    env.AddPostAction("${BUILD_DIR}/${PROGNAME}.bin", _post_action)
    env.AddPostAction("buildprog", _post_action)

# ---------------- Standalone mode ----------------
if __name__ == "__main__":
    # Allow running: `python rename_firmware.py`
    projdir = Path(__file__).resolve().parent
    build_root = projdir / ".pio" / "build"
    if not build_root.exists():
        print("[rename] No .pio/build found. Run `pio run` first.")
        sys.exit(0)

    packaged = 0
    for env_dir in build_root.iterdir():
        fw = env_dir / "firmware.bin"
        if fw.exists():
            env_name = env_dir.name
            # Try read FW_VERSION from env header in platformio.ini (best effort)
            fw_ver = ""
            try:
                # fallback: read from dist/<env>/manifest.json (to keep version consistent)
                old_manifest = projdir / "dist" / env_name / "manifest.json"
                if old_manifest.exists():
                    fw_ver = json.loads(old_manifest.read_text(encoding="utf-8")).get("version", "")
            except Exception:
                pass
            package_one(projdir, env_name, fw, fw_ver or time.strftime("%Y%m%d-%H%M%S"))
            packaged += 1
    update_index(projdir)
    print(f"[rename] Done. Packaged {packaged} env(s).")
