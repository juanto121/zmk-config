#!/bin/bash
# Installs the Claude usage sender as a launchd agent (starts at login, restarts on exit).
# Usage: host/install.sh            install/refresh
#        host/install.sh uninstall  stop and remove the agent
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
label="local.claude-usage-hid"
plist="$HOME/Library/LaunchAgents/$label.plist"
log="$HOME/Library/Logs/claude-usage-hid.log"
# launchd agents can't read ~/Documents (macOS privacy protection), so the sender
# and its venv are installed outside the repo. Re-run this script after editing it.
app="$HOME/.local/share/claude-usage-hid"

launchctl bootout "gui/$(id -u)/$label" 2>/dev/null || true
# bootout returns before the job is gone; bootstrapping too early fails with error 5
for _ in $(seq 50); do
  launchctl print "gui/$(id -u)/$label" >/dev/null 2>&1 || break
  sleep 0.1
done

if [ "${1:-}" = "uninstall" ]; then
  rm -f "$plist"
  rm -rf "$app"
  echo "removed $label"
  exit 0
fi

mkdir -p "$app"
if [ ! -x "$app/venv/bin/python" ]; then
  /usr/bin/python3 -m venv "$app/venv"
fi
"$app/venv/bin/pip" install -q --disable-pip-version-check hidapi
cp "$here/claude_usage_hid.py" "$app/claude_usage_hid.py"

mkdir -p "$(dirname "$plist")"
cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$label</string>
  <key>ProgramArguments</key>
  <array>
    <string>$app/venv/bin/python</string>
    <string>$app/claude_usage_hid.py</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Background</string>
  <key>StandardOutPath</key><string>$log</string>
  <key>StandardErrorPath</key><string>$log</string>
</dict>
</plist>
EOF

launchctl bootstrap "gui/$(id -u)" "$plist"
echo "installed $label (log: $log)"
