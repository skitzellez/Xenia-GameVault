#!/usr/bin/env python3
"""Generate version.h for a direct CMake invocation using the build helper."""
import importlib.util
from pathlib import Path
import sys

source = Path(__file__).resolve().parents[2] / "xenia-build.py"
spec = importlib.util.spec_from_file_location("xenia_build", source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.generate_version_h(sys.argv[1])
