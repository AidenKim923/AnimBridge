"""Read-only probe executed by the bundled interpreter in isolated mode."""
import argparse
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", required=True, type=Path)
    args = parser.parse_args()
    expected = Path(__file__).resolve().parent / "src"
    if args.backend.resolve() != expected.resolve():
        raise SystemExit("The probe only accepts its own bundled backend")
    if not sys.flags.isolated or not sys.dont_write_bytecode:
        raise SystemExit("The probe requires -I -B")
    sys.path.insert(0, str(expected))
    import numpy
    import yaml
    import unityanimbridge
    print(json.dumps({"executable": sys.executable, "version": list(sys.version_info[:3]), "backend": unityanimbridge.__file__,
                      "isolated": sys.flags.isolated, "dont_write_bytecode": sys.dont_write_bytecode,
                      "numpy": numpy.__version__, "yaml": yaml.__version__,
                      "numpy_path": numpy.__file__, "yaml_path": yaml.__file__, "sys_path": sys.path}))


if __name__ == "__main__":
    main()
