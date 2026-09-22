// feed.cpp - user-mode feeder for isightmic.sys  (M1 milestone)
//
// Reads a 48 kHz / 16-bit WAV (mono or stereo), downmixes stereo to mono, and
// loops it into the driver's ring buffer through IOCTL_ISIGHTMIC_PUSH.  This
// proves the kernel driver end-to-end: a real "iSight Microphone (FireWire)"
// endpoint shows up in the system and whatever we push is what recording apps
// capture -- without needing the camera at all.
//
// Build (see make.svc.bat):
//   cl /nologo /O2 /W3 /EHsc /MD feed.cpp /Fe:isight-micsvc.exe
//
// Usage:
//   isight-micsvc.exe <file.wav> [loop]
//   (loops forever; Ctrl+C to stop)

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "isightmic.h"

#pragma pack(push, 1)
struct WavHdr {
    char     riff[4];
    uint32_t size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmtSize;
    uint16_t format;
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bits;
};
#pragma pack(pop)

// Read the whole PCM payload of a WAV into a malloc'd buffer (mono, 48k, 16-bit).
// Returns number of bytes, or 0 on error.  Caller frees with free().
static uint8_t* load_wav_mono16_48k(const char* path, uint32_t* outBytes) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return NULL; }
    WavHdr h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); return NULL; }
    if (memcmp(h.riff, "RIFF", 4) || memcmp(h.wave, "WAVE", 4) ||
        memcmp(h.fmt, "fmt ", 4)) { printf("not a WAV\n"); fclose(f); return NULL; }
    if (h.format != 1) { printf("only PCM WAV supported\n"); fclose(f); return NULL; }

    // find the "data" chunk
    uint32_t dataLen = 0;
    char tag[4];
    uint32_t sz = 0;
    long pos = sizeof(h);
    while (fread(tag, 1, 4, f) == 4) {
        if (fread(&sz, 1, 4, f) != 4) break;
        if (memcmp(tag, "data", 4) == 0) { dataLen = sz; break; }
        fseek(f, (long)sz, SEEK_CUR);
        pos += 8 + sz;
    }
    if (dataLen == 0) { printf("no data chunk\n"); fclose(f); return NULL; }

    // For M1 we only handle 48 kHz / 16-bit (mono or stereo).  Our processed
    // files are 48k/16/stereo, so the stereo->mono downmix path is what runs.
    if (h.sampleRate != ISIGHTMIC_SAMPLERATE || h.bits != 16) {
        printf("expected %d Hz / 16-bit, got %u Hz / %u-bit\n",
               ISIGHTMIC_SAMPLERATE, h.sampleRate, h.bits);
        fclose(f);
        return NULL;
    }

    uint32_t nFrames = dataLen / h.blockAlign;
    uint8_t* raw = (uint8_t*)malloc(dataLen);
    if (!raw) { fclose(f); return NULL; }
    if (fread(raw, 1, dataLen, f) != dataLen) { free(raw); fclose(f); return NULL; }
    fclose(f);

    uint32_t outFrames = nFrames;
    uint8_t* out = (uint8_t*)malloc(outFrames * ISIGHTMIC_FRAME_BYTES);
    if (!out) { free(raw); return NULL; }

    int ch = h.channels;
    for (uint32_t i = 0; i < nFrames; i++) {
        int32_t sum = 0;
        const int16_t* s = (const int16_t*)(raw + i * h.blockAlign);
        for (int c = 0; c < ch; c++) sum += s[c];
        int16_t v = (int16_t)(sum / ch);
        out[i * 2]     = (uint8_t)(v & 0xFF);
        out[i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
    }
    free(raw);
    *outBytes = outFrames * ISIGHTMIC_FRAME_BYTES;
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: %s <file.wav>\n", argv[0]);
        return 1;
    }
    uint32_t pcmBytes = 0;
    uint8_t* pcm = load_wav_mono16_48k(argv[1], &pcmBytes);
    if (!pcm || pcmBytes == 0) return 1;
    printf("loaded %u bytes (%u ms) of mono 48k/16 audio\n",
           pcmBytes, pcmBytes / ISIGHTMIC_FRAME_BYTES * 1000 / ISIGHTMIC_SAMPLERATE);

    HANDLE hDev = CreateFileW(L"\\\\.\\IsightMicCtl",
                              GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hDev == INVALID_HANDLE_VALUE) {
        printf("open \\\\.\\IsightMicCtl failed (0x%08X). Is the driver loaded?\n",
               GetLastError());
        free(pcm);
        return 1;
    }

    const ULONG chunk = ISIGHTMIC_SAMPLERATE * ISIGHTMIC_FRAME_BYTES / 4; // 250 ms
    uint32_t pos = 0;
    printf("pushing (Ctrl+C to stop)...\n");
    while (1) {
        DWORD written = 0;
        if (!DeviceIoControl(hDev, IOCTL_ISIGHTMIC_PUSH,
                             pcm + pos, chunk, NULL, 0, &written, NULL)) {
            printf("push failed 0x%08X\n", GetLastError());
            break;
        }
        pos += chunk;
        if (pos + chunk > pcmBytes) pos = 0;   // loop
        Sleep(250);
    }

    CloseHandle(hDev);
    free(pcm);
    return 0;
}
