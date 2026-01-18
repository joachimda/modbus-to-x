#!/usr/bin/env python3
# Ensures required Python build dependencies are available in PlatformIO's venv.
# Runs on import (as PlatformIO imports extra_scripts) and fails the build if installation cannot be completed.

import sys
import subprocess

REQUIRED_PKGS = {
    "intelhex": "intelhex>=2.3.0",
}

def ensure_package(module_name: str, pip_spec: str) -> None:
    try:
        __import__(module_name)
        print(f"[deps] '{module_name}' already available")
    except ImportError:
        print(f"[deps] '{module_name}' not found. Installing '{pip_spec}' ...")
        try:
            subprocess.check_call([sys.executable, "-m", "pip", "install", pip_spec])
            __import__(module_name)  # verify
            print(f"[deps] Installed '{pip_spec}' successfully")
        except Exception as exc:
            print(f"[deps] Failed to install '{pip_spec}': {exc}")
            sys.exit(1)

def main() -> None:
    print("[deps] Running build-time dependency check")
    for module_name, pip_spec in REQUIRED_PKGS.items():
        ensure_package(module_name, pip_spec)

main()