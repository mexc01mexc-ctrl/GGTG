#!/usr/bin/env python3
"""
tools/relocate_paths.py
Updates hardcoded paths in desktop/Libraries and build files when the project
is moved to a new directory or another computer with a different username.
"""

import os
import sys

def relocate(project_root, old_path="/Users/macbook/Desktop/GGTG", new_path=None):
    if new_path is None:
        new_path = os.path.abspath(project_root)

    old_path = os.path.abspath(old_path)
    new_path = os.path.abspath(new_path)

    if old_path == new_path:
        print(f"Path unchanged: {new_path}")
        return

    print(f"Relocating project paths:\n  FROM: {old_path}\n  TO:   {new_path}\n")

    targets = [
        os.path.join(project_root, "desktop", "Libraries", "local"),
        os.path.join(project_root, "desktop", "nicegram-desktop", "out"),
    ]

    skip_extensions = (".a", ".o", ".dylib", ".so", ".bin", ".pcm", ".dat", ".png", ".jpg", ".icns", ".gif", ".zip", ".tar", ".gz")
    replaced_count = 0
    files_modified = 0

    for target in targets:
        if not os.path.exists(target):
            continue
        for root, _, files in os.walk(target):
            for f in files:
                if any(f.endswith(ext) for ext in skip_extensions):
                    continue
                file_path = os.path.join(root, f)
                try:
                    with open(file_path, "r", encoding="utf-8", errors="ignore") as fp:
                        content = fp.read()
                    if old_path in content:
                        new_content = content.replace(old_path, new_path)
                        count = content.count(old_path)
                        with open(file_path, "w", encoding="utf-8", errors="ignore") as fp:
                            fp.write(new_content)
                        replaced_count += count
                        files_modified += 1
                except Exception as e:
                    print(f"Warning: could not process {file_path}: {e}")

    print(f"Done: modified {files_modified} files, replaced {replaced_count} path occurrences.")

if __name__ == "__main__":
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    old_p = sys.argv[1] if len(sys.argv) > 1 else "/Users/macbook/Desktop/GGTG"
    new_p = sys.argv[2] if len(sys.argv) > 2 else root
    relocate(root, old_p, new_p)

