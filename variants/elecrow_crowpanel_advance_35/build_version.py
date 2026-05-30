#  Injects the current git short-hash as FW_GIT_REV so the on-device Node Info
#  page can show exactly what's flashed. "+dirty" is appended when the working
#  tree has uncommitted changes. Falls back to "nogit" if git isn't available.
Import("env")
import subprocess

def git_rev():
    d = env["PROJECT_DIR"]
    try:
        rev = subprocess.check_output(["git", "rev-parse", "--short=8", "HEAD"], cwd=d).decode().strip()
        if subprocess.run(["git", "diff", "--quiet"], cwd=d).returncode != 0:
            rev += "+dirty"
        return rev
    except Exception:
        return "nogit"

env.Append(CPPDEFINES=[("FW_GIT_REV", env.StringifyMacro(git_rev()))])
