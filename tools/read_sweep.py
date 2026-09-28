#!/usr/bin/env python3
"""Read a V46 sweep report (sweep\\SWEEP_SUMMARY.txt) and print the verdict.

Run it next to the package (or point it at a sweep folder):

    python tools\\read_sweep.py sweep

What it answers, in this order:

  1. Did each combo's registry switches actually reach the driver?
     (CFGCODE "expect" vs the code the driver reported.  A mismatch means the
     combo never applied -- its result must NOT be read as "this does not
     help".)
  2. Which combos are merely alive, which got a mix format, and which really
     captured audio (the actual fix for AUDCLNT_E_UNSUPPORTED_FORMAT)?
  3. What is the smallest set of switches that fixes it, and what does each
     single bit do on its own (baseline vs one-bit-flip)?

Exit code is 0 if at least one combo captured audio, 1 otherwise.
"""

import re
import sys
from pathlib import Path

BIT_NAMES = ["Intersect", "Ranges", "Jack", "Topo", "Mono"]


def parse(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    blocks = re.split(r"^-{2,}\s*combo\s+", text, flags=re.M)[1:]
    out = []
    for b in blocks:
        head = b.splitlines()[0] if b.splitlines() else ""
        m = re.match(r"(\S+)\s+\(I=(\d+)\s+R=(\d+)\s+J=(\d+)\s+T=(\d+)\s+M=(\d+)\)", head)
        if not m:
            continue
        label, bits = m.group(1), [int(m.group(i)) for i in range(2, 7)]

        def one(pat):
            mm = re.search(pat, b)
            return mm.group(1).strip() if mm else ""

        cfg_expect = one(r"CFGCODE:\s*expect\s+(\d+)")
        cfg_reported = one(r"config code\s+(\d+)")
        control = one(r"CONTROL:\s*(.+)")
        endpoint = one(r"ENDPOINT:\s*(.+)")
        mix = one(r"MIX:\s*(.+)")
        cap = one(r"CAP:\s*(.+)")
        b4 = re.findall(r"\[4b\]\s*(.+)", b)

        out.append(
            {
                "label": label,
                "bits": bits,
                "cfg_expect": cfg_expect,
                "cfg_reported": cfg_reported,
                "control": control,
                "endpoint": endpoint,
                "mix": mix,
                "cap": cap,
                "b4": b4,
            }
        )
    return out


def cfg_ok(r):
    return r["cfg_reported"] != "" and r["cfg_expect"] == r["cfg_reported"]


def verdict(r):
    if "NOT reachable" in r["control"] or "not found" in r["endpoint"]:
        return "DEAD"
    if "captured" in r["cap"]:
        return "FIXED"
    if r["mix"] and "failed" not in r["mix"]:
        return "MIX-OK"
    if r["mix"]:
        return "BROKEN"
    return "UNKNOWN"


def bits_str(bits):
    on = [n for n, v in zip(BIT_NAMES, bits) if v]
    return "+".join(on) if on else "(none)"


def main():
    folder = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("sweep")
    summary = folder / "SWEEP_SUMMARY.txt"
    if not summary.exists():
        print("no %s -- run sweep-mic.bat first (as Administrator)" % summary)
        return 2

    rows = parse(summary)
    if not rows:
        print("no combos parsed from %s" % summary)
        return 2

    print("=" * 78)
    print("V46 sweep verdict  --  %s" % summary)
    print("=" * 78)
    print("%-22s %-26s %-8s %-9s %s" % ("combo", "switches on", "cfgcode", "verdict", "capture"))
    print("-" * 78)
    for r in rows:
        v = verdict(r)
        cfg = "%s/%s" % (r["cfg_expect"] or "?", r["cfg_reported"] or "-")
        flag = "" if cfg_ok(r) else "  <-- SWITCHES DID NOT APPLY"
        print(
            "%-22s %-26s %-8s %-9s %s%s"
            % (
                r["label"][:22],
                bits_str(r["bits"])[:26],
                cfg,
                v,
                (r["cap"][:18] if r["cap"] and "none" not in r["cap"] else "-"),
                flag,
            )
        )
    print("-" * 78)

    # 1. did the switches apply?
    bad = [r["label"] for r in rows if not cfg_ok(r)]
    if bad:
        print("\n!! combos whose CfgCode did not match: %s" % ", ".join(bad))
        print("   Those runs did not load the requested config (registry view /")
        print("   permissions / stale driver image) -- ignore their verdicts.")

    # 2. outcome buckets
    fixed = [r for r in rows if verdict(r) == "FIXED" and cfg_ok(r)]
    mixok = [r for r in rows if verdict(r) == "MIX-OK" and cfg_ok(r)]
    dead = [r for r in rows if verdict(r) == "DEAD"]

    if fixed:
        fixed.sort(key=lambda r: (sum(r["bits"]), r["label"]))
        best = fixed[0]
        print("\nFIXED (endpoint really captured audio): %s" % ", ".join(r["label"] for r in fixed))
        print("  smallest switch set that does it: %-20s %s" % (best["label"], bits_str(best["bits"])))
        print("  reproduce with:  switch-mic.bat %s" % " ".join(str(b) for b in best["bits"]))
    elif mixok:
        print("\nNo combo captured audio, but GetMixFormat SUCCEEDED for: %s"
              % ", ".join(r["label"] for r in mixok))
        print("  -> 0x88890008 is gone there; check [4b] / the capture step next.")
    else:
        print("\nEvery combo still fails GetMixFormat (or the device was dead).")
        print("  -> the fix is not in this switch matrix; next suspects are the")
        print("     endpoint property keys / the format list the engine caches.")

    if dead:
        print("\nDEAD combos (device/endpoint missing): %s" % ", ".join(r["label"] for r in dead))

    # 3. single-bit effect vs baseline
    base = next((r for r in rows if r["label"].startswith("baseline")), None)
    if base:
        print("\nSingle-bit effect (vs %s = %s):" % (base["label"], verdict(base)))
        for i, name in enumerate(BIT_NAMES):
            for r in rows:
                if r is base:
                    continue  # the baseline is a one-bit combo itself; skip it
                if sum(r["bits"]) == 1 and r["bits"][i] == 1:
                    print("  %-10s %-20s %s -> %s" % (name, r["label"], verdict(base), verdict(r)))

    # 4. what the engine would accept ([4b])
    print("\n[4b] engine format acceptance (per combo):")
    for r in rows:
        if r["b4"]:
            print("  %s" % r["label"])
            for line in r["b4"]:
                print("      %s" % line.strip())

    print("\n(per-combo full reports: %s\\sweep_*.txt)" % folder)
    return 0 if fixed else 1


if __name__ == "__main__":
    sys.exit(main())
