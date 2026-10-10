// Audio: native replacement for the Xbox DirectSound API on XAudio2.
//
// Sound buffers play PCM or Xbox ADPCM from game memory; streams queue
// XMEDIAPACKETs and complete them as they finish playing. As on the Xbox,
// packet status words and completion events are updated as playback advances,
// while client callbacks are delivered from DirectSoundDoWork on the game's
// thread. The XDK's software WMA decoder and file media objects run natively.

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "core/window.h"
#include "kernel/kernel.h"
#include "xapi/xapi.h"

namespace swrots::audio {

// --- Xbox DirectSound types -----------------------------------------------------

struct Vec3 {
    float x, y, z;
};

#pragma pack(push, 1)
struct XWaveFormat {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
};
#pragma pack(pop)

constexpr WORD kFormatPcm = 1;
constexpr WORD kFormatXboxAdpcm = 0x69;

struct MixBinVolumePair {
    DWORD dwMixBin;
    LONG lVolume;
};

struct MixBins {
    DWORD dwCount;
    MixBinVolumePair* lpMixBinVolumePairs;
};

struct BufferDesc {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    XWaveFormat* lpwfxFormat;
    MixBins* lpMixBins;
    DWORD dwInputMixBin;
};

using XmoCallback = void(__stdcall*)(void* streamContext, void* packetContext, DWORD status);

struct StreamDesc {
    DWORD dwFlags;
    DWORD dwMaxAttachedPackets;
    XWaveFormat* lpwfxFormat;
    XmoCallback lpfnCallback;
    void* lpvContext;
    MixBins* lpMixBins;
};

struct MediaPacket {
    void* pvBuffer;
    DWORD dwMaxSize;
    DWORD* pdwCompletedSize;
    DWORD* pdwStatus;
    union {
        HANDLE hCompletionEvent;
        void* pContext;
    };
    LONGLONG* prtTimestamp;
};

struct MediaInfo {
    DWORD dwFlags;
    DWORD dwInputSize;
    DWORD dwOutputSize;
    DWORD dwMaxLookahead;
};

struct Ds3dBuffer {
    DWORD dwSize;
    Vec3 vPosition;
    Vec3 vVelocity;
    DWORD dwInsideConeAngle;
    DWORD dwOutsideConeAngle;
    Vec3 vConeOrientation;
    LONG lConeOutsideVolume;
    float flMinDistance;
    float flMaxDistance;
    DWORD dwMode;
    float flDistanceFactor;
    float flRolloffFactor;
    float flDopplerFactor;
};

constexpr DWORD DSBCAPS_CTRL3D = 0x10;
constexpr DWORD DSBCAPS_MUTE3DATMAXDISTANCE = 0x20000;
constexpr DWORD DSBPLAY_LOOPING = 1;
constexpr DWORD DSBPLAY_FROMSTART = 2;
constexpr DWORD DSBSTATUS_PLAYING = 1, DSBSTATUS_PAUSED = 2, DSBSTATUS_LOOPING = 4;
constexpr DWORD DSSSTATUS_READY = 0x1, DSSSTATUS_PLAYING = 0x10000, DSSSTATUS_PAUSED = 0x20000,
                DSSSTATUS_STARVED = 0x40000;
constexpr DWORD DS3DMODE_NORMAL = 0, DS3DMODE_HEADRELATIVE = 1, DS3DMODE_DISABLE = 2;
constexpr DWORD XMO_STREAMF_FIXED_SAMPLE_SIZE = 1, XMO_STREAMF_INPUT_ASYNC = 4;
constexpr HRESULT XMP_STATUS_SUCCESS = S_OK;
constexpr HRESULT XMP_STATUS_PENDING = E_PENDING;
constexpr HRESULT XMP_STATUS_FLUSHED = E_ABORT;

// --- Xbox ADPCM -------------------------------------------------------------------
// 36 bytes per channel per block: 4-byte header (predictor, step index), then
// 8 groups of 4 bytes per channel, 8 nibbles each (low nibble first). 65
// samples per block including the header sample.

static const int16_t kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};
static const int8_t kIndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
constexpr uint32_t kAdpcmBlock = 36;
constexpr uint32_t kAdpcmSamplesPerBlock = 65;

static void DecodeAdpcm(const uint8_t* in, uint32_t bytes, uint32_t channels, std::vector<int16_t>& out)
{
    uint32_t blocks = bytes / (kAdpcmBlock * channels);
    out.resize(size_t(blocks) * kAdpcmSamplesPerBlock * channels);
    int16_t* o = out.data();
    int pred[8], index[8];
    for (uint32_t b = 0; b < blocks; ++b) {
        for (uint32_t c = 0; c < channels; ++c) {
            pred[c] = int16_t(in[0] | (in[1] << 8));
            index[c] = std::clamp<int>(in[2], 0, 88);
            *o++ = int16_t(pred[c]);
            in += 4;
        }
        for (int group = 0; group < 8; ++group) {
            int16_t decoded[8][8];
            for (uint32_t c = 0; c < channels; ++c) {
                uint32_t code = in[0] | (in[1] << 8) | (in[2] << 16) | (uint32_t(in[3]) << 24);
                in += 4;
                for (int j = 0; j < 8; ++j, code >>= 4) {
                    int n = code & 15, step = kStepTable[index[c]];
                    int diff = step >> 3;
                    if (n & 4) diff += step;
                    if (n & 2) diff += step >> 1;
                    if (n & 1) diff += step >> 2;
                    pred[c] = std::clamp(n & 8 ? pred[c] - diff : pred[c] + diff, -32768, 32767);
                    index[c] = std::clamp(index[c] + kIndexTable[n], 0, 88);
                    decoded[c][j] = int16_t(pred[c]);
                }
            }
            for (int j = 0; j < 8; ++j)
                for (uint32_t c = 0; c < channels; ++c)
                    *o++ = decoded[c][j];
        }
    }
}

// --- Engine ------------------------------------------------------------------------

static IXAudio2* g_XAudio = nullptr;
static IXAudio2MasteringVoice* g_Master = nullptr;
static DWORD g_MasterChannels = 2;
static std::recursive_mutex g_Lock;
static LARGE_INTEGER g_StartQpc, g_QpcFreq;

struct Listener {
    Vec3 position = { 0, 0, 0 };
    Vec3 front = { 0, 0, 1 };
    Vec3 top = { 0, 1, 0 };
    float distanceFactor = 1.0f, rolloffFactor = 1.0f, dopplerFactor = 1.0f;
} g_Listener;

static bool g_AudioFailed = false; // no audio device or engine: buffers keep time silently

static bool EnsureEngine()
{
    if (g_XAudio)
        return true;
    if (g_AudioFailed)
        return false;
    QueryPerformanceFrequency(&g_QpcFreq);
    QueryPerformanceCounter(&g_StartQpc);
    // The mastering voice opens the audio device through COM (MMDevice API), which needs COM on the
    // calling thread or a multithreaded apartment in the process. Game threads never initialize COM, and
    // whether something else in the process has (the installer's dialogs do; on some systems input or
    // graphics libraries) varies, so the process keeps one for good. It changes no thread's apartment.
    APTTYPE apartment;
    APTTYPEQUALIFIER qualifier;
    HRESULT com = CoGetApartmentType(&apartment, &qualifier);
    LOG_DEBUG("Audio: COM on this thread before audio: %08lX (apartment %d, qualifier %d)", com, int(apartment),
        int(qualifier));
    static CO_MTA_USAGE_COOKIE mtaCookie = nullptr;
    if (!mtaCookie && FAILED(CoIncrementMTAUsage(&mtaCookie)))
        mtaCookie = nullptr;
    // Development aid: SWROTS_NO_AUDIO=1 runs the game as without an audio device.
    HRESULT hr = GetEnvironmentVariableA("SWROTS_NO_AUDIO", nullptr, 0) ? E_FAIL
                                                                        : XAudio2Create(&g_XAudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr))
        hr = g_XAudio->CreateMasteringVoice(&g_Master, 2, 48000);
    if (FAILED(hr)) {
        LOG_ERROR("XAudio2 initialisation failed (%08lX); the game runs without sound", hr);
        if (g_XAudio) {
            g_XAudio->Release();
            g_XAudio = nullptr;
        }
        g_Master = nullptr;
        g_AudioFailed = true;
        return false;
    }
    // What the output device is (the game mixes to stereo; the device's own layout is converted to by
    // XAudio2, or by Wine's on Linux): for reports of sound on one side only.
    XAUDIO2_VOICE_DETAILS master = {};
    g_Master->GetVoiceDetails(&master);
    DWORD layout = 0;
    g_Master->GetChannelMask(&layout);
    LOG_INFO("Audio: output %u channel(s) at %u Hz, speaker layout %08lX", master.InputChannels,
        master.InputSampleRate, layout);
    if (RunningInBackground()) // a background test run (core/window.h) is silent
        g_Master->SetVolume(0.0f);
    LOG_INFO("Audio: XAudio2 ready (48 kHz stereo)");
    return true;
}

static LONGLONG QpcNow()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static float DbToGain(LONG hundredthsDb)
{
    if (hundredthsDb <= -10000)
        return 0.0f;
    return powf(10.0f, float(hundredthsDb) / 2000.0f);
}

static float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

// Common voice state for buffers and streams.
struct Voice : IXAudio2VoiceCallback {
    IXAudio2SourceVoice* source = nullptr;
    XWaveFormat format = {};
    DWORD flags = 0;
    LONG volume = 0;
    LONG headroom = 0;
    DWORD frequency = 0;
    std::vector<MixBinVolumePair> mixBins;
    Ds3dBuffer params3d = {};
    // Xbox rolloff curve: volume factors spaced evenly from min to max
    // distance; replaces the inverse-distance falloff when set.
    std::vector<float> rolloffCurve;
    bool paused = false;

    Voice()
    {
        params3d.flMinDistance = 1.0f;
        params3d.flMaxDistance = 1.0e9f;
        params3d.flRolloffFactor = 1.0f;
    }
    virtual ~Voice()
    {
        if (source)
            source->DestroyVoice();
    }

    DWORD Channels() const { return std::max<DWORD>(1, format.nChannels); }

    bool CreateSource()
    {
        if (source)
            return true;
        if (!EnsureEngine() || !format.nSamplesPerSec)
            return false;
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = WORD(Channels());
        wf.nSamplesPerSec = format.nSamplesPerSec;
        wf.wBitsPerSample = format.wFormatTag == kFormatXboxAdpcm ? 16 : format.wBitsPerSample;
        wf.nBlockAlign = WORD(wf.nChannels * wf.wBitsPerSample / 8);
        wf.nAvgBytesPerSec = wf.nBlockAlign * wf.nSamplesPerSec;
        HRESULT hr = g_XAudio->CreateSourceVoice(&source, &wf, 0, 4.0f, this);
        if (FAILED(hr)) {
            LOG_WARN("CreateSourceVoice (%u ch, %lu Hz, %u bit) failed: %08lX", wf.nChannels, wf.nSamplesPerSec,
                wf.wBitsPerSample, hr);
            source = nullptr;
            return false;
        }
        ApplyMix();
        return true;
    }

    // Volume, pitch and speaker mix from the Xbox parameters.
    void ApplyMix()
    {
        if (!source)
            return;
        float gain = DbToGain(volume - headroom);
        float left = 1.0f, right = 1.0f;

        if ((flags & DSBCAPS_CTRL3D) && params3d.dwMode != DS3DMODE_DISABLE) {
            Vec3 rel = params3d.dwMode == DS3DMODE_HEADRELATIVE ? params3d.vPosition
                                                                     : Sub(params3d.vPosition, g_Listener.position);
            float dist = sqrtf(Dot(rel, rel)) * g_Listener.distanceFactor;
            float minD = std::max(params3d.flMinDistance, 0.0001f);
            float rolloff = params3d.flRolloffFactor * g_Listener.rolloffFactor;
            float att;
            if (!rolloffCurve.empty()) {
                float span = params3d.flMaxDistance - params3d.flMinDistance;
                float t = span > 0 ? std::clamp((dist - params3d.flMinDistance) / span, 0.0f, 1.0f) : 0.0f;
                float x = t * float(rolloffCurve.size() - 1);
                size_t i = std::min(size_t(x), rolloffCurve.size() - 1);
                size_t j = std::min(i + 1, rolloffCurve.size() - 1);
                att = std::clamp(rolloffCurve[i] + (rolloffCurve[j] - rolloffCurve[i]) * (x - float(i)), 0.0f, 1.0f);
            } else {
                att = dist <= minD ? 1.0f : minD / (minD + rolloff * (dist - minD));
            }
            if ((flags & DSBCAPS_MUTE3DATMAXDISTANCE) && dist > params3d.flMaxDistance)
                att = 0.0f;
            gain *= att;
            if (params3d.dwMode == DS3DMODE_NORMAL && dist > 0.001f) {
                Vec3 right3 = Cross(g_Listener.top, g_Listener.front);
                float len = sqrtf(Dot(right3, right3));
                float pan = len > 0 ? Dot(rel, right3) / (len * sqrtf(Dot(rel, rel))) : 0.0f;
                left = sqrtf(0.5f * (1.0f - pan)) * 1.41421f;
                right = sqrtf(0.5f * (1.0f + pan)) * 1.41421f;
            }
        } else if (!mixBins.empty()) {
            float l = 0, r = 0;
            for (const MixBinVolumePair& p : mixBins) {
                float g = DbToGain(p.lVolume);
                switch (p.dwMixBin) {
                case 0: l = std::max(l, g); break;                         // front left
                case 1: r = std::max(r, g); break;                         // front right
                case 2: l = std::max(l, g * 0.7f); r = std::max(r, g * 0.7f); break; // center
                case 4: l = std::max(l, g); break;                         // back left
                case 5: r = std::max(r, g); break;                         // back right
                default: l = std::max(l, g); r = std::max(r, g); break;    // 3D / effect bins
                }
            }
            left = l;
            right = r;
        }

        DWORD ch = Channels();
        float matrix[16] = {};
        const bool positioned = (flags & DSBCAPS_CTRL3D) && params3d.dwMode != DS3DMODE_DISABLE;
        if (ch == 1) {
            matrix[0] = left;
            matrix[1] = right;
        } else if (!positioned && mixBins.size() >= ch) {
            // As on the Xbox, a multichannel sound's channel n goes to the n-th mix bin: the movies play their
            // 5.1 audio as three stereo streams (front left and right, back left and right, centre and LFE),
            // and the centre (the dialogue) is not a left channel. Down to stereo: centre at -3 dB to both
            // sides, LFE at half to both, back channels to their side.
            for (DWORD c = 0; c < ch && c < 8; ++c) {
                const float g = DbToGain(mixBins[c].lVolume);
                float l = 0, r = 0;
                switch (mixBins[c].dwMixBin) {
                case 0: l = g; break;                       // front left
                case 1: r = g; break;                       // front right
                case 2: l = r = g * 0.7071f; break;         // centre
                case 3: l = r = g * 0.5f; break;            // LFE
                case 4: l = g; break;                       // back left
                case 5: r = g; break;                       // back right
                default: l = (c % 2 == 0) ? g : 0.0f; r = (c % 2 == 1) ? g : 0.0f; break;
                }
                matrix[c * 2 + 0] = l;
                matrix[c * 2 + 1] = r;
            }
        } else {
            for (DWORD c = 0; c < ch && c < 8; ++c) {
                // Even source channels feed the left speaker, odd ones the right.
                matrix[c * 2 + 0] = (c % 2 == 0) ? left : 0.0f;
                matrix[c * 2 + 1] = (c % 2 == 1) ? right : 0.0f;
            }
        }
        source->SetOutputMatrix(g_Master, ch, g_MasterChannels, matrix);
        source->SetVolume(gain);
        float ratio = frequency && format.nSamplesPerSec ? float(frequency) / float(format.nSamplesPerSec) : 1.0f;
        source->SetFrequencyRatio(std::clamp(ratio, XAUDIO2_MIN_FREQ_RATIO, 4.0f));
    }

    void SetMixBins(const MixBins* bins)
    {
        mixBins.clear();
        if (bins && bins->lpMixBinVolumePairs)
            mixBins.assign(bins->lpMixBinVolumePairs, bins->lpMixBinVolumePairs + bins->dwCount);
        ApplyMix();
    }

    void SetMixBinVolumes(const MixBins* bins)
    {
        if (!bins || !bins->lpMixBinVolumePairs)
            return;
        for (DWORD i = 0; i < bins->dwCount; ++i) {
            const MixBinVolumePair& in = bins->lpMixBinVolumePairs[i];
            auto it = std::find_if(mixBins.begin(), mixBins.end(), [&](auto& p) { return p.dwMixBin == in.dwMixBin; });
            if (it != mixBins.end())
                it->lVolume = in.lVolume;
            else
                mixBins.push_back(in);
        }
        ApplyMix();
    }

    // IXAudio2VoiceCallback
    void __stdcall OnVoiceProcessingPassStart(UINT32) override {}
    void __stdcall OnVoiceProcessingPassEnd() override {}
    void __stdcall OnStreamEnd() override {}
    void __stdcall OnBufferStart(void*) override {}
    void __stdcall OnBufferEnd(void* context) override { BufferEnded(context); }
    void __stdcall OnLoopEnd(void*) override {}
    void __stdcall OnVoiceError(void*, HRESULT error) override { LOG_WARN("XAudio2 voice error %08lX", error); }

    virtual void BufferEnded(void* context) { (void)context; }
};

static std::vector<Voice*> g_Voices; // for listener-driven 3D updates

// --- Sound buffers --------------------------------------------------------------------

struct SoundBuffer : Voice {
    LONG refCount = 1;
    uint8_t* data = nullptr;
    DWORD bytes = 0;
    std::unique_ptr<uint8_t[]> owned;
    std::vector<int16_t> decoded; // Xbox ADPCM, converted at play time
    bool playing = false;
    bool looping = false;
    UINT64 startSample = 0;   // voice SamplesPlayed when playback (re)started
    DWORD startOffset = 0;    // sample offset playback started at
    volatile LONG ended = 0;
    // Without an audio engine a buffer still plays, silently, on the clock: the game paces movies and
    // scripts by play positions and ends of sounds, which would otherwise never move.
    bool silent = false;
    LONGLONG silentStart = 0;    // QPC when silent playback (re)started, moved on by pauses
    LONGLONG silentPausedAt = 0; // QPC when paused

    UINT64 SilentSamplesPlayed() const
    {
        LONGLONG elapsed = (paused ? silentPausedAt : QpcNow()) - silentStart;
        return elapsed <= 0 ? 0 : UINT64(elapsed) * format.nSamplesPerSec / UINT64(std::max<LONGLONG>(1, g_QpcFreq.QuadPart));
    }

    void SilentPause(bool pause)
    {
        if (pause)
            silentPausedAt = QpcNow();
        else
            silentStart += QpcNow() - silentPausedAt;
    }

    DWORD TotalSamples() const
    {
        if (format.wFormatTag == kFormatXboxAdpcm)
            return bytes / (kAdpcmBlock * Channels()) * kAdpcmSamplesPerBlock;
        return format.nBlockAlign ? bytes / format.nBlockAlign : 0;
    }

    DWORD SampleToByte(DWORD sample) const
    {
        if (format.wFormatTag == kFormatXboxAdpcm)
            return sample / kAdpcmSamplesPerBlock * kAdpcmBlock * Channels();
        return sample * format.nBlockAlign;
    }

    DWORD ByteToSample(DWORD offset) const
    {
        if (format.wFormatTag == kFormatXboxAdpcm)
            return offset / (kAdpcmBlock * Channels()) * kAdpcmSamplesPerBlock;
        return format.nBlockAlign ? offset / format.nBlockAlign : 0;
    }

    void Submit(DWORD fromSample)
    {
        if (!data || !bytes)
            return;
        if (!CreateSource()) {
            DWORD total = TotalSamples();
            silent = g_AudioFailed && total && format.nSamplesPerSec;
            if (silent) {
                startOffset = fromSample % total;
                silentStart = QpcNow();
                ended = 0;
            }
            return;
        }
        silent = false;
        source->Stop();
        source->FlushSourceBuffers();
        XAUDIO2_BUFFER b = {};
        const uint8_t* pcm = data;
        UINT32 pcmBytes = bytes;
        if (format.wFormatTag == kFormatXboxAdpcm) {
            DecodeAdpcm(data, bytes, Channels(), decoded);
            pcm = reinterpret_cast<const uint8_t*>(decoded.data());
            pcmBytes = UINT32(decoded.size() * sizeof(int16_t));
        }
        DWORD total = TotalSamples();
        if (!total)
            return;
        b.pAudioData = pcm;
        b.AudioBytes = pcmBytes;
        b.PlayBegin = fromSample % total;
        if (looping) {
            b.LoopCount = XAUDIO2_LOOP_INFINITE;
            b.LoopBegin = 0;
            b.LoopLength = 0;
        } else {
            b.Flags = XAUDIO2_END_OF_STREAM;
        }
        ended = 0;
        source->SubmitSourceBuffer(&b);
        XAUDIO2_VOICE_STATE st;
        source->GetState(&st);
        startSample = st.SamplesPlayed;
        startOffset = b.PlayBegin;
    }

    DWORD CurrentSample()
    {
        if (silent && playing) {
            DWORD total = std::max<DWORD>(1, TotalSamples());
            UINT64 position = startOffset + SilentSamplesPlayed();
            if (!looping && position >= total) {
                InterlockedExchange(&ended, 1);
                return 0;
            }
            return DWORD(position % total);
        }
        if (!source || !playing)
            return startOffset;
        XAUDIO2_VOICE_STATE st;
        source->GetState(&st);
        DWORD total = std::max<DWORD>(1, TotalSamples());
        return DWORD((startOffset + (st.SamplesPlayed - startSample)) % total);
    }

    void BufferEnded(void*) override
    {
        if (!looping)
            InterlockedExchange(&ended, 1);
    }

    bool IsPlaying()
    {
        if (silent && playing && !looping)
            CurrentSample(); // notes the end
        if (playing && ended) {
            // A one-shot buffer that played to its end rewinds, like on the Xbox.
            playing = false;
            startOffset = 0;
        }
        return playing;
    }
};

// --- Streams --------------------------------------------------------------------------

struct StreamPacket {
    MediaPacket packet;
    std::vector<int16_t> pcm;
};

struct Stream;
struct StreamVtbl {
    ULONG(__stdcall* AddRef)(Stream*);
    ULONG(__stdcall* Release)(Stream*);
    HRESULT(__stdcall* GetInfo)(Stream*, MediaInfo*);
    HRESULT(__stdcall* GetStatus)(Stream*, DWORD*);
    HRESULT(__stdcall* Process)(Stream*, MediaPacket*, MediaPacket*);
    HRESULT(__stdcall* Discontinuity)(Stream*);
    HRESULT(__stdcall* Flush)(Stream*);
};

struct PendingCallback {
    XmoCallback callback;
    void* streamContext;
    void* packetContext;
    DWORD status;
};
static std::mutex g_CallbackLock;
static std::vector<PendingCallback> g_Callbacks;

// The game calls the XMediaObject methods through this vtable, so it must be
// the object's first member; the Voice part follows at a fixed offset.
struct StreamHeader {
    const StreamVtbl* vtbl;
    Stream* self;
};

struct Stream : Voice {
    StreamHeader header;
    LONG refCount = 1;
    DWORD maxPackets = 1;
    XmoCallback callback = nullptr;
    void* context = nullptr;
    // Packets submitted to XAudio2, oldest first. Guarded by packetLock, which
    // is never held while calling into XAudio2 (voice callbacks take it).
    std::mutex packetLock;
    std::deque<std::unique_ptr<StreamPacket>> queued;
    bool started = false;

    size_t QueuedCount()
    {
        std::lock_guard<std::mutex> lock(packetLock);
        return queued.size();
    }

    void Complete(std::unique_ptr<StreamPacket> p, HRESULT status)
    {
        MediaPacket& m = p->packet;
        if (m.pdwCompletedSize)
            *m.pdwCompletedSize = status == XMP_STATUS_SUCCESS ? m.dwMaxSize : 0;
        if (m.pdwStatus)
            *m.pdwStatus = DWORD(status);
        if (callback) {
            std::lock_guard<std::mutex> lock(g_CallbackLock);
            g_Callbacks.push_back({ callback, context, m.pContext, DWORD(status) });
        } else if (m.hCompletionEvent) {
            SetEvent(m.hCompletionEvent);
        }
    }

    void BufferEnded(void* ctx) override
    {
        std::unique_ptr<StreamPacket> done;
        {
            std::lock_guard<std::mutex> lock(packetLock);
            for (auto it = queued.begin(); it != queued.end(); ++it) {
                if (it->get() == ctx) {
                    done = std::move(*it);
                    queued.erase(it);
                    break;
                }
            }
        }
        if (done)
            Complete(std::move(done), XMP_STATUS_SUCCESS);
    }

    void FlushAll()
    {
        if (source) {
            source->Stop();
            source->FlushSourceBuffers();
        }
        std::deque<std::unique_ptr<StreamPacket>> flushed;
        {
            std::lock_guard<std::mutex> lock(packetLock);
            flushed.swap(queued);
        }
        for (auto& p : flushed)
            Complete(std::move(p), XMP_STATUS_FLUSHED);
        started = false;
    }
};

static Stream* StreamFromThis(void* pThis)
{
    // `pThis` is the address of Stream::header as handed to the game.
    return reinterpret_cast<StreamHeader*>(pThis)->self;
}

static ULONG __stdcall StreamAddRef(Stream* h) { return InterlockedIncrement(&StreamFromThis(h)->refCount); }

static ULONG __stdcall StreamRelease(Stream* h)
{
    Stream* s = StreamFromThis(h);
    LONG n = InterlockedDecrement(&s->refCount);
    if (n == 0) {
        std::lock_guard<std::recursive_mutex> lock(g_Lock);
        s->FlushAll();
        g_Voices.erase(std::remove(g_Voices.begin(), g_Voices.end(), s), g_Voices.end());
        delete s;
    }
    return ULONG(n);
}

static HRESULT __stdcall StreamGetInfo(Stream* h, MediaInfo* info)
{
    Stream* s = StreamFromThis(h);
    info->dwFlags = XMO_STREAMF_FIXED_SAMPLE_SIZE | XMO_STREAMF_INPUT_ASYNC;
    info->dwInputSize = s->format.nBlockAlign ? s->format.nBlockAlign : 1;
    info->dwOutputSize = 0;
    info->dwMaxLookahead = s->format.nAvgBytesPerSec / 10;
    return S_OK;
}

static HRESULT __stdcall StreamGetStatus(Stream* h, DWORD* status)
{
    Stream* s = StreamFromThis(h);
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    DWORD st = 0;
    size_t queued = s->QueuedCount();
    if (queued < s->maxPackets)
        st |= DSSSTATUS_READY;
    if (s->paused)
        st |= DSSSTATUS_PAUSED;
    else if (queued)
        st |= DSSSTATUS_PLAYING;
    else if (s->started)
        st |= DSSSTATUS_STARVED;
    *status = st;
    return S_OK;
}

static HRESULT __stdcall StreamProcess(Stream* h, MediaPacket* input, MediaPacket* output)
{
    (void)output;
    Stream* s = StreamFromThis(h);
    if (!input)
        return E_INVALIDARG;
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    auto p = std::make_unique<StreamPacket>();
    p->packet = *input;
    if (input->pdwStatus)
        *input->pdwStatus = DWORD(XMP_STATUS_PENDING);
    if (input->pdwCompletedSize)
        *input->pdwCompletedSize = 0;
    if (!s->CreateSource()) {
        s->Complete(std::move(p), XMP_STATUS_SUCCESS);
        return S_OK;
    }

    XAUDIO2_BUFFER b = {};
    if (s->format.wFormatTag == kFormatXboxAdpcm) {
        DecodeAdpcm(static_cast<uint8_t*>(input->pvBuffer), input->dwMaxSize, s->Channels(), p->pcm);
        b.pAudioData = reinterpret_cast<const BYTE*>(p->pcm.data());
        b.AudioBytes = UINT32(p->pcm.size() * sizeof(int16_t));
    } else {
        b.pAudioData = static_cast<const BYTE*>(input->pvBuffer);
        b.AudioBytes = input->dwMaxSize;
    }
    if (b.AudioBytes == 0) {
        s->Complete(std::move(p), XMP_STATUS_SUCCESS);
        return S_OK;
    }
    StreamPacket* raw = p.get();
    b.pContext = raw;
    {
        std::lock_guard<std::mutex> plock(s->packetLock);
        s->queued.push_back(std::move(p));
    }
    if (FAILED(s->source->SubmitSourceBuffer(&b))) {
        s->BufferEnded(raw); // completes it immediately
        return S_OK;
    }
    if (!s->paused) {
        s->source->Start();
        s->started = true;
    }
    return S_OK;
}

static HRESULT __stdcall StreamDiscontinuity(Stream* h)
{
    (void)h;
    return S_OK; // queued packets simply play out
}

static HRESULT __stdcall StreamFlush(Stream* h)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    StreamFromThis(h)->FlushAll();
    return S_OK;
}

static const StreamVtbl kStreamVtbl = {
    StreamAddRef, StreamRelease, StreamGetInfo, StreamGetStatus, StreamProcess, StreamDiscontinuity, StreamFlush,
};

// --- DirectSound object -------------------------------------------------------------

struct DirectSound {
    LONG refCount = 1;
};
static DirectSound g_DirectSound;

static HRESULT __stdcall XbDirectSoundCreate(void* guid, DirectSound** ds, void* outer)
{
    (void)guid;
    (void)outer;
    EnsureEngine();
    InterlockedIncrement(&g_DirectSound.refCount);
    *ds = &g_DirectSound;
    return S_OK;
}

static ULONG __stdcall XbDirectSoundRelease(DirectSound* ds) { return InterlockedDecrement(&ds->refCount); }

static HRESULT CreateBuffer(const BufferDesc* desc, SoundBuffer** out)
{
    auto* b = new SoundBuffer();
    b->flags = desc->dwFlags;
    if (desc->lpwfxFormat)
        b->format = *desc->lpwfxFormat;
    if (desc->dwBufferBytes) {
        b->owned.reset(new uint8_t[desc->dwBufferBytes]());
        b->data = b->owned.get();
        b->bytes = desc->dwBufferBytes;
    }
    if (desc->lpMixBins)
        b->SetMixBins(desc->lpMixBins);
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    g_Voices.push_back(b);
    *out = b;
    return S_OK;
}

static HRESULT __stdcall XbCreateSoundBuffer(DirectSound* ds, const BufferDesc* desc, SoundBuffer** out, void* outer)
{
    (void)ds;
    (void)outer;
    return CreateBuffer(desc, out);
}

static HRESULT __stdcall XbDirectSoundCreateBuffer(const BufferDesc* desc, SoundBuffer** out)
{
    return CreateBuffer(desc, out);
}

static HRESULT __stdcall XbDirectSoundCreateStream(const StreamDesc* desc, void** out)
{
    auto* s = new Stream();
    s->header.vtbl = &kStreamVtbl;
    s->header.self = s;
    s->flags = desc->dwFlags;
    s->maxPackets = std::max<DWORD>(1, desc->dwMaxAttachedPackets);
    s->callback = desc->lpfnCallback;
    s->context = desc->lpvContext;
    if (desc->lpwfxFormat)
        s->format = *desc->lpwfxFormat;
    if (desc->lpMixBins)
        s->SetMixBins(desc->lpMixBins);
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    g_Voices.push_back(s);
    *out = &s->header;
    return S_OK;
}

static void Update3dVoices()
{
    for (Voice* v : g_Voices)
        if (v->flags & DSBCAPS_CTRL3D)
            v->ApplyMix();
}

static HRESULT __stdcall XbDsSetPosition(DirectSound*, float x, float y, float z, DWORD)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    g_Listener.position = { x, y, z };
    Update3dVoices();
    return S_OK;
}

static HRESULT __stdcall XbDsSetOrientation(DirectSound*, float xf, float yf, float zf, float xt, float yt, float zt, DWORD)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    g_Listener.front = { xf, yf, zf };
    g_Listener.top = { xt, yt, zt };
    Update3dVoices();
    return S_OK;
}

static HRESULT __stdcall XbDsSetVelocity(DirectSound*, float, float, float, DWORD) { return S_OK; }
static HRESULT __stdcall XbDsSetDistanceFactor(DirectSound*, float f, DWORD) { g_Listener.distanceFactor = f; return S_OK; }
static HRESULT __stdcall XbDsSetRolloffFactor(DirectSound*, float f, DWORD) { g_Listener.rolloffFactor = f; return S_OK; }
static HRESULT __stdcall XbDsSetDopplerFactor(DirectSound*, float f, DWORD) { g_Listener.dopplerFactor = f; return S_OK; }
static HRESULT __stdcall XbDsCommitDeferredSettings(DirectSound*) { return S_OK; }
static HRESULT __stdcall XbDsSetI3DL2Listener(DirectSound*, const void*, DWORD) { return S_OK; }
static HRESULT __stdcall XbDsEnableHeadphones(DirectSound*, BOOL) { return S_OK; }
static HRESULT __stdcall XbDsSetMixBinHeadroom(DirectSound*, DWORD, DWORD) { return S_OK; }

// DSP effect images (reverb etc. for the audio chip). Report an image with no
// effects; I3DL2 reverb is not reproduced yet.
struct EffectImageDesc {
    DWORD dwEffectCount;
    DWORD dwTotalScratchSize;
    DWORD maps[64 * 4];
};
static EffectImageDesc g_EffectImage = {};

static HRESULT __stdcall XbDsDownloadEffectsImage(DirectSound*, const void*, DWORD, const void*, EffectImageDesc** desc)
{
    if (desc)
        *desc = &g_EffectImage;
    return S_OK;
}

static HRESULT __stdcall XbXAudioDownloadEffectsImage(const char* name, void* location, DWORD flags, EffectImageDesc** desc)
{
    (void)location;
    (void)flags;
    LOG_INFO("XAudioDownloadEffectsImage('%s')", name ? name : "");
    if (desc)
        *desc = &g_EffectImage;
    return S_OK;
}

static DWORD __stdcall XbDirectSoundGetSampleTime()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return DWORD((now.QuadPart - g_StartQpc.QuadPart) * 48000 / std::max<LONGLONG>(1, g_QpcFreq.QuadPart));
}

static void __stdcall XbDirectSoundUseFullHRTF() {}

// Delivers stream completion callbacks on the calling (game) thread.
static void __stdcall XbDirectSoundDoWork()
{
    std::vector<PendingCallback> pending;
    {
        std::lock_guard<std::mutex> lock(g_CallbackLock);
        pending.swap(g_Callbacks);
    }
    for (const PendingCallback& c : pending)
        c.callback(c.streamContext, c.packetContext, c.status);
}

// --- IDirectSoundBuffer -------------------------------------------------------------

static ULONG __stdcall XbBufRelease(SoundBuffer* b)
{
    LONG n = InterlockedDecrement(&b->refCount);
    if (n == 0) {
        std::lock_guard<std::recursive_mutex> lock(g_Lock);
        g_Voices.erase(std::remove(g_Voices.begin(), g_Voices.end(), b), g_Voices.end());
        delete b;
    }
    return ULONG(n);
}

static HRESULT __stdcall XbBufSetBufferData(SoundBuffer* b, void* data, DWORD bytes)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    if (b->source)
        b->source->Stop();
    b->playing = false;
    b->owned.reset();
    b->data = static_cast<uint8_t*>(data);
    b->bytes = bytes;
    b->startOffset = 0;
    return S_OK;
}

static HRESULT __stdcall XbBufSetFormat(SoundBuffer* b, const XWaveFormat* format)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    if (b->source && (b->format.nChannels != format->nChannels || b->format.nSamplesPerSec != format->nSamplesPerSec ||
            b->format.wFormatTag != format->wFormatTag || b->format.wBitsPerSample != format->wBitsPerSample)) {
        b->source->DestroyVoice();
        b->source = nullptr;
    }
    b->format = *format;
    return S_OK;
}

static HRESULT __stdcall XbBufPlay(SoundBuffer* b, DWORD, DWORD, DWORD flags)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    bool loop = (flags & DSBPLAY_LOOPING) != 0;
    bool playing = b->IsPlaying();
    if (b->paused && playing && !(flags & DSBPLAY_FROMSTART)) {
        if (b->source)
            b->source->Start();
        else if (b->silent)
            b->SilentPause(false);
        b->paused = false;
        return S_OK;
    }
    if (playing && loop == b->looping && !(flags & DSBPLAY_FROMSTART))
        return S_OK;
    static const bool trace = GetEnvironmentVariableA("SWROTS_AUDIO_TRACE", nullptr, 0) != 0;
    if (trace && (b->flags & DSBCAPS_CTRL3D)) {
        const Ds3dBuffer& p = b->params3d;
        Vec3 rel = Sub(p.vPosition, g_Listener.position);
        LOG_INFO("3D play %p: pos %.1f %.1f %.1f listener %.1f %.1f %.1f dist %.1f mode %lu min %.1f max %.1f "
                 "rolloff %.2f vol %ld headroom %ld bins %zu",
            static_cast<void*>(b), p.vPosition.x, p.vPosition.y, p.vPosition.z, g_Listener.position.x,
            g_Listener.position.y, g_Listener.position.z, sqrtf(Dot(rel, rel)), p.dwMode, p.flMinDistance,
            p.flMaxDistance, p.flRolloffFactor, b->volume, b->headroom, b->mixBins.size());
    }
    DWORD from = (flags & DSBPLAY_FROMSTART) ? 0 : (playing ? b->CurrentSample() : b->startOffset);
    b->looping = loop;
    b->paused = false;
    b->Submit(from);
    if (b->source || b->silent) {
        if (b->source)
            b->source->Start();
        b->playing = true;
    }
    return S_OK;
}

static HRESULT __stdcall XbBufStop(SoundBuffer* b)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    if ((b->source || b->silent) && b->playing) {
        b->startOffset = b->CurrentSample();
        if (b->source) {
            b->source->Stop();
            b->source->FlushSourceBuffers();
        }
    }
    b->playing = false;
    b->paused = false;
    return S_OK;
}

static HRESULT __stdcall XbBufPause(SoundBuffer* b, DWORD pause)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    if (!b->source && !b->silent)
        return S_OK;
    if ((pause & 1) && !b->paused) {
        if (b->source)
            b->source->Stop();
        else
            b->SilentPause(true);
        b->paused = true;
    } else if (!(pause & 1) && b->paused) {
        if (b->source)
            b->source->Start();
        else
            b->SilentPause(false);
        b->paused = false;
    }
    return S_OK;
}

static HRESULT __stdcall XbBufGetStatus(SoundBuffer* b, DWORD* status)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    DWORD st = 0;
    if (b->IsPlaying()) {
        st |= DSBSTATUS_PLAYING;
        if (b->looping)
            st |= DSBSTATUS_LOOPING;
    }
    if (b->paused)
        st |= DSBSTATUS_PAUSED;
    *status = st;
    return S_OK;
}

static HRESULT __stdcall XbBufGetCurrentPosition(SoundBuffer* b, DWORD* play, DWORD* write)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    DWORD pos = b->SampleToByte(b->CurrentSample());
    if (play) *play = pos;
    if (write) *write = pos;
    return S_OK;
}

static HRESULT __stdcall XbBufSetCurrentPosition(SoundBuffer* b, DWORD offset)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    DWORD sample = b->ByteToSample(offset);
    if (b->IsPlaying()) {
        b->Submit(sample);
        if (b->source && !b->paused)
            b->source->Start();
        else if (b->silent && b->paused)
            b->silentPausedAt = b->silentStart; // resumes from the new position
    } else {
        b->startOffset = sample;
    }
    return S_OK;
}

static HRESULT __stdcall XbBufLock(SoundBuffer* b, DWORD offset, DWORD bytes, void** p1, DWORD* n1, void** p2, DWORD* n2,
    DWORD flags)
{
    (void)flags;
    if (!b->data || !b->bytes)
        return E_FAIL;
    offset %= b->bytes;
    bytes = std::min(bytes ? bytes : b->bytes, b->bytes);
    DWORD first = std::min(bytes, b->bytes - offset);
    *p1 = b->data + offset;
    *n1 = first;
    if (p2) *p2 = first < bytes ? b->data : nullptr;
    if (n2) *n2 = bytes - first;
    return S_OK;
}

static HRESULT __stdcall XbBufUnlock(SoundBuffer*, void*, DWORD, void*, DWORD) { return S_OK; }

template <typename T>
static void Apply(T* v)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    v->ApplyMix();
}

static HRESULT __stdcall XbBufSetVolume(SoundBuffer* b, LONG volume) { b->volume = volume; Apply(b); return S_OK; }
static HRESULT __stdcall XbBufSetHeadroom(SoundBuffer* b, DWORD headroom) { b->headroom = LONG(headroom); Apply(b); return S_OK; }
static HRESULT __stdcall XbBufSetFrequency(SoundBuffer* b, DWORD freq) { b->frequency = freq; Apply(b); return S_OK; }
static HRESULT __stdcall XbBufSetMixBins(SoundBuffer* b, const MixBins* bins)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    b->SetMixBins(bins);
    return S_OK;
}
static HRESULT __stdcall XbBufSetMixBinVolumes(SoundBuffer* b, const MixBins* bins)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    b->SetMixBinVolumes(bins);
    return S_OK;
}
static HRESULT __stdcall XbBufSetPosition(SoundBuffer* b, float x, float y, float z, DWORD)
{
    b->params3d.vPosition = { x, y, z };
    Apply(b);
    return S_OK;
}
static HRESULT __stdcall XbBufSetMinDistance(SoundBuffer* b, float d, DWORD) { b->params3d.flMinDistance = d; Apply(b); return S_OK; }
static HRESULT __stdcall XbBufSetMaxDistance(SoundBuffer* b, float d, DWORD) { b->params3d.flMaxDistance = d; Apply(b); return S_OK; }
template <typename T> static void SetRolloffCurve(T* v, const float* points, DWORD count)
{
    static int logged = 0;
    if (points && count && logged++ < 3) {
        std::string values;
        char num[16];
        for (DWORD i = 0; i < count && i < 16; ++i) {
            snprintf(num, sizeof(num), " %.3f", points[i]);
            values += num;
        }
        LOG_DEBUG("SetRolloffCurve (%lu points):%s", count, values.c_str());
    }
    if (points && count)
        v->rolloffCurve.assign(points, points + count);
    else
        v->rolloffCurve.clear();
}

static HRESULT __stdcall XbBufSetRolloffCurve(SoundBuffer* b, const float* points, DWORD count, DWORD)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    SetRolloffCurve(b, points, count);
    Apply(b);
    return S_OK;
}
static HRESULT __stdcall XbBufSetI3DL2Source(SoundBuffer*, const void*, DWORD) { return S_OK; }
static HRESULT __stdcall XbBufSetAllParameters(SoundBuffer* b, const Ds3dBuffer* params, DWORD)
{
    b->params3d = *params;
    Apply(b);
    return S_OK;
}

// --- IDirectSoundStream (non-XMO methods) ------------------------------------------------

static HRESULT __stdcall XbStrSetVolume(void* h, LONG volume) { Stream* s = StreamFromThis(h); s->volume = volume; Apply(s); return S_OK; }
static HRESULT __stdcall XbStrSetFrequency(void* h, DWORD freq) { Stream* s = StreamFromThis(h); s->frequency = freq; Apply(s); return S_OK; }
static HRESULT __stdcall XbStrSetMixBins(void* h, const MixBins* bins)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    StreamFromThis(h)->SetMixBins(bins);
    return S_OK;
}
static HRESULT __stdcall XbStrSetMixBinVolumes(void* h, const MixBins* bins)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    StreamFromThis(h)->SetMixBinVolumes(bins);
    return S_OK;
}
static HRESULT __stdcall XbStrSetPosition(void* h, float x, float y, float z, DWORD)
{
    Stream* s = StreamFromThis(h);
    s->params3d.vPosition = { x, y, z };
    Apply(s);
    return S_OK;
}
static HRESULT __stdcall XbStrSetMinDistance(void* h, float d, DWORD) { Stream* s = StreamFromThis(h); s->params3d.flMinDistance = d; Apply(s); return S_OK; }
static HRESULT __stdcall XbStrSetMaxDistance(void* h, float d, DWORD) { Stream* s = StreamFromThis(h); s->params3d.flMaxDistance = d; Apply(s); return S_OK; }
static HRESULT __stdcall XbStrSetRolloffCurve(void* h, const float* points, DWORD count, DWORD)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    Stream* s = StreamFromThis(h);
    SetRolloffCurve(s, points, count);
    Apply(s);
    return S_OK;
}
static HRESULT __stdcall XbStrSetFormat(void* h, const XWaveFormat* format)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    Stream* s = StreamFromThis(h);
    s->FlushAll();
    if (s->source) {
        s->source->DestroyVoice();
        s->source = nullptr;
    }
    s->format = *format;
    return S_OK;
}

static HRESULT __stdcall XbStrPause(void* h, DWORD pause)
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    Stream* s = StreamFromThis(h);
    s->paused = (pause & 1) != 0; // PAUSE (1) and PAUSENOACTIVATE (3)
    if (s->source) {
        if (s->paused)
            s->source->Stop();
        else if (s->QueuedCount()) {
            s->source->Start();
            s->started = true;
        }
    }
    return S_OK;
}

static HRESULT __stdcall XbStrFlushEx(void* h, LONGLONG timestamp, DWORD flags)
{
    (void)timestamp;
    (void)flags;
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    StreamFromThis(h)->FlushAll();
    return S_OK;
}

// --- XDK globals ----------------------------------------------------------------------------

// DirectSound's allocator (which runs natively) adds to usage counters through
// pointers its own initialisation would set up.
static DWORD g_PoolBytes = 0, g_PhysicalBytes = 0;

void InitXboxGlobals()
{
    *reinterpret_cast<DWORD**>(uintptr_t(0x0053C140)) = &g_PoolBytes;
    *reinterpret_cast<DWORD**>(uintptr_t(0x0053C13C)) = &g_PhysicalBytes;
}

// --- Registration -----------------------------------------------------------------------

SDK_REPLACE("DirectSoundCreate", XbDirectSoundCreate);
SDK_REPLACE("IDirectSound_Release", XbDirectSoundRelease);
SDK_REPLACE("IDirectSound_CreateSoundBuffer", XbCreateSoundBuffer);
SDK_REPLACE("DirectSoundCreateBuffer", XbDirectSoundCreateBuffer);
SDK_REPLACE("DirectSoundCreateStream", XbDirectSoundCreateStream);
SDK_REPLACE("IDirectSound_SetPosition", XbDsSetPosition);
SDK_REPLACE("IDirectSound_SetOrientation", XbDsSetOrientation);
SDK_REPLACE("IDirectSound_SetVelocity", XbDsSetVelocity);
SDK_REPLACE("IDirectSound_SetDistanceFactor", XbDsSetDistanceFactor);
SDK_REPLACE("IDirectSound_SetRolloffFactor", XbDsSetRolloffFactor);
SDK_REPLACE("IDirectSound_SetDopplerFactor", XbDsSetDopplerFactor);
SDK_REPLACE("IDirectSound_CommitDeferredSettings", XbDsCommitDeferredSettings);
SDK_REPLACE("IDirectSound_SetI3DL2Listener", XbDsSetI3DL2Listener);
SDK_REPLACE("IDirectSound_EnableHeadphones", XbDsEnableHeadphones);
SDK_REPLACE("IDirectSound_SetMixBinHeadroom", XbDsSetMixBinHeadroom);
SDK_REPLACE("IDirectSound_DownloadEffectsImage", XbDsDownloadEffectsImage);
SDK_REPLACE("XAudioDownloadEffectsImage", XbXAudioDownloadEffectsImage);
SDK_REPLACE("DirectSoundGetSampleTime", XbDirectSoundGetSampleTime);
SDK_REPLACE("DirectSoundUseFullHRTF", XbDirectSoundUseFullHRTF);
SDK_REPLACE("DirectSoundDoWork", XbDirectSoundDoWork);

SDK_REPLACE("IDirectSoundBuffer_Release", XbBufRelease);
SDK_REPLACE("IDirectSoundBuffer_SetBufferData", XbBufSetBufferData);
SDK_REPLACE("IDirectSoundBuffer_SetFormat", XbBufSetFormat);
SDK_REPLACE("IDirectSoundBuffer_Play", XbBufPlay);
SDK_REPLACE("IDirectSoundBuffer_Stop", XbBufStop);
SDK_REPLACE("IDirectSoundBuffer_Pause", XbBufPause);
SDK_REPLACE("IDirectSoundBuffer_GetStatus", XbBufGetStatus);
SDK_REPLACE("IDirectSoundBuffer_GetCurrentPosition", XbBufGetCurrentPosition);
SDK_REPLACE("IDirectSoundBuffer_SetCurrentPosition", XbBufSetCurrentPosition);
SDK_REPLACE("IDirectSoundBuffer_Lock", XbBufLock);
SDK_REPLACE("IDirectSoundBuffer_Unlock", XbBufUnlock);
SDK_REPLACE("IDirectSoundBuffer_SetVolume", XbBufSetVolume);
SDK_REPLACE("IDirectSoundBuffer_SetHeadroom", XbBufSetHeadroom);
SDK_REPLACE("IDirectSoundBuffer_SetFrequency", XbBufSetFrequency);
SDK_REPLACE("IDirectSoundBuffer_SetMixBins", XbBufSetMixBins);
SDK_REPLACE("IDirectSoundBuffer_SetMixBinVolumes_8", XbBufSetMixBinVolumes);
SDK_REPLACE("IDirectSoundBuffer_SetPosition", XbBufSetPosition);
SDK_REPLACE("IDirectSoundBuffer_SetMinDistance", XbBufSetMinDistance);
SDK_REPLACE("IDirectSoundBuffer_SetMaxDistance", XbBufSetMaxDistance);
SDK_REPLACE("IDirectSoundBuffer_SetRolloffCurve", XbBufSetRolloffCurve);
SDK_REPLACE("IDirectSoundBuffer_SetI3DL2Source", XbBufSetI3DL2Source);
SDK_REPLACE("IDirectSoundBuffer_SetAllParameters", XbBufSetAllParameters);

SDK_REPLACE("IDirectSoundStream_SetVolume", XbStrSetVolume);
SDK_REPLACE("IDirectSoundStream_SetFrequency", XbStrSetFrequency);
SDK_REPLACE("IDirectSoundStream_SetMixBins", XbStrSetMixBins);
SDK_REPLACE("IDirectSoundStream_SetMixBinVolumes_8", XbStrSetMixBinVolumes);
SDK_REPLACE("IDirectSoundStream_SetPosition", XbStrSetPosition);
SDK_REPLACE("IDirectSoundStream_SetMinDistance", XbStrSetMinDistance);
SDK_REPLACE("IDirectSoundStream_SetMaxDistance", XbStrSetMaxDistance);
SDK_REPLACE("IDirectSoundStream_SetRolloffCurve", XbStrSetRolloffCurve);
SDK_REPLACE("IDirectSoundStream_SetFormat", XbStrSetFormat);
SDK_REPLACE("IDirectSoundStream_Pause", XbStrPause);
SDK_REPLACE("IDirectSoundStream_FlushEx", XbStrFlushEx);

// Pure CPU parts of the XDK audio libraries (format helpers, file media
// objects, the software WMA decoder), run as-is.
SDK_PASSTHROUGH("DSound_CMemoryManager_PoolAlloc"); // generic pool allocator
SDK_PASSTHROUGH("DSound_CRefCount_AddRef");
SDK_PASSTHROUGH("DSound_CRefCount_Release");
SDK_PASSTHROUGH("XAudioCreatePcmFormat");
SDK_PASSTHROUGH("XAudioCreateAdpcmFormat");
SDK_PASSTHROUGH("DSOUND_sub_51D879"); // -> XAudioCreatePcmFormat
SDK_PASSTHROUGH("DSOUND_sub_51D87E"); // -> XAudioCreateAdpcmFormat
SDK_PASSTHROUGH("DSOUND_sub_51D7CA"); // returns 0
SDK_PASSTHROUGH("XFileCreateMediaObjectAsync");
SDK_PASSTHROUGH("DSOUND_sub_5274C4"); // file media object destructor
SDK_PASSTHROUGH("WMADEC_sub_53CC0D"); // WMA decoder media object factory
SDK_PASSTHROUGH("WMADEC_sub_540E34");

void Silence()
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    if (g_XAudio)
        g_XAudio->StopEngine();
}

void ResetForReboot()
{
    std::lock_guard<std::recursive_mutex> lock(g_Lock);
    if (g_XAudio)
        g_XAudio->StopEngine();
    const size_t voices = g_Voices.size();
    for (Voice* v : g_Voices)
        delete v; // destroys its XAudio2 voice
    g_Voices.clear();
    {
        std::lock_guard<std::mutex> callbacks(g_CallbackLock);
        g_Callbacks.clear();
    }
    g_DirectSound.refCount = 1;
    g_EffectImage = {};
    g_PoolBytes = g_PhysicalBytes = 0;
    if (g_XAudio)
        g_XAudio->StartEngine();
    LOG_INFO("Reboot: %zu sound buffers and streams released", voices);
}

} // namespace swrots::audio
