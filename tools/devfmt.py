#!/usr/bin/env python3
"""
devfmt.py -- inspect / set PKEY_AudioEngine_DeviceFormat on MMDevices endpoints.

PKEY_AudioEngine_DeviceFormat = {E4870E26-3CC5-4CD2-BA46-CA0A9A70ED04},3
stored under
  HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\
      Audio\\Capture\\{endpoint-guid}\\Properties

This is the format the audio engine hands back from IAudioClient::GetMixFormat.
A working capture endpoint (Realtek) has it; the iSightMic endpoint does NOT --
which is the direct cause of AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008) even
though the driver's own DataRangeIntersection returns STATUS_SUCCESS, the pin
instantiates and the port copies data.

Usage:
    python devfmt.py list                 show every endpoint + its device format
    python devfmt.py get   <endpoint-guid>
    python devfmt.py set   <endpoint-guid> [ch] [rate] [bits]   (default 2 48000 16)
    python devfmt.py clear <endpoint-guid>
"""
import sys
import struct
import winreg

BASE = r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture"
FMT = r"{e4870e26-3cc5-4cd2-ba46-ca0a9a70ed04},3"


def open_props(guid, write=False):
    acc = winreg.KEY_READ | (winreg.KEY_WRITE if write else 0)
    return winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                          BASE + "\\" + guid + r"\Properties", 0,
                          acc | winreg.KEY_WOW64_64KEY)


def endpoints():
    root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, BASE, 0,
                          winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
    out = []
    i = 0
    while True:
        try:
            g = winreg.EnumKey(root, i)
        except OSError:
            break
        i += 1
        try:
            pk = winreg.OpenKey(root, g + r"\Properties")
        except OSError:
            continue
        name = ""
        j = 0
        while True:
            try:
                n, v, t = winreg.EnumValue(pk, j)
            except OSError:
                break
            j += 1
            if n.lower().startswith("{b3f8fa53") and n.endswith("},6"):
                name = v
            elif n.lower().startswith("{1da5d803") and n.endswith("},8"):
                pass
        winreg.CloseKey(pk)
        out.append((g, name))
    winreg.CloseKey(root)
    return out


def describe(b):
    """Device format blobs are a WAVEFORMATEXTENSIBLE, sometimes preceded by
    a small header.  Find the WAVEFORMATEX and print it."""
    for off in (0, 4, 8, 16, 22, 24):
        if len(b) < off + 18:
            continue
        try:
            tag, ch, rate, avg, align, bits = struct.unpack_from("<HHIIHH", b, off)
        except struct.error:
            continue
        if 1 <= ch <= 8 and rate in (8000, 11025, 16000, 22050, 32000, 44100,
                                     48000, 88200, 96000, 192000) \
                and bits in (8, 16, 24, 32) and 0 < align <= 32:
            s = "off=%d tag=0x%04X ch=%d rate=%d bits=%d align=%d" % (
                off, tag, ch, rate, bits, align)
            if tag == 0xFFFE and len(b) >= off + 18 + 2:
                cb, = struct.unpack_from("<H", b, off + 16)
                if len(b) >= off + 18 + 22:
                    vb, mask = struct.unpack_from("<HI", b, off + 18)
                    s += " EXT cb=%d validbits=%d mask=0x%X" % (cb, vb, mask)
            return s
    return None


def read_fmt(guid):
    try:
        pk = open_props(guid)
    except OSError as e:
        return None, "cannot open: %r" % (e,)
    try:
        v, t = winreg.QueryValueEx(pk, FMT)
    except OSError:
        winreg.CloseKey(pk)
        return None, None
    winreg.CloseKey(pk)
    return v, t


def build_ext(ch=2, rate=48000, bits=16):
    """WAVEFORMATEXTENSIBLE as the engine stores it (48 bytes for 16-bit)."""
    cb = 22
    valid = bits
    mask = 0x3 if ch == 2 else 0x4   # FL|FR  /  FC
    wfx = struct.pack("<HHIIHH", 0xFFFE, ch, rate, rate * ch * bits // 8,
                      ch * bits // 8, bits) + struct.pack("<H", cb)
    ext = wfx + struct.pack("<HI", valid, mask) + b"\x00" * 16  # + SubFormat
    # SubFormat (KSDATAFORMAT_SUBTYPE_PCM) at the end
    pcm = bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])
    if len(ext) + 16 == 18 + cb:
        ext = wfx + struct.pack("<HI", valid, mask) + pcm
    return ext


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1].lower()

    if cmd == "list":
        for g, name in endpoints():
            v, t = read_fmt(g)
            if v is None:
                print("%s  %-40s  NO DEVICE FORMAT" % (g, name))
            else:
                d = describe(v) or "?"
                print("%s  %-40s  %d bytes  %s" % (g, name, len(v), d))
        return 0

    guid = sys.argv[2]
    if cmd == "get":
        v, t = read_fmt(guid)
        if v is None:
            print("no device format on %s" % guid)
            return 1
        print("type=%d len=%d" % (t, len(v)))
        print("desc: %s" % (describe(v) or "?"))
        print("hex : %s" % v.hex())
        return 0

    if cmd == "set":
        ch = int(sys.argv[3]) if len(sys.argv) > 3 else 2
        rate = int(sys.argv[4]) if len(sys.argv) > 4 else 48000
        bits = int(sys.argv[5]) if len(sys.argv) > 5 else 16
        blob = build_ext(ch, rate, bits)
        pk = open_props(guid, write=True)
        winreg.SetValueEx(pk, FMT, 0, winreg.REG_BINARY, blob)
        winreg.CloseKey(pk)
        print("wrote %d bytes -> %s  (%s)"
              % (len(blob), guid, describe(blob) or "?"))
        return 0

    if cmd == "clear":
        pk = open_props(guid, write=True)
        try:
            winreg.DeleteValue(pk, FMT)
            print("deleted device format on %s" % guid)
        except OSError as e:
            print("delete failed: %r" % (e,))
        winreg.CloseKey(pk)
        return 0

    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())
