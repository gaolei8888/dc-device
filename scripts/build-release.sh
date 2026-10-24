#!/bin/bash
# Builds a release image of the camera board firmware and writes a manifest next to it.
#
#   scripts/build-release.sh [--allow-dirty]
#
# Output: release/camera_board/<version>/   (git-ignored)
#   bootloader.bin  partition-table.bin  dc_device_camera_board.bin  manifest.json
#
# A release build has no debug.* capabilities and no Wi-Fi settings in it: the network
# comes only from USB setup (docs/usb-setup-protocol-v0.1.md). The version is
# 0.1.0+<git commit>, so two builds can be told apart. The tree must be clean, so the
# commit named in the version is really what was built; --allow-dirty is for trying
# the script and marks the version "-dirty".
#
# Needs Docker (image espressif/idf:release-v5.4, ESP-IDF v5.4) and python3.
set -euo pipefail

IDF_IMAGE="espressif/idf:release-v5.4"
TARGET="esp32s3"
BASE_VERSION="0.1.0"
EXAMPLE="examples/esp32/camera_board"

cd "$(dirname "$0")/.."
ROOT="$PWD"

ALLOW_DIRTY=0
[ "${1:-}" = "--allow-dirty" ] && ALLOW_DIRTY=1

COMMIT_FULL="$(git rev-parse HEAD)"
COMMIT_SHORT="$(git rev-parse --short=7 HEAD)"
DIRTY=0
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  DIRTY=1
  if [ "$ALLOW_DIRTY" != 1 ]; then
    echo "The working tree has uncommitted changes. Commit them, or pass --allow-dirty to try the script." >&2
    exit 1
  fi
fi
VERSION="$BASE_VERSION+$COMMIT_SHORT"
[ "$DIRTY" = 1 ] && VERSION="$VERSION-dirty"

OUT="$ROOT/release/camera_board/$VERSION"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/dc-device-release.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT" "$WORK/build" "$WORK/src"
# The build runs on a copy of the files git knows (tracked, plus untracked that are not ignored):
# the component manager writes managed_components/ and dependencies.lock next to the project,
# and nothing it downloads may land in, or be picked up from, the working tree.
git ls-files -z -co --exclude-standard | tar --null -T - -cf - | tar -x -C "$WORK/src"
cat > "$WORK/release.conf" <<CONF
CONFIG_DC_DEVICE_RELEASE=y
CONFIG_APP_REPRODUCIBLE_BUILD=y
CONF

docker run --rm -v "$WORK/src":/project -v "$WORK":/out -w "/project/$EXAMPLE" "$IDF_IMAGE" \
  idf.py -B /out/build -DIDF_TARGET="$TARGET" -DSDKCONFIG=/out/sdkconfig \
  -DDC_DEVICE_VERSION="$VERSION" \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;/out/release.conf" build

B="$WORK/build"
grep -q '^CONFIG_DC_DEVICE_RELEASE=y' "$WORK/sdkconfig" || { echo "Release option was not applied." >&2; exit 1; }
grep -q '^CONFIG_DC_DEVICE_WIFI_SSID=""' "$WORK/sdkconfig" || { echo "A Wi-Fi name is set in this build." >&2; exit 1; }
# Checks on what was actually built, not on what was asked for.
if grep -aq 'debug\.' "$B/dc_device_camera_board.bin"; then
  echo "The image contains debug.* capabilities." >&2; exit 1
fi
grep -aq "$VERSION" "$B/dc_device_camera_board.bin" || { echo "The version is not in the image." >&2; exit 1; }

cp "$B/bootloader/bootloader.bin" "$OUT/bootloader.bin"
cp "$B/partition_table/partition-table.bin" "$OUT/partition-table.bin"
cp "$B/dc_device_camera_board.bin" "$OUT/dc_device_camera_board.bin"

python3 - "$B/flasher_args.json" "$OUT" "$VERSION" "$COMMIT_FULL" "$DIRTY" "$IDF_IMAGE" "$TARGET" <<'PY'
import hashlib, json, os, sys, datetime
args_path, out, version, commit, dirty, image, target = sys.argv[1:8]
flasher = json.load(open(args_path))
names = {  # path inside the build -> file name in the release folder
    "bootloader/bootloader.bin": "bootloader.bin",
    "partition_table/partition-table.bin": "partition-table.bin",
    "dc_device_camera_board.bin": "dc_device_camera_board.bin",
}
files = []
for offset, path in sorted(flasher["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    name = names[path]
    data = open(os.path.join(out, name), "rb").read()
    files.append({"file": name, "offset": offset, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
settings = flasher["flash_settings"]
manifest = {
    "schema": 1,
    "name": "dc-device camera_board",
    "device_type": "camera_board",
    "version": version,
    "git_commit": commit,
    "dirty": dirty == "1",
    "built_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "build": {"idf_image": image, "idf_version": "v5.4", "target": target},
    "flash": {
        "chip": target,
        "flash_mode": settings["flash_mode"],
        "flash_freq": settings["flash_freq"],
        "flash_size": settings["flash_size"],
        "files": files,
        "erase_whole_flash": False,
    },
    "notes": [
        "Write only the three files at their offsets; never erase the whole flash.",
        "No Wi-Fi settings are in the image. Give them with the USB setup protocol.",
        "After flashing, identify the board by serial (the MAC), not by device_id or address.",
    ],
}
json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=2)
print(json.dumps(manifest["flash"]["files"], indent=2))
PY

echo "Release written to $OUT"
