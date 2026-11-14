# Claude usage on the dongle's nice!view

The dongle's display shows Claude plan usage in place of the time and keyboard layout:

```
 🔊 44%     volume
 5h  23%    5-hour window, % used
 7d  41%    weekly window, % used
```

`--%` means no data yet. A window shows `0%` once its reset time has passed.

## How it works

**`claude_usage_hid.py`** runs in the background. It writes the usage and the macOS output volume
to the dongle's Raw HID interface (`0x1D50:0x615E`, usage page `0xFF60`). It has two usage
sources and uses whichever is newer.

1. **Usage endpoint (primary).**
   - Every 2 minutes it calls `https://api.anthropic.com/api/oauth/usage`, the endpoint behind
     `/usage`. It is not a documented public API and may change.
   - The token is Claude Code's own login token, read from the Keychain entry
     `Claude Code-credentials` before each fetch. It is never refreshed, stored or logged.
   - The token lasts a few hours, and Claude Code renews it while you use Claude Code. Once it
     has expired, fetches are skipped until you next use Claude Code.
   - A `claude setup-token` token does not work here: it lacks the `user:profile` scope the
     endpoint requires (`403`).
2. **Status line (fallback).**
   - **`claude-statusline.sh`** is the Claude Code status line command (`statusLine` in
     `~/.claude/settings.json`).
   - For Pro/Max plans it receives `rate_limits.five_hour` and `rate_limits.seven_day` and
     caches them in `~/.cache/claude-usage-hid/rate_limits.json`.
   - It runs in terminal `claude` sessions only, not in the VS Code panel.

**`modules/nice_view_claude`** (firmware) draws the values. It is based on
[zzeneg/zmk-nice-view-hid](https://github.com/zzeneg/zmk-nice-view-hid).

## Install / uninstall the sender

```sh
host/install.sh            # venv + launchd agent local.claude-usage-hid
host/install.sh uninstall
tail -f ~/Library/Logs/claude-usage-hid.log
```

This replaces `qmk-hid-host` for this keyboard, so stop that one.

The agent runs a copy installed in `~/.local/share/claude-usage-hid`, because macOS blocks
launchd agents from reading `~/Documents`. Re-run `host/install.sh` after editing
`claude_usage_hid.py`.

## Raw HID messages (32-byte reports)

| byte 0 | payload |
| --- | --- |
| `0xAB` | `[1]` volume 0–100 |
| `0xC0` | `[1]` 5-hour % used, `[2]` 7-day % used (0–100, `0xFF` = unknown) |

The firmware shows "HID not found" if nothing arrives for 65 s. The sender sends a heartbeat
every 20 s.
