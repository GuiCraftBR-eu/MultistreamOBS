"""Build the strictly scoped automatic-update package for a Windows release."""
import argparse
import base64
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("plugin_dir", type=Path)
parser.add_argument("version")
parser.add_argument("output", type=Path)
args = parser.parse_args()
if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
    parser.error("version must be a stable three-part version")
root = args.plugin_dir.resolve()
files = []
for path in sorted((root / "data" / "locale").glob("*.ini")) + [root / "qt-plugins/tls/qschannelbackend.dll", root / "multistream-obs.dll"]:
    if path.is_symlink() or not path.is_file():
        raise SystemExit(f"Unsafe or missing file: {path}")
    relative = path.relative_to(root).as_posix()
    if relative not in ("multistream-obs.dll", "qt-plugins/tls/qschannelbackend.dll") and not re.fullmatch(r"data/locale/[A-Za-z0-9]+(?:-[A-Za-z0-9]+)*\.ini", relative):
        raise SystemExit(f"Unsupported locale filename: {relative}")
    data = path.read_bytes()
    if not data or len(data) > 8 * 1024 * 1024:
        raise SystemExit(f"Invalid file size: {relative}")
    if relative.endswith(".dll") and not data.startswith(b"MZ"):
        raise SystemExit("The update DLL is not a Windows executable")
    files.append({"path": relative, "data": base64.b64encode(data).decode("ascii")})
if len(files) < 2 or len(files) > 128:
    raise SystemExit("Missing plugin locales or too many files")
payload = json.dumps({"format": 1, "version": args.version, "obs_major": 33, "files": files}, separators=(",", ":")).encode()
if len(payload) > 16 * 1024 * 1024:
    raise SystemExit("Update package exceeds the maximum size")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(payload)
