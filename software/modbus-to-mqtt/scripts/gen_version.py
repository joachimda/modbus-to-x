from SCons.Script import DefaultEnvironment
import subprocess
import datetime

env = DefaultEnvironment()

def _git(cmd):
    try:
        return subprocess.check_output(cmd, shell=True, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return None

describe = _git("git describe --tags --dirty --always")
tag = _git("git describe --tags --abbrev=0")
commit = _git("git rev-parse --short HEAD")
branch = _git("git rev-parse --abbrev-ref HEAD")
dirty_flag = "1" if _git("git status --porcelain") else "0"

version = describe or commit or "dev"
build_date = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")

flags = [
    f'-DFW_VERSION=\\"{version}\\"',
    f'-DFW_GIT_COMMIT=\\"{commit or ""}\\"',
    f'-DFW_GIT_BRANCH=\\"{branch or ""}\\"',
    f'-DFW_GIT_TAG=\\"{tag or ""}\\"',
    f'-DFW_GIT_DIRTY={dirty_flag}',
    f'-DFW_BUILD_DATE=\\"{build_date}\\"',
]

env.Append(BUILD_FLAGS=flags)