// surround-probe -- empirically test the 3DS "Surround" audio output mode.
//
// Plays a steady 440 Hz sine on one NDSP channel at the DSP-native 32728 Hz
// (no resampler in the path) and lets you flip the OUTPUT MODE (Stereo/Surround/
// Mono) and the voice ROUTING (front-only vs rear-only, or a continuous
// front<->rear blend) live, while capturing the headphone jack on a PC.
//
// Hypothesis: in STEREO the DSP folds rear into front at unity, so front-only
// and rear-only are identical; in SURROUND the firmware virtualizes them and
// they differ.  That difference is what makes the NW4C "span" command audible.
//
// A ~200 ms silence "marker" is dropped on every setting change (and on demand
// with B) so separate PC recordings can be sample-aligned by tools/analyze.py.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <3ds.h>

#define SR        32728           // DSP native sample rate (avoids the resampler)
#define CYCLES    440             // whole sine cycles in the buffer ...
#define NSAMP     SR              // ... over exactly 1.0 s => exactly 440 Hz, seamless loop
#define AMP       0.30f           // 0.30 full-scale: headroom for any surround boost
#define MARKERFR  12              // marker silence length in frames (~200 ms @ 60 fps)

static float g_rearBlend = 0.0f;  // 0.0 = front-only, 1.0 = rear-only, in between = blend
static int   g_markerFr  = 0;     // >0 => output gated to silence (the marker gap)

static const ndspOutputMode MODES[3] = { NDSP_OUTPUT_STEREO, NDSP_OUTPUT_SURROUND, NDSP_OUTPUT_MONO };
static const char* MODE_NAME[3]      = { "STEREO  ", "SURROUND", "MONO    " };

// Recompute and push the 12-float mix. During the marker gap everything is 0
// (silence); otherwise the mono voice is split front/rear by g_rearBlend.
static void update_mix(void) {
    float mix[12];
    memset(mix, 0, sizeof(mix));
    if (g_markerFr == 0) {
        float f = 1.0f - g_rearBlend, r = g_rearBlend;
        mix[0] = mix[1] = f;      // front L / front R
        mix[2] = mix[3] = r;      // rear  L / rear  R
    }
    ndspChnSetMix(0, mix);
}

static void trigger_marker(void) { g_markerFr = MARKERFR; }

static void padline(int row, const char* s) {  // print s at row, padded to clear the line
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%-49s", s);
    (void)n;
    printf("\x1b[%d;1H%s", row, buf);
}

int main(int argc, char** argv) {
    gfxInitDefault();
    PrintConsole top;
    consoleInit(GFX_TOP, &top);
    consoleSelect(&top);

    // --- read the System Settings sound mode (Mono/Stereo/Surround), block 0x00070001
    u8 sysSound = 0xFF;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetConfigInfoBlk2(1, 0x00070001, &sysSound);
        cfguExit();
    }

    // --- audio up.  dspInit() so DSP_GetHeadphoneStatus works; ndspInit() for playback.
    bool haveDsp = R_SUCCEEDED(dspInit());
    if (R_FAILED(ndspInit())) {
        consoleClear();
        printf("ndspInit() failed.\n\nThe DSP firmware could not be loaded.\n"
               "On CFW this usually means dspfirm was not dumped.\n\nPress START to exit.\n");
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
        if (haveDsp) dspExit();
        gfxExit();
        return 0;
    }
    bool haveMcu = R_SUCCEEDED(mcuHwcInit());

    // --- generate one seamless-looping 440 Hz mono PCM16 buffer
    s16* buf = (s16*)linearAlloc(NSAMP * sizeof(s16));
    for (int i = 0; i < NSAMP; i++)
        buf[i] = (s16)(AMP * 32767.0f * sinf(2.0f * M_PI * (float)CYCLES * (float)i / (float)NSAMP));
    DSP_FlushDataCache(buf, NSAMP * sizeof(s16));

    // --- channel setup
    int   modeIdx   = 0;                    // start in STEREO
    u16   depth     = 0x7FFF;               // ndsp default
    u16   rearRatio = 0x8000;               // ndsp default
    ndspSpeakerPos pos = NDSP_SPKPOS_SQUARE;

    ndspSetMasterVol(1.0f);
    ndspSetOutputMode(MODES[modeIdx]);
    ndspSurroundSetDepth(depth);
    ndspSurroundSetPos(pos);
    ndspSurroundSetRearRatio(rearRatio);

    ndspChnReset(0);
    ndspChnIirBiquadSetEnable(0, false);    // ensure no stray filter colors the tone
    ndspChnSetInterp(0, NDSP_INTERP_NONE);  // native rate => no interpolation
    ndspChnSetRate(0, (float)SR);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    update_mix();

    static ndspWaveBuf wb;
    memset(&wb, 0, sizeof(wb));
    wb.data_pcm16 = buf;
    wb.nsamples   = NSAMP;
    wb.looping    = true;
    ndspChnWaveBufAdd(0, &wb);

    // --- static UI
    consoleClear();
    printf("\x1b[1;1H===== 3DS surround-probe =====");
    printf("\x1b[15;1H------------------------------------------------");
    printf("\x1b[16;1HA: front/rear    Up/Down: blend sweep");
    printf("\x1b[17;1HX: output mode   Y: surround pos");
    printf("\x1b[18;1HL/R: rear ratio  B: re-drop marker");
    printf("\x1b[19;1HSELECT: reset surround params  START: exit");
    printf("\x1b[21;1H440 Hz @ 32728 Hz.  Keep the volume slider FIXED");
    printf("\x1b[22;1Hfor every capture.  A silence marker is dropped");
    printf("\x1b[23;1Hon each change so recordings can be aligned.");

    trigger_marker();

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

        if (kDown & KEY_A) { g_rearBlend = (g_rearBlend < 0.5f) ? 1.0f : 0.0f; trigger_marker(); }

        if (kDown & KEY_X) {
            modeIdx = (modeIdx + 1) % 3;
            ndspSetOutputMode(MODES[modeIdx]);
            ndspSurroundSetDepth(depth);       // re-assert surround params on mode switch
            ndspSurroundSetPos(pos);
            ndspSurroundSetRearRatio(rearRatio);
            trigger_marker();
        }
        if (kDown & KEY_Y) {
            pos = (pos == NDSP_SPKPOS_SQUARE) ? NDSP_SPKPOS_WIDE : NDSP_SPKPOS_SQUARE;
            ndspSurroundSetPos(pos);
            trigger_marker();
        }
        if (kDown & KEY_L) {
            rearRatio = (rearRatio >= 0x0800) ? (u16)(rearRatio - 0x0800) : 0;
            ndspSurroundSetRearRatio(rearRatio); trigger_marker();
        }
        if (kDown & KEY_R) {
            rearRatio = (rearRatio <= 0xF7FF) ? (u16)(rearRatio + 0x0800) : 0xFFFF;
            ndspSurroundSetRearRatio(rearRatio); trigger_marker();
        }
        if (kDown & KEY_B) trigger_marker();
        if (kDown & KEY_SELECT) {
            depth = 0x7FFF; rearRatio = 0x8000; pos = NDSP_SPKPOS_SQUARE;
            ndspSurroundSetDepth(depth); ndspSurroundSetPos(pos); ndspSurroundSetRearRatio(rearRatio);
            trigger_marker();
        }

        // continuous front<->rear blend sweep (does NOT drop a marker)
        if (kHeld & KEY_DUP)   { g_rearBlend += 0.01f; if (g_rearBlend > 1.0f) g_rearBlend = 1.0f; }
        if (kHeld & KEY_DDOWN) { g_rearBlend -= 0.01f; if (g_rearBlend < 0.0f) g_rearBlend = 0.0f; }

        if (g_markerFr > 0) g_markerFr--;
        update_mix();

        // --- live status
        char line[64];
        // cfg block value: 0=Mono, 1=Stereo, 2=Surround
        const char* sysName = (sysSound == 0) ? "MONO    " :
                              (sysSound == 1) ? "STEREO  " :
                              (sysSound == 2) ? "SURROUND" : "unknown ";
        snprintf(line, sizeof(line), "System Settings sound mode : %s", sysName);
        padline(3, line);

        bool hp = false; const char* hpStr = "n/a";
        if (haveDsp && R_SUCCEEDED(DSP_GetHeadphoneStatus(&hp))) hpStr = hp ? "INSERTED" : "no";
        bool headset = osIsHeadsetConnected();
        u8 slider = 0; char slStr[8] = "n/a";
        if (haveMcu && R_SUCCEEDED(MCUHWC_GetSoundSliderLevel(&slider))) snprintf(slStr, sizeof(slStr), "%u", slider);
        snprintf(line, sizeof(line), "Headphone(dsp):%s  headset:%s  vol:%s", hpStr, headset ? "yes" : "no", slStr);
        padline(4, line);

        snprintf(line, sizeof(line), "FORCED output mode (ndsp)  : %s", MODE_NAME[modeIdx]);
        padline(6, line);

        const char* route = (g_rearBlend <= 0.001f) ? "FRONT-only" :
                            (g_rearBlend >= 0.999f) ? "REAR-only " : "BLEND     ";
        snprintf(line, sizeof(line), "Routing : %s  (rear=%3d%%)", route, (int)(g_rearBlend * 100.0f + 0.5f));
        padline(7, line);
        snprintf(line, sizeof(line), "  front gain %.2f   rear gain %.2f", 1.0f - g_rearBlend, g_rearBlend);
        padline(8, line);

        snprintf(line, sizeof(line), "Surround: pos=%-6s depth=0x%04X rear=0x%04X",
                 (pos == NDSP_SPKPOS_SQUARE) ? "SQUARE" : "WIDE", depth, rearRatio);
        padline(9, line);

        padline(11, (g_markerFr > 0) ? ">>> MARKER (silence) <<<" : "tone playing");

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    ndspChnWaveBufClear(0);
    ndspExit();
    if (haveMcu) mcuHwcExit();
    if (haveDsp) dspExit();
    linearFree(buf);
    gfxExit();
    return 0;
}
