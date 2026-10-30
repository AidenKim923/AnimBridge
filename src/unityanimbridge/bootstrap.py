"""Standalone launcher for the private CPython worker (stdlib only).

Invoked as: python -I -B -X utf8 bootstrap.py --backend <backend>/src -- -m <module> [args...]
Only the backend source directory is added to sys.path; the working directory, user site and
environment-provided paths are never imported (the interpreter runs in isolated mode).
"""
import os
import json
from pathlib import Path
import runpy
import sys


def main(argv):
    if len(argv) < 5 or argv[0] != "--backend" or argv[2] != "--":
        sys.stderr.write("usage: bootstrap.py --backend <src> -- -m <module> [args...]\n")
        return 2
    if not sys.flags.isolated:
        sys.stderr.write("bootstrap.py: an isolated interpreter (-I) is required\n")
        return 2
    if not os.path.isabs(argv[1]):
        sys.stderr.write("bootstrap.py: backend must be absolute\n")
        return 2
    backend = os.path.realpath(argv[1])
    rest = argv[3:]
    if len(rest) < 2 or rest[0] != "-m" or rest[1].split(".")[0] != "unityanimbridge":
        sys.stderr.write("bootstrap.py: expected '-m unityanimbridge.<module>' after '--'\n")
        return 2
    if not os.path.isdir(os.path.join(backend, "unityanimbridge")):
        sys.stderr.write("bootstrap.py: backend source directory is invalid\n")
        return 2
    if os.environ.get("UAB_RUNTIME_MODE") == "bundled":
        try:
            value = os.environ.get("UAB_RUNTIME_ROOT")
            manifest_path = os.environ.get("UAB_RUNTIME_MANIFEST")
            if not value and not manifest_path:
                raise ValueError("the host runtime manifest is missing")
            root = Path(value) if value else Path(manifest_path).parent
            if not value and root.name in ("Resources", "share"):
                root = root.parent
            root = root.resolve()
            path = Path(manifest_path) if manifest_path else next((root / p for p in
                ("runtime-manifest.json", "Resources/runtime-manifest.json", "share/runtime-manifest.json") if (root / p).is_file()), None)
            if path is None or not path.resolve().is_relative_to(root):
                raise ValueError("the runtime manifest is outside the bundle")
            manifest = json.loads(path.read_text(encoding="utf-8-sig"))
            if manifest.get("schemaVersion") != 1:
                raise ValueError("unsupported runtime manifest")
            def component(name, key):
                relative = manifest["components"][name][key].replace("\\", "/")
                if Path(relative).is_absolute() or ":" in relative or ".." in relative.split("/"):
                    raise ValueError("component path escapes the bundle")
                resolved = (root / relative).resolve()
                if not resolved.is_relative_to(root):
                    raise ValueError("component path escapes the bundle")
                return resolved
            expected = component("backend", "resourcePath")
            if Path(backend) != expected / "src" or Path(os.environ.get("UAB_BACKEND_DIR", "")).resolve() != expected:
                raise ValueError("backend override differs from the runtime manifest")
            if Path(sys.executable).resolve() != component("python", "executablePath"):
                raise ValueError("interpreter override differs from the runtime manifest")
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
            sys.stderr.write("bootstrap.py: " + str(error) + "\n")
            return 2
    prefixes = [os.path.realpath(value) for value in (sys.prefix, sys.base_prefix)]
    def private_path(value):
        if not value:
            return False
        resolved = os.path.realpath(value)
        try:
            return any(os.path.normcase(os.path.commonpath((prefix, resolved))) == os.path.normcase(prefix) for prefix in prefixes)
        except ValueError:
            return False
    sys.dont_write_bytecode = True
    sys.path[:] = [backend] + [p for p in sys.path if private_path(p) and os.path.realpath(p) != backend]
    sys.argv = [rest[1]] + rest[2:]
    runpy.run_module(rest[1], run_name="__main__", alter_sys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
