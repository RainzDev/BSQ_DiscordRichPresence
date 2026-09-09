#!/usr/bin/env python3
"""Inject and verify MBF's optional manifest-requirements QMOD extension."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tempfile
import zipfile
from pathlib import Path
from typing import Any


FIELD_NAME = "mbfManifestRequirements"
PACKAGE_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+")
MAX_QUERY_PACKAGES = 32


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"Could not read JSON from {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def validate_requirements(value: Any) -> dict[str, list[str]]:
    if not isinstance(value, dict) or set(value) != {"queryPackages"}:
        raise ValueError(f"{FIELD_NAME} must contain only queryPackages")

    packages = value["queryPackages"]
    if not isinstance(packages, list) or not 1 <= len(packages) <= MAX_QUERY_PACKAGES:
        raise ValueError(
            f"queryPackages must contain between 1 and {MAX_QUERY_PACKAGES} package names"
        )
    if any(not isinstance(package, str) for package in packages):
        raise ValueError("Every queryPackages item must be a string")
    if len(set(packages)) != len(packages):
        raise ValueError("queryPackages must not contain duplicates")

    for package in packages:
        if len(package) > 255 or PACKAGE_NAME.fullmatch(package) is None:
            raise ValueError(f"Invalid Android package name: {package!r}")
    return {"queryPackages": packages}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def archive_entries(path: Path) -> tuple[list[zipfile.ZipInfo], list[bytes], bytes]:
    try:
        with zipfile.ZipFile(path, "r") as archive:
            infos = archive.infolist()
            data = [archive.read(info) for info in infos]
            return infos, data, archive.comment
    except (OSError, zipfile.BadZipFile, RuntimeError) as error:
        raise ValueError(f"Could not read QMOD archive {path}: {error}") from error


def verify_archive(
    path: Path,
    expected_names: list[str],
    expected_requirements: dict[str, list[str]],
    expected_payload_hashes: list[tuple[int, str]],
) -> None:
    infos, data, _ = archive_entries(path)
    names = [info.filename for info in infos]
    if names != expected_names:
        raise ValueError("QMOD entry order or names changed during manifest injection")

    manifest_indexes = [index for index, name in enumerate(names) if name == "mod.json"]
    if len(manifest_indexes) != 1:
        raise ValueError("QMOD must contain exactly one root mod.json")
    manifest_index = manifest_indexes[0]
    try:
        manifest = json.loads(data[manifest_index].decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"Injected mod.json is invalid: {error}") from error
    if not isinstance(manifest, dict) or manifest.get(FIELD_NAME) != expected_requirements:
        raise ValueError(f"Final mod.json does not contain the expected {FIELD_NAME}")

    actual_payload_hashes = [
        (index, sha256(contents))
        for index, contents in enumerate(data)
        if index != manifest_index
    ]
    if actual_payload_hashes != expected_payload_hashes:
        raise ValueError("A non-manifest QMOD payload changed during manifest injection")


def inject(qmod_path: Path, template_path: Path) -> bool:
    template = read_json(template_path)
    if FIELD_NAME not in template:
        raise ValueError(f"{template_path} does not declare {FIELD_NAME}")
    requirements = validate_requirements(template[FIELD_NAME])

    infos, data, comment = archive_entries(qmod_path)
    names = [info.filename for info in infos]
    manifest_indexes = [index for index, name in enumerate(names) if name == "mod.json"]
    if len(manifest_indexes) != 1:
        raise ValueError("QMOD must contain exactly one root mod.json")
    manifest_index = manifest_indexes[0]

    try:
        manifest = json.loads(data[manifest_index].decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"QMOD mod.json is invalid: {error}") from error
    if not isinstance(manifest, dict):
        raise ValueError("QMOD mod.json must contain a JSON object")

    payload_hashes = [
        (index, sha256(contents))
        for index, contents in enumerate(data)
        if index != manifest_index
    ]
    if manifest.get(FIELD_NAME) == requirements:
        verify_archive(qmod_path, names, requirements, payload_hashes)
        return False

    manifest[FIELD_NAME] = requirements
    data[manifest_index] = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode(
        "utf-8"
    )

    qmod_path = qmod_path.resolve()
    temporary_path: Path | None = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{qmod_path.name}.", suffix=".tmp", dir=qmod_path.parent
        )
        os.close(descriptor)
        temporary_path = Path(temporary_name)
        with zipfile.ZipFile(temporary_path, "w") as archive:
            archive.comment = comment
            for info, contents in zip(infos, data, strict=True):
                archive.writestr(info, contents)

        os.chmod(temporary_path, qmod_path.stat().st_mode)
        verify_archive(temporary_path, names, requirements, payload_hashes)
        os.replace(temporary_path, qmod_path)
        temporary_path = None
        verify_archive(qmod_path, names, requirements, payload_hashes)
        return True
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qmod", type=Path, help="QMOD archive produced by QPM.CLI")
    parser.add_argument(
        "--template", type=Path, default=Path("mod.template.json"), help="source mod template"
    )
    arguments = parser.parse_args()

    try:
        changed = inject(arguments.qmod, arguments.template)
    except ValueError as error:
        parser.error(str(error))
    print(
        f"{'Injected and verified' if changed else 'Already present and verified'} "
        f"{FIELD_NAME} in {arguments.qmod}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
