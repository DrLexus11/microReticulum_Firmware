"""Supply a host C-library sysroot when the OS development headers are absent.

Most Linux hosts need no special handling. SteamOS can provide the glibc
runtime without /usr/include; an active Conda installation supplies a complete
compatible sysroot in that case.
"""

import os
from pathlib import Path

Import("env")  # noqa: F821


if not Path("/usr/include/features.h").is_file():
    conda_prefix = os.environ.get("CONDA_PREFIX")
    if conda_prefix:
        sysroot = (
            Path(conda_prefix)
            / "x86_64-conda-linux-gnu"
            / "sysroot"
        )
        if (sysroot / "usr/include/features.h").is_file():
            sysroot_flag = f"--sysroot={sysroot}"
            env.Append(CCFLAGS=[sysroot_flag], LINKFLAGS=[sysroot_flag])
            print(f"[rrc-test] Using Conda host sysroot: {sysroot}")
        else:
            print(
                "[rrc-test] WARNING: host development headers are absent and "
                "the active Conda environment has no compatible sysroot"
            )
