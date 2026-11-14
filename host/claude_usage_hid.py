#!/usr/bin/env python3
"""Send Claude plan usage and system volume to the Ferris dongle's nice!view.

Usage sources (the most recent one wins):
  1. Endpoint: https://api.anthropic.com/api/oauth/usage, the endpoint behind
     Claude Code's /usage dialog (undocumented, may change). Authenticated with
     Claude Code's own login token, read from the macOS Keychain entry
     "Claude Code-credentials" before every fetch. The token is never refreshed,
     stored or logged here: Claude Code refreshes it while you use it, and when
     it has expired the fetch is skipped.
  2. Status line cache written by claude-statusline.sh (terminal sessions).

Writes to the dongle's Raw HID interface (usage page 0xFF60, usage 0x61) in the
format the nice_view_claude firmware module expects:

    [0xAB, volume]                       volume 0..100
    [0xC0, five_hour_pct, seven_day_pct] 0..100, 0xFF = unknown
"""

import ctypes
import getpass
import json
import logging
import math
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from ctypes import Structure, byref, c_float, c_uint32, sizeof
from datetime import datetime
from pathlib import Path

import hid

VENDOR_ID = 0x1D50
PRODUCT_ID = 0x615E
USAGE_PAGE = 0xFF60
USAGE = 0x61
REPORT_SIZE = 32

MSG_VOLUME = 0xAB
MSG_CLAUDE_USAGE = 0xC0
UNKNOWN = 0xFF

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
KEYCHAIN_SERVICE = "Claude Code-credentials"

POLL_SECONDS = 0.5           # volume check / HID send loop
FETCH_SECONDS = 120          # usage endpoint poll interval
HEARTBEAT_SECONDS = 20       # firmware shows "HID not found" after 65 s of silence
RECONNECT_SECONDS = 3

CACHE_FILE = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "claude-usage-hid" / "rate_limits.json"

log = logging.getLogger("claude-usage-hid")


# --- macOS output volume (CoreAudio) ---------------------------------------

class _PropertyAddress(Structure):
    _fields_ = [("mSelector", c_uint32), ("mScope", c_uint32), ("mElement", c_uint32)]


def _fourcc(code: str) -> int:
    return int.from_bytes(code.encode(), "big")


_coreaudio = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreAudio.framework/CoreAudio")
_SYSTEM_OBJECT = 1


def _get_property(obj: int, selector: str, scope: str, element: int, ctype):
    address = _PropertyAddress(_fourcc(selector), _fourcc(scope), element)
    if not _coreaudio.AudioObjectHasProperty(c_uint32(obj), byref(address)):
        return None
    value = ctype()
    size = c_uint32(sizeof(value))
    err = _coreaudio.AudioObjectGetPropertyData(c_uint32(obj), byref(address), 0, None, byref(size), byref(value))
    return None if err else value.value


def read_volume():
    """Default output volume 0..100 (0 when muted), or None if the device has no volume."""
    device = _get_property(_SYSTEM_OBJECT, "dOut", "glob", 0, c_uint32)
    if not device:
        return None
    if _get_property(device, "mute", "outp", 0, c_uint32):
        return 0
    for selector, element in (("vmvc", 0), ("volm", 0)):
        scalar = _get_property(device, selector, "outp", element, c_float)
        if scalar is not None:
            return max(0, min(100, round(scalar * 100)))
    channels = [_get_property(device, "volm", "outp", ch, c_float) for ch in (1, 2)]
    channels = [c for c in channels if c is not None]
    return max(0, min(100, round(sum(channels) / len(channels) * 100))) if channels else None


# --- Usage windows -----------------------------------------------------------
# A "window" is {"pct": float, "resets_at": epoch seconds or None}.

def _epoch(value):
    """resets_at as epoch seconds; accepts ISO 8601 strings, seconds or milliseconds."""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return value / 1000 if value > 1e12 else value
    try:
        return datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def _window(raw, *pct_keys):
    if not isinstance(raw, dict):
        return None
    for key in pct_keys:
        if raw.get(key) is not None:
            return {"pct": float(raw[key]), "resets_at": _epoch(raw.get("resets_at"))}
    return None


def read_status_line_cache():
    """(windows, updated_at) from the status line cache, or (None, 0)."""
    try:
        data = json.loads(CACHE_FILE.read_text())
    except (OSError, ValueError):
        return None, 0.0
    windows = {name: _window(data.get(name), "used_percentage") for name in ("five_hour", "seven_day")}
    return windows, float(data.get("updated_at") or 0)


def read_login_token():
    """Claude Code's current access token, or None if missing or expired. Never logged."""
    for args in (["-a", getpass.getuser()], []):
        result = subprocess.run(
            ["/usr/bin/security", "find-generic-password", "-s", KEYCHAIN_SERVICE, *args, "-w"],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            continue
        try:
            oauth = json.loads(result.stdout).get("claudeAiOauth") or {}
        except ValueError:
            continue
        token = oauth.get("accessToken")
        if not token:
            continue
        expires_at = _epoch(oauth.get("expiresAt"))
        if expires_at is not None and expires_at <= time.time() + 30:
            return None  # expired; Claude Code refreshes it the next time you use it
        return token
    return None


class _RefuseRedirects(urllib.request.HTTPRedirectHandler):
    """urllib copies the Authorization header onto redirects, even to other hosts.
    Never follow one, so the token can only ever go to USAGE_URL."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # turns the 3xx into an HTTPError


_opener = urllib.request.build_opener(_RefuseRedirects)


class EndpointFetcher(threading.Thread):
    """Polls the usage endpoint in the background."""

    def __init__(self):
        super().__init__(daemon=True)
        self._lock = threading.Lock()
        self._windows = None
        self._fetched_at = 0.0
        self._last_problem = None

    def _problem(self, message):
        if message != self._last_problem:  # log each distinct problem once
            log.warning("%s", message)
            self._last_problem = message

    def run(self):
        while True:
            self._fetch_once()
            time.sleep(FETCH_SECONDS)

    def _fetch_once(self):
        token = read_login_token()
        if not token:
            self._problem("no valid Claude Code login token (expired or not found); using status line cache")
            return
        request = urllib.request.Request(USAGE_URL, headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": "oauth-2025-04-20",
            "Accept": "application/json",
            "User-Agent": "claude-usage-hid/1.0",
        })
        del token  # only the Request object holds it from here on
        try:
            with _opener.open(request, timeout=20) as response:
                body = json.load(response)
            if not isinstance(body, dict):
                raise ValueError("unexpected response shape")
        except urllib.error.HTTPError as err:
            self._problem(f"usage endpoint returned HTTP {err.code}: {err.read(200).decode(errors='replace')}")
            return
        except Exception as err:  # network errors, bad JSON, ...: keep the thread alive
            # type and message only: never a traceback, which could show request internals
            self._problem(f"usage fetch failed: {type(err).__name__}: {err}")
            return

        windows = {name: _window(body.get(name), "utilization", "used_percentage")
                   for name in ("five_hour", "seven_day")}
        with self._lock:
            self._windows = windows
            self._fetched_at = time.time()
        if self._last_problem is not None:
            log.info("usage endpoint OK")
            self._last_problem = None

    def latest(self):
        with self._lock:
            return self._windows, self._fetched_at


def current_usage(fetcher):
    """(five_hour, seven_day) used percentages 0..100, UNKNOWN when not available."""
    endpoint, endpoint_at = fetcher.latest()
    cache, cache_at = read_status_line_cache()
    windows = endpoint if endpoint is not None and endpoint_at >= cache_at else cache
    if windows is None:
        return UNKNOWN, UNKNOWN

    now = time.time()

    def pct(window):
        if window is None:
            return UNKNOWN
        if window["resets_at"] is not None and window["resets_at"] <= now:
            return 0  # the window has reset since it was last reported
        return max(0, min(100, math.floor(window["pct"])))

    return pct(windows.get("five_hour")), pct(windows.get("seven_day"))


# --- HID ---------------------------------------------------------------------

def open_device():
    candidates = [
        info for info in hid.enumerate(VENDOR_ID, PRODUCT_ID)
        if info["usage_page"] == USAGE_PAGE and info["usage"] == USAGE
    ]
    # The dongle can show up twice: over USB (interface >= 0) and over its BLE link
    # to the Mac (interface -1). Prefer USB.
    candidates.sort(key=lambda info: info["interface_number"] < 0)
    for info in candidates:
        dev = hid.device()
        try:
            dev.open_path(info["path"])
        except OSError:
            continue
        log.info("using %s interface", "BLE" if info["interface_number"] < 0 else "USB")
        return dev
    return None


def send(dev, payload):
    report = bytes([0x00]) + bytes(payload).ljust(REPORT_SIZE, b"\x00")  # leading 0 = report ID
    if dev.write(report) < 0:
        raise OSError("HID write failed")


def run():
    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    fetcher = EndpointFetcher()
    fetcher.start()

    dev = None
    waiting_logged = False
    while running:
        if dev is None:
            try:
                dev = open_device()
            except OSError as err:
                log.warning("could not open dongle: %s", err)
                dev = None
            if dev is None:
                if not waiting_logged:
                    log.info("waiting for Ferris Dongle raw HID interface...")
                    waiting_logged = True
                time.sleep(RECONNECT_SECONDS)
                continue
            log.info("connected to Ferris Dongle")
            waiting_logged = False
            last_volume = last_usage = None
            last_sent = 0.0

        try:
            volume = read_volume()
            usage = current_usage(fetcher)
            heartbeat = time.monotonic() - last_sent >= HEARTBEAT_SECONDS

            if volume is not None and (volume != last_volume or heartbeat):
                send(dev, [MSG_VOLUME, volume])
                if volume != last_volume:
                    log.info("volume %d%%", volume)
                last_volume = volume
                last_sent = time.monotonic()
            if usage != last_usage or heartbeat:
                send(dev, [MSG_CLAUDE_USAGE, *usage])
                if usage != last_usage:
                    log.info("usage 5h=%s 7d=%s", *("--" if u == UNKNOWN else f"{u}%" for u in usage))
                last_usage = usage
                last_sent = time.monotonic()
        except OSError as err:
            log.warning("dongle disconnected: %s", err)
            try:
                dev.close()
            except Exception:
                pass
            dev = None
            continue

        time.sleep(POLL_SECONDS)

    if dev is not None:
        dev.close()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s", stream=sys.stdout)
    run()
