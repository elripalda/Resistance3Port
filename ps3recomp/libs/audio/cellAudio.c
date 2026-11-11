/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "cellAudio.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "ps3emu/log_first.h"
#include "ps3emu/port_hitch.h"   /* [hitch] audio late-block wait */
#include "../../runtime/ps3_log.h"   /* ps3_diag_enabled (R2_DIAG) */
#include "audio_stretch.h"
#include "audio_out.h"

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  ts_us;          /* v124: guest system time when read_index last advanced */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Timestamp anchor for cellAudioGetPortTimestamp (microseconds, set at init) */
static u64            s_audio_start_us = 0;

/* Mixing thread */
static unsigned long long g_audio_dropped;   /* frames the device had no room for */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
/* Is s_mix_thread a thread that exists and has not been joined? The Win32
 * branch asks that of the HANDLE itself, which it nulls after closing;
 * pthread_t has no such value, so carry the flag. */
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

/* Output mix buffer (stereo, one block worth) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

/* Logical CPUs on the host (all processor groups). */
static int audio_host_cpus(void)
{
    static int n = 0;
    if (!n) {
#ifdef _WIN32
        n = (int)GetActiveProcessorCount(0xFFFF /* ALL_PROCESSOR_GROUPS */);
#else
        n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
        if (n <= 0) n = 1;
    }
    return n;
}

/* v140: target host-side audio buffer in ms. PORT_AUDIO_BUFFER_MS (or the
 * older AUDIO_BUFFER_MS); default 100 (RPCS3's "audio buffer duration"),
 * 150 with fewer than 12 logical CPUs: a CPU-bound host wakes every thread
 * later, the mixer included, and the stretcher below needs headroom. */
static unsigned audio_buffer_ms(void)
{
    static int ms = -1;
    if (ms < 0) { const char* e = getenv("PORT_AUDIO_BUFFER_MS");
                  if (!e || !*e) e = getenv("AUDIO_BUFFER_MS");
                  ms = (e && *e) ? atoi(e) : (audio_host_cpus() < 12 ? 150 : 100);
                  if (ms < 20) ms = 20; if (ms > 500) ms = 500; }
    return (unsigned)ms;
}

/* Audio health counters: always on, summarised on stderr every 10 s by the
 * mix thread ([audio] lines in logs\run.log) so tester logs show stutter. */
static struct {
    unsigned concealed;     /* guest blocks never written in time: gap filled */
    unsigned rescued;       /* late guest blocks played via the read delay */
    unsigned waits, caught, wait_timeouts;
    unsigned long long slip_us;
    unsigned reprimes;      /* host buffer ran empty: refilled with silence */
    unsigned dev_dry;       /* the device itself ran dry (WASAPI padding 0) */
    unsigned resyncs;       /* mix thread > 50 ms late: blocks skipped */
    unsigned fifo_drops;    /* device stalled: oldest mixed block dropped */
    unsigned stretch_ticks, ticks;
    double   tempo_min;
    double   late_max_ms;   /* worst mix-thread wake-up lateness */
    unsigned long long q_sum; unsigned q_min, q_n;
} s_ah = { 0 };

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

/* SDL queues without bound: keep about four blocks ahead of the device. */
static int audio_backend_room(void)
{
    if (!s_sdl_audio_dev) return -1;
    return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float)) <
           CELL_AUDIO_BLOCK_SAMPLES * 4 ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;
static unsigned             s_wasapi_dev_rate = 0, s_wasapi_dev_ch = 0;   /* device mix format */
static long long            s_wasapi_period_100ns = 0;                  /* engine period */

static int audio_backend_init(void)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Request event-driven mode. v140: the buffer holds AUDIO_BUFFER_MS
     * (default 100, RPCS3's "audio buffer duration") plus 20 ms headroom, so
     * the mix thread can wait for a late guest block without starving the
     * device (was a fixed 20 ms). */
    REFERENCE_TIME buf_duration = (REFERENCE_TIME)(audio_buffer_ms() + 20) * 10000;
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* Shared mode only takes the device's own mix format unless the stream
     * asks the audio engine to convert: on a device not running 48 kHz stereo
     * float (a 44.1 kHz or 5.1 output) Initialize failed with
     * AUDCLNT_E_UNSUPPORTED_FORMAT and every title ran silent. */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
        buf_duration, 0, &wfx, NULL);

    if (FAILED(hr)) {
        /* Fallback: try without event callback */
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            convert, buf_duration, 0, &wfx, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            CloseHandle(s_wasapi_event);
            s_wasapi_event = NULL;
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);
    { WAVEFORMATEX* mf = NULL;   /* for the startup [audio] line: a 44.1 kHz or
                                  * Bluetooth device means engine resampling */
      if (SUCCEEDED(s_wasapi_client->lpVtbl->GetMixFormat(s_wasapi_client, &mf)) && mf) {
          s_wasapi_dev_rate = mf->nSamplesPerSec; s_wasapi_dev_ch = mf->nChannels; CoTaskMemFree(mf); }
      REFERENCE_TIME def = 0, mn = 0;
      if (SUCCEEDED(s_wasapi_client->lpVtbl->GetDevicePeriod(s_wasapi_client, &def, &mn))) s_wasapi_period_100ns = def; }

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
}

/* PC port: silence the output while the window is in the background
 * (project/r2_pcport.cpp, R2_BACKGROUND). */
volatile int g_port_audio_mute = 0;
/* Set by the port while it parks the game loop (focus pause / quit prompt,
 * sys_ppu_thread.c). No guest blocks arrive then: waiting for them only
 * drained the host buffer into 0.5x stretching ("static") and slipped the
 * clock by seconds. PORT_AUDIO_PAUSE_AWARE=0 restores the old behaviour. */
extern volatile int g_ps3_host_paused;
static int audio_host_paused(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PORT_AUDIO_PAUSE_AWARE"); on = !(e && *e == '0'); }
    return on && g_ps3_host_paused;
}
static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        /* PS3_MUTE=1: silence at the device, nothing else changes -- the
         * title still gets a working cellAudio and the device still paces
         * it, unlike PS3_NO_AUDIO, which sends the title down its no-audio
         * path. For unattended runs. */
        static int mute = -1;
        if (mute < 0) mute = getenv("PS3_MUTE") ? 1 : 0;
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples,
                                               (mute || g_port_audio_mute) ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    }
}

/* Frames the device can take right now; -1 with no device open. */
static int audio_backend_room(void)
{
    if (!s_wasapi_client) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        /* Device gone (AUDCLNT_E_DEVICE_INVALIDATED on a default-device
         * change, sleep, ...): pace on the clock rather than spin. */
        static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
            fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX -- clock pacing%c", (unsigned long)hr, 10);
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

/* Block until the device has consumed some of its buffer. */
static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}

/* Frames queued in the device, not yet played. */
static u32 audio_backend_queued_samples(void)
{
    if (!s_wasapi_client) return 0;
    UINT32 padding = 0;
    if (FAILED(s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding))) return 0;
    return padding;
}


#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use. Reading it as a host float
 * turned the whole mix into denormal noise -- silence after clipping (LBP
 * had a verified end-to-end pipeline with no audible output). */
static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Resistance port: run.bat pipes stderr through Tee-Object, which does not
 * reach logs\run.log until the game exits, so the 5 s audio diagnostics are
 * also appended to logs\audiodiag.txt where they can be read live.
 * Debug only: nothing is printed or written unless R2_DIAG=1. */
void audio_diag_line(const char* line)
{
    if (!ps3_diag_enabled()) return;
    fputs(line, stderr);
#ifdef _WIN32
    { FILE* f = fopen("logs/audiodiag.txt", "a");
      if (f) { fprintf(f, "pid=%lu t=%llu %s", (unsigned long)GetCurrentProcessId(),
                       (unsigned long long)GetTickCount64(), line); fclose(f); } }
#endif
}

static unsigned g_audio_limited = 0;   /* blocks the limiter turned down (5 s report) */
static int g_audio_held_tick = 0;      /* a port waited this tick: no guest notify */

/* Resistance port (v140): late-block tolerance, after RPCS3's buffered audio
 * thread. R2's nextsynth job list is compute-bound on its one SPU lane and
 * lands ~25-40% of port-0 blocks after the mixer has already read them
 * ([audio-land] "landed late"); consuming those as the -0.0 sentinel played
 * a 5.3 ms hole each -- the choppiness. Now:
 *  - the host output holds AUDIO_BUFFER_MS (default 100) of audio, so
 *  - the mix thread can WAIT for a block that is still unwritten, up to
 *    AUDIO_LATE_WAIT_US (now PORT_AUDIO_LATE_WAIT_MS, on by default but only
 *    while blocks are being concealed, its slip absorbed by the output
 *    stretcher -- see the mix thread; v141: default 0 = off; it never caught a block --
 *    late blocks land ~17 ms after the read, not within one block -- and its
 *    clock slip, ~150 ms per 5 s, ate the catch-up so the host buffer sat at
 *    ~16 ms) and never below a 3-block cushion; the wait slips the mix clock
 *    and a ~3% faster clock rebuilds the cushion (RPCS3's dynamic period);
 *  - v141 DELAYED SNAPSHOT (AUDIO_READ_DELAY_MS, default 21 = 4 blocks; 0 =
 *    off): the read index, timestamp and notify still advance on schedule,
 *    and every block is snapshotted at that read and cleared to the sentinel
 *    as before, but the OUTPUT plays block k D blocks later. If k was still
 *    unwritten at its read, the ring is looked at again then: a write that
 *    landed in between (the late data for that read; [audio-land] measured
 *    them ~17 ms after the read, while the next lap's on-time write lands
 *    ~28 ms after it, 14 ms lead of a 43 ms lap) is played in its proper
 *    place ("rescued" in [audio-late]). A rescued block is NOT cleared again
 *    (the producer may be DMAing its next-lap write right then); instead its
 *    checksum is kept, and if the next read finds the very same data it is
 *    treated as unwritten rather than played a lap stale ("duplicate").
 *    [audio-window] reports the measured write-time distributions, to check
 *    that the delay sits between the late and the next-lap writes.
 *  - a block still unwritten after the wait / delay is CONCEALED (AUDIO_LATE_FILL,
 *    default 1): the last good block played ping-pong (reversed, forward,
 *    ...) so the waveform stays continuous, fading 0.75x per block, with a
 *    64-sample crossfade back into real data -- no click, no stale lap data.
 *    The ring block is still consumed and the read index, timestamp and
 *    notify advance exactly as before, so the game's cadence is unchanged.
 *  - AUDIO_STALE_SKIP=1 (default 0): a block concealed (and, with the delay,
 *    not rescued) and written afterwards is ALSO concealed at its next read
 *    instead of played. Off by default: with the delay, such a write landed
 *    more than D after the read, which is when the producer's on-time
 *    next-lap write lands too, so it is more likely fresh than stale;
 *    counted either way in [audio-late].
 * All of it needs AUDIO_SENTINEL (default on). */
#define LT_XFADE 64
#define DL_N     16     /* delay-line slots per port (max delay DL_N-1 blocks) */
static float    s_lt_last[CELL_AUDIO_PORT_MAX][CELL_AUDIO_BLOCK_SAMPLES * 2]; /* last good block, stereo */
static unsigned s_lt_run[CELL_AUDIO_PORT_MAX];      /* consecutive concealed blocks */
static u8       s_lt_seen[CELL_AUDIO_PORT_MAX];     /* port has produced real data */
static u8       s_lt_filled[CELL_AUDIO_PORT_MAX][32]; /* block was concealed at its last read */
static unsigned s_lt_waits, s_lt_arrived, s_lt_fills, s_lt_resumes, s_lt_stale_played, s_lt_stale_skipped;
static unsigned long long s_lt_wait_us_sum; static unsigned s_lt_wait_us_max;
/* v141 delay line: raw (big-endian) snapshots taken at each block's read */
static float    s_dl_data[CELL_AUDIO_PORT_MAX][DL_N][CELL_AUDIO_BLOCK_SAMPLES * 8];
static u8       s_dl_idx[CELL_AUDIO_PORT_MAX][DL_N], s_dl_miss[CELL_AUDIO_PORT_MAX][DL_N];
static unsigned s_dl_pos[CELL_AUDIO_PORT_MAX];
static u8       s_dl_taken[CELL_AUDIO_PORT_MAX][32];  /* ring block holds data rescued late (not cleared) */
static uint32_t s_dl_sum[CELL_AUDIO_PORT_MAX][32];
static unsigned s_dl_rescued, s_dl_dups;

static void audio_lt_reset_port(int p)
{
    s_lt_run[p] = 0; s_lt_seen[p] = 0; s_dl_pos[p] = 0;
    memset(s_lt_filled[p], 0, sizeof(s_lt_filled[p]));
    memset(s_dl_taken[p], 0, sizeof(s_dl_taken[p]));
}

/* FNV-1a over a block's raw words: recognises rescued data seen again. */
static uint32_t lt_block_sum(const void* blk, u32 nw)
{
    const uint32_t* w = (const uint32_t*)blk; uint32_t h = 2166136261u;
    for (u32 i = 0; i < nw; i++) { h ^= w[i]; h *= 16777619u; }
    return h;
}

/* Output delay in blocks (AUDIO_READ_DELAY_MS), clamped so a snapshot is
 * always replayed before its ring block is read again. */
static u32 audio_read_delay_blocks(u32 nblock)
{
    static int d = -1;
    if (d < 0) { const char* e = getenv("AUDIO_READ_DELAY_MS"); int ms = e ? atoi(e) : 32;   /* 6 blocks: heavy scenes write up to ~28 ms late */
                 if (ms < 0) ms = 0; if (ms > 200) ms = 200;
                 d = (ms * (CELL_AUDIO_SAMPLE_RATE / 1000) + CELL_AUDIO_BLOCK_SAMPLES / 2) / CELL_AUDIO_BLOCK_SAMPLES; }
    u32 b = (u32)d;
    if (b + 2 > nblock) b = nblock >= 2 ? nblock - 2 : 0;
    if (b > DL_N - 1) b = DL_N - 1;
    return b;
}

static int audio_sentinel_on(void)
{
    static int sen = -1;
    if (sen < 0) { const char* e = getenv("AUDIO_SENTINEL"); sen = e ? atoi(e) : 1; }
    return sen;
}

/* Sample v of the ping-pong concealment of block `last` (stereo), v counted
 * from the end of the last good block: reversed, forward, reversed, ... so
 * every segment boundary is continuous; gain decays per block. */
static void lt_conceal_sample(const float* last, unsigned v, float* l, float* r)
{
    static float decay = -1.0f;
    if (decay < 0.0f) { const char* e = getenv("AUDIO_FILL_DECAY"); decay = e ? (float)atof(e) : 0.75f;
                        if (decay < 0.0f || decay > 1.0f) decay = 0.75f; }
    const unsigned seg = v / CELL_AUDIO_BLOCK_SAMPLES, pos = v % CELL_AUDIO_BLOCK_SAMPLES;
    if (seg >= 24) { *l = *r = 0.0f; return; }
    const unsigned idx = (seg & 1) ? pos : (CELL_AUDIO_BLOCK_SAMPLES - 1 - pos);
    float g0 = 1.0f; for (unsigned k = 0; k < seg; k++) g0 *= decay;
    const float g = g0 + (g0 * decay - g0) * (float)pos / (float)CELL_AUDIO_BLOCK_SAMPLES;
    *l = last[idx * 2] * g; *r = last[idx * 2 + 1] * g;
}
/* Mix one block from all active ports into s_mix_buffer (stereo float) */
/* Resistance port (v138): which speaker layout the game is mixing for. Every
 * 10 s of port-0 audio: each channel's share of the energy, and "surround"
 * when the centre/LFE/surround channels carry sound, "STEREO" when only L/R
 * do (the broken-positions state, which a launch picks at random). */
extern void port_snd_mode(int surround, double c_pct, double nan_pct);   /* project/stubs.cpp (v144) */
static void audio_mode_monitor(const float* src, u32 nch)
{
    static double e[8]; static u32 blocks; static int last = -1;
    static unsigned long long nans, samples;
    if (nch != 8) return;
    for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s += 4)
        for (u32 c = 0; c < 8; c++) {
            u32 v; memcpy(&v, &src[s * 8 + c], 4);
            v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
            if (v == 0x80000000u) continue;
            float f; memcpy(&f, &v, 4);
            samples++;
            if (f == f) e[c] += (double)f * f; else nans++;
        }
    if (++blocks < 1875u) return;   /* 10 s */
    double t = 0; for (int c = 0; c < 8; c++) t += e[c];
    if (t > 1e-6) {
        const double sur = e[2] + e[3] + e[4] + e[5] + e[6] + e[7];
        const int mode = sur > t * 0.01 ? 1 : 0;
        if (ps3_diag_enabled())
        fprintf(stderr, "[audio-mode] %s | L %.0f%% R %.0f%% C %.0f%% LFE %.0f%% Ls %.0f%% Rs %.0f%% Lb %.0f%% Rb %.0f%%%s%c",
                mode ? "surround" : "STEREO",
                100*e[0]/t, 100*e[1]/t, 100*e[2]/t, 100*e[3]/t, 100*e[4]/t, 100*e[5]/t, 100*e[6]/t, 100*e[7]/t,
                (last >= 0 && last != mode) ? "  <-- changed" : "", 10);
        last = mode;
#ifdef _WIN32
        if (ps3_diag_enabled())
        { FILE* mf = fopen("logs/sndmode.txt", "a");
          if (mf) { fprintf(mf, "pid=%lu t=%llu %s C=%.0f%% LFE=%.0f%% NaN=%llu\n", (unsigned long)GetCurrentProcessId(),
                            (unsigned long long)GetTickCount64(),
                            mode ? "surround" : "STEREO", 100*e[2]/t, 100*e[3]/t, nans); fclose(mf); } }
#endif
        port_snd_mode(mode, 100*e[2]/t, samples ? 100.0 * (double)nans / (double)samples : 0.0);
    }
    for (int c = 0; c < 8; c++) e[c] = 0;
    blocks = 0; nans = 0; samples = 0;
}
/* Resistance port (v132): F9 audio recording (see the mix loop). */
volatile long g_audio_rec_blocks = 0;   /* blocks still to record; set by the F9 key */
volatile long g_audio_rec_seq = 0;      /* bumped per request: a new file */
static void audio_rec_block(const float* src, u32 nch)
{
    static FILE* f = NULL; static long seq = 0; static u32 frames = 0, chs = 0;
    if (g_audio_rec_blocks <= 0) {
        if (f) goto finish;
        return;
    }
    if (!f || seq != g_audio_rec_seq) {
        if (f) { /* a new request while one runs: just keep going in this file */ }
        else {
            char fn[64]; seq = g_audio_rec_seq;
            snprintf(fn, sizeof fn, "logs/audiorec_%ld.wav", seq);
            f = fopen(fn, "wb");
            if (!f) { g_audio_rec_blocks = 0; return; }
            chs = nch; frames = 0;
            unsigned char h[44] = {0};
            fwrite(h, 1, 44, f);             /* header patched at the end */
            fprintf(stderr, "[audiorec] recording %s (%u channels)%c", fn, nch, 10);
        }
    }
    if (nch == chs) {
        const uint32_t* w = (const uint32_t*)src;
        uint32_t tmp[CELL_AUDIO_BLOCK_SAMPLES * 8];
        const u32 n = CELL_AUDIO_BLOCK_SAMPLES * nch;
        for (u32 k = 0; k < n && k < CELL_AUDIO_BLOCK_SAMPLES * 8; k++) {
            uint32_t v = w[k];
            v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
            if (v == 0x80000000u) v = 0;     /* the consumed-block sentinel (-0.0) */
            tmp[k] = v;
        }
        fwrite(tmp, 4, n, f);
        frames += CELL_AUDIO_BLOCK_SAMPLES;
    }
    if (InterlockedDecrement(&g_audio_rec_blocks) > 0) return;
finish:
    { const uint32_t data = frames * chs * 4, rate = 48000, br = rate * chs * 4;
      unsigned char h[44];
      memcpy(h, "RIFF", 4); uint32_t v = 36 + data; memcpy(h + 4, &v, 4);
      memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
      uint16_t s16 = 3; memcpy(h + 20, &s16, 2);            /* IEEE float */
      s16 = (uint16_t)chs; memcpy(h + 22, &s16, 2);
      memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4);
      s16 = (uint16_t)(chs * 4); memcpy(h + 32, &s16, 2);
      s16 = 32; memcpy(h + 34, &s16, 2);
      memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
      fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fclose(f); f = NULL;
      fprintf(stderr, "[audiorec] done: %u frames (%.1f s)%c", frames, frames / 48000.0, 10); }
}

static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;

        /* AUDIO_RINGLOG=1 (with AUDIO_SENTINEL): for 300 ticks out of every
         * 3000, port 0's whole ring at read time -- '#' written since we last
         * consumed it, '.' still the sentinel, '>' marks the block read now --
         * to see where and when the producer writes. */
#ifdef _WIN32
        { static int rl = -1;
          if (rl < 0) { const char* e = getenv("AUDIO_RINGLOG"); rl = e ? atoi(e) : 0; }
          static unsigned tick;
          if (rl && p == 0 && (tick++ % 3000u) < 300u) {
              char map[40]; u32 b;
              for (b = 0; b < nblock && b < 32; b++) {
                  const uint32_t* w = (const uint32_t*)(port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch);
                  u32 nn = 0;
                  for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i += 61) if (w[i] != 0x00000080u) { nn = 1; break; }
                  map[b] = (b == block_idx) ? (nn ? 'R' : 'r') : (nn ? '#' : '.');
              }
              map[b] = 0;
              LARGE_INTEGER qc, qf2; QueryPerformanceCounter(&qc); QueryPerformanceFrequency(&qf2);
              fprintf(stderr, "[ring] t=%.2fms ridx=%llu [%s]%c",
                      (double)qc.QuadPart * 1000.0 / (double)qf2.QuadPart,
                      (unsigned long long)port->read_index, map, 10);
          } }
#endif
        /* AUDIO_PORTWAV=<file>: port 0's raw block as the game wrote it (nch
         * channels, big-endian float), to tell a broken producer from a broken
         * fold. ffmpeg -f f32be -ar 48000 -ac 8 -i <file> */
        { static FILE* _pf = (FILE*)-1;
          if (_pf == (FILE*)-1) { const char* e = getenv("AUDIO_PORTWAV"); _pf = e ? fopen(e, "wb") : NULL; }
          if (_pf && p == 0) fwrite(src, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * nch, _pf); }
        /* Resistance port (v132): F9 in game = record the next 20 s of port 0
         * (the game's 8-channel mix, before the stereo fold) to
         * logs/audiorec_N.wav, 48 kHz 32-bit float, 8 channels
         * (L R C LFE Ls Rs Lb Rb). For diagnosing wrong 3D sound positions:
         * which output channels a sound lands in. */
        if (p == 0) { audio_rec_block(src, nch); audio_mode_monitor(src, nch); }
        /* v123: AUDIO_PORTWAV1=<file> -- the same for port 1 (libmixer: movies). */
        { static FILE* _pf1 = (FILE*)-1;
          if (_pf1 == (FILE*)-1) { const char* e = getenv("AUDIO_PORTWAV1"); _pf1 = e ? fopen(e, "wb") : NULL; }
          if (_pf1 && p == 1) fwrite(src, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * nch, _pf1); }
        /* Resistance port: underrun hold. A block that is still all zero right
         * after a block with sound is almost always one the game's producer
         * has not written yet (it runs late in a heavy frame) -- consuming it
         * played a 5 ms hole, the crackle. Instead, replay the last block
         * softer and DON'T advance the read index: the producer gets one more
         * block of lead, and the underrun turns into a 5 ms time stretch. At
         * most 2 holds in a row (genuine silence passes after 10 ms).
         * AUDIO_HOLD=0 = old behaviour. Gaps/holds logged every 5 s. */
        { static int hold_on = -1;
          if (hold_on < 0) { const char* e = getenv("AUDIO_HOLD"); hold_on = e ? atoi(e) : 0; }   /* v115: off -- it made the crackle worse and turned movie audio robotic */
          static unsigned char hist[CELL_AUDIO_PORT_MAX], run[CELL_AUDIO_PORT_MAX];
          static float last[CELL_AUDIO_PORT_MAX][CELL_AUDIO_BLOCK_SAMPLES * 8];
          static unsigned gaps, holds, blocks;
          /* raw big-endian words: +0.0 is 0, the -0.0 sentinel reads 0x00000080
           * on this little-endian host (as a host float it is a denormal, not 0) */
          int nz = 0;
          { const uint32_t* w = (const uint32_t*)src;
            for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 3)
                if (w[s2] != 0u && w[s2] != 0x00000080u) { nz = 1; break; } }
          blocks++;
          /* v120 wait-hold: with AUDIO_SENTINEL (consumed blocks are -0.0), a
           * block still all sentinel was NEVER WRITTEN -- the producer is late,
           * not silent (libmixer writes its blocks in bursts that land 1-2
           * blocks behind the read position in heavy scenes; consumed, those
           * late writes replayed 43 ms stale = the crackle). Wait for it: play
           * the last block softly, do not consume, do not notify the guest
           * (it writes one block per notify). At most 3 ticks in a row.
           * AUDIO_WAITHOLD=0 off. */
          static int wait_on = -1;
          if (wait_on < 0) { const char* e = getenv("AUDIO_WAITHOLD"); wait_on = e ? atoi(e) : 0; }   /* v121: off -- heavy scenes only get 60-80% of the audio, holding made it unrecognisable */
          int unwritten = 0;
          if (wait_on && !nz) {
              const uint32_t* w = (const uint32_t*)src; unwritten = 1;
              for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i += 13) if (w[i] != 0x00000080u) { unwritten = 0; break; }
          }
          const int hold = ((hold_on && !nz && (hist[p] & 1) && run[p] < 2) ||
                            (unwritten && run[p] < 3)) && nch <= 8;
          if (hold) g_audio_held_tick = 1;
          if (hold) {
              holds++; run[p]++;
              const float* lp = last[p];
              for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
                  float l = 0.0f, r = 0.0f;
                  if (nch >= 2) { l = ld_be_f32(&lp[s * nch]); r = ld_be_f32(&lp[s * nch + 1]); }
                  else l = r = ld_be_f32(&lp[s]);
                  if (nch == 8) {
                      const float c = ld_be_f32(&lp[s * 8 + 2]) * 0.707f + ld_be_f32(&lp[s * 8 + 3]) * 0.5f;
                      l += c + ld_be_f32(&lp[s * 8 + 4]) * 0.5f + ld_be_f32(&lp[s * 8 + 6]) * 0.3f;
                      r += c + ld_be_f32(&lp[s * 8 + 5]) * 0.5f + ld_be_f32(&lp[s * 8 + 7]) * 0.3f;
                  }
                  s_mix_buffer[s * 2 + 0] += l * level * 0.6f;
                  s_mix_buffer[s * 2 + 1] += r * level * 0.6f;
              }
          } else {
              if (nz && (hist[p] & 1) == 0 && (hist[p] & 0x6)) gaps++;
              run[p] = 0;
              hist[p] = (unsigned char)((hist[p] << 1) | (nz ? 1 : 0));
              if (nz && nch <= 8) memcpy(last[p], src, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));
          }
#ifdef _WIN32
          { static ULONGLONG t0; ULONGLONG now = GetTickCount64(); if (!t0) t0 = now;
            if (now - t0 >= 5000) {
                if ((gaps || holds || g_audio_limited) && ps3_diag_enabled())
                    fprintf(stderr, "[audio] 5 s: %u underrun gap(s), %u hold(s), %u limited, %u blocks%c",
                            gaps, holds, g_audio_limited, blocks, 10);
                gaps = holds = blocks = 0; g_audio_limited = 0; t0 = now;
            } }
#endif
          if (hold) continue;   /* block not consumed: no clear, no read-index advance */
        }

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            } } }

        /* Resistance port (v124): the movie port's LFE is left out of the stereo
         * fold. v123 dumps: libmixer's port-1 output is the decoded AC3 x 0.42,
         * sample-exact on L/R/C/Ls/Rs, but its LFE channel carries the LFE at
         * 12x for the first 43 samples of EVERY 256-sample block -- a
         * 187.5 Hz rasp under every bass line (the movie "static"). The bug is
         * in the game's lifted channel-strip code; dropping LFE is also what a
         * standard Dolby stereo downmix does. AUDIO_P1_LFE=<gain> (0.5 = old). */
        float lfe_gain = 0.5f;
        if (p == 1) { static float g1 = -1.0f;
                      if (g1 < 0.0f) { const char* e = getenv("AUDIO_P1_LFE"); g1 = e ? (float)atof(e) : 0.0f; }
                      lfe_gain = g1; }
        /* v140/v141 late-block tolerance (see s_lt_last): classify the block
         * at its read, snapshot it into the delay line, and pick the block
         * this tick OUTPUTS (D blocks older with the delayed snapshot). */
        static int lt_fill_on = -1, lt_stale_skip = -1;
        if (lt_fill_on < 0) { const char* e = getenv("AUDIO_LATE_FILL"); lt_fill_on = e ? atoi(e) : 1;
                              e = getenv("AUDIO_STALE_SKIP"); lt_stale_skip = e ? atoi(e) : 0; }
        const u32 nwords = CELL_AUDIO_BLOCK_SAMPLES * nch;
        const float* out = src;   /* raw big-endian block to fold; NULL = silence */
        int out_miss = 0;         /* nothing real to play: conceal */
#ifdef _WIN32
        if (audio_sentinel_on() && nch <= 8 && nblock <= 32) {
            const uint32_t* w = (const uint32_t*)src;
            int miss = 1;
            for (u32 i = 0; i < nwords; i++) if (w[i] != 0x00000080u) { miss = 0; break; }
            const int never_written = miss;   /* only this arms the next lap's stale check */
            const int was_filled = s_lt_filled[p][block_idx];
            s_lt_filled[p][block_idx] = 0;
            if (!miss && s_dl_taken[p][block_idx] &&
                lt_block_sum(w, nwords) == s_dl_sum[p][block_idx]) {
                miss = 1; s_dl_dups++;          /* the rescued data again: already played */
            } else if (!miss && was_filled) {
                if (lt_stale_skip) { miss = 1; s_lt_stale_skipped++; } else s_lt_stale_played++;
            }
            s_dl_taken[p][block_idx] = 0;
            const u32 dly = audio_read_delay_blocks(nblock);
            if (!dly) {
                if (miss) { out_miss = 1; s_lt_filled[p][block_idx] = (u8)never_written; }
            } else {
                const u32 slot = s_dl_pos[p] % DL_N;
                s_dl_idx[p][slot] = (u8)block_idx;
                s_dl_miss[p][slot] = (u8)(miss ? (never_written ? 1 : 2) : 0);   /* 2 = discarded at read */
                if (!miss) memcpy(s_dl_data[p][slot], src, nwords * sizeof(float));
                s_dl_pos[p]++;
                if (s_dl_pos[p] <= dly) out = NULL;          /* delay line still filling */
                else {
                    const u32 os = (s_dl_pos[p] - 1 - dly) % DL_N;
                    const u32 j = s_dl_idx[p][os];
                    out = s_dl_data[p][os];
                    if (s_dl_miss[p][os]) {
                        /* unwritten at its read: has the late write landed
                         * since? (first, middle and last word: not a block
                         * whose DMA is still in flight) */
                        const uint32_t* rw = (const uint32_t*)(port->buffer + j * nwords);
                        if (rw[0] != 0x00000080u && rw[nwords / 2] != 0x00000080u && rw[nwords - 1] != 0x00000080u) {
                            memcpy(s_dl_data[p][os], rw, nwords * sizeof(float));
                            s_dl_taken[p][j] = 1;
                            s_dl_sum[p][j] = lt_block_sum(s_dl_data[p][os], nwords);
                            s_dl_rescued++; s_ah.rescued++;
                        } else { out_miss = 1; s_lt_filled[p][j] = (u8)(s_dl_miss[p][os] == 1); }
                    }
                }
            }
        }
#endif
        float pst[CELL_AUDIO_BLOCK_SAMPLES * 2];
        if (!out || out_miss) memset(pst, 0, sizeof(pst));
        else for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = ld_be_f32(&out[s * nch + 0]) * level;
                right = ld_be_f32(&out[s * nch + 1]) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = ld_be_f32(&out[s]) * level;
            }

            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                float center = ld_be_f32(&out[s * 8 + 2]) * level * 0.707f;
                float lfe    = ld_be_f32(&out[s * 8 + 3]) * level * lfe_gain;
                float rl     = ld_be_f32(&out[s * 8 + 4]) * level * 0.5f;
                float rr     = ld_be_f32(&out[s * 8 + 5]) * level * 0.5f;
                float sl     = ld_be_f32(&out[s * 8 + 6]) * level * 0.3f;
                float sr     = ld_be_f32(&out[s * 8 + 7]) * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
            }

            pst[s * 2 + 0] = left;
            pst[s * 2 + 1] = right;
        }
        if (out_miss && lt_fill_on && s_lt_seen[p] && !audio_host_paused()) {
            const unsigned v0 = s_lt_run[p] * CELL_AUDIO_BLOCK_SAMPLES;
            for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++)
                lt_conceal_sample(s_lt_last[p], v0 + s, &pst[s * 2], &pst[s * 2 + 1]);
            if (s_lt_run[p] < 1000) s_lt_run[p]++;
            s_lt_fills++;
            if (s_lt_run[p] <= 24) s_ah.concealed++;   /* not a port left idle (unwritten) */
        } else if (out && !out_miss) {
            /* real data: crossfade out of a concealment, then remember it */
            if (s_lt_run[p] && s_lt_seen[p]) {
                const unsigned v0 = s_lt_run[p] * CELL_AUDIO_BLOCK_SAMPLES;
                for (u32 s = 0; s < LT_XFADE; s++) {
                    float cl, cr; lt_conceal_sample(s_lt_last[p], v0 + s, &cl, &cr);
                    const float t = (float)(s + 1) / (float)(LT_XFADE + 1);
                    pst[s * 2]     = pst[s * 2] * t + cl * (1.0f - t);
                    pst[s * 2 + 1] = pst[s * 2 + 1] * t + cr * (1.0f - t);
                }
                s_lt_resumes++;
            }
            s_lt_run[p] = 0;
            memcpy(s_lt_last[p], pst, sizeof(pst)); s_lt_seen[p] = 1;
        }
        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES * 2; s++) s_mix_buffer[s] += pst[s];

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        /* AUDIO_SENTINEL=1 (diagnostic): clear consumed blocks to -0.0 instead
         * of +0.0 (equal as floats, inaudible), then classify every block at
         * read time: still all -0.0 = the producer never wrote it (late);
         * all +0.0 = the producer wrote silence. Reported every 5 s. */
#ifdef _WIN32
        { static int sen = -1;
          if (sen < 0) { const char* e = getenv("AUDIO_SENTINEL"); sen = e ? atoi(e) : 1; }   /* v120: on (wait-hold needs it) */
          if (sen) {
              static unsigned unwritten, silent, total; static ULONGLONG t0;
              const uint32_t* w = (const uint32_t*)src;
              const u32 nw = CELL_AUDIO_BLOCK_SAMPLES * nch;
              u32 neg = 0, pos = 0;
              for (u32 i = 0; i < nw; i++) { if (w[i] == 0x00000080u) neg++; else if (w[i] == 0) pos++; }
              if (neg == nw) unwritten++; else if (pos == nw) silent++;
              total++;
              /* [audio-land] (port 0): WHEN the producer's writes land, from a
               * per-tick scan of the ring for blocks leaving the sentinel.
               * on-time = written before its read (lead = read - write time);
               * late = the read found it unwritten and the write landed
               * afterwards (the job finished too late -- the data then plays
               * one ring lap, 43 ms, stale); lost = still unwritten at the
               * next read too (the producer skipped the block). */
              if (p == 0 && nblock <= 32) {
                  static u8 st[32];            /* 0 waiting, 1 written, 2 missed at read, 3 written late */
                  static LONGLONG tw[32], tr[32], tc[32];
                  static unsigned ontime, late, lost, stale; static LONGLONG lead_sum, lead_min = -1, late_sum;
                  static ULONGLONG lt0;
                  /* v141 [audio-window]: when writes land after the block's
                   * last read, in 4 ms bins (last = 40+): late = the read
                   * missed it, next = next-lap write of a block read on time.
                   * AUDIO_READ_DELAY_MS should sit after most "late" and
                   * before most "next" writes. */
                  static unsigned hl[11], hn[11];
                  LARGE_INTEGER qn, qf; QueryPerformanceCounter(&qn); QueryPerformanceFrequency(&qf);
                  const LONGLONG now_us = qn.QuadPart * 1000000 / qf.QuadPart;
                  for (u32 b = 0; b < nblock; b++) {
                      if (st[b] == 1 || st[b] == 3) continue;
                      const uint32_t* bw = (const uint32_t*)(port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch);
                      if (bw[0] == 0x00000080u && bw[nw / 2] == 0x00000080u && bw[nw - 1] == 0x00000080u) continue;
                      if (tc[b]) { LONGLONG bin = (now_us - tc[b]) / 4000; if (bin > 10) bin = 10; if (bin < 0) bin = 0;
                                   if (st[b] == 2) hl[bin]++; else hn[bin]++; }
                      if (st[b] == 2) { late++; late_sum += now_us - tr[b]; st[b] = 3; }
                      else st[b] = 1;
                      tw[b] = now_us;
                  }
                  tc[block_idx] = now_us;
                  if (neg == nw) {
                      if (st[block_idx] == 2) lost++;
                      st[block_idx] = 2; tr[block_idx] = now_us;
                  } else {
                      if (st[block_idx] == 1) {
                          const LONGLONG l = now_us - tw[block_idx];
                          ontime++; lead_sum += l; if (lead_min < 0 || l < lead_min) lead_min = l;
                      } else if (st[block_idx] == 3) stale++;
                      st[block_idx] = 0;
                  }
                  const ULONGLONG lnow = GetTickCount64(); if (!lt0) lt0 = lnow;
                  if (lnow - lt0 >= 5000) {
                      char ln[256];
                      snprintf(ln, sizeof ln, "[audio-land] 5 s: %u on time (lead avg %.1f ms, min %.1f ms), "
                               "%u landed late (avg %.1f ms after the read; %u played a lap stale), %u lost%c",
                               ontime, ontime ? lead_sum / 1000.0 / ontime : 0.0,
                               lead_min < 0 ? 0.0 : lead_min / 1000.0,
                               late, late ? late_sum / 1000.0 / late : 0.0, stale, lost, 10);
                      audio_diag_line(ln);
                      { char wl[320]; int o = snprintf(wl, sizeof wl, "[audio-window] 5 s, write time after the last read, 4 ms bins 0..40+: late");
                        for (int i = 0; i < 11 && o > 0 && o < (int)sizeof wl; i++) o += snprintf(wl + o, sizeof wl - (size_t)o, " %u", hl[i]);
                        if (o > 0 && o < (int)sizeof wl) o += snprintf(wl + o, sizeof wl - (size_t)o, " | next");
                        for (int i = 0; i < 11 && o > 0 && o < (int)sizeof wl; i++) o += snprintf(wl + o, sizeof wl - (size_t)o, " %u", hn[i]);
                        if (o > 0 && o < (int)sizeof wl - 2) { wl[o] = 10; wl[o + 1] = 0; }
                        audio_diag_line(wl);
                        memset(hl, 0, sizeof hl); memset(hn, 0, sizeof hn); }
                      ontime = late = lost = stale = 0; lead_sum = late_sum = 0; lead_min = -1; lt0 = lnow;
                  }
              }
              ULONGLONG now = GetTickCount64(); if (!t0) t0 = now;
              if (now - t0 >= 5000) {
                  char ln[160];
                  snprintf(ln, sizeof ln, "[audio-sentinel] 5 s: %u blocks never written, %u written as silence, %u total%c",
                           unwritten, silent, total, 10);
                  audio_diag_line(ln);
                  unwritten = silent = total = 0; t0 = now;
              }
              uint32_t* ww = (uint32_t*)src;
              for (u32 i = 0; i < nw; i++) ww[i] = 0x00000080u;   /* -0.0f, big-endian */
          } else
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float)); }
#else
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));
#endif

        /* Keep the monotonic counter for tags/timestamps, but publish the
         * ring slot to the guest. SurMixer multiplies this value by the block
         * size directly; an unbounded counter walks beyond the audio buffer. */
        port->read_index++;
        { extern uint64_t ps3_guest_system_time_us(void); port->ts_us = ps3_guest_system_time_us(); }
        /* Resistance port: AUDIO_LEAD (default 3 blocks, 16 ms): the index the
         * guest sees runs that many blocks AHEAD of the block this mixer reads.
         * The game's libmixer writes the block AT the published index -- the
         * one consumed 5.3 ms later -- so any wake-up later than one block left
         * that block unwritten (AUDIO_SENTINEL: up to 27% of blocks "never
         * written" in heavy scenes). The lead gives it 3 blocks of slack, the
         * cost is 16 ms more latency. 0 = old behaviour. */
        if (port->read_idx_addr) {
            static int lead = -1;
            if (lead < 0) { const char* e = getenv("AUDIO_LEAD"); lead = e ? atoi(e) : 0;   /* v119b: 3 made it worse */
                            if (lead < 0) lead = 0; }
            u32 l = (u32)lead; if (l > nblock - 2) l = nblock > 2 ? nblock - 2 : 0;
            vm_write64((u32)port->read_idx_addr, (port->read_index + l) % nblock);
        }
    }

    mutex_unlock(&s_audio_mutex);

    /* Resistance port: limiter instead of a bare clip. The 7.1 -> stereo fold
     * above adds centre, LFE and four surrounds into each side (gains summing
     * to ~3), so loud passages -- explosions, a movie's bass line -- went far
     * past full scale and the hard clip below turned them into crackle. A
     * block whose peak would clip has its gain pulled down to fit (ramped over
     * the first 64 samples, the block is known in full before output), then
     * recovers over ~0.3 s. AUDIO_LIMIT=0 = hard clip only. Peak reductions
     * are counted in the 5 s [audio] line. */
    { static int lim = -1; static float g = 1.0f;
      if (lim < 0) { const char* e = getenv("AUDIO_LIMIT"); lim = e ? atoi(e) : 1; }
      if (lim) {
          float pk = 0.0f;
          for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
              const float a2 = s_mix_buffer[i] < 0 ? -s_mix_buffer[i] : s_mix_buffer[i];
              if (a2 > pk) pk = a2;
          }
          float target = (pk * g > 0.97f) ? 0.97f / pk : g;
          if (target >= g) target = g + (1.0f - g) * 0.02f;     /* release */
          else g_audio_limited++;
          if (target > 1.0f) target = 1.0f;
          for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES; i++) {
              const float t = i < 64 ? (float)i / 64.0f : 1.0f;
              const float gg = target < g ? target : g + (target - g) * t;   /* down: at once */
              s_mix_buffer[i * 2]     *= gg;
              s_mix_buffer[i * 2 + 1] *= gg;
          }
          g = target;
      } }
    /* Clip to [-1.0, 1.0] */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
/* v140: is any running port's next block still the -0.0 sentinel (the
 * producer is late, see s_lt_last)? Only ports that have produced data and
 * are not idle (fewer than 4 blocks concealed in a row) count. The mix
 * thread polls this between short timer waits (RPCS3's "untouched" wait):
 * the guest clock slips, the producer gets the time, and the output
 * stretcher absorbs the slip instead of the device playing a hole. */
static int audio_pending_ports(void)
{
    if (!audio_sentinel_on()) return 0;
    int np = 0;
    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer) continue;
        if (!s_lt_seen[p] || s_lt_run[p] >= 4) continue;
        const u32 nch = (u32)port->param.nChannel, nblock = (u32)port->param.nBlock;
        if (nch > 8 || nblock == 0 || nblock > 32) continue;
        const u32 nw = CELL_AUDIO_BLOCK_SAMPLES * nch;
        const uint32_t* w = (const uint32_t*)(port->buffer + (u32)(port->read_index % nblock) * nw);
        int miss = 1;
        for (u32 i = 0; i < nw; i += 5) if (w[i] != 0x00000080u) { miss = 0; break; }
        np += miss;
    }
    mutex_unlock(&s_audio_mutex);
    return np;
}
#endif

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid)
            sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                       0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
    }
}

#ifdef _WIN32
/* Keep the mixer off the cores the affinity keeper (project/debugtools.inc)
 * reserves for the main guest thread (PORT_MAIN_CPU, default 2), the RSX
 * FIFO / rsx::thread (PORT_FIFO_CPU, default 4) and rawSPU0 (PORT_SPU0_CPU,
 * default 6): all three are ~100% busy, and a mixer sharing their core waits
 * for its time slice even with MMCSS. Same conditions as the keeper (>= 12
 * logical CPUs or PORT_MAIN_CPU set, PORT_MAIN_CORE=0 off), so with the
 * keeper on this is the mask it gives every other thread anyway -- set here
 * from the start instead of up to 5 s later. PORT_AUDIO_CPU=<n> pins to
 * logical CPU n instead, -1 leaves the affinity alone. `def_on` = the
 * default applies (the SoundTouch output stage is on). */
static void audio_thread_affinity(int def_on, char* desc, size_t dn)
{
    snprintf(desc, dn, "any CPU");
    const char* e = getenv("PORT_AUDIO_CPU");
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc) return;
    if (e && *e) {
        const int c = atoi(e);
        if (c < 0 || c >= 64 || !(proc >> c & 1)) return;
        if (SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << c)) snprintf(desc, dn, "CPU %d (PORT_AUDIO_CPU)", c);
        return;
    }
    if (!def_on) return;
    { const char* k = getenv("PORT_MAIN_CORE"); if (k && *k && atoi(k) == 0) return; }
    const char* me = getenv("PORT_MAIN_CPU");
    int n = 0; for (DWORD_PTR m = proc; m; m &= m - 1) n++;
    if (n < 12 && !me) return;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION buf[256];
    DWORD len = sizeof buf;
    const int topo = GetLogicalProcessorInformation(buf, &len) != 0;
    DWORD_PTR mask = proc;
    const char* names[3] = { "PORT_MAIN_CPU", "PORT_FIFO_CPU", "PORT_SPU0_CPU" };
    const int defs[3] = { 2, 4, n >= 12 ? 6 : -1 };
    for (int i = 0; i < 3; i++) {
        const char* v = getenv(names[i]);
        const int c = (v && *v) ? atoi(v) : defs[i];
        if (c < 0 || c >= 64) continue;
        DWORD_PTR core = ((DWORD_PTR)1 << c) | ((DWORD_PTR)1 << (c ^ 1));
        if (topo)
            for (DWORD j = 0; j < len / sizeof buf[0]; j++)
                if (buf[j].Relationship == RelationProcessorCore && (buf[j].ProcessorMask >> c & 1)) { core = buf[j].ProcessorMask; break; }
        if ((mask & ~core) != 0) mask &= ~core;
    }
    if (mask != proc && SetThreadAffinityMask(GetCurrentThread(), mask))
        snprintf(desc, dn, "mask 0x%llX (off the main/FIFO/SPU0 cores)", (unsigned long long)mask);
}

typedef HANDLE (WINAPI* audio_avset_fn)(LPCWSTR, LPDWORD);
typedef BOOL   (WINAPI* audio_avrevert_fn)(HANDLE);
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"cellAudio mixer");
#endif

    /* This thread is the audio clock. It sleeps on a timer between blocks,
     * so a top priority costs nothing -- at normal priority a busy frame (9
     * polling SPU lanes, a spinning FIFO thread) woke it late. Set here, by
     * the thread itself, before MMCSS: a SetThreadPriority from the creating
     * thread could land after the MMCSS registration and undo it.
     * AUDIO_MIX_PRIO=0 off. Then MMCSS "Pro Audio" (what WASAPI clients and
     * RPCS3's cubeb/XAudio2 backends get): the real-time priority band, above
     * every TIME_CRITICAL game/SPU thread and other processes' threads, so a
     * CPU-bound host cannot delay the mix. PORT_AUDIO_MMCSS=0 off. */
    const char* prio_s = "normal";
    { const char* e = getenv("AUDIO_MIX_PRIO");
      if (!e || atoi(e)) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); prio_s = "TIME_CRITICAL"; } }
    HANDLE mmcss = NULL; audio_avrevert_fn av_revert = NULL; DWORD mm_err = 0; int mm_want;
    { const char* e = getenv("PORT_AUDIO_MMCSS"); mm_want = !(e && *e && atoi(e) == 0); }
    if (mm_want) {
        HMODULE av = LoadLibraryW(L"avrt.dll");
        audio_avset_fn av_set = av ? (audio_avset_fn)(void*)GetProcAddress(av, "AvSetMmThreadCharacteristicsW") : NULL;
        av_revert = av ? (audio_avrevert_fn)(void*)GetProcAddress(av, "AvRevertMmThreadCharacteristics") : NULL;
        DWORD idx = 0;
        if (av_set) mmcss = av_set(L"Pro Audio", &idx);
        if (!mmcss) mm_err = av_set ? GetLastError() : (DWORD)-1;
    }

    /* Pace to the device clock. The PS3 consumes one block every 5.33 ms;
     * the old Sleep(2)/Sleep(5) loop keyed on a queue depth the 1056-frame
     * WASAPI buffer can never reach, so it mixed ~225 blocks/s and the device
     * dropped the surplus: 20% of the audio thrown away, and a guest mixer
     * (GH3's FMOD) that could not keep ahead of the read index played gaps.
     * With no device, pace on the performance counter instead. */
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    unsigned long long blocks = 0;
    /* Resistance port (v115): mix on a steady 5.33 ms clock, like the PS3,
     * and feed the device from a small FIFO. Pacing on device room mixed in
     * bursts -- the WASAPI event fires every ~10 ms, and each wake took two
     * or more blocks back to back -- so the guest's read index jumped ahead
     * of a producer that keeps only a block or two of lead: "underrun gaps"
     * even at full frame rate, more under load. AUDIO_STEADY=0 = old pacing. */
    static int steady = -1;
    if (steady < 0) { const char* e = getenv("AUDIO_STEADY"); steady = e ? atoi(e) : 1; }
    enum { FIFO_N = 128 };   /* v140: was 24; must cover AUDIO_BUFFER_MS when the device buffer is small */
    static float fifo[FIFO_N][CELL_AUDIO_BLOCK_SAMPLES * 2];
    unsigned fhead = 0, fcount = 0;
    HANDLE tmr = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!tmr) tmr = CreateWaitableTimerW(NULL, FALSE, NULL);
    /* v140 (RPCS3-style buffering): the next block's due time on the QPC
     * clock. A late-block wait slips it (time stretch of the guest cadence);
     * while the host buffer is below AUDIO_BUFFER_MS the period runs
     * AUDIO_CATCHUP_PCT (default 3) % short to rebuild it, and as much long
     * when it is far above (device clock drift). */
    const long long period_q = (long long)CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE;
    long long due_q = q0.QuadPart;
    const unsigned target_frames = audio_buffer_ms() * CELL_AUDIO_SAMPLE_RATE / 1000;
    static int late_us = -1, catchup = -1;
    if (late_us < 0) { const char* e = getenv("AUDIO_LATE_WAIT_US"); late_us = e ? atoi(e) : 0;   /* v141: off, see s_lt_last */
                       if (late_us < 0) late_us = 0;
                       e = getenv("AUDIO_CATCHUP_PCT"); catchup = e ? atoi(e) : 3;
                       if (catchup < 0) catchup = 0; if (catchup > 20) catchup = 20; }
    int primed = 0;
    unsigned lt_q_min = ~0u; unsigned long long lt_q_sum = 0, lt_slip_us = 0; unsigned lt_q_n = 0, lt_reprimes = 0, lt_fast = 0;
    ULONGLONG lt_t0 = GetTickCount64();
    /* RPCS3-style robustness for a CPU-bound host (see audio_stretch.h):
     *  - PORT_AUDIO_STRETCH (default 1): between the FIFO and the device, a
     *    pitch-preserving WSOLA stretcher plays slower (down to
     *    PORT_AUDIO_STRETCH_MIN, default 0.70) while the host buffer is below
     *    75% of its target, instead of the device running dry. At tempo 1 it
     *    is an exact passthrough.
     *  - PORT_AUDIO_LATE_WAIT_MS (default 22 with the stretcher, else the old
     *    AUDIO_LATE_WAIT_US, else 0): RPCS3's "untouched buffer" wait. While
     *    guest blocks are being concealed (the producer cannot keep up),
     *    a block still unwritten at its read is waited for, the guest clock
     *    slips (fewer blocks asked of the producer per second) and the
     *    stretcher absorbs the slip. Paused for 10 s when it stops catching
     *    blocks. */
    static AudioStretch s_as;
    as_init(&s_as);
    /* PORT_AUDIO_STRETCH (default 1): the SoundTouch output stage
     * (audio_out.h) -- a ring buffer of PORT_AUDIO_BUFFER_MS fed to the
     * device through SoundTouch, slowed (pitch kept) while the buffer is
     * below PORT_AUDIO_STRETCH_THRESHOLD % of it, faded to silence if it
     * still runs dry. PORT_AUDIO_STRETCH=0: the previous output path, as
     * shipped before (FIFO + the WSOLA stretcher above, PORT_AUDIO_WSOLA=0
     * turns that off too). */
    AudioOut* ao = NULL;
    int ao_want;
    { const char* e = getenv("PORT_AUDIO_STRETCH"); ao_want = !(e && *e && atoi(e) == 0); }
    char aff[96];
    audio_thread_affinity(ao_want, aff, sizeof aff);
    if (ao_want && steady && audio_backend_room() >= 0) {
        AudioOutConfig cfg;
        audio_out_default_config(&cfg, CELL_AUDIO_SAMPLE_RATE, audio_buffer_ms());
        unsigned period = 0;
#if AUDIO_BACKEND_WASAPI
        period = (unsigned)(s_wasapi_period_100ns * CELL_AUDIO_SAMPLE_RATE / 10000000);
#endif
        audio_out_env_config(&cfg, period);
        /* never more than the WASAPI buffer can hold */
        { const int room = audio_backend_room() + (int)audio_backend_queued_samples();
          if (room > 0 && cfg.device_frames > (unsigned)room) cfg.device_frames = (unsigned)room; }
        ao = audio_out_create(&cfg);
    }
    static float ao_buf[4096 * 2];
    int ao_log_ms = 5000;
    { const char* e = getenv("PORT_AUDIO_LOG"); const char* p = getenv("R2_RSX_PROF");
      if (e && *e) ao_log_ms = atoi(e) == 0 ? 0 : (atoi(e) == 1 ? 1000 : atoi(e) < 100 ? 1000 : atoi(e));
      else if (p && *p && atoi(p)) ao_log_ms = 1000; }
    ULONGLONG ao_t0 = GetTickCount64();
    __typeof__(s_ah) ah_prev; memset(&ah_prev, 0, sizeof ah_prev);
    int stretch_on, late_ms, health_log; double smin;
    { const char* e = getenv("PORT_AUDIO_WSOLA"); stretch_on = !(e && *e && atoi(e) == 0);
      e = getenv("PORT_AUDIO_STRETCH_MIN"); smin = (e && *e) ? atof(e) : 0.70;
      if (!(smin >= 0.5)) smin = 0.5; if (smin > 1.0) smin = 1.0;
      e = getenv("PORT_AUDIO_LATE_WAIT_MS");
      if (e && *e) late_ms = atoi(e);
      else if (late_us > 0) late_ms = (late_us + 999) / 1000;
      else late_ms = (stretch_on || ao) ? 22 : 0;
      if (late_ms < 0) late_ms = 0; if (late_ms > 40) late_ms = 40;
      e = getenv("PORT_AUDIO_LOG"); health_log = !(e && *e && atoi(e) == 0); }
    double tempo = 1.0;
    static float hop[AS_HS * 2];
    long long wait_q0 = 0;                 /* QPC: this tick started waiting for a late block */
    long long last_conceal_q = 0; unsigned conceal_seen = 0;
    int wait_suspended = 0, dev_dry_flag = 0;
    ULONGLONG ah_t0 = GetTickCount64(), ah_quiet0 = ah_t0;
    s_ah.tempo_min = 1.0; s_ah.q_min = ~0u;
    { extern int ps3_host_prio_boost(void);
      char dev[160];
#if AUDIO_BACKEND_WASAPI
      if (s_wasapi_client)
          snprintf(dev, sizeof dev, "WASAPI shared %s, device %u Hz %u ch (engine period %.1f ms), buffer %u frames (%.0f ms)",
                   s_wasapi_event ? "event" : "polled", s_wasapi_dev_rate, s_wasapi_dev_ch,
                   s_wasapi_period_100ns / 10000.0, (unsigned)s_wasapi_buf_frames,
                   s_wasapi_buf_frames * 1000.0 / CELL_AUDIO_SAMPLE_RATE);
      else
#endif
          snprintf(dev, sizeof dev, "NO DEVICE (clock pacing, silent)");
      char mm[48];
      if (mmcss) snprintf(mm, sizeof mm, "Pro Audio");
      else if (!mm_want) snprintf(mm, sizeof mm, "off");
      else snprintf(mm, sizeof mm, "FAILED (%ld)", (long)mm_err);
      char outs[200];
      if (ao) { const AudioOutConfig* c = audio_out_config(ao);
                snprintf(outs, sizeof outs, "SoundTouch stretch (below %.0f%% of the target, min tempo %.2f, device queue %u ms, fade %u ms)",
                         c->threshold * 100.0, c->min_tempo, c->device_frames * 1000 / c->rate, c->fade_frames * 1000 / c->rate); }
      else snprintf(outs, sizeof outs, "legacy FIFO, WSOLA stretch %s (min tempo %.2f)%s", stretch_on ? "on" : "off", smin,
                    ao_want ? "" : " [PORT_AUDIO_STRETCH=0]");
      fprintf(stderr, "[audio] output: %s; target buffer %u ms, read delay %u blocks, %s, "
              "late-block wait %d ms, mixer %s, MMCSS %s, affinity %s, %s pacing, %d logical CPUs, prio boost %s%c",
              dev, audio_buffer_ms(), audio_read_delay_blocks(32), outs, late_ms,
              prio_s, mm, aff, steady ? "steady" : "device", audio_host_cpus(), ps3_host_prio_boost() ? "on" : "off", 10); }
    while (s_mix_thread_running) {
        if (steady) {
            /* the device itself ran dry since the last pass (it played a hole) */
            if (primed) {
                const int r0 = audio_backend_room();
                if (r0 >= 0 && audio_backend_queued_samples() == 0) { if (!dev_dry_flag) { s_ah.dev_dry++; dev_dry_flag = 1; } }
                else dev_dry_flag = 0;
            }
            /* 1. drain the FIFO into the device as far as it has room --
             * through the stretcher when it is on. With the SoundTouch
             * stage: keep the device topped up to its queue cap from the
             * ring (the rest of the buffer stays in the ring, where the
             * tempo still applies to it) */
            if (ao) {
                if (audio_backend_room() >= 0) {
                    const unsigned cap = audio_out_config(ao)->device_frames;
                    for (;;) {
                        const int room = audio_backend_room();
                        if (room <= 0) break;
                        const unsigned dq = audio_backend_queued_samples();
                        if (dq >= cap) break;
                        unsigned n = cap - dq;
                        if (n > (unsigned)room) n = (unsigned)room;
                        if (n > 4096) n = 4096;
                        const unsigned got = audio_out_render(ao, ao_buf, n, dq);
                        if (!got) break;
                        audio_backend_submit(ao_buf, got);
                    }
                    if (!audio_out_is_dry(ao)) primed = 1;   /* playing: device dry from now on is a real glitch */
                }
            } else
            if (stretch_on && audio_backend_room() >= 0) {
                for (;;) {
                    const int room = audio_backend_room();
                    if (room < AS_HS) break;
                    if (as_process(&s_as, tempo, hop)) { audio_backend_submit(hop, AS_HS); continue; }
                    if (!fcount) break;
                    const unsigned tail = (fhead + FIFO_N - fcount) % FIFO_N;
                    if (as_push(&s_as, fifo[tail], CELL_AUDIO_BLOCK_SAMPLES) != CELL_AUDIO_BLOCK_SAMPLES) break;
                    fcount--;
                }
            } else
            while (fcount) {
                const int room = audio_backend_room();
                if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) break;
                const unsigned tail = (fhead + FIFO_N - fcount) % FIFO_N;
                if (room >= 0) audio_backend_submit(fifo[tail], CELL_AUDIO_BLOCK_SAMPLES);
                fcount--;
            }
            /* host-side audio not yet played: device queue + FIFO */
            const int dev = audio_backend_room() >= 0;
            const unsigned qdev = dev ? audio_backend_queued_samples() + (ao ? 0u : fcount * CELL_AUDIO_BLOCK_SAMPLES) : 0;
            const unsigned queued = ao ? (dev ? audio_out_level(ao, qdev) : 0u)
                                       : qdev + (dev && stretch_on ? (unsigned)as_pending(&s_as) : 0u);
            /* prime the buffer with silence at start and after a device
             * underrun, so there is a cushion to wait into right away (the
             * SoundTouch stage primes with real audio and conceals itself) */
            if (!ao && dev && (!primed || qdev < CELL_AUDIO_BLOCK_SAMPLES)) {
                static const float zero[CELL_AUDIO_BLOCK_SAMPLES * 2] = { 0.0f };
                unsigned n = (target_frames - (queued < target_frames ? queued : target_frames)) / CELL_AUDIO_BLOCK_SAMPLES;
                if (primed) { n /= 2; lt_reprimes++; s_ah.reprimes++; }
                while (n-- && fcount < FIFO_N) { memcpy(fifo[fhead], zero, sizeof(fifo[0])); fhead = (fhead + 1) % FIFO_N; fcount++; }
                primed = 1;
                continue;
            }
            /* 2. next block due on the guest clock? */
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (now.QuadPart < due_q) {
                long long wait100ns = (due_q - now.QuadPart) * 10000000LL / qf.QuadPart;
                if (wait100ns > 20000) wait100ns = 20000;          /* <= 2 ms, then re-check the device */
                if (tmr) { LARGE_INTEGER d; d.QuadPart = -wait100ns;
                           SetWaitableTimer(tmr, &d, 0, NULL, NULL, FALSE);
                           WaitForSingleObject(tmr, 5); }
                else Sleep(1);
                continue;
            }
            if (!wait_q0) {
                const double lms = (double)(now.QuadPart - due_q) * 1000.0 / (double)qf.QuadPart;
                if (lms > s_ah.late_max_ms) s_ah.late_max_ms = lms;
                if (now.QuadPart - due_q > qf.QuadPart / 20) {     /* >50 ms behind: resync */
                    due_q = now.QuadPart; s_ah.resyncs++;
                }
            }
            /* 3. late guest block (see PORT_AUDIO_LATE_WAIT_MS): armed for 5 s
             * after a concealment; waits in 0.5 ms steps (not spinning: the
             * producer needs the cores) while the host buffer keeps 4 blocks */
            if (audio_host_paused()) last_conceal_q = 0;   /* nothing is late while parked */
            if (dev && late_ms && !wait_suspended && last_conceal_q &&
                now.QuadPart - last_conceal_q < qf.QuadPart * 5) {
                if (audio_pending_ports()) {
                    if (!wait_q0) { wait_q0 = now.QuadPart; s_lt_waits++; s_ah.waits++; }
                    const unsigned keep = 4 * CELL_AUDIO_BLOCK_SAMPLES;
                    const long long waited_us = (now.QuadPart - wait_q0) * 1000000 / qf.QuadPart;
                    long long budget_us = (long long)late_ms * 1000;
                    const long long cushion_us = queued > keep ? (long long)(queued - keep) * 1000000 / CELL_AUDIO_SAMPLE_RATE : 0;
                    if (budget_us > cushion_us) budget_us = cushion_us;
                    if (waited_us < budget_us) {
                        if (tmr) { LARGE_INTEGER d; d.QuadPart = -5000; SetWaitableTimer(tmr, &d, 0, NULL, NULL, FALSE);
                                   WaitForSingleObject(tmr, 2); }
                        else Sleep(1);
                        continue;
                    }
                    s_ah.wait_timeouts++;
                } else if (wait_q0) { s_lt_arrived++; s_ah.caught++; }
            }
            if (wait_q0) {
                /* the tick happens now: slip the clock by the wait, but not
                 * past now (a tick that was already late stays late) */
                const long long slip = now.QuadPart - wait_q0;
                due_q = due_q + slip < now.QuadPart ? due_q + slip : now.QuadPart;
                const unsigned us = (unsigned)(slip * 1000000 / qf.QuadPart);
                lt_slip_us += us; s_ah.slip_us += us;
                s_lt_wait_us_sum += us; if (us > s_lt_wait_us_max) s_lt_wait_us_max = us;
                port_hitch_add_ns(PH_AUDIO, (uint64_t)us * 1000u);
                wait_q0 = 0;
            }
            if (dev) { if (queued < lt_q_min) lt_q_min = queued; lt_q_sum += queued; lt_q_n++;
                       if (queued < s_ah.q_min) s_ah.q_min = queued; s_ah.q_sum += queued; s_ah.q_n++; }
            long long per = period_q;
            if (dev && catchup) {
                if (queued < target_frames) { per -= period_q * catchup / 100; lt_fast++; }
                else if (queued > target_frames + target_frames / 2) per += period_q * catchup / 100;
            }
            due_q += per;
            blocks++;
            audio_mix_one_block();
            { static FILE* _wf = (FILE*)-1;   /* AUDIO_WAV=<file>: raw f32le stereo 48 kHz of the final mix */
              if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
              if (_wf) fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf); }
            if (!ao && fcount == FIFO_N) { fcount--; s_ah.fifo_drops++; } /* device stalled: drop the oldest */
            if (s_ah.concealed != conceal_seen) { conceal_seen = s_ah.concealed; last_conceal_q = now.QuadPart; }
            /* stretcher tempo, after RPCS3's frequency ratio: below 75% of
             * the target buffer, tempo follows fill / 0.75 (smoothed over
             * ~50 ms, floored at PORT_AUDIO_STRETCH_MIN); above it, ramps
             * back to 1 over ~0.25 s */
            if (!ao && stretch_on && dev) {
                const double fill = (double)queued / (double)target_frames;
                if (fill < 0.75) { double req = fill / 0.75; if (req < smin) req = smin; tempo += (req - tempo) * 0.1; }
                else tempo = tempo + 0.02 > 0.9995 ? 1.0 : tempo + 0.02;
                if (tempo < 1.0) s_ah.stretch_ticks++;
                if (tempo < s_ah.tempo_min) s_ah.tempo_min = tempo;
            }
            s_ah.ticks++;
            /* [audio] health, always on (PORT_AUDIO_LOG=0 off): every 10 s
             * when anything went wrong, else once a minute */
            { const ULONGLONG tn = GetTickCount64();
              if (tn - ah_t0 >= 10000) {
                  const int bad = s_ah.concealed || s_ah.reprimes || s_ah.dev_dry || s_ah.resyncs ||
                                  s_ah.fifo_drops || s_ah.stretch_ticks || s_ah.wait_timeouts;
                  const int was_suspended = wait_suspended;
                  /* the wait is only worth its slip if it catches blocks */
                  wait_suspended = !was_suspended && s_ah.waits >= 100 && s_ah.caught * 10 < s_ah.waits;
                  if (health_log && !ao && (bad || tn - ah_quiet0 >= 60000)) {
                      fprintf(stderr, "[audio] underruns %u in last 10 s (device dry %u, buffer refilled %u) | "
                              "guest blocks late: %u concealed, %u rescued | waits %u (caught %u, gave up %u, slip %.0f ms)%s | "
                              "stretched %.0f%% of the time (min tempo %.2f) | host buffer avg %.0f ms, min %.0f ms (target %u) | "
                              "mixer wake late max %.1f ms, %u resync(s), %u drop(s)%c",
                              s_ah.dev_dry + s_ah.reprimes, s_ah.dev_dry, s_ah.reprimes,
                              s_ah.concealed, s_ah.rescued,
                              s_ah.waits, s_ah.caught, s_ah.wait_timeouts, s_ah.slip_us / 1000.0,
                              wait_suspended ? " -- wait paused 10 s (not catching blocks)" : (was_suspended ? " (paused)" : ""),
                              s_ah.ticks ? 100.0 * s_ah.stretch_ticks / s_ah.ticks : 0.0, s_ah.tempo_min,
                              s_ah.q_n ? s_ah.q_sum * 1000.0 / CELL_AUDIO_SAMPLE_RATE / s_ah.q_n : 0.0,
                              s_ah.q_min == ~0u ? 0.0 : s_ah.q_min * 1000.0 / CELL_AUDIO_SAMPLE_RATE, audio_buffer_ms(),
                              s_ah.late_max_ms, s_ah.resyncs, s_ah.fifo_drops, 10);
                      ah_quiet0 = tn;
                  }
                  memset(&s_ah, 0, sizeof(s_ah)); s_ah.tempo_min = 1.0; s_ah.q_min = ~0u;
                  memset(&ah_prev, 0, sizeof ah_prev);
                  conceal_seen = 0; ah_t0 = tn;
              } }
            if (ao) {
                audio_out_push(ao, s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
                audio_out_tick(ao, qdev);
                /* [audio] every 5 s (1 s with PORT_AUDIO_LOG=1 or
                 * R2_RSX_PROF=1, PORT_AUDIO_LOG=<ms> any period, 0 off).
                 * Good: buffer min well above 0, tempo avg ~1.00, underruns 0,
                 * device dry 0. */
                const ULONGLONG tn = GetTickCount64();
                if (ao_log_ms && tn - ao_t0 >= (ULONGLONG)ao_log_ms) {
                    AudioOutStats st; audio_out_take_stats(ao, &st);
                    fprintf(stderr, "[audio] %.1f s: buffer min %.0f avg %.0f ms (target %u) | tempo min %.2f avg %.3f "
                            "(stretched %.0f%%) | underruns %u (silence %.0f ms, device dry %u) | guest blocks late: "
                            "%u concealed, %u rescued, waits %u (caught %u, gave up %u, slip %.0f ms) | mixer wake late max %.1f ms%s%c",
                            (tn - ao_t0) / 1000.0, st.level_min_ms, st.level_avg_ms, audio_buffer_ms(),
                            st.tempo_min, st.tempo_avg, st.ticks ? 100.0 * st.stretch_ticks / st.ticks : 0.0,
                            st.underruns, st.silence_ms, s_ah.dev_dry - ah_prev.dev_dry,
                            s_ah.concealed - ah_prev.concealed, s_ah.rescued - ah_prev.rescued,
                            s_ah.waits - ah_prev.waits, s_ah.caught - ah_prev.caught,
                            s_ah.wait_timeouts - ah_prev.wait_timeouts, (s_ah.slip_us - ah_prev.slip_us) / 1000.0,
                            s_ah.late_max_ms, st.overflow_frames ? " | ring overflow" : "", 10);
                    ah_prev = s_ah; ao_t0 = tn;
                }
            } else {
                memcpy(fifo[fhead], s_mix_buffer, sizeof(fifo[0]));
                fhead = (fhead + 1) % FIFO_N; fcount++;
            }
            if (!g_audio_held_tick) audio_notify_event_queues();
            g_audio_held_tick = 0;
            /* [audio-late] every 5 s: the waits, what they caught, what had
             * to be concealed, and the host buffer level */
            { const ULONGLONG tn = GetTickCount64();
              if (tn - lt_t0 >= 5000) {
                  char ln[480];
                  snprintf(ln, sizeof ln, "[audio-late] 5 s: %u wait(s) (avg %.2f ms, max %.2f ms, clock slip %.1f ms), "
                           "%u block(s) arrived in the wait, %u rescued by the %u-block read delay, %u duplicate(s) dropped, "
                           "%u concealed, %u resume crossfade(s), "
                           "late-lap data %u played / %u skipped; host buffer avg %.1f ms min %.1f ms (target %u), "
                           "%u fast tick(s), %u re-prime(s)%c",
                           s_lt_waits, s_lt_waits ? s_lt_wait_us_sum / 1000.0 / s_lt_waits : 0.0, s_lt_wait_us_max / 1000.0,
                           lt_slip_us / 1000.0, s_lt_arrived, s_dl_rescued, audio_read_delay_blocks(8), s_dl_dups,
                           s_lt_fills, s_lt_resumes,
                           s_lt_stale_played, s_lt_stale_skipped,
                           lt_q_n ? lt_q_sum * 1000.0 / CELL_AUDIO_SAMPLE_RATE / lt_q_n : 0.0,
                           lt_q_min == ~0u ? 0.0 : lt_q_min * 1000.0 / CELL_AUDIO_SAMPLE_RATE,
                           audio_buffer_ms(), lt_fast, lt_reprimes, 10);
                  audio_diag_line(ln);
                  s_lt_waits = s_lt_arrived = s_lt_fills = s_lt_resumes = s_lt_stale_played = s_lt_stale_skipped = 0;
                  s_dl_rescued = s_dl_dups = 0;
                  s_lt_wait_us_sum = 0; s_lt_wait_us_max = 0;
                  lt_q_min = ~0u; lt_q_sum = 0; lt_q_n = 0; lt_slip_us = 0; lt_fast = 0; lt_reprimes = 0; lt_t0 = tn;
              } }
            continue;
        }
        const int room = audio_backend_room();
        if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) { audio_backend_wait(); continue; }
        if (room < 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const long long due = q0.QuadPart +
                (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            if (now.QuadPart < due) { Sleep(1); continue; }
            /* More than 50 ms behind (device just went away, a debugger
             * stop): drop the backlog instead of mixing it in a burst. */
            if (now.QuadPart - due > qf.QuadPart / 20)
                q0.QuadPart = now.QuadPart -
                    (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
        }
        blocks++;
        /* Mix and submit one block */
        audio_mix_one_block();
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; float pk = 0.0f;
            for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
                float a = s_mix_buffer[i]; if (a < 0) a = -a; if (a > pk) pk = a; }
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        /* AUDIO_WAV=<file>: append the final mix as raw f32le stereo 48 kHz
         * (ffmpeg -f f32le -ar 48000 -ac 2 -i <file>). */
        { static FILE* _wf = (FILE*)-1;
          if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
          if (_wf) fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf); }
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        /* AUDIO_RATE=1: blocks mixed vs real time, and frames dropped. */
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static ULONGLONG t0; static unsigned n, z; static unsigned long long d0;
            ULONGLONG now = GetTickCount64(); if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, device buf %u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                #if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, 10);
#else
                        0u, 10);
#endif
                n = z = 0; d0 = g_audio_dropped; t0 = now; } } }

        /* Notify event queues */
        audio_notify_event_queues();
    }

    if (ao) audio_out_destroy(ao);
    if (mmcss && av_revert) av_revert(mmcss);
    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();

        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            usleep(5000);
        } else {
            usleep(2000);
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
    /* priority and MMCSS: set by audio_mix_thread_func itself */
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    /* Same guard. Init leaves s_audio_initialized set even when the mixing
     * thread failed to start ("continuing silently" -- a title without audio
     * still runs), so Quit reaches here with s_mix_thread never assigned.
     * pthread_t has no null value to test, and joining an unset one, or
     * joining the same one twice, is undefined rather than an error the way
     * WaitForSingleObject on a null HANDLE is. */
    if (s_mix_thread_live) {
        pthread_join(s_mix_thread, NULL);
        s_mix_thread_live = 0;
    }
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    /* PS3_NO_AUDIO=1: report that the audio library is unavailable. Middleware
     * that drives its mixer through SPU jobs will otherwise block the whole boot
     * waiting on work we cannot yet complete; failing here lets a title take its
     * silent path and keep going. Diagnostic first, workaround second. */
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }


#ifdef _WIN32
    { LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      s_audio_start_us = (u64)(c.QuadPart / f.QuadPart) * 1000000ULL +
          (u64)(c.QuadPart % f.QuadPart) * 1000000ULL / (u64)f.QuadPart; }
#else
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      s_audio_start_us = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL; }
#endif
    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->read_index  = 0;
    port->write_index = 0;
    audio_lt_reset_port(found);   /* v141: no delay-line / concealment state from a previous open */

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    /* Each port reuses its own two pages in the runtime-owned audio window. */
    _Static_assert(CELL_AUDIO_PORT_MAX * VM_AUDIO_PORT_SIZE <= VM_AUDIO_SIZE,
                   "Audio port storage exceeds its reserved VM window");
    u32 guest_buf = VM_AUDIO_BASE + (u32)found * VM_AUDIO_PORT_SIZE;
    u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, VM_AUDIO_PORT_SIZE) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* read index counter (u64, see below) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (reserved per port in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    uint32_t tag_ea = (uint32_t)(uintptr_t)tag;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!tag_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    u64 t = port->read_index + blockNo - (port->read_index % nblk);
    if (blockNo < port->read_index % nblk) t += nblk;
    mutex_unlock(&s_audio_mutex);

    vm_write64(tag_ea, t);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    uint32_t stamp_ea = (uint32_t)(uintptr_t)stamp;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!stamp_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_ports[portNum].read_index;
    u64 ts = s_ports[portNum].ts_us;
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    /* Resistance port (v124): as RPCS3 -- the time the port's newest block
     * was consumed, minus one block period per block back to the tag, in the
     * guest's own clock (sys_time_get_system_time). It was the HOST's absolute
     * QPC time at cellAudioInit plus tag * period: hours ahead of the guest
     * clock, and blind to when this port actually started (movies open port 1
     * long after init). AUDIO_OLD_STAMP=1 = old value. */
    static int old = -1;
    if (old < 0) { const char* e = getenv("AUDIO_OLD_STAMP"); old = e ? atoi(e) : 0; }
    u64 t;
    if (old || !ts) t = s_audio_start_us + tag * 256000000ULL / 48000ULL;
    else {
        const u64 back = (global_counter - tag) * 256000000ULL / 48000ULL;
        t = ts > back ? ts - back : 0;
    }
    vm_write64(stamp_ea, t);
    return CELL_OK;
}

/* cellAudioCreateNotifyEventQueue(sys_event_queue_t* id, u64* key)
 *
 * Creates the queue the game will block on for audio-period events, and
 * registers it for notification in one step -- the game never calls
 * SetNotifyEventQueue for this one.
 *
 * This was missing entirely. YDKJ imports it, got the unimplemented-NID path,
 * and then blocked on an id nothing had created: sys_event_queue_receive
 * answered CELL_ESRCH every time and the title logged 1.7 MILLION
 * "ERROR - LIBAUDIO DROPOUT" lines in a 60 s run.
 *
 * Both params are GUEST addresses and both are big-endian out-values; the id
 * is 32-bit and the key 64-bit, so they cannot be written with a plain store. */
s32 cellAudioCreateNotifyEventQueue(u32* id, u64* key)
{
    uint32_t id_ea  = (uint32_t)(uintptr_t)id;
    uint32_t key_ea = (uint32_t)(uintptr_t)key;

    printf("[cellAudio] CreateNotifyEventQueue(id_ea=0x%08X key_ea=0x%08X)\n",
           id_ea, key_ea);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (!id_ea || !key_ea)
        return CELL_AUDIO_ERROR_PARAM;

    /* A distinct key per queue: the notify path resolves queues BY KEY, and the
     * game creates its other queues with key 0, so reusing 0 here would make
     * find_queue_by_key ambiguous and post audio events to whichever matched. */
    static uint64_t s_next_key = 0x8000000000000001ull;
    uint64_t k = s_next_key++;

    uint32_t qid = sys_event_queue_create_direct(k, 8);
    if (!qid)
        return CELL_AUDIO_ERROR_EVENT_QUEUE;

    mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { mutex_unlock(&s_audio_mutex); return CELL_AUDIO_ERROR_PARAM; }
    s_notify_queues[slot].in_use = 1;
    s_notify_queues[slot].key    = k;
    mutex_unlock(&s_audio_mutex);

    vm_write32(id_ea, qid);
    vm_write64(key_ea, k);
    printf("[cellAudio] CreateNotifyEventQueue -> id=%u key=0x%llX\n",
           qid, (unsigned long long)k);
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if (PS3_LOG_FIRST(_n, 24))
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_CLOSE);
    } else if (s_ports[portNum].running) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_RUN);
    } else {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_READY);
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
