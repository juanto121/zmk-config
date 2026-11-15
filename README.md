# Ferris Sweep (cradio) ZMK config with a display dongle

ZMK firmware for a wireless Ferris Sweep (`cradio`) split keyboard. Both halves connect over
Bluetooth to a **dongle**: a third nice!nano that stays plugged into the computer and has a
**nice!view** display.

The display shows the keyboard's status plus data sent from the Mac: system volume, Claude plan
usage and a focus (Pomodoro) timer.

## What's in this repo

| Path | What it is |
| --- | --- |
| `config/cradio.keymap` | Keymap: 4 layers with home-row mods, plus the focus-timer key |
| `config/cradio.conf`, `config/cradio_dongle.conf` | Kconfig for the halves and the dongle (battery reporting, display) |
| `config/west.yml` | ZMK `v0.3` plus [`zmk-raw-hid`](https://github.com/zzeneg/zmk-raw-hid) |
| `build.yaml` | Firmware the GitHub Action builds: left half, right half, dongle, settings reset |
| `boards/shields/cradio_dongle/` | Dongle shield: the hub that both halves connect to, with no keys of its own |
| `boards/shields/nice_view_claude_adapter/` | Shield that enables the custom display widget |
| `modules/nice_view_claude/` | Custom nice!view widget, based on [zmk-nice-view-hid](https://github.com/zzeneg/zmk-nice-view-hid) |
| `host/` | macOS background sender that feeds volume and Claude usage to the dongle ([details](host/README.md)) |
| `scripts/build.sh` | Builds the firmware locally with Docker (see [From the command line](#from-the-command-line)) |
| `hardware/sauron-dongle/` | Printable Sauron tower dongle case: STLs, editable STEP, CAD source and assembly instructions |

## The display

The nice!view on the dongle, read top to bottom:

```
┌────────────────┐
│ L 82%      USB │  ① keyboard halves' battery · output (USB / Bluetooth)
│ R 79%          │
├────────────────┤
│    🔊 44%      │  ② Mac output volume
│ 5h        19%  │     Claude: % of the 5-hour usage limit used
│ 7d         4%  │     Claude: % of the weekly usage limit used
├────────────────┤
│   /\/\/\/\/\   │  ③ flaming eye looking around, or the focus timer
│  <(:::|::::)>  │     while it runs: FOCUS / BREAK / LONG BRK + mm:ss
│   \/\/\/\/\/   │
│    Today 3     │     focus sessions completed today
└────────────────┘
```

**① Status**
- `L` / `R`: battery of the left and right halves. Shows `--` until a half has reported a level.
- Output symbol:
  - USB symbol: keystrokes go over the USB cable.
  - Wi-Fi-style symbol: keystrokes go over Bluetooth and the current profile is connected.
  - ✕: the current Bluetooth profile is paired but not connected.
  - ⚙: the current Bluetooth profile is empty, ready to pair.
- If `L` and `R` are swapped, set `CONFIG_NICE_VIEW_CLAUDE_SWAP_PERIPHERALS=y` in
  `config/cradio_dongle.conf`. The dongle numbers the halves by pairing order, not by side.

**② Data from the Mac** (sent by `host/claude_usage_hid.py` over Raw HID)
- Volume: the Mac's output volume. A mute icon appears at 0%.
- `5h` and `7d`: how much of the Claude plan's 5-hour and weekly limits has been used.
  - `--%` means no data yet.
  - A limit shows `0%` once its reset time passes.
- If nothing arrives from the Mac for 65 s, these three lines are replaced by **"HID not found"**.

**③ Eye / focus timer**
- Normally: a flaming, lidded eye.
  - Its slit pupil wanders to random spots, pauses, and searches again.
  - The flames flicker. The animation runs at 8 fps.
  - Drawn in `modules/nice_view_claude/src/widgets/eye.c`.
- While the focus timer runs: the phase and a countdown replace the eye.
  - `FOCUS`: 25:00.
  - `BREAK`: 5:00.
  - `LONG BRK`: 30:00, after every 4th completed session of the day.
- `Today N`: focus sessions completed today.
  - Shown under the countdown, and under the eye once it's above 0.
  - Only sessions that run the full 25 minutes count.
  - The count is stored in the dongle's flash, so it survives power loss.
  - It resets when the date changes. The dongle has no clock, so the Mac sender reports the local
    time every 20 s.
- A running countdown restarts from idle after a power loss; only the count is kept.
- Settings in `config/cradio_dongle.conf`:

  | Setting | Default |
  | --- | --- |
  | `CONFIG_NICE_VIEW_CLAUDE_FOCUS_MINUTES` | 25 |
  | `CONFIG_NICE_VIEW_CLAUDE_BREAK_MINUTES` | 5 |
  | `CONFIG_NICE_VIEW_CLAUDE_LONG_BREAK_MINUTES` | 30 |
  | `CONFIG_NICE_VIEW_CLAUDE_LONG_BREAK_EVERY` | 4 |

## Keymap

| Layer | How to reach it | Contents |
| --- | --- | --- |
| Default | – | QWERTY with home-row mods (Shift, Alt, Ctrl, GUI) |
| Right | hold `Backspace` (right thumb) | Numbers, arrows, Home/End/PgUp/PgDn, Caps Word |
| Left | hold `Tab` (left thumb) | Symbols and brackets, **BREAK** |
| Tri | hold both thumbs | F1–F12, media keys, Bluetooth profiles, reset, bootloader, **FOCUS** |

Focus timer keys, both on the `Z` position:

| Key | Where | What it does |
| --- | --- | --- |
| **FOCUS** | Tri layer + `Z` | Starts a focus session; stops the timer while it runs |
| **BREAK** | Left layer (hold `Tab`) + `Z` | Goes straight to a 5-minute break. The interrupted session isn't counted |

## Building and flashing

There are four firmware files:

| Target | Shield(s) | Flash it to |
| --- | --- | --- |
| `dongle` | `cradio_dongle nice_view_adapter nice_view_claude_adapter raw_hid_adapter` | the dongle |
| `left`, `right` | `cradio_left` / `cradio_right`, built as peripherals | each half |
| `reset` | `settings_reset` | any board, to clear stored Bluetooth pairings |

Keymap and display changes only need the dongle reflashed.

### On GitHub

Every push runs the GitHub Action. The `.uf2` files are attached to the run as the `firmware`
artifact.

### From the command line

Requires [Docker](https://www.docker.com/products/docker-desktop/), running. The script uses ZMK's
build image, the same one the GitHub Action uses, so nothing else needs installing.

```sh
scripts/build.sh                # dongle only (default)
scripts/build.sh left right     # one or more of: dongle, left, right, reset
scripts/build.sh all            # all four
```

- **Output:** each build writes `firmware/<target>.uf2`. This folder is git-ignored.
- **First run:** downloads ZMK, Zephyr and the modules (about 2 GB, several minutes) into
  `~/.cache/zmk-workspace`. Set `ZMK_WORKSPACE` to use another folder.
- **Later runs:** reuse that download and rebuild only what changed, which takes well under a
  minute. The download only happens again after `config/west.yml` changes.

To start from a completely clean build, delete `~/.cache/zmk-workspace/build`.

### Flashing

1. Double-tap the reset button on the nice!nano. It mounts as a USB drive (`NICENANO`).
2. Copy the `.uf2` onto that drive. The board reboots by itself when the copy finishes.

### Wiring the nice!view to the dongle

| nice!view | nice!nano v2 / nRF52840 Pro Micro |
| --- | --- |
| MOSI | P0.17 (D2), left column, 6th pin |
| SCK | P0.20 (D3), left column, 7th pin |
| CS | P0.06 (D1), left column, 2nd pin |
| VCC | VCC (3.3 V), right column, 5th pin |
| GND | GND |

Pin positions are counted from the USB end, with the component side facing you.

## Printable tower case

The [Sauron tower case](hardware/sauron-dongle/README.md) holds a 33 × 18 mm
ProMicro nRF52840 flat in its base, with USB-C exiting the rear, and a 36 × 14 mm
nice!view standing upright at 90° to it. The enclosure is 66 × 60 × 152 mm and
has a removable controller tray and rear display cover.

[Preview](hardware/sauron-dongle/preview.png) ·
[Download the print pack](hardware/sauron-dongle/sauron-dongle-print-pack.zip)

The pack includes three print-oriented STLs, a display fit gauge, editable STEP
files and parametric CadQuery source. Geometry is digitally checked; physical fit
still needs a trial with your boards. Print the tray and gauge first, and follow
the hardware guide for supports, screws and wiring clearance.

## Mac companion

`host/` contains the sender that runs in the background on macOS:

- **Volume:** read through CoreAudio.
- **Claude plan usage:** read from the endpoint behind Claude Code's `/usage`, with the Claude
  Code status line as a fallback.
- **Delivery:** both are written to the dongle's Raw HID interface.

```sh
host/install.sh            # install as a launchd agent
host/install.sh uninstall
```

See [host/README.md](host/README.md) for how it works and the Raw HID message format.
