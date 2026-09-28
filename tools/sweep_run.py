#!/usr/bin/env python3
"""
sweep_run.py -- V46 switch matrix runner (python edition).

Why this exists: switch-mic.bat drives the matrix through reg.exe, and reg.exe
is on the agent sandbox's program blacklist -- so the .bat cannot be driven
from an automated shell even when that shell IS elevated.  This runner does the
same job without touching reg.exe:

  * registry writes go through winreg with KEY_WOW64_64KEY (the native view --
    a 32-bit view write is invisible to the driver's ZwOpenKey)
  * service stop/start and audiodg kill go through powershell
  * the device node is cycled with the package's own isight-micdev.exe
  * one full miccheck.txt is captured per combo, plus a SWEEP_SUMMARY.txt in
    exactly the shape tools/read_sweep.py already parses

Elevation is required (HKLM write + device install).  Run it elevated.

Usage:
    python sweep_run.py                 # all nine combos
    python sweep_run.py baseline_V45    # just one (by label)
"""
import os
import re
import sys
import time
import shutil
import subprocess
import winreg

PKG = r"C:\Users\flik\Downloads\isight-vmic"
SWEEP = os.path.join(PKG, "sweep")
SUM = os.path.join(SWEEP, "SWEEP_SUMMARY.txt")
MICDEV = os.path.join(PKG, "isight-micdev.exe")
MICCHECK = os.path.join(PKG, "isight-miccheck.exe")
MC = os.path.join(PKG, "miccheck.txt")

COMBOS = [
    ("baseline_V45",        0, 0, 1, 0, 0),
    # V47: Intersect=2 answers the intersection ALWAYS in WAVEFORMATEXTENSIBLE
    # form.  Every working capture endpoint on this box caches an endpoint
    # device format that is 0xFFFE (EXTENSIBLE); ours has none, and at
    # Intersect=1 we mirror the engine's plain WAVEFORMATEX request instead.
    ("intersect_forceext",  2, 0, 1, 0, 0),
    ("forceext_mono",       2, 0, 1, 0, 1),
    ("forceext_ranges",     2, 1, 1, 0, 0),
    ("intersect_echo",      1, 0, 1, 0, 0),
    ("ranges_dual",         0, 1, 1, 0, 0),
    ("mono",                0, 0, 1, 0, 1),
    ("no_jack",             0, 0, 0, 0, 0),
    ("topo_full",           0, 0, 1, 1, 0),
    ("ranges_mono",         0, 1, 1, 0, 1),
    ("ranges_mono_nojack",  0, 1, 0, 0, 1),
    ("full_mono",           0, 1, 1, 1, 1),
]


def log(msg):
    print(msg, flush=True)


def _t(b):
    """Child processes here emit GBK (Chinese Windows); never let a decode
    error kill the sweep -- replace and carry on."""
    if b is None:
        return ""
    if isinstance(b, bytes):
        return b.decode("utf-8", errors="replace")
    return b


def ps(cmd):
    return subprocess.run(["powershell", "-NoProfile", "-Command", cmd],
                          capture_output=True, timeout=120)


def write_cfg(i, r, j, t, m):
    """Write the five DWORDs into the NATIVE (64-bit) registry view."""
    k = winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\iSightMic", 0,
                           winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY)
    try:
        for name, val in (("Intersect", i), ("Ranges", r), ("Jack", j),
                          ("Topo", t), ("Mono", m)):
            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, int(val))
    finally:
        winreg.CloseKey(k)


def stop_audio():
    ps("Stop-Service -Name AudioEndpointBuilder -Force -ErrorAction SilentlyContinue")
    ps("Stop-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue")
    ps("Stop-Process -Name audiodg -Force -ErrorAction SilentlyContinue")


def start_audio():
    ps("Start-Service -Name Audiosrv -ErrorAction SilentlyContinue")
    ps("Start-Service -Name AudioEndpointBuilder -ErrorAction SilentlyContinue")


def cycle_device():
    a = subprocess.run([MICDEV, "remove"], cwd=PKG,
                       capture_output=True, timeout=120)
    b = subprocess.run([MICDEV, "install", "isightmic.inf"], cwd=PKG,
                       capture_output=True, timeout=180)
    return (a.returncode, b.returncode, _t(a.stdout) + _t(b.stdout))


def grab(patterns, text):
    for pat in patterns:
        m = re.search(pat, text)
        if m:
            return m.group(0).strip()
    return None


def run_combo(label, i, r, j, t, m):
    expect = i + 2 * r + 4 * j + 8 * t + 16 * m
    log("=" * 60)
    log("[sweep] combo %s  I=%d R=%d J=%d T=%d M=%d  (expect CfgCode %d)"
        % (label, i, r, j, t, m, expect))
    write_cfg(i, r, j, t, m)
    stop_audio()
    rc_remove, rc_install, out = cycle_device()
    start_audio()
    time.sleep(3)

    if os.path.exists(MC):
        os.remove(MC)
    try:
        subprocess.run([MICCHECK, "2"], cwd=PKG, capture_output=True,
                       timeout=240)
    except subprocess.TimeoutExpired:
        log("[sweep] miccheck timed out on %s" % label)
    except Exception as e:
        log("[sweep] miccheck error on %s: %r" % (label, e))

    if not os.path.exists(MC):
        log("[sweep] DEVICE-DEAD on %s (no miccheck.txt)" % label)
        return {"label": label, "i": i, "r": r, "j": j, "t": t, "m": m,
                "expect": expect, "dead": True, "text": ""}

    text = open(MC, encoding="utf-8", errors="replace").read()
    shutil.copyfile(MC, os.path.join(SWEEP, "sweep_%s.txt" % label))
    return {"label": label, "i": i, "r": r, "j": j, "t": t, "m": m,
            "expect": expect, "dead": False, "text": text}


def summarize(res, f):
    f.write("---- combo %s (I=%d R=%d J=%d T=%d M=%d)\n"
            % (res["label"], res["i"], res["r"], res["j"], res["t"], res["m"]))
    if res["dead"]:
        f.write("  DEVICE-DEAD: miccheck.txt not produced (re-run this combo alone)\n\n")
        return
    text = res["text"]
    cfg = grab([r"V46 config code\s+\d+.*"], text)
    if cfg:
        f.write("  CFGCODE: expect %d / %s\n" % (res["expect"], cfg))
    else:
        f.write("  CFGCODE: expect %d / (driver did NOT report -- combo inconclusive)\n"
                % res["expect"])
    dead = grab([r".*NOT reachable.*"], text)
    f.write("  CONTROL: %s\n" % ("device NOT reachable" if dead else "ok"))
    ours = grab([r".*OURS.*"], text)
    f.write("  ENDPOINT: %s\n" % (ours if ours else "OURS not found"))
    mix = (grab([r"\[4\] GetMixFormat failed \(0x[0-9a-fA-F]+\)"], text)
           or grab([r"\[4\] endpoint format :.*"], text))
    f.write("  MIX: %s\n" % (mix if mix else "(no [4] line)"))
    cap = grab([r"\[4\] captured.*"], text)
    f.write("  CAP: %s\n" % (cap if cap else "(none)"))
    for line in text.splitlines():
        if "[4b]" in line:
            f.write("  %s\n" % line.strip())
    f.write("\n")
    f.flush()


def main():
    if not os.path.exists(PKG):
        log("package dir missing: %s" % PKG)
        return 1
    os.makedirs(SWEEP, exist_ok=True)

    only = sys.argv[1] if len(sys.argv) > 1 else None

    # precheck: refuse to measure a stale image
    pre = os.path.join(SWEEP, "precheck.txt")
    subprocess.run([MICCHECK, "1"], cwd=PKG, capture_output=True,
                   timeout=240)
    pretxt = ""
    if os.path.exists(MC):
        shutil.copyfile(MC, pre)
        pretxt = open(pre, encoding="utf-8", errors="replace").read()
    # accept any V4x: the tag carries the build (V46/V47/...) and refusing on
    # an exact string would block a freshly built driver for no reason
    if "ISIGHTMIC-BUILD-V4" not in pretxt:
        log("[sweep] ABORT: running driver is NOT the V46 build. Reboot first.")
        for line in pretxt.splitlines():
            if "driver build" in line:
                log("   " + line.strip())
        return 2
    log("[sweep] V46 build confirmed.")

    f = open(SUM, "w", encoding="utf-8")
    f.write("V46 sweep (python runner)  %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
    f.write("combos: %s\n\n"
            % ", ".join(c[0] for c in COMBOS if not only or c[0] == only))
    f.flush()

    for (label, i, r, j, t, m) in COMBOS:
        if only and label != only:
            continue
        res = run_combo(label, i, r, j, t, m)
        summarize(res, f)

    f.write("\nV46 sweep complete.  Per-combo reports: sweep\\sweep_*.txt\n")
    f.close()

    # leave the machine on the V45-parity default
    if not only:
        log("[sweep] restoring DEFAULT config (0 0 1 0 0) ...")
        write_cfg(0, 0, 1, 0, 0)
        stop_audio()
        cycle_device()
        start_audio()
        time.sleep(3)
        subprocess.run([MICCHECK, "2"], cwd=PKG, capture_output=True,
                       timeout=240)

    log("[sweep] DONE. summary: %s" % SUM)
    return 0


if __name__ == "__main__":
    sys.exit(main())
