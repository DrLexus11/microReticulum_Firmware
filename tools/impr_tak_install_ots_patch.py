#!/usr/bin/env python3
"""Restore the IMPR OpenTAK 1.7.13 EUD stability patch after reinstalls."""

from __future__ import annotations

import py_compile
import shutil
from pathlib import Path

EXPECTED_VERSION = "1.7.13"
MARKER = "IMPR_TAK_STABILITY_PATCH_V1"
SOURCE = Path.home() / "projects/OpenTAKServer/opentakserver/eud_handler/EudHandler.py"
SITE_PACKAGES = next(
    (Path.home() / ".impr-tak/ots-venv/lib").glob("python*/site-packages"), None
)

if SITE_PACKAGES is None:
    raise SystemExit("OpenTAK virtual environment is missing")

metadata_files = list(SITE_PACKAGES.glob("opentakserver-*.dist-info/METADATA"))
if len(metadata_files) != 1:
    raise SystemExit("Cannot identify the installed OpenTAK version")

version = next(
    (line.split(":", 1)[1].strip() for line in metadata_files[0].read_text().splitlines()
     if line.startswith("Version:")),
    None,
)
if version != EXPECTED_VERSION:
    raise SystemExit(
        f"Refusing to patch OpenTAK {version!r}; repair is verified for {EXPECTED_VERSION}"
    )

# The patched EudHandler is newer than the installed 1.7.13 and imports
# opentakserver.models.CITrap, which that release does not ship. Installing the
# model is the wrong repair: CITrap declares a relationship back-populating
# "citrap" on Point, 1.7.13's Point has no such property, and registering it
# turns an ImportError into
#   InvalidRequestError: Mapper 'Mapper[Point(points)]' has no property 'citrap'
# which fails at mapper configuration -- so ATAK connects and is dropped.
#
# The import is unused by this file. It sits in the block marked "required by
# SQLAlchemy", whose only purpose is model registration, and registering this
# particular model is precisely what 1.7.13 cannot support. Dropping the line
# keeps the stability patch and removes only the part the release cannot carry.
CITRAP_IMPORT = "from opentakserver.models.CITrap import CITrap\n"

source_text = SOURCE.read_text()
if MARKER not in source_text:
    raise SystemExit(f"Canonical repair marker is missing from {SOURCE}")
if CITRAP_IMPORT in source_text:
    source_text = source_text.replace(CITRAP_IMPORT, "")


TARGET = SITE_PACKAGES / "opentakserver/eud_handler/EudHandler.py"
target_text = TARGET.read_text() if TARGET.exists() else ""
if target_text != source_text:
    backup = TARGET.with_suffix(".py.pre-impr")
    if TARGET.exists() and not backup.exists():
        shutil.copy2(TARGET, backup)
    TARGET.write_text(source_text)
    print(f"Restored OpenTAK EUD stability patch in {TARGET}")

py_compile.compile(str(TARGET), doraise=True)
