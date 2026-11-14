#!/bin/bash
# Claude Code status line command.
#
# Claude Code pipes session JSON on stdin. For Pro/Max subscribers it includes
#   .rate_limits.five_hour.{used_percentage,resets_at}
#   .rate_limits.seven_day.{used_percentage,resets_at}
# This script saves those windows to a cache file that claude_usage_hid.py
# sends to the nice!view, then prints a short status line.

input=$(cat)

cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/claude-usage-hid"
cache="$cache_dir/rate_limits.json"

# Windows present in this update (each one can be absent independently).
new=$(printf '%s' "$input" | jq -c '(.rate_limits // {}) | {five_hour, seven_day}
  | with_entries(select(.value.used_percentage != null))' 2>/dev/null)

if [ -n "$new" ] && [ "$new" != "{}" ]; then
  mkdir -p "$cache_dir"
  old=$(jq -c . "$cache" 2>/dev/null) || old='{}'
  [ -n "$old" ] || old='{}'
  tmp="$cache.$$.tmp"
  if jq -n --argjson old "$old" --argjson new "$new" --argjson now "$(date +%s)" \
      '$old + $new + {updated_at: $now}' > "$tmp"; then
    mv -f "$tmp" "$cache"
  else
    rm -f "$tmp"
  fi
fi

# Status line text: model plus the cached windows (0% once a window has reset).
model=$(printf '%s' "$input" | jq -r '.model.display_name // empty' 2>/dev/null)
usage=$(jq -r --argjson now "$(date +%s)" '
  def pct(w): if w == null then "--"
    elif (w.resets_at // 0) <= $now then "0%"
    else "\(w.used_percentage | floor)%" end;
  "5h \(pct(.five_hour)) · 7d \(pct(.seven_day))"' "$cache" 2>/dev/null)

printf '%s\n' "${model:+[$model] }${usage:-5h -- · 7d --}"
