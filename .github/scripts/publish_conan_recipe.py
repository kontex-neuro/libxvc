#!/usr/bin/env python3
"""Add an immutable versioned prebuilt recipe to a Conan index checkout."""

import argparse
from pathlib import Path
import re


def write_identical_or_new(path: Path, content: str) -> None:
    if path.exists():
        if path.read_text(encoding="utf-8") != content:
            raise RuntimeError(f"existing recipe differs: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    parser.add_argument("recipe_template", type=Path)
    parser.add_argument("recipes_dir", type=Path)
    args = parser.parse_args()
    template = args.recipe_template.read_text(encoding="utf-8")
    content, count = re.subn(
        r'^\s*version\s*=\s*"[^"]*"', f'    version = "{args.version}"',
        template, count=1, flags=re.MULTILINE,
    )
    if count != 1:
        raise RuntimeError("recipe template must contain exactly one version assignment")
    write_identical_or_new(args.recipes_dir / args.version / "conanfile.py", content)
    config = args.recipes_dir / "config.yml"
    config_text = config.read_text(encoding="utf-8") if config.exists() else "versions:\n"
    entry = f'  "{args.version}":\n    folder: {args.version}\n'
    version_line = f'  "{args.version}":\n'
    if version_line in config_text and entry not in config_text:
        raise RuntimeError(f"existing config entry differs for {args.version}")
    if version_line not in config_text:
        if not config_text.endswith("\n"):
            config_text += "\n"
        config.write_text(config_text + entry, encoding="utf-8")


if __name__ == "__main__":
    main()
