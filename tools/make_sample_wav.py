#!/usr/bin/env python3
"""Write a small test WAV for the iSight virtual-microphone loopback check.

The virtual mic presents 48 kHz / 16-bit / mono, so a plain 440 Hz stereo tone
is enough to prove the whole chain: feeder -> IOCTL -> ring -> PortCls -> the
recording endpoint a chat app actually opens.

Why this is a file and not an inline heredoc in the workflow: PowerShell
here-strings require the closing marker at column 0, which terminates the YAML
block scalar that the `run:` step lives in, and GitHub then rejects the whole
workflow at dispatch (a failed run with zero jobs).  Keeping the payload in a
real file sidesteps that class of bug entirely.

Usage:  python tools/make_sample_wav.py [output.wav]
"""

import math
import struct
import sys
import wave

SAMPLE_RATE = 48000
SECONDS = 3.0
TONE_HZ = 440.0
AMPLITUDE = 0.3
CHANNELS = 2


def main(out_path: str) -> int:
    frames = int(SAMPLE_RATE * SECONDS)
    peak = int(32767 * AMPLITUDE)

    with wave.open(out_path, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        for i in range(frames):
            v = int(peak * math.sin(2.0 * math.pi * TONE_HZ * i / SAMPLE_RATE))
            w.writeframes(struct.pack("<hh", v, v))

    print(
        "wrote %s (%d Hz / 16-bit / %d ch, %.1f s, %.0f Hz tone)"
        % (out_path, SAMPLE_RATE, CHANNELS, SECONDS, TONE_HZ)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "sample.wav"))
