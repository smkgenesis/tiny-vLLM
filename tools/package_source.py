"""Create a source-only archive, without build trees, results, Git, or environments."""
import argparse
from pathlib import Path
import zipfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    source = Path(__file__).resolve().parents[1]
    selected = [source / name for name in ("README.md", "LICENSE", "CMakeLists.txt", "CMakePresets.json", ".gitignore", ".gitattributes")]
    for directory in ("include", "src", "rtl", "examples", "tests", "tools", "docs"):
        selected.extend(path for path in (source / directory).rglob("*")
                        if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc")
    if any(path.is_symlink() for path in selected):
        p.error("Source archive does not follow symlinks")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(selected):
            archive.write(path, "tiny-vllm/" + path.relative_to(source).as_posix())
    print(f"Packaged {len(selected)} source files: {args.output}")


if __name__ == "__main__":
    main()
