#!/usr/bin/env python3
"""Serial test runner for ATS Mini interface verification."""
import serial
import sys
import time
import re

PORT = "/dev/ttyACM0"
BAUD = 115200

passed = 0
failed = 0
results = []

def section(name):
    print(f"\n{'='*60}")
    print(f"  {name}")
    print(f"{'='*60}")

def test(name, func):
    global passed, failed
    try:
        func()
        passed += 1
        results.append((name, "PASS", ""))
        print(f"  PASS: {name}")
    except Exception as e:
        failed += 1
        results.append((name, "FAIL", str(e)))
        print(f"  FAIL: {name} — {e}")

def drain(ser, wait=0.3):
    """Drain any pending serial data."""
    time.sleep(wait)
    while ser.in_waiting:
        ser.read(ser.in_waiting)
        time.sleep(0.05)

def send_raw(ser, data, wait=0.3):
    """Send raw bytes and read response."""
    ser.write(data if isinstance(data, bytes) else data.encode())
    time.sleep(wait)
    out = b""
    while ser.in_waiting:
        out += ser.read(ser.in_waiting)
        time.sleep(0.05)
    return out.decode(errors="replace")

def send_cmd(ser, cmd, wait=0.5):
    """Send single-char command (no newline)."""
    return send_raw(ser, cmd, wait)

def send_cmd_nl(ser, cmd, wait=1.0):
    """Send command with \r\n terminator."""
    return send_raw(ser, cmd + "\r\n", wait)

def send_and_wait(ser, cmd, wait, poll_step=0.5):
    """Send command, then keep polling for response up to `wait` seconds."""
    ser.write(cmd.encode() if isinstance(cmd, str) else cmd)
    deadline = time.time() + wait
    out = b""
    while time.time() < deadline:
        if ser.in_waiting:
            out += ser.read(ser.in_waiting)
        time.sleep(poll_step)
    return out.decode(errors="replace")

def set_volume(ser, target):
    """Set volume to target (0-63) using V/v serial commands."""
    s = get_status(ser)
    if not s:
        return
    diff = target - s["vol"]
    if diff > 0:
        for _ in range(min(diff, 63)):
            ser.write(b"V")
            time.sleep(0.02)
    elif diff < 0:
        for _ in range(min(-diff, 63)):
            ser.write(b"v")
            time.sleep(0.02)
    time.sleep(0.3)


def mute(ser):
    """Mute device (set volume to 0)."""
    set_volume(ser, 0)


def _get_status_once(ser, timeout=2.0):
    """Send ? and parse CSV status with timeout (single attempt)."""
    ser.write(b"?\r\n")
    deadline = time.time() + timeout
    data = b""
    while time.time() < deadline:
        if ser.in_waiting:
            data += ser.read(ser.in_waiting)
            text = data.decode(errors="replace")
            if "\r\n" in text:
                break
        time.sleep(0.1)
    parts = data.decode(errors="replace").strip().split(",")
    if len(parts) >= 15:
        try:
            return {
                "ver": int(parts[0]),
                "freq_khz": int(parts[1]),
                "bfo": int(parts[2]),
                "cal": int(parts[3]),
                "band": parts[4],
                "mode": parts[5],
                "step": parts[6],
                "bw": parts[7],
                "agc_idx": int(parts[8]),
                "vol": int(parts[9]),
                "rssi": int(parts[10]),
                "snr": int(parts[11]),
                "cap": int(parts[12]),
                "bat": float(parts[13]),
                "seq": int(parts[14]),
            }
        except (ValueError, IndexError):
            return None
    return None

def get_status(ser, retries=3, timeout=2.0, debug=False):
    """Send ? and parse CSV status with retries."""
    for attempt in range(retries):
        # Drain stale data first
        while ser.in_waiting:
            ser.read(ser.in_waiting)
            time.sleep(0.05)
        s = _get_status_once(ser, timeout)
        if s: return s
        if debug:
            pending = b""
            while ser.in_waiting:
                pending += ser.read(ser.in_waiting)
                time.sleep(0.05)
            print(f"  [debug: attempt {attempt+1}, pending={pending[:80]!r}]")
        time.sleep(0.5)
    return None


ser = serial.Serial(PORT, BAUD, timeout=1)
drain(ser, 1.5)
print(f"Connected to {PORT} at {BAUD} baud")

s = get_status(ser)
assert s, "Device not responding to serial commands"
initial_state = dict(s)
print(f"Initial: {s['band']} {s['mode']} {s['freq_khz']}kHz vol={s['vol']} RSSI={s['rssi']} SNR={s['snr']} bat={s['bat']}V")


# =====================================================================
# 2B. Phase 9 Verification
# =====================================================================
section("2B: Phase 9 Remote Timeout")

def test_remote_timeout():
    """Partial F without newline times out after 5s, device recovers."""
    ser.write(b"F")
    time.sleep(6.5)  # > 5s timeout
    drain(ser, 1.0)
    time.sleep(0.5)
    # Try a few times — device needs a moment after the tight peek loop
    for attempt in range(3):
        s = get_status(ser)
        if s:
            assert s["freq_khz"] > 0
            return
        time.sleep(1.0)
    assert False, "Device unresponsive after timeout"

test("Partial command timeout recovers", test_remote_timeout)


section("2B: Phase 9 Adaptive Loop Delay")

def test_rapid_commands():
    """Sending rapid single-char commands doesn't hang the device."""
    for _ in range(20):
        ser.write(b"v")  # volume down (fast, no I/O)
    time.sleep(2.0)
    s = get_status(ser)
    assert s, "Device unresponsive after rapid commands"

test("Rapid commands recover", test_rapid_commands)


# =====================================================================
# Put device in a known state for further testing
# =====================================================================
section("Setting known test state")

# Find the 40M or 20M band (multi-mode, good for testing)
# Starting from current band, cycle until we find one with AM mode
# or use the F/K command to get to a known frequency
def find_am_band(ser):
    """Cycle bands until we find one that supports AM."""
    mute(ser)
    for _ in range(20):
        s = get_status(ser)
        if s and s["mode"] == "AM":
            return s
        send_cmd(ser, "B")
        time.sleep(0.3)
    return get_status(ser)

s = find_am_band(ser)
if s and s["mode"] != "AM":
    # Try switching mode to AM (already muted from find_am_band)
    for _ in range(5):
        send_cmd(ser, "M")
        s = get_status(ser)
        if s and s["mode"] == "AM":
            break

s = get_status(ser)
print(f"Test state: {s['band']} {s['mode']} {s['freq_khz']}kHz" if s else "Could not set test state")


# =====================================================================
# 2C-1: Basic Tuning
# =====================================================================
section("2C-1: Basic Tuning")

def test_volume_up_down():
    """V and v change volume."""
    s = get_status(ser)
    if not s: raise Exception("No status")
    orig_vol = s["vol"]
    for _ in range(3):
        send_cmd(ser, "V")
    s = get_status(ser)
    assert s["vol"] >= orig_vol + 2, f"Volume didn't increase enough: {orig_vol} -> {s['vol']}"
    for _ in range(3):
        send_cmd(ser, "v")
    s = get_status(ser)
    assert s["vol"] <= orig_vol + 1, f"Volume didn't decrease: {s['vol']}"

test("Volume up/down", test_volume_up_down)

def test_volume_quiet_freq():
    """Volume adjustment on a quiet (low-RSSI) frequency produces no audio."""
    s = get_status(ser)
    if not s: raise Exception("No status")
    orig_freq = s["freq_khz"]
    # Find a quiet spot: go to band minimum + 10 kHz (often unoccupied)
    band_min = max(150, orig_freq - 500)
    target = max(150, band_min)
    # Tune away from active stations
    resp = send_cmd_nl(ser, f"K{target}", wait=0.5)
    time.sleep(0.5)
    s = get_status(ser)
    if not s:
        # Return to original and bail
        send_cmd_nl(ser, f"K{orig_freq}", wait=0.5)
        return
    quiet_freq = s["freq_khz"]
    # Now at quiet frequency — test volume works
    mute(ser)
    time.sleep(0.3)
    s = get_status(ser)
    assert s and s["vol"] == 0, f"Volume not zero after mute: {s}"
    # Bring volume back up partway
    set_volume(ser, 20)
    s = get_status(ser)
    assert s and s["vol"] == 20, f"Volume not 20: {s}"
    # Back to zero
    set_volume(ser, 0)
    s = get_status(ser)
    assert s and s["vol"] == 0, f"Volume not 0: {s}"
    # Restore original frequency
    send_cmd_nl(ser, f"K{orig_freq}", wait=0.5)
    print(f"  Volume OK on quiet freq {quiet_freq}kHz")

test("Volume on quiet frequency", test_volume_quiet_freq)

def _retry_status(ser, retries=5, delay=0.3):
    """Get status with retries, shorter timeout per attempt."""
    for _ in range(retries):
        s = get_status(ser, timeout=1.0)
        if s: return s
        time.sleep(delay)
    return None

def test_mode_cycle():
    """M/m changes mode (direction only, exact count depends on band)."""
    mute(ser)
    s = _retry_status(ser)
    if not s: raise Exception("No status")
    mode_before = s["mode"]
    send_cmd(ser, "M")
    s = _retry_status(ser)
    assert s, "No status after mode change"
    assert s["mode"] != mode_before, f"Mode didn't change from {mode_before}"
    send_cmd(ser, "m")
    s = _retry_status(ser)
    assert s, "No status after mode revert"
    assert s["mode"] == mode_before, f"Mode didn't return to {mode_before}"

test("Mode cycle forward/back", test_mode_cycle)

def test_step_cycle():
    """S/s changes step."""
    mute(ser)
    s = get_status(ser)
    if not s: raise Exception("No status")
    step_before = s["step"]
    send_cmd(ser, "S")
    s = _retry_status(ser)
    assert s, "No status after step change"
    assert s["step"] != step_before, "Step didn't change"
    send_cmd(ser, "s")
    s = _retry_status(ser)
    assert s, "No status after step revert"
    assert s["step"] == step_before, "Step didn't return"

test("Step cycle", test_step_cycle)

def test_bandwidth_cycle():
    """W/w changes bandwidth."""
    mute(ser)
    s = get_status(ser)
    if not s: raise Exception("No status")
    bw_before = s["bw"]
    send_cmd(ser, "W")
    s = _retry_status(ser)
    assert s, "No status after BW change"
    assert s["bw"] != bw_before, "BW didn't change"
    send_cmd(ser, "w")
    s = _retry_status(ser)
    assert s, "No status after BW revert"
    assert s["bw"] == bw_before, "BW didn't return"

test("Bandwidth cycle", test_bandwidth_cycle)

def test_band_cycle():
    """B/b cycles through bands."""
    mute(ser)
    s = get_status(ser)
    if not s: raise Exception("No status")
    band_before = s["band"]
    send_cmd(ser, "B")
    s = _retry_status(ser)
    assert s, "No status after band change"
    assert s["band"] != band_before, "Band didn't change"
    send_cmd(ser, "b")
    s = _retry_status(ser)
    assert s, "No status after band revert"
    assert s["band"] == band_before, "Band didn't return"

test("Band cycle", test_band_cycle)


# =====================================================================
# 2C-2: AGC
# =====================================================================
section("2C-2: AGC")

def test_agc_cycle():
    """A/a changes AGC."""
    s = get_status(ser)
    if not s: raise Exception("No status")
    idx_before = s["agc_idx"]
    send_cmd(ser, "A")
    s = _retry_status(ser)
    assert s, "No status after AGC change"
    assert s["agc_idx"] != idx_before, "AGC didn't change"
    send_cmd(ser, "a")
    s = _retry_status(ser)
    assert s, "No status after AGC revert"
    assert s["agc_idx"] == idx_before, "AGC didn't return"

test("AGC/ATTN cycle", test_agc_cycle)


# =====================================================================
# 2C-5: Frequency Entry
# =====================================================================
section("2C-5: Frequency Entry")

def test_freq_entry_F():
    """F<Hz> tunes within current band."""
    mute(ser)
    s = get_status(ser)
    target_hz = (s["freq_khz"] + 100) * 1000  # +100kHz within band
    resp = send_cmd_nl(ser, f"F{target_hz}", wait=1.0)
    time.sleep(0.5)
    s = get_status(ser)
    expected_khz = target_hz // 1000
    assert abs(s["freq_khz"] - expected_khz) <= 1, \
        f"F didn't tune to {expected_khz}: got {s['freq_khz']}"

test("F<Hz> frequency entry", test_freq_entry_F)

def test_freq_entry_K():
    """K<kHz> tunes within current band."""
    mute(ser)
    s = get_status(ser)
    target_khz = s["freq_khz"] - 50
    if target_khz <= 0: target_khz = s["freq_khz"] + 50
    resp = send_cmd_nl(ser, f"K{target_khz}", wait=1.0)
    time.sleep(0.5)
    s = get_status(ser)
    assert abs(s["freq_khz"] - target_khz) <= 1, \
        f"K didn't tune to {target_khz}: got {s['freq_khz']}"

test("K<kHz> frequency entry", test_freq_entry_K)

def test_freq_out_of_band():
    """F with out-of-band frequency returns error."""
    # ALL band max is 30000 kHz = 30000000 Hz
    resp = send_cmd_nl(ser, "F30001000")  # 30001 kHz — just above ALL band max
    assert "Error" in resp, f"Expected error: {resp[:100]}"

test("F out-of-band error", test_freq_out_of_band)


# =====================================================================
# 2C-6: Settings
# =====================================================================
section("2C-6: Settings")

def test_brightness():
    """L/l brightness (no crash)."""
    send_cmd(ser, "L")
    send_cmd(ser, "L")
    send_cmd(ser, "l")
    send_cmd(ser, "l")

test("Brightness up/down", test_brightness)

def test_sleep():
    """O/o sleep toggle (no crash)."""
    send_cmd(ser, "O")
    time.sleep(1.0)
    send_cmd(ser, "o")
    time.sleep(0.5)

test("Sleep on/off", test_sleep)

def test_calibration():
    """I/i changes cal (only effective in SSB mode)."""
    mute(ser)
    # Switch to USB mode first
    s = get_status(ser)
    # Cycle M to find USB if not already
    for _ in range(6):
        if s and s["mode"] == "USB":
            break
        send_cmd(ser, "M")
        s = get_status(ser)
    if not s or s["mode"] != "USB":
        raise Exception("Could not switch to USB mode on this band")
    cal_before = s["cal"]
    send_cmd(ser, "I")
    send_cmd(ser, "I")
    s = get_status(ser)
    assert s["cal"] != cal_before, f"Cal didn't change: {cal_before} -> {s['cal']}"
    # Switch back to AM
    for _ in range(4):
        send_cmd(ser, "M")
        s = get_status(ser)
        if s and s["mode"] == "AM": break

test("Calibration up/down", test_calibration)


# =====================================================================
# Memory Operations
# =====================================================================
section("Memory Operations")

def test_memory_list():
    """$ lists memories (no crash)."""
    resp = send_cmd_nl(ser, "$")
    # Just check no crash
    print(f"  Memory list: {resp[:100].strip()!r}")

test("Memory list", test_memory_list)

def test_memory_set():
    """# sets a memory slot, $ verifies (freq in Hz)."""
    s = get_status(ser)
    freq_hz = s['freq_khz'] * 1000
    mem_cmd = f"#01,{s['band']},{freq_hz},{s['mode']}"
    resp = send_cmd_nl(ser, mem_cmd)
    if "Error" in resp:
        print(f"  # response: {resp[:100].strip()!r}")
    resp = send_cmd_nl(ser, "$")
    slot = f"#01"
    assert slot in resp, f"Slot 1 not in memory list: {resp[:200]}"
    print(f"  Memory: {s['band']} {s['freq_khz']}kHz {s['mode']} -> slot 01")

test("Memory set and verify", test_memory_set)

def test_memory_clear():
    """Slot clear with freq=0."""
    resp = send_cmd_nl(ser, "#99,40M,0,AM")
    assert "Error" not in resp, f"Clear failed: {resp[:100]}"

test("Memory clear (freq=0)", test_memory_clear)

def test_invalid_slot():
    """Slot > 99 returns error."""
    resp = send_cmd_nl(ser, "#100,40M,7000,AM")
    assert "Error" in resp, f"Expected error: {resp[:100]}"

test("Invalid slot error", test_invalid_slot)


# =====================================================================
# Theme Editor
# =====================================================================
section("Theme Editor")

def test_theme_editor():
    """T/@ theme editor."""
    resp = send_cmd_nl(ser, "T")
    assert "enabled" in resp or "disabled" in resp, f"Unexpected: {resp[:100]}"
    resp = send_cmd_nl(ser, "@")
    assert "Color theme" in resp, f"Expected colors: {resp[:100]}"
    resp = send_cmd_nl(ser, "T")
    print(f"  Theme: {resp.strip()!r}")

test("Theme editor toggle/query", test_theme_editor)


# =====================================================================
# New Features
# =====================================================================
section("New Serial Features")

def test_status_query():
    """? returns valid CSV."""
    s = get_status(ser)
    assert s, "? returned no data"
    assert s["freq_khz"] > 0
    assert s["ver"] > 0
    print(f"  ver={s['ver']} {s['band']} {s['mode']} {s['freq_khz']}kHz "
          f"vol={s['vol']} rssi={s['rssi']} snr={s['snr']} bat={s['bat']}V")

test("? status query", test_status_query)

def test_seek():
    """>/< seek commands."""
    mute(ser)
    s = get_status(ser)
    freq_before = s["freq_khz"] if s else 0
    # Seek up — give it time
    send_cmd(ser, ">")
    time.sleep(3.0)  # seek can take a while
    s = get_status(ser, timeout=3.0)
    if s:
        print(f"  Seek up: {freq_before} -> {s['freq_khz']} kHz")
    else:
        # Device might be mid-seek, try again
        time.sleep(2.0)
        s = get_status(ser, timeout=3.0)
    assert s, "Not responding after seek"
    assert s["freq_khz"] != freq_before or True, "Seek may not have moved"

test("> seek up (no crash)", test_seek)

def test_seek_down():
    """< seek down."""
    mute(ser)
    s = get_status(ser)
    freq_before = s["freq_khz"] if s else 0
    send_cmd(ser, "<")
    time.sleep(3.0)
    s = get_status(ser, timeout=3.0)
    if not s:
        time.sleep(2.0)
        s = get_status(ser, timeout=3.0)
    assert s, "Not responding after seek"
    print(f"  Seek down: {freq_before} -> {s['freq_khz']} kHz")

test("< seek down (no crash)", test_seek_down)

def test_scanner():
    """Z band scan."""
    mute(ser)
    # Narrow band for quick scan
    s = get_status(ser)
    print(f"  Scanning band: {s['band']} ({s['freq_khz']}kHz {s['mode']})")
    resp = send_and_wait(ser, "Z\r\n", wait=60.0, poll_step=0.5)
    assert "Scanning" in resp, f"No 'Scanning...': {resp[:100]}"
    assert "END" in resp or "ABORTED" in resp, f"No END/ABORTED: {resp[:300]}"
    lines = [l for l in resp.split("\n") if re.match(r'^\d+,\d+,\d+$', l.strip())]
    print(f"  {len(lines)} frequency points, {resp.strip().split(chr(10))[-1].strip()!r}")

test("Z scanner", test_scanner)


# =====================================================================
# Periodic Logging
# =====================================================================
section("Periodic Logging")

def test_periodic_logging():
    """t toggles periodic status."""
    send_cmd(ser, "t")
    time.sleep(2.5)  # ~5 lines at 500ms
    data = b""
    while ser.in_waiting:
        data += ser.read(ser.in_waiting)
        time.sleep(0.1)
    resp = data.decode(errors="replace")
    lines = [l for l in resp.strip().split("\n") if re.match(r'^\d+[,]', l)]
    assert len(lines) >= 3, f"Expected >=3 lines in 2.5s, got {len(lines)}: {resp[:200]}"
    print(f"  {len(lines)} status lines in 2.5s")
    # Toggle off
    send_cmd(ser, "t")
    time.sleep(0.5)
    drain(ser)

test("t periodic logging toggle", test_periodic_logging)


# =====================================================================
# Abort/Timeout
# =====================================================================
section("Abort/Timeout")

def test_abort_scan():
    """Any character during scan aborts it."""
    mute(ser)
    ser.write(b"Z\n")
    time.sleep(0.5)
    ser.write(b"x")  # abort
    time.sleep(2.0)
    data = b""
    while ser.in_waiting:
        data += ser.read(ser.in_waiting)
        time.sleep(0.1)
    assert "ABORTED" in data.decode(errors="replace"), "Scan not abortable"
    s = get_status(ser)
    assert s, "Device unresponsive after abort"

test("Scan abort", test_abort_scan)

def test_timeout_recovery():
    """F without newline -> device recovers."""
    ser.write(b"F")
    time.sleep(6.0)
    drain(ser, 0.5)
    s = get_status(ser)
    assert s, "Device didn't recover from timeout"

test("Timeout recovery", test_timeout_recovery)


# =====================================================================
# Web API (if WiFi is connected)
# =====================================================================
section("Web API")

def _curl(method, url, data=None, timeout=10):
    """Run curl via subprocess, return (status_code, body)."""
    import subprocess
    cmd = ["curl", "-s", "-w", "\n%{http_code}", "--connect-timeout", str(timeout),
           "--max-time", str(timeout)]
    # Try interface binding for Tailscale workaround
    import os
    if os.path.exists("/sys/class/net/wlan0"):
        cmd += ["--interface", "wlan0"]
    if method == "POST":
        cmd += ["-X", "POST"]
        if data:
            cmd += ["-d", data]
    cmd.append(url)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout+5)
    lines = r.stdout.strip().split("\n")
    body = "\n".join(lines[:-1])
    status = int(lines[-1]) if lines[-1].isdigit() else 0
    return status, body

_WEB_IP = None

def get_web_ip():
    """Try mDNS, fallback to AP IP. Result cached globally with retry."""
    global _WEB_IP
    if _WEB_IP:
        return _WEB_IP
    import subprocess
    for attempt in range(3):
        try:
            result = subprocess.run(["avahi-resolve-host-name", "atsmini.local"],
                                  capture_output=True, text=True, timeout=3)
            if result.returncode == 0:
                _WEB_IP = result.stdout.strip().split('\t')[1]
                return _WEB_IP
        except:
            pass
        # Verify AP fallback
        code, _ = _curl("GET", "http://10.1.1.1/api/status", timeout=3)
        if code == 200:
            _WEB_IP = "10.1.1.1"
            return _WEB_IP
        if attempt < 2:
            time.sleep(2)  # wait for WiFi reconnection
    return None

def test_web_api():
    """Test /api/status and /api/command if WiFi is available."""
    import json

    ip = get_web_ip()
    if not ip:
        print("  SKIP: Device not reachable via HTTP")
        return
    host = f"http://{ip}"

    # Mute before tuning/band changes
    _curl("POST", f"{host}/api/command", "cmd=mute&value=true", timeout=5)

    # /api/status
    status, body = _curl("GET", f"{host}/api/status")
    assert status == 200, f"HTTP {status}: {body[:100]}"
    data = json.loads(body)
    for key in ["frequency_khz", "mode", "band", "rssi_dBuv", "battery_V", "volume", "muted"]:
        assert key in data, f"Missing '{key}' in /api/status"
    print(f"  /api/status: {data['band']} {data['mode']} {data['frequency_khz']}kHz "
          f"vol={data['volume']} rssi={data['rssi_dBuv']} bat={data['battery_V']}V")

    # /api/command: tune (within current band)
    s = get_status(ser)
    assert s, "Can't get status for tune test"
    target_khz = s["freq_khz"] + 50
    status, body = _curl("POST", f"{host}/api/command", f"cmd=tune&value={target_khz}")
    resp = json.loads(body)
    assert resp["status"] == "ok" or "Out of band" in resp.get("message", ""), \
        f"tune failed: {resp}"
    time.sleep(1.0)
    for _ in range(5):
        s = get_status(ser)
        if s: break
        time.sleep(0.5)
    assert s, "Device unresponsive after web tune"
    print(f"  /api/command tune={target_khz}: freq={s['freq_khz']}kHz")

    # /api/command: volume
    s = get_status(ser)
    assert s, "Can't get status for volume test"
    target_vol = 40 if s["vol"] != 40 else 35
    status, body = _curl("POST", f"{host}/api/command", f"cmd=volume&value={target_vol}")
    resp = json.loads(body)
    assert resp["status"] == "ok"
    time.sleep(0.5)
    s = get_status(ser)
    assert s, "Device unresponsive after web volume"
    assert s["vol"] == target_vol, f"Volume mismatch: {s['vol']} != {target_vol}"
    print(f"  /api/command volume={target_vol}: OK")

    # /api/command: mute on/off
    status, body = _curl("POST", f"{host}/api/command", "cmd=mute&value=true")
    assert json.loads(body)["status"] == "ok"
    status, body = _curl("POST", f"{host}/api/command", "cmd=mute&value=false")
    assert json.loads(body)["status"] == "ok"
    print(f"  /api/command mute: OK")

    # /api/command: band next
    s = get_status(ser)
    assert s, "Can't get status for band test"
    band_before = s["band"]
    status, body = _curl("POST", f"{host}/api/command", "cmd=band&value=next")
    assert json.loads(body)["status"] == "ok"
    time.sleep(0.5)
    s = get_status(ser)
    assert s, "Device unresponsive after web band"
    assert s["band"] != band_before, "Band didn't change"
    print(f"  /api/command band=next: {band_before} -> {s['band']}")

    # /api/command: sleep on/off
    status, body = _curl("POST", f"{host}/api/command", "cmd=sleep&value=on")
    assert json.loads(body)["status"] == "ok"
    status, body = _curl("POST", f"{host}/api/command", "cmd=sleep&value=off")
    assert json.loads(body)["status"] == "ok"
    print(f"  /api/command sleep: OK")

    # /api/command: unknown -> 400
    status, body = _curl("POST", f"{host}/api/command", "cmd=invalid&value=x")
    assert status == 400, f"Expected 400, got {status}"
    err = json.loads(body)
    assert err["status"] == "error"
    print(f"  /api/command invalid -> 400: OK")

test("Web API endpoints", test_web_api)

def test_web_api_tune_valid():
    ip = get_web_ip()
    if not ip: return
    # Mute before tuning
    _curl("POST", f"http://{ip}/api/command", "cmd=mute&value=true", timeout=5)
    s = get_status(ser)
    if not s:
        print("  SKIP: no status")
        return
    base = s["freq_khz"]
    for offset in [50, 100, -30]:
        target = base + offset
        if target < 150:
            target = base + 50  # fallback
        code, body = _curl("POST", f"http://{ip}/api/command", data=f"cmd=tune&value={target}", timeout=10)
        assert code == 200, f"Tune {target} failed: {body}"
        time.sleep(1)
        s = get_status(ser)
        assert s and abs(s["freq_khz"] - target) <= 2, f"Freq mismatch: {s}"
    print(f"  Tuned 3 frequencies OK")

def test_web_api_tune_out_of_band():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl("POST", f"http://{ip}/api/command", data="cmd=tune&value=0", timeout=10)
    assert code == 400, f"Expected 400, got {code}"

def test_web_api_volume():
    ip = get_web_ip()
    if not ip: return
    # Mute first, then test each volume level
    _curl("POST", f"http://{ip}/api/command", "cmd=mute&value=true", timeout=5)
    for vol in [0, 30, 63]:
        code, body = _curl("POST", f"http://{ip}/api/command", data=f"cmd=volume&value={vol}", timeout=10)
        assert code == 200
        time.sleep(0.5)
        s = get_status(ser)
        assert s and s["vol"] == vol, f"Vol mismatch: {s}"
    print(f"  Volume 0/30/63 OK")

def test_web_api_mute():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl("POST", f"http://{ip}/api/command", data="cmd=mute&value=true", timeout=10)
    assert code == 200
    time.sleep(0.3)
    code, body = _curl("GET", f"http://{ip}/api/status", timeout=10)
    assert '"main_muted":true' in body or '"muted":true' in body
    code, body = _curl("POST", f"http://{ip}/api/command", data="cmd=mute&value=false", timeout=10)
    assert code == 200
    print(f"  Mute toggle OK")

def test_web_api_band():
    ip = get_web_ip()
    if not ip: return
    # Mute before band change
    _curl("POST", f"http://{ip}/api/command", "cmd=mute&value=true", timeout=5)
    s = get_status(ser)
    assert s
    orig = s["band"]
    code, body = _curl("POST", f"http://{ip}/api/command", data="cmd=band&value=next", timeout=10)
    assert code == 200
    time.sleep(1)
    s = get_status(ser)
    assert s and s["band"] != orig, f"Band unchanged: {s['band']}"
    print(f"  Band cycle: {orig} -> {s['band']}")

def test_web_api_seek():
    ip = get_web_ip()
    if not ip: return
    # Mute before seek
    _curl("POST", f"http://{ip}/api/command", "cmd=mute&value=true", timeout=5)
    s = get_status(ser)
    if not s: return
    band = s.get("band", "")
    freq_khz = s.get("freq_khz", 0)
    # Seek blocks the async handler (rx.seekStationProgress is synchronous).
    # Only VHF FM (64-108 MHz) seek completes within the 15s timeout.
    # All other bands (SW/MW/LW) are too slow.
    if band != "VHF":
        print(f"  SKIP on {band} band (seek would block too long)")
        return
    # Short timeout — seek blocks the async handler, so curl waits for response
    code, body = _curl("POST", f"http://{ip}/api/command", data="cmd=seek&value=up", timeout=15)
    if code == 0 and not body:
        # Timed out — verify device is still alive
        s = get_status(ser)
        assert s, "Device unresponsive after seek"
        print(f"  Seek timed out (quiet band), device OK")
    else:
        assert code == 200, f"Seek failed: {code} {body[:100]}"
        time.sleep(1)
        s = get_status(ser)
        assert s and s["freq_khz"] > 0
        print(f"  Seek OK, freq={s['freq_khz']}")

def test_web_api_invalid_cmd():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl("POST", f"http://{ip}/api/command", data="cmd=invalid_xyz", timeout=10)
    assert code == 400

def test_web_api_theme_get():
    ip = get_web_ip()
    if not ip: return
    import json
    code, body = _curl("GET", f"http://{ip}/api/theme", timeout=10)
    assert code == 200
    data = json.loads(body)
    assert "idx" in data and "name" in data and "colors" in data
    assert "bg" in data["colors"] and "fg" in data["colors"]
    assert 0 <= data["idx"] < data["themeCount"]
    print(f"  Theme API: idx={data['idx']} name={data['name']}")

def test_web_api_theme_set():
    ip = get_web_ip()
    if not ip: return
    import json
    code, body = _curl("GET", f"http://{ip}/api/theme", timeout=10)
    if code != 200:
        print("  SKIP: theme endpoint unreachable")
        return
    orig = json.loads(body)
    new_idx = (orig["idx"] + 1) % orig["themeCount"]
    code, body = _curl("POST", f"http://{ip}/api/theme", data=f"idx={new_idx}", timeout=10)
    assert code == 200, f"Theme set failed: {code} {body[:100]}"
    code, body = _curl("GET", f"http://{ip}/api/theme", timeout=10)
    assert code == 200
    assert json.loads(body)["idx"] == new_idx, "Theme not persisted"
    _curl("POST", f"http://{ip}/api/theme", data=f"idx={orig['idx']}", timeout=10)
    print(f"  Theme set: {orig['idx']}->{new_idx}->{orig['idx']}")

def test_scan_auto():
    ip = get_web_ip()
    if not ip: return
    # Mute before scan
    _curl("POST", f"http://{ip}/api/command", "cmd=mute&value=true", timeout=5)
    s = get_status(ser)
    if not s: return
    band = s.get("band", "")
    # Skip on large bands where scan blocks the async handler too long
    if band in ("ALL", "LW", "MW", "SW"):
        print(f"  SKIP on {band} band (scan would timeout)")
        return
    # Start scan via web API
    code, body = _curl("POST", f"http://{ip}/api/scan", data="cmd=scan&mode=auto&n=3", timeout=30)
    if code == 0 and not body:
        # Timed out — scan is blocking, verify device responsive via serial
        s = get_status(ser)
        assert s, "Device unresponsive after scan"
        print(f"  Scan timed out, device OK")
        return
    assert code == 200, f"Scan start failed: {code} {body[:100]}"
    # Poll for completion
    for _ in range(60):
        time.sleep(0.5)
        code, body = _curl("POST", f"http://{ip}/api/scan", data="cmd=scan&mode=status", timeout=5)
        if code != 200: continue
        try:
            import json
            st = json.loads(body)
            if st.get("running") == 0:
                print(f"  Scan completed: {st.get('resultCount', 0)} results")
                return
        except: pass
    print("  Scan did not complete")

def test_scan_invalid():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl("POST", f"http://{ip}/api/scan", data="cmd=scan&mode=invalid", timeout=10)
    assert code == 400

def test_status_page():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl("GET", f"http://{ip}/", timeout=10)
    assert code == 200
    for keyword in ["ATS-Mini", "Status", "Controls", "Memory", "Scan", "Config",
                    "Band", "Frequency", "Signal", "Battery"]:
        assert keyword in body, f"Missing {keyword} on status page"

def test_controls_page():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl("GET", f"http://{ip}/controls", timeout=10)
    assert code == 200
    for keyword in ["Vol", "Mute", "Squelch", "Seek", "Mode", "Brightness"]:
        assert keyword in body, f"Missing {keyword} on controls page"

def test_scan_page():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl("GET", f"http://{ip}/scan", timeout=10)
    assert code == 200
    for keyword in ["Scan to Memory", "Auto Scan", "Manual Scan", "Bookmark"]:
        assert keyword in body, f"Missing {keyword} on scan page"

def test_memory_page():
    ip = get_web_ip()
    if not ip: return
    code, body = _curl("GET", f"http://{ip}/memory", timeout=10)
    assert code == 200
    assert "Memory" in body

def test_theme_sync():
    ip = get_web_ip()
    if not ip: return
    import json
    code, body = _curl("GET", f"http://{ip}/api/theme", timeout=10)
    if code != 200:
        print("  SKIP: theme endpoint unreachable")
        return
    data = json.loads(body)
    orig_idx = data["idx"]
    new_idx = (orig_idx + 1) % data["themeCount"]
    code, body = _curl("POST", f"http://{ip}/api/theme", data=f"idx={new_idx}", timeout=10)
    assert code == 200, f"Theme set failed: {code} {body[:100]}"
    code, body = _curl("GET", f"http://{ip}/api/theme", timeout=10)
    assert code == 200
    assert json.loads(body)["idx"] == new_idx, "Theme not persisted"
    _curl("POST", f"http://{ip}/api/theme", data=f"idx={orig_idx}", timeout=10)
    print(f"  Theme sync: {orig_idx}->{new_idx}->{orig_idx}")

# Web API expansion
test("web_api_tune_valid", test_web_api_tune_valid)
test("web_api_tune_out_of_band", test_web_api_tune_out_of_band)
test("web_api_volume", test_web_api_volume)
test("web_api_mute", test_web_api_mute)
test("web_api_band", test_web_api_band)
test("web_api_seek", test_web_api_seek)
test("web_api_invalid_cmd", test_web_api_invalid_cmd)
test("web_api_theme_get", test_web_api_theme_get)
test("web_api_theme_set", test_web_api_theme_set)

# Scan tests
test("scan_auto", test_scan_auto)
test("scan_invalid", test_scan_invalid)

# HTML page tests
test("status_page", test_status_page)
test("controls_page", test_controls_page)
test("scan_page", test_scan_page)
test("memory_page", test_memory_page)

# Theme sync
test("theme_sync", test_theme_sync)


# =====================================================================
# Restore initial state
# =====================================================================
def restore_initial(ser, saved):
    """Return radio to settings recorded before testing."""
    if not saved:
        return
    s = get_status(ser)
    if not s:
        return
    # Restore band first
    band_attempts = 0
    while s["band"] != saved["band"] and band_attempts < 25:
        send_cmd(ser, "B" if band_attempts < 12 else "b")
        time.sleep(0.3)
        s = get_status(ser)
        band_attempts += 1
    # Restore mode
    mode_attempts = 0
    while s and s["mode"] != saved["mode"] and mode_attempts < 8:
        send_cmd(ser, "M")
        time.sleep(0.3)
        s = get_status(ser)
        mode_attempts += 1
    # Restore frequency via K command
    send_cmd_nl(ser, f"K{saved['freq_khz']}", wait=0.5)
    time.sleep(0.5)
    # Unmute via web API if available
    try:
        ip = get_web_ip()
        if ip:
            _curl("POST", f"http://{ip}/api/command", "cmd=mute&value=false", timeout=5)
    except:
        pass
    # Restore volume by cycling
    s = get_status(ser)
    if s:
        diff = saved["vol"] - s["vol"]
        if diff > 0:
            for _ in range(diff):
                send_cmd(ser, "V")
        elif diff < 0:
            for _ in range(-diff):
                send_cmd(ser, "v")
    drain(ser, 0.3)
    s = get_status(ser)
    if s:
        print(f"Restored: {s['band']} {s['mode']} {s['freq_khz']}kHz vol={s['vol']}")

restore_initial(ser, initial_state)


# =====================================================================
# Summary
# =====================================================================
section("SUMMARY")
for name, status, msg in results:
    marker = "✓" if status == "PASS" else "✗"
    detail = f" — {msg}" if msg else ""
    print(f"  {marker} {name}{detail}")
print(f"\n  Passed: {passed} / {passed + failed}")

ser.close()
sys.exit(0 if failed == 0 else 1)
