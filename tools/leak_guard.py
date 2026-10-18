"""Refuse a commit that would put story text in the public repo.

Run from the pre-commit hook (.githooks/pre-commit). It fails when a staged path is under
Data/Story/, or when a staged added line contains a string of 32+ characters taken from the
local Data/Story/*.json copy. Without a local copy only the path check runs.
"""
import json
import pathlib
import subprocess
import sys

MIN_LEN = 32


def strings(node, out):
    if isinstance(node, str):
        text = node.strip()
        if len(text) >= MIN_LEN:
            out.add(text)
    elif isinstance(node, dict):
        for value in node.values():
            strings(value, out)
    elif isinstance(node, list):
        for value in node:
            strings(value, out)


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout


def main():
    root = pathlib.Path(git("rev-parse", "--show-toplevel").strip())
    staged = [p for p in git("diff", "--cached", "--name-only", "--diff-filter=ACMR").splitlines() if p]
    bad = [p for p in staged if p.replace("\\", "/").lower().startswith("data/story/")]
    if bad:
        print("leak_guard: story files are staged:", *bad[:10], sep="\n  ")
        return 1

    story = root / "Data" / "Story"
    if not story.is_dir():
        main_story = pathlib.Path(git("rev-parse", "--path-format=absolute", "--git-common-dir").strip()).parent / "Data" / "Story"
        story = main_story if main_story.is_dir() else None
    if story is None:
        return 0

    texts = set()
    for path in story.rglob("*.json"):
        try:
            strings(json.loads(path.read_text(encoding="utf-8")), texts)
        except (OSError, ValueError):
            continue
    if not texts:
        return 0

    added = [line[1:] for line in git("diff", "--cached", "-U0", "--no-color", "--text").splitlines()
             if line.startswith("+") and not line.startswith("+++")]
    blob = "\n".join(added)
    hits = [t for t in texts if t in blob]
    if hits:
        print(f"leak_guard: {len(hits)} story string(s) found in staged changes. Unstage them.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
