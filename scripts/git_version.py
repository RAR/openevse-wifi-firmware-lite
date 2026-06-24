#
# openevse_lite WiFi-gateway firmware version stamp.
#
# Derives LITE_FW_VERSION from the git revision so every build is traceable to a commit.
# Reported as the gateway `version` in /status and /config (the UI's "WiFi gateway
# firmware" row on the About/Firmware pages). Overrides the "lite-web2" fallback in
# web_server_lite.cpp (#ifndef-guarded). Scoped to the openevse_lite env via platformio.ini.
#
# Format: "lite-<git-describe>", e.g. "lite-1def4ce" (short hash, tag-relative if tags
# exist), with a "-dirty" suffix when TRACKED files differ from HEAD. The embedded UI
# bundle src/lite/web_ui_lite.h is excluded from the dirty check — it is regenerated
# locally and left perpetually modified by design, so it would otherwise pin every build
# to "-dirty". Falls back to "lite-nogit" if git is unavailable.
#
Import("env")
import subprocess

PROJ = env["PROJECT_DIR"]

def git_version():
    try:
        base = subprocess.check_output(
            ["git", "describe", "--tags", "--always"],
            cwd=PROJ, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return "lite-nogit"
    try:
        # rc != 0 => tracked files differ from HEAD, ignoring the perpetually-modified
        # embedded UI bundle. (subprocess.call returns the exit code without raising.)
        rc = subprocess.call(
            ["git", "diff", "--quiet", "--", ".", ":(exclude)src/lite/web_ui_lite.h"],
            cwd=PROJ, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dirty = "-dirty" if rc != 0 else ""
    except Exception:
        dirty = ""
    return "lite-" + base + dirty

version = git_version()
env.Append(CPPDEFINES=[("LITE_FW_VERSION", env.StringifyMacro(version))])
print("git_version: LITE_FW_VERSION = " + version)
