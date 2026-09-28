#!/usr/bin/env python3
"""
dump_mmdevices.py -- dump the audio engine's per-endpoint property cache.

Why: after the V46 sweep proved that (a) the pin category is exactly right,
(b) DataRangeIntersection returns STATUS_SUCCESS in echo mode, (c) NewStream
succeeds and the port copies data -- GetMixFormat STILL returns
0x88890008 in all nine combos.  So the failure is not in the driver's format
negotiation at all.  Next suspect: the format the audio engine has cached for
this endpoint under

  HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\{guid}\\Properties

GetMixFormat hands the caller the engine's device format for the endpoint; if
that property is missing, empty or malformed, WASAPI reports
AUDCLNT_E_UNSUPPORTED_FORMAT no matter what the driver is willing to do.

This prints, per capture endpoint, every property key with its type and byte
length, plus a decoded WAVEFORMATEX when a blob looks like one -- so our
endpoint can be compared line by line against a working one (Realtek).
"""
import winreg
import struct
import sys

BASE = r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture"

# property keys we care about, by name fragment
FMT_KEYS = {
    "1da5d803-d492-4edd-8c23-e0c0ffee7f0e": "AudioEngine_DeviceFormat",
    "f19f064d-082c-4e27-bc73-6882a1bb8e4c": "AudioEngine (alt)",
    "a45c254e-df1c-4efd-8020-67d146a850e0": "Device_Interface/Name",
}


def decode_wfx(b):
    """MMDevices stores device formats as a WAVEFORMATEX often prefixed by
    a 0/4-byte length or wrapped in a PROPERTYKEY-tagged blob.  Try the plain
    layout first, then a couple of common offsets."""
    for off in (0, 4, 8, 16):
        if len(b) < off + 18:
            continue
        try:
            tag, ch, rate, avg, align, bits = struct.unpack_from("<HHIIHH", b, off)
        except struct.error:
            continue
        if 1 <= ch <= 8 and rate in (8000, 11025, 16000, 22050, 32000,
                                     44100, 48000, 88200, 96000, 192000) \
                and bits in (8, 16, 24, 32) and 0 < align <= 32:
            extra = ""
            if tag == 0xFFFE and len(b) >= off + 18 + 2:
                cb = struct.unpack_from("<H", b, off + 16)[0]
                extra = " EXT cb=%d" % cb
            return "wFormatTag=0x%04X ch=%d rate=%d bits=%d align=%d%s" % (
                tag, ch, rate, bits, align, extra)
    return None


def main():
    try:
        root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, BASE, 0,
                              winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
    except FileNotFoundError:
        print("no such key: %s" % BASE)
        return 1

    i = 0
    while True:
        try:
            guid = winreg.EnumKey(root, i)
        except OSError:
            break
        i += 1
        try:
            pk = winreg.OpenKey(root, guid + r"\Properties")
        except OSError:
            continue
        props = {}
        j = 0
        while True:
            try:
                name, val, typ = winreg.EnumValue(pk, j)
            except OSError:
                break
            j += 1
            props[name] = (typ, val)
        winreg.CloseKey(pk)

        # friendly name if we can find it (unicode REG_SZ or binary blob)
        label = guid
        for name, (typ, val) in props.items():
            if "a45c254e" in name.lower() and typ == winreg.REG_SZ:
                label = "%s  [%s]" % (guid, val)
                break
        print("=" * 70)
        print("endpoint %s   (%d properties)" % (label, len(props)))
        interesting = False
        for name in sorted(props):
            typ, val = props[name]
            low = name.lower()
            if any(f in low for f in ("1da5d803", "f19f064d")):
                interesting = True
            if isinstance(val, bytes):
                w = decode_wfx(val)
                extra = ("  -> " + w) if w else ""
                print("   %-52s bin %4d bytes%s" % (name, len(val), extra))
            else:
                print("   %-52s %s" % (name, val))
        if not interesting:
            print("   *** NO AudioEngine device-format property on this endpoint ***")

    winreg.CloseKey(root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
