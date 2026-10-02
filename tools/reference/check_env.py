"""Read-only inventory; does not install packages or claim scheduler equivalence."""

import importlib.metadata
import json
import sys

REFERENCE_COMMIT = "b558f160a2c0abcb5902acc3c91a14c38a4af173"
PACKAGES = ("torch", "numpy", "pytest", "transformers", "vllm")


def main():
    versions = {}
    for package in PACKAGES:
        try:
            versions[package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            versions[package] = None
    missing = [package for package, version in versions.items() if version is None]
    print(json.dumps({
        "reference_commit": REFERENCE_COMMIT,
        "python": sys.version,
        "packages": versions,
        "missing": missing,
        "scheduler_comparison": "not_run",
        "note": "Package presence alone does not establish a usable or matching upstream build.",
    }, indent=2))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
