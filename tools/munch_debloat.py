#!/usr/bin/env python3
"""Load and validate the Munch launchd debloat profile.

The profile deliberately operates on launchd job definitions only. It never
deletes iOS binaries, frameworks, cryptex files, or hardware firmware.
"""

from __future__ import annotations

import json
import os
from typing import Any


PROFILE_KEYS = {
    "name",
    "description",
    "remove_labels",
    "protected_labels",
    "required_paths",
}


def _labels(profile: dict[str, Any], key: str) -> tuple[str, ...]:
    value = profile.get(key, [])
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        raise ValueError(f"profile field {key!r} must be a list of non-empty strings")
    if len(set(value)) != len(value):
        raise ValueError(f"profile field {key!r} contains duplicate labels")
    return tuple(value)


def load_profile(path: str) -> dict[str, Any]:
    with open(path, encoding="utf-8") as stream:
        profile = json.load(stream)
    if not isinstance(profile, dict):
        raise ValueError("debloat profile must be a JSON object")
    unknown = set(profile) - PROFILE_KEYS
    if unknown:
        raise ValueError(f"unknown profile field(s): {', '.join(sorted(unknown))}")
    if not isinstance(profile.get("name", ""), str) or not profile.get("name", "").strip():
        raise ValueError("profile field 'name' must be a non-empty string")
    for key in ("remove_labels", "protected_labels", "required_paths"):
        _labels(profile, key)
    removed = set(_labels(profile, "remove_labels"))
    protected = set(_labels(profile, "protected_labels"))
    overlap = removed & protected
    if overlap:
        raise ValueError(f"labels cannot be both removed and protected: {', '.join(sorted(overlap))}")
    return profile


def should_remove(label: str, profile: dict[str, Any]) -> bool:
    return label in set(profile.get("remove_labels", ()))


def is_protected(label: str, profile: dict[str, Any]) -> bool:
    return label in set(profile.get("protected_labels", ()))


def validate_source(profile: dict[str, Any], src_root: str) -> None:
    """Check that protected capability jobs and binaries exist in the fetched rootfs."""

    missing_jobs = [
        label
        for label in profile.get("protected_labels", ())
        if not os.path.isfile(os.path.join(src_root, "System/Library/LaunchDaemons", f"{label}.plist"))
    ]
    if missing_jobs:
        raise ValueError("protected launchd jobs are missing: " + ", ".join(missing_jobs))

    missing_paths = [
        path
        for path in profile.get("required_paths", ())
        if not os.path.exists(os.path.join(src_root, path.lstrip("/")))
    ]
    if missing_paths:
        raise ValueError("required iOS paths are missing: " + ", ".join(missing_paths))


def verify_output(profile: dict[str, Any], dest: str) -> None:
    """Ensure no profile-selected job was emitted into the image."""

    leaked = [
        label
        for label in profile.get("remove_labels", ())
        if os.path.exists(os.path.join(dest, f"{label}.plist"))
    ]
    if leaked:
        raise ValueError("debloat jobs still present in output: " + ", ".join(leaked))


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile")
    parser.add_argument("rootfs")
    parser.add_argument("output", nargs="?")
    args = parser.parse_args()
    profile = load_profile(args.profile)
    validate_source(profile, args.rootfs)
    if args.output:
        verify_output(profile, args.output)
    print(f"{profile['name']}: profile is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
