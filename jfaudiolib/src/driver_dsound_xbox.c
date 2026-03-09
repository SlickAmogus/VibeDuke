/*
 * Xbox DirectSound audio driver for jfaudiolib (MultiVoc backend)
 *
 * Replaces the SDL audio path with RXDK's DirectSound library, which
 * programs the APU's full VP→GP→EP→AC97 pipeline including:
 *   - Voice Processor hardware mixing
 *   - GP FIFO DMA (not DSP MOVE — the key difference from our manual code)
 *   - EP Dolby AC3 encoding for optical 5.1 output
 *
 * Architecture:
 *   MultiVoc generates mixed PCM into ring buffers via MixCallBack.
 *   We create a DirectSound looping buffer and periodically copy PCM
 *   into it via Lock/Unlock in a pump function called from the game loop.
 *   DirectSoundDoWork() drives the hardware frame processing.
 */

#define NOD3D 1      /* Don't include D3D from xtl.h */
#define NODSOUND 1   /* We include dsound.h manually below */
#include "xbox_defs.h"

/* Include RXDK DirectSound header */
#undef NODSOUND
#include <dsound.h>

#include "driver_dsound_xbox.h"
#include "asssys.h"
#include <string.h>

#ifdef _XBOX
extern void xbox_log(const char *fmt, ...);
#else
#define xbox_log(...) ((void)0)
#endif

/* ---- Error codes -------------------------------------------------------- */

enum {
    XDS_Err_Warning = -2,
    XDS_Err_Error   = -1,
    XDS_Err_Ok      = 0,
    XDS_Err_Uninitialised,
    XDS_Err_CreateDS,
    XDS_Err_CreateBuffer,
    XDS_Err_Lock,
    XDS_Err_Play
};

/* ---- State -------------------------------------------------------------- */

static int ErrorCode = XDS_Err_Ok;
static int Initialised = 0;
static int Playing = 0;

static IDirectSound *pDS = NULL;
static IDirectSoundBuffer *pDSBuf = NULL;

/* Ring buffer from MultiVoc */
static char *MixBuffer = NULL;
static int MixBufferSize = 0;       /* Size of ONE division */
static int MixBufferCount = 0;      /* Number of divisions */
static int MixBufferCurrent = 0;    /* Current division index */
static int MixBufferUsed = 0;       /* Bytes consumed from current div */
static void (*MixCallBack)(void) = NULL;

/* DirectSound buffer state */
#define DS_BUFFER_SIZE  (32768)      /* 32KB ring buffer (~185ms at 44100/16/2) */
static DWORD DSWriteCursor = 0;     /* Our write position in DS buffer */
static CRITICAL_SECTION DSLock;

static int PumpCallCount = 0;

/* Volume amplification — Xbox APU pipeline with AC3 encoding outputs quieter
 * than expected.  Boost PCM samples before writing to the DS buffer. */
#define VOLUME_BOOST 4  /* 4x = ~12 dB gain */

static void AmplifyBuffer(void *buf, DWORD bytes)
{
    short *samples = (short *)buf;
    DWORD count = bytes / 2;  /* 16-bit samples */
    DWORD i;

    for (i = 0; i < count; i++) {
        int val = (int)samples[i] * VOLUME_BOOST;
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;
        samples[i] = (short)val;
    }
}

/* ---- API ---------------------------------------------------------------- */

int XboxDSDrv_GetError(void)
{
    return ErrorCode;
}

const char *XboxDSDrv_ErrorString(int ErrorNumber)
{
    switch (ErrorNumber) {
        case XDS_Err_Warning:
        case XDS_Err_Error:
            return XboxDSDrv_ErrorString(ErrorCode);
        case XDS_Err_Ok:
            return "Xbox DirectSound ok.";
        case XDS_Err_Uninitialised:
            return "Xbox DirectSound uninitialised.";
        case XDS_Err_CreateDS:
            return "Xbox DirectSound: DirectSoundCreate failed.";
        case XDS_Err_CreateBuffer:
            return "Xbox DirectSound: CreateSoundBuffer failed.";
        case XDS_Err_Lock:
            return "Xbox DirectSound: Lock failed.";
        case XDS_Err_Play:
            return "Xbox DirectSound: Play failed.";
        default:
            return "Unknown Xbox DirectSound error.";
    }
}

int XboxDSDrv_PCM_Init(int *mixrate, int *numchannels, int *samplebits, void *initdata)
{
    HRESULT hr;
    DSBUFFERDESC dsbd;
    WAVEFORMATEX wfx;

    (void)initdata;

    if (Initialised) {
        XboxDSDrv_PCM_Shutdown();
    }

    xbox_log("XboxDS: PCM_Init rate=%d ch=%d bits=%d\n",
        *mixrate, *numchannels, *samplebits);

    /* Force stereo 16-bit (DirectSound on Xbox requires 16-bit PCM) */
    *numchannels = 2;
    *samplebits = 16;

    /* Initialize critical section */
    RtlInitializeCriticalSection(&DSLock);

    /* Reset APU to clean state before DirectSoundCreate.
     * The Xbox kernel leaves APU registers in a partially-configured state
     * from boot. RXDK's DirectSound expects to own the APU from scratch.
     * Disable APU interrupts and reset the front-end/setup-engine. */
    {
        volatile unsigned long *apu = (volatile unsigned long *)0xFE800000u;

        /* Disable all APU interrupts */
        apu[0x1004 / 4] = 0;  /* NV_PAPU_IEN = 0 */

        /* Stop setup engine processing */
        apu[0x2000 / 4] = 0;  /* NV_PAPU_SECTL = 0 (disable XCNTMODE) */

        /* Halt front-end */
        apu[0x1100 / 4] = 0;  /* NV_PAPU_FECTL = 0 */

        /* Clear any pending interrupts */
        apu[0x1000 / 4] = 0xFFFFFFFF;  /* NV_PAPU_ISTS = write-1-to-clear all */

        /* Brief stall to let hardware settle */
        KeStallExecutionProcessor(100);

        xbox_log("XboxDS: APU reset done\n");
    }

    /* Initialize DirectSound's global critical section.
     * dsound.lib expects this to be initialized by DLL startup, but since
     * we link the .obj files directly, no DLL init runs.
     * g_DirectSoundCriticalSection is defined in globals.obj. */
    {
        extern CRITICAL_SECTION g_DirectSoundCriticalSection;
        RtlInitializeCriticalSection(&g_DirectSoundCriticalSection);
        xbox_log("XboxDS: initialized g_DirectSoundCriticalSection\n");
    }

    /* Create DirectSound */
    xbox_log("XboxDS: calling DirectSoundCreate...\n");
    hr = DirectSoundCreate(NULL, &pDS, NULL);
    if (FAILED(hr)) {
        xbox_log("XboxDS: DirectSoundCreate FAILED hr=0x%08X\n", (unsigned)hr);
        ErrorCode = XDS_Err_CreateDS;
        return XDS_Err_Error;
    }
    xbox_log("XboxDS: DirectSoundCreate OK pDS=%p\n", (void *)pDS);

    /* Override speaker config for 5.1 surround with AC3 */
    DirectSoundOverrideSpeakerConfig(DSSPEAKER_ENABLE_AC3 | DSSPEAKER_SURROUND);
    xbox_log("XboxDS: Set speaker config SURROUND+AC3\n");

    /* Set up wave format */
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (unsigned short)*numchannels;
    wfx.nSamplesPerSec = *mixrate;
    wfx.wBitsPerSample = (unsigned short)*samplebits;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    /* Create sound buffer */
    memset(&dsbd, 0, sizeof(dsbd));
    dsbd.dwSize = sizeof(dsbd);
    dsbd.dwFlags = 0; /* Default 2D buffer */
    dsbd.dwBufferBytes = DS_BUFFER_SIZE;
    dsbd.lpwfxFormat = &wfx;
    dsbd.lpMixBins = NULL; /* Use default stereo mixbins */
    dsbd.dwInputMixBin = 0;

    hr = IDirectSound_CreateSoundBuffer(pDS, &dsbd, &pDSBuf, NULL);
    if (FAILED(hr)) {
        xbox_log("XboxDS: CreateSoundBuffer FAILED hr=0x%08X\n", (unsigned)hr);
        IDirectSound_Release(pDS);
        pDS = NULL;
        ErrorCode = XDS_Err_CreateBuffer;
        return XDS_Err_Error;
    }
    xbox_log("XboxDS: CreateSoundBuffer OK pBuf=%p size=%d\n",
        (void *)pDSBuf, DS_BUFFER_SIZE);

    /* Clear the buffer */
    {
        LPVOID ptr1;
        DWORD bytes1;
        hr = IDirectSoundBuffer_Lock(pDSBuf, 0, DS_BUFFER_SIZE,
            &ptr1, &bytes1, NULL, NULL, DSBLOCK_ENTIREBUFFER);
        if (SUCCEEDED(hr)) {
            memset(ptr1, 0, bytes1);
            IDirectSoundBuffer_Unlock(pDSBuf, ptr1, bytes1, NULL, 0);
        }
    }

    DSWriteCursor = 0;
    PumpCallCount = 0;
    Initialised = 1;

    xbox_log("XboxDS: PCM_Init OK\n");
    return XDS_Err_Ok;
}

void XboxDSDrv_PCM_Shutdown(void)
{
    if (!Initialised) return;

    xbox_log("XboxDS: PCM_Shutdown\n");

    if (Playing) {
        XboxDSDrv_PCM_StopPlayback();
    }

    if (pDSBuf) {
        IDirectSoundBuffer_Release(pDSBuf);
        pDSBuf = NULL;
    }

    if (pDS) {
        IDirectSound_Release(pDS);
        pDS = NULL;
    }

    Initialised = 0;
}

/* Pump PCM data from MultiVoc ring buffer into DirectSound buffer.
 * Called from the game loop (via DirectSoundDoWork integration). */
static void PumpAudio(void)
{
    DWORD playCursor, writeCursor;
    DWORD writeBytes;
    HRESULT hr;
    LPVOID ptr1 = NULL, ptr2 = NULL;
    DWORD bytes1 = 0, bytes2 = 0;

    if (!Playing || !pDSBuf || !MixCallBack) return;

    PumpCallCount++;

    /* Get current play position */
    hr = IDirectSoundBuffer_GetCurrentPosition(pDSBuf, &playCursor, &writeCursor);
    if (FAILED(hr)) return;

    /* Calculate how much space is available to write.
     * We write from our DSWriteCursor up to (but not including) playCursor. */
    if (DSWriteCursor <= playCursor) {
        writeBytes = playCursor - DSWriteCursor;
    } else {
        writeBytes = DS_BUFFER_SIZE - DSWriteCursor + playCursor;
    }

    /* Don't write too close to play cursor — leave some headroom */
    if (writeBytes < 1024) return;
    writeBytes -= 512; /* Safety margin */

    /* Log periodically */
    if (PumpCallCount <= 5 || (PumpCallCount % 200 == 0)) {
        xbox_log("XboxDS: Pump#%d play=%u write=%u ours=%u avail=%u\n",
            PumpCallCount, (unsigned)playCursor, (unsigned)writeCursor,
            (unsigned)DSWriteCursor, (unsigned)writeBytes);
    }

    /* Lock the DirectSound buffer region we want to fill */
    hr = IDirectSoundBuffer_Lock(pDSBuf, DSWriteCursor, writeBytes,
        &ptr1, &bytes1, &ptr2, &bytes2, 0);
    if (FAILED(hr)) return;

    /* Fill from MultiVoc ring buffer */
    {
        char *dst = (char *)ptr1;
        DWORD remaining = bytes1;

        while (remaining > 0) {
            if (MixBufferUsed >= MixBufferSize) {
                /* Need next division — call MultiVoc to generate more PCM */
                MixCallBack();
                MixBufferUsed = 0;
                MixBufferCurrent++;
                if (MixBufferCurrent >= MixBufferCount) {
                    MixBufferCurrent = 0;
                }
            }

            char *src = MixBuffer + (MixBufferCurrent * MixBufferSize) + MixBufferUsed;
            DWORD avail = MixBufferSize - MixBufferUsed;
            DWORD chunk = (remaining < avail) ? remaining : avail;

            memcpy(dst, src, chunk);
            dst += chunk;
            MixBufferUsed += chunk;
            remaining -= chunk;
        }

        /* Handle wrap-around region (ptr2) */
        if (ptr2 && bytes2 > 0) {
            dst = (char *)ptr2;
            remaining = bytes2;

            while (remaining > 0) {
                if (MixBufferUsed >= MixBufferSize) {
                    MixCallBack();
                    MixBufferUsed = 0;
                    MixBufferCurrent++;
                    if (MixBufferCurrent >= MixBufferCount) {
                        MixBufferCurrent = 0;
                    }
                }

                char *src = MixBuffer + (MixBufferCurrent * MixBufferSize) + MixBufferUsed;
                DWORD avail = MixBufferSize - MixBufferUsed;
                DWORD chunk = (remaining < avail) ? remaining : avail;

                memcpy(dst, src, chunk);
                dst += chunk;
                MixBufferUsed += chunk;
                remaining -= chunk;
            }
        }
    }

    /* Boost volume before handing buffer back to hardware */
    if (ptr1 && bytes1) AmplifyBuffer(ptr1, bytes1);
    if (ptr2 && bytes2) AmplifyBuffer(ptr2, bytes2);

    IDirectSoundBuffer_Unlock(pDSBuf, ptr1, bytes1, ptr2, bytes2);

    /* Advance our write cursor */
    DSWriteCursor = (DSWriteCursor + bytes1 + bytes2) % DS_BUFFER_SIZE;
}

int XboxDSDrv_PCM_BeginPlayback(char *BufferStart, int BufferSize,
                                 int NumDivisions, void (*CallBackFunc)(void))
{
    HRESULT hr;

    xbox_log("XboxDS: BeginPlayback buf=%p size=%d divs=%d\n",
        (void *)BufferStart, BufferSize, NumDivisions);

    if (!Initialised) {
        ErrorCode = XDS_Err_Uninitialised;
        return XDS_Err_Error;
    }

    if (Playing) {
        XboxDSDrv_PCM_StopPlayback();
    }

    MixBuffer = BufferStart;
    MixBufferSize = BufferSize;
    MixBufferCount = NumDivisions;
    MixBufferCurrent = 0;
    MixBufferUsed = 0;
    MixCallBack = CallBackFunc;
    DSWriteCursor = 0;
    PumpCallCount = 0;

    /* Prime the mix buffer */
    MixCallBack();

    /* Pre-fill the DS buffer with initial audio data */
    {
        LPVOID ptr1;
        DWORD bytes1;
        hr = IDirectSoundBuffer_Lock(pDSBuf, 0, DS_BUFFER_SIZE,
            &ptr1, &bytes1, NULL, NULL, DSBLOCK_ENTIREBUFFER);
        if (SUCCEEDED(hr)) {
            char *dst = (char *)ptr1;
            DWORD remaining = bytes1;
            while (remaining > 0) {
                if (MixBufferUsed >= MixBufferSize) {
                    MixCallBack();
                    MixBufferUsed = 0;
                    MixBufferCurrent++;
                    if (MixBufferCurrent >= MixBufferCount)
                        MixBufferCurrent = 0;
                }
                char *src = MixBuffer + (MixBufferCurrent * MixBufferSize) + MixBufferUsed;
                DWORD avail = MixBufferSize - MixBufferUsed;
                DWORD chunk = (remaining < avail) ? remaining : avail;
                memcpy(dst, src, chunk);
                dst += chunk;
                MixBufferUsed += chunk;
                remaining -= chunk;
            }
            AmplifyBuffer(ptr1, bytes1);
            IDirectSoundBuffer_Unlock(pDSBuf, ptr1, bytes1, NULL, 0);
            DSWriteCursor = bytes1 % DS_BUFFER_SIZE;
            xbox_log("XboxDS: Pre-filled %u bytes\n", (unsigned)bytes1);
        }
    }

    /* Start looping playback */
    hr = IDirectSoundBuffer_Play(pDSBuf, 0, 0, DSBPLAY_LOOPING);
    if (FAILED(hr)) {
        xbox_log("XboxDS: Play FAILED hr=0x%08X\n", (unsigned)hr);
        ErrorCode = XDS_Err_Play;
        return XDS_Err_Error;
    }

    Playing = 1;
    xbox_log("XboxDS: Play started, looping\n");
    return XDS_Err_Ok;
}

void XboxDSDrv_PCM_StopPlayback(void)
{
    if (!Initialised || !Playing) return;

    xbox_log("XboxDS: StopPlayback\n");

    if (pDSBuf) {
        IDirectSoundBuffer_Stop(pDSBuf);
    }

    Playing = 0;
}

void XboxDSDrv_PCM_Lock(void)
{
    RtlEnterCriticalSection(&DSLock);
}

void XboxDSDrv_PCM_Unlock(void)
{
    RtlLeaveCriticalSection(&DSLock);
}

/* ---- Public pump function (called from game loop) ----------------------- */

void XboxDS_Pump(void)
{
    if (!Initialised || !Playing) return;

    /* Pump our audio data */
    PumpAudio();

    /* Drive DirectSound's hardware frame processing.
     * This is REQUIRED on Xbox — it triggers the VP→GP→EP pipeline. */
    DirectSoundDoWork();
}
