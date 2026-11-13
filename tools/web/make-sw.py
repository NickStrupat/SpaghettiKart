"""Writes the browser build's service worker (src/port/web/sw.js, docs/WEB.md "Offline") next to the page, with the
list of the page's files and the SHA-256 of each filled in. Run by cmake/web/WebTarget.cmake after every build.

    python3 tools/web/make-sw.py src/port/web/sw.js build-web/web

The player's mk64.o2r, dot files and source maps are left out. The file is rewritten only when it changes.
"""
import hashlib
import json
import pathlib
import sys

kPlaceholder = "{/* the list */}"
kLeftOut = {"mk64.o2r", "sw.js"}


def main():
    template_path, web_dir = (pathlib.Path(arg) for arg in sys.argv[1:3])
    files = {}
    for path in sorted(web_dir.iterdir()):
        if path.is_file() and path.name not in kLeftOut and not path.name.startswith(".") \
                and path.suffix != ".map":
            files[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    template = template_path.read_text()
    if kPlaceholder not in template:
        sys.exit(f"{template_path} has no {kPlaceholder}")
    script = template.replace(kPlaceholder, json.dumps(files, indent=2))
    out = web_dir / "sw.js"
    if not out.exists() or out.read_text() != script:
        out.write_text(script)


if __name__ == "__main__":
    main()
