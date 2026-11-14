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
│      (1)       │  ③ Bluetooth profile, or the focus timer while it runs:
│                │       FOCUS 24:13 / BREAK 4:59
├────────────────┤
│ Default Layer  │  ④ active keymap layer
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

**③ Profile / focus timer**
- Normally: the active Bluetooth profile number in a circle.
- While the focus timer runs: `FOCUS` with a 25:00 countdown, then `BREAK` with 5:00.
  - The timer runs on the dongle and resets if the dongle loses power.
  - Change the lengths with `CONFIG_NICE_VIEW_CLAUDE_FOCUS_MINUTES` / `..._BREAK_MINUTES`.

**④ Layer**
- The `display-name` of the active layer in `config/cradio.keymap`.

## Keymap

| Layer | How to reach it | Contents |
| --- | --- | --- |
| Default | – | QWERTY with home-row mods (Shift, Alt, Ctrl, GUI) |
| Right | hold `Backspace` (right thumb) | Numbers, arrows, Home/End/PgUp/PgDn, Caps Word |
| Left | hold `Tab` (left thumb) | Symbols and brackets |
| Tri | hold both thumbs | F1–F12, media keys, Bluetooth profiles, reset, bootloader, **FOCUS** |

**FOCUS** (Tri layer, bottom-left) starts the focus timer, or stops it while it runs.

## Building and flashing

Every push runs the GitHub Action, which builds these firmware files:

- `cradio_left`, `cradio_right`: the two halves, both as peripherals.
- `cradio_dongle nice_view_adapter nice_view_claude_adapter raw_hid_adapter`: the dongle.
- `settings_reset`: clears stored Bluetooth pairings.

To flash, double-tap reset on a nice!nano so it mounts as a USB drive, then copy its `.uf2` onto
it. Keymap and display changes only need the dongle reflashed.

### Wiring the nice!view to the dongle

| nice!view | nice!nano v2 / nRF52840 Pro Micro |
| --- | --- |
| MOSI | P0.17 (D2), left column, 6th pin |
| SCK | P0.20 (D3), left column, 7th pin |
| CS | P0.06 (D1), left column, 2nd pin |
| VCC | VCC (3.3 V), right column, 5th pin |
| GND | GND |

Pin positions are counted from the USB end, with the component side facing you.

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
