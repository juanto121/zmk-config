#!/bin/bash
# Build the firmware locally with ZMK's Docker image (the same one GitHub Actions uses).
#
#   scripts/build.sh                  # dongle (default)
#   scripts/build.sh left right       # one or more of: dongle left right reset
#   scripts/build.sh all
#
# Output: firmware/<target>.uf2
# The ZMK/Zephyr checkout (~2 GB) lives in $ZMK_WORKSPACE (default ~/.cache/zmk-workspace)
# and is only re-fetched when config/west.yml changes.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
workspace="${ZMK_WORKSPACE:-$HOME/.cache/zmk-workspace}"
image="zmkfirmware/zmk-build-arm:stable"
out="$repo/firmware"

targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(dongle)
[ "${targets[0]}" = "all" ] && targets=(left right dongle reset)

if ! docker info >/dev/null 2>&1; then
  echo "Docker is not running. Start Docker Desktop and try again." >&2
  exit 1
fi

mkdir -p "$workspace" "$out"

for target in "${targets[@]}"; do
  peripheral="-DCONFIG_ZMK_SPLIT=y -DCONFIG_ZMK_SPLIT_ROLE_CENTRAL=n"
  case "$target" in
    dongle) shield="cradio_dongle nice_view_adapter nice_view_claude_adapter raw_hid_adapter"; extra="" ;;
    left)   shield="cradio_left";    extra="$peripheral" ;;
    right)  shield="cradio_right";   extra="$peripheral" ;;
    reset)  shield="settings_reset"; extra="" ;;
    *) echo "unknown target '$target' (use: dongle left right reset all)" >&2; exit 2 ;;
  esac

  echo "==> building $target"
  docker run --rm \
    -v "$repo":/src:ro -v "$workspace":/ws -w /ws \
    -e TARGET="$target" -e SHIELD="$shield" -e EXTRA="$extra" \
    "$image" sh -ec '
      rm -rf config && cp -R /src/config config
      [ -d .west ] || west init -l config
      if ! cmp -s config/west.yml .west-synced.yml; then
        echo "fetching ZMK and Zephyr (first run or west.yml changed)..."
        west update --narrow --fetch-opt=--depth=1
        cp config/west.yml .west-synced.yml
      fi
      west zephyr-export >/dev/null
      west build -p auto -s zmk/app -d "build/$TARGET" -b nice_nano_v2 -- \
        -DZMK_CONFIG=/ws/config -DZMK_EXTRA_MODULES=/src -DSHIELD="$SHIELD" $EXTRA
    '
  cp "$workspace/build/$target/zephyr/zmk.uf2" "$out/$target.uf2"
  echo "==> firmware/$target.uf2"
done
