"""
PlatformIO pre-build script that injects the firmware version + git short hash
as a compile-time define:  -DFW_VERSION='"1.0.0-abc1234"'

Place this at the repo root and add to each platformio.ini:
    extra_scripts = pre:../inject_version.py
"""
import subprocess, os

Import("env")  # PlatformIO build environment

# Locate version.h next to this script (repo root)
script_dir = os.path.dirname(os.path.abspath(env.subst("$PROJECT_DIR") + "/../inject_version.py"))

# Parse FW_VERSION_BASE from version.h
version_base = "0.0.0"
version_h = os.path.join(script_dir, "version.h")
if os.path.isfile(version_h):
    with open(version_h) as f:
        for line in f:
            if line.strip().startswith("#define FW_VERSION_BASE"):
                version_base = line.split('"')[1]
                break

# Get short git hash
git_hash = "unknown"
try:
    git_hash = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=script_dir,
        stderr=subprocess.DEVNULL
    ).decode().strip()
except Exception:
    pass

fw_version = "%s-%s" % (version_base, git_hash)

# Check for uncommitted changes (tracked files only) and append -dirty
try:
    status = subprocess.check_output(
        ["git", "diff", "--shortstat"],
        cwd=script_dir,
        stderr=subprocess.DEVNULL
    ).decode().strip()
    staged = subprocess.check_output(
        ["git", "diff", "--cached", "--shortstat"],
        cwd=script_dir,
        stderr=subprocess.DEVNULL
    ).decode().strip()
    if status or staged:
        fw_version += "-dirty"
except Exception:
    pass

print("*** Firmware version: %s ***" % fw_version)

# Inject as a build flag so FW_VERSION is defined before version.h is included
env.Append(CPPDEFINES=[
    ("FW_VERSION", '\\"%s\\"' % fw_version)
])
