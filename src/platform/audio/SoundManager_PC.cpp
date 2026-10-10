#include "net/minecraft/src/SoundManager.h"
#include "platform/Log.h"

#if defined(NO_SOUND)


void *SoundManager::sndSystem = nullptr;
bool  SoundManager::loaded    = false;

SoundManager::SoundManager()
    : soundPoolSounds(), soundPoolStreaming(), soundPoolMusic(), soundVolume(0),
      options(nullptr), rand(), ticksBeforeMusic(0)
{
}

void SoundManager::loadSoundSettings(GameSettings *gamesettings) { options = gamesettings; loaded = false; }
void SoundManager::onSoundOptionsChanged() {}
void SoundManager::closeMinecraft() { loaded = false; }
void SoundManager::addSound(const jstring &s, const std::string &file) { soundPoolSounds.addSound(s, file); }
void SoundManager::addStreaming(const jstring &s, const std::string &file) { soundPoolStreaming.addSound(s, file); }
void SoundManager::addMusic(const jstring &s, const std::string &file) { soundPoolMusic.addSound(s, file); }
void SoundManager::playRandomMusicIfReady() {}
bool SoundManager::playMusicFileNow(const std::string &) { return false; }
void SoundManager::setListenerPosition(EntityLiving *, float) {}
void SoundManager::playStreaming(const jstring &, float, float, float, float, float) {}
void SoundManager::playSound(const jstring &, float, float, float, float, float) {}
void SoundManager::playSoundFX(const jstring &, float, float) {}
void SoundManager::tryToSetLibraryAndCodecs() {}


#else

#include "net/minecraft/src/SoundManager.h"

#include <cmath>
#include <iostream>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <cstdlib>

#ifdef VITA_PLATFORM
#include <chrono>
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <psp2/kernel/threadmgr.h>
#include "java/System.h"
#include "platform/Profiler.h"
#endif

#include "SDL.h"

#include "pc/external/stb_vorbis.h"
#include "platform/Resources.h"
#include "platform/audio/VorbisAssetOpen.h"
#include "pc/audio/PcMusicStream.h"

#include "net/minecraft/src/GameSettings.h"
#include "net/minecraft/src/EntityLiving.h"
#include "net/minecraft/src/MathHelper.h"
#include "net/minecraft/src/SoundPoolEntry.h"
#include "platform/audio/AudioAssetFormat.h"
#include "platform/audio/AudioSpatialization.h"

// ─── Internal audio mixer ────────────────────────────────────────────────────

static constexpr int SAMPLE_RATE   = 44100;
static constexpr int CHANNELS      = 2;      // stereo
static constexpr int MAX_SOUNDS    = 32;     // concurrent sound slots

enum class AudioBus
{
    Sound,
    Music,
    Streaming
};

// Shared with the sound cache, so playing a cached sound copies nothing.
using PcmBuffer = std::shared_ptr<const std::vector<float>>;

struct AudioChannel
{
    PcmBuffer pcm;             // interleaved L/R samples, normalised [-1,1]
    size_t pos     = 0;
    bool   active  = false;
    bool   looping = false;
    float  volume  = 1.0f;
    float  pitch   = 1.0f;   // resampling not yet implemented; stored for future use
    AudioBus bus   = AudioBus::Sound;
};

static SDL_AudioDeviceID s_device   = 0;
static AudioChannel      s_channels[MAX_SOUNDS];
static std::mutex        s_mutex;
static float             s_soundVolume   = 1.0f;
static float             s_musicVolume   = 1.0f;
static int               s_musicChannel  = -1; // index into s_channels, or -1
static int               s_streamingChannel = -1;
static AudioListenerState s_listener;

// Decode an OGG file to float PCM (mono → stereo interleaved, 44100 Hz)
static std::vector<float> decodeOgg(const std::string &path, float pitch)
{
    int channels, sampleRate;
    short *pcmShort = nullptr;
    int samples = platformDecodeVorbis(path, &channels, &sampleRate, &pcmShort);
    if (samples <= 0 || pcmShort == nullptr)
        return {};

    // Resample + convert to float stereo
    std::vector<float> out;
    out.reserve((size_t)samples * 2);
    for (int i = 0; i < samples; i++)
    {
        float l = pcmShort[i * channels + 0] / 32768.0f;
        float r = (channels > 1) ? pcmShort[i * channels + 1] / 32768.0f : l;
        out.push_back(l);
        out.push_back(r);
    }
    free(pcmShort);
    return out;
}

// Decode a WAV file to float PCM stereo
static std::vector<float> decodeWav(const std::string &path)
{
    SDL_AudioSpec spec;
    Uint8 *buf = nullptr;
    Uint32 len = 0;
    // SDL_LoadWAV_RW over the bytes so a pak entry loads the same way a loose
    // file does; PlatformResources::loadFile reads either.
    unsigned int fileBytes = 0;
    unsigned char *fileData = PlatformResources::loadFile(path, &fileBytes);
    if (fileData == nullptr)
        return {};
    SDL_RWops *rw = SDL_RWFromConstMem(fileData, (int)fileBytes);
    const bool loaded = rw != nullptr && SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len) != nullptr;
    std::free(fileData);
    if (!loaded)
        return {};

    SDL_AudioCVT cvt;
    SDL_BuildAudioCVT(&cvt,
        spec.format, spec.channels, spec.freq,
        AUDIO_F32SYS, CHANNELS, SAMPLE_RATE);

    std::vector<Uint8> cvtBuf(len * (cvt.len_mult ? cvt.len_mult : 1));
    std::memcpy(cvtBuf.data(), buf, len);
    SDL_FreeWAV(buf);

    cvt.buf = cvtBuf.data();
    cvt.len = (int)len;
    SDL_ConvertAudio(&cvt);

    size_t floatSamples = cvt.len_cvt / sizeof(float);
    std::vector<float> out(floatSamples);
    std::memcpy(out.data(), cvtBuf.data(), cvt.len_cvt);
    return out;
}

#ifdef VITA_PLATFORM
static std::unordered_map<std::string, PcmBuffer> s_soundCache;
static std::size_t s_soundCacheBytes = 0;
static std::mutex s_cacheMutex;
static constexpr std::size_t kSoundCacheLimitBytes = 32u * 1024u * 1024u;
static constexpr std::size_t kSoundCacheEntryLimitBytes = 2u * 1024u * 1024u;
#endif

static std::vector<float> loadSoundUncached(const std::string &path, float pitch);

#ifdef VITA_PLATFORM
static PcmBuffer cachedSound(const std::string &path)
{
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    auto cached = s_soundCache.find(path);
    return cached != s_soundCache.end() ? cached->second : PcmBuffer();
}
#endif

static PcmBuffer loadSound(const std::string &path, float pitch = 1.0f)
{
#ifdef VITA_PLATFORM
    if (PcmBuffer hit = cachedSound(path))
        return hit;
    PcmBuffer decoded = std::make_shared<const std::vector<float>>(loadSoundUncached(path, pitch));
    if (decoded->empty())
        return decoded;
    const std::size_t bytes = decoded->size() * sizeof(float);
    if (bytes > kSoundCacheEntryLimitBytes)
        return decoded;
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    if (s_soundCacheBytes + bytes > kSoundCacheLimitBytes)
    {
        s_soundCache.clear();
        s_soundCacheBytes = 0;
    }
    if (s_soundCache.emplace(path, decoded).second)
        s_soundCacheBytes += bytes;
    return decoded;
#else
    return std::make_shared<const std::vector<float>>(loadSoundUncached(path, pitch));
#endif
}

static std::vector<float> loadSoundUncached(const std::string &path, float pitch)
{
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".ogg")
        return decodeOgg(path, pitch);
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".wav")
        return decodeWav(path);
    return {};
}

// Play decoded PCM on the next free channel; returns channel index or -1
static int playPCM(PcmBuffer pcm, float volume, bool looping, AudioBus bus)
{
    if (!pcm || pcm->empty()) return -1;
    std::lock_guard<std::mutex> lock(s_mutex);
    for (int i = 0; i < MAX_SOUNDS; i++)
    {
        if (!s_channels[i].active)
        {
            s_channels[i].pcm     = std::move(pcm);
            s_channels[i].pos     = 0;
            s_channels[i].active  = true;
            s_channels[i].looping = looping;
            s_channels[i].volume  = volume;
            s_channels[i].bus     = bus;
            return i;
        }
    }
    return -1;
}

#ifdef VITA_PLATFORM
// ---- Off-thread sound decode -------------------------------------------------
// Decoding an uncached sound on the game thread froze that tick for tens of
// ms. It now decodes on a worker on cores 1-2 and starts when ready; one that
// would start more than kMaxPlayDelayMs late is dropped. Ambient sets are
// decoded ahead on the first sound request.
namespace
{
struct DecodeJob
{
    std::string path;
    float volume;
    bool play;
    long long requestedMs;
};
std::mutex s_jobMutex;
std::condition_variable s_jobCondition;
std::deque<DecodeJob> s_jobs;
std::unordered_set<std::string> s_jobPaths; // queued or decoding
std::thread s_decodeThread;
bool s_decodeThreadStarted = false;
bool s_prewarmQueued = false;
constexpr std::size_t kMaxQueuedPlays = 12;
constexpr long long kMaxPlayDelayMs = 600;
constexpr long long kMaxStepDelayMs = 100;

void decodeWorker()
{
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
                                         SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
    for (;;)
    {
        DecodeJob job;
        {
            std::unique_lock<std::mutex> lock(s_jobMutex);
            s_jobCondition.wait(lock, [] { return !s_jobs.empty(); });
            job = std::move(s_jobs.front());
            s_jobs.pop_front();
        }
        PcmBuffer pcm = loadSound(job.path);
        {
            std::lock_guard<std::mutex> lock(s_jobMutex);
            s_jobPaths.erase(job.path);
        }
        // A footstep heard late plays mid-jump; drop it instead, as a step
        // that is out of sync is worse than a missing one.
        const long long maxDelayMs = job.path.find("step") != std::string::npos ? kMaxStepDelayMs : kMaxPlayDelayMs;
        if (job.play && pcm && !pcm->empty() &&
            System::currentTimeMillis() - job.requestedMs <= maxDelayMs)
            playPCM(std::move(pcm), job.volume, false, AudioBus::Sound);
    }
}

// Called on the game thread. `play` jobs go to the front of the queue.
void queueDecode(const std::string &path, float volume, bool play)
{
    std::lock_guard<std::mutex> lock(s_jobMutex);
    if (s_jobPaths.count(path) != 0)
        return;
    if (play && s_jobs.size() >= kMaxQueuedPlays)
        return;
    if (!s_decodeThreadStarted)
    {
        s_decodeThread = std::thread(decodeWorker);
        s_decodeThread.detach();
        s_decodeThreadStarted = true;
    }
    s_jobPaths.insert(path);
    DecodeJob job{path, volume, play, System::currentTimeMillis()};
    if (play)
        s_jobs.push_front(std::move(job));
    else
        s_jobs.push_back(std::move(job));
    s_jobCondition.notify_one();
}
}
#endif

static void stopChannel(int idx, AudioBus expectedBus)
{
    if (idx < 0 || idx >= MAX_SOUNDS) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_channels[idx].active || s_channels[idx].bus != expectedBus)
        return;
    s_channels[idx].active = false;
    s_channels[idx].pcm.reset();
    s_channels[idx].pos = 0;
}

// SDL2 audio callback – mixes all active channels into float32 stereo
static void audioCallback(void * /*userdata*/, Uint8 *stream, int len)
{
    float *out   = reinterpret_cast<float *>(stream);
    int   frames = len / (int)(sizeof(float) * CHANNELS);

    std::memset(stream, 0, len);

    float musicVolume = 1.0f;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        musicVolume = s_musicVolume;
        for (auto &ch : s_channels)
        {
            if (!ch.active) continue;
            float busVolume = ch.bus == AudioBus::Music ? s_musicVolume : s_soundVolume;
            float vol = ch.volume * busVolume;

            const std::vector<float> &pcm = *ch.pcm;
            for (int f = 0; f < frames; f++)
            {
                if (ch.pos + 1 >= pcm.size())
                {
                    if (ch.looping)
                        ch.pos = 0;
                    else
                    {
                        ch.active = false;
                        ch.pcm.reset();
                        break;
                    }
                }
                out[f * 2 + 0] += pcm[ch.pos++] * vol;
                out[f * 2 + 1] += pcm[ch.pos++] * vol;
            }
        }
    }

    PcMusicStream::mix(out, frames, musicVolume);

    // Soft clip to [-1, 1]
    for (int i = 0; i < frames * CHANNELS; i++)
    {
        if      (out[i] >  1.0f) out[i] =  1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
}

// ─── SoundManager static members ─────────────────────────────────────────────

void *SoundManager::sndSystem = nullptr;
bool  SoundManager::loaded    = false;

// ─── SoundManager implementation ─────────────────────────────────────────────

SoundManager::SoundManager() :
    soundPoolSounds(),
    soundPoolStreaming(),
    soundPoolMusic(),
    soundVolume(0),
    options(nullptr),
    rand(),
    ticksBeforeMusic(rand.nextInt(100) + 40)
{
}

void SoundManager::loadSoundSettings(GameSettings *gamesettings)
{
    soundPoolStreaming.motionX = false;
    options = gamesettings;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_soundVolume = gamesettings ? gamesettings->soundVolume : 1.0f;
        s_musicVolume = gamesettings ? gamesettings->musicVolume : 1.0f;
    }

    if (!loaded && (gamesettings == nullptr || gamesettings->soundVolume != 0.0f || gamesettings->musicVolume != 0.0f))
        tryToSetLibraryAndCodecs();
}

void SoundManager::tryToSetLibraryAndCodecs()
{
    if (loaded) return;

    SDL_AudioSpec want{}, have{};
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = CHANNELS;
    want.samples  = 1024;
    want.callback = audioCallback;

    s_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (s_device == 0)
    {
        MC_LOG_ERROR("audio", "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(s_device, 0); // start playback
    loaded = true;
}

void SoundManager::onSoundOptionsChanged()
{
    if (!loaded && options && (options->soundVolume != 0.0f || options->musicVolume != 0.0f))
        tryToSetLibraryAndCodecs();

    if (!loaded) return;

    float musicVolume = options ? options->musicVolume : 1.0f;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_soundVolume = options ? options->soundVolume : 1.0f;
        s_musicVolume = musicVolume;
    }

    // Match Java: setting Music to zero stops only ambient background music.
    // Records/streaming remain on the Sound bus.
    if (musicVolume == 0.0f)
    {
        PcMusicStream::stop();
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
    }
}

void SoundManager::closeMinecraft()
{
    PcMusicStream::stop();
    if (s_device)
    {
        SDL_PauseAudioDevice(s_device, 1);
        SDL_CloseAudioDevice(s_device);
        s_device = 0;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto &ch : s_channels)
        {
            ch.active = false;
            ch.looping = false;
            ch.pos = 0;
            ch.pcm.reset();
        }
        s_musicChannel = -1;
        s_streamingChannel = -1;
    }

    loaded = false;
}

void SoundManager::addSound(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".ogg") || audioPathHasExtension(file, ".wav"))
        soundPoolSounds.addSound(s, file);
}

void SoundManager::addStreaming(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".ogg") || audioPathHasExtension(file, ".wav") || audioPathHasExtension(file, ".pcm"))
        soundPoolStreaming.addSound(s, file);
}

void SoundManager::addMusic(const jstring &s, const std::string &file)
{
    if (audioPathHasExtension(file, ".ogg") || audioPathHasExtension(file, ".wav") || audioPathHasExtension(file, ".pcm"))
        soundPoolMusic.addSound(s, file);
}

bool SoundManager::playMusicFileNow(const std::string &file)
{
    if (!loaded || !options || options->musicVolume == 0.0f)
        return false;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_streamingChannel >= 0 && s_channels[s_streamingChannel].active
            && s_channels[s_streamingChannel].bus == AudioBus::Streaming)
            return false;
    }

    if (PcMusicStream::active())
        return false;

    if (audioPathHasExtension(file, ".ogg") || audioPathHasExtension(file, ".pcm"))
    {
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        s_musicChannel = -1;
        if (!PcMusicStream::start(file))
            return false;
    }
    else
    {
        PcmBuffer pcm = loadSound(file);
        if (pcm->empty())
            return false;
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        const int channel = playPCM(std::move(pcm), 1.0f, false, AudioBus::Music);
        if (channel < 0)
            return false;
        s_musicChannel = channel;
    }

    ticksBeforeMusic = rand.nextInt(1200) + 600;
    return true;
}

void SoundManager::playRandomMusicIfReady()
{
    if (!loaded || !options || options->musicVolume == 0.0f)
        return;

    bool channelMusicPlaying = false;
    bool streamingPlaying = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        channelMusicPlaying = s_musicChannel >= 0 && s_channels[s_musicChannel].active
            && s_channels[s_musicChannel].bus == AudioBus::Music;
        streamingPlaying = s_streamingChannel >= 0 && s_channels[s_streamingChannel].active
            && s_channels[s_streamingChannel].bus == AudioBus::Streaming;
    }

    if (PcMusicStream::active() || channelMusicPlaying || streamingPlaying)
        return;

    if (ticksBeforeMusic > 0)
    {
        ticksBeforeMusic--;
        return;
    }

    SoundPoolEntry *entry = soundPoolMusic.getRandomSound();
    ticksBeforeMusic = rand.nextInt(1200) + 600;
    if (entry == nullptr)
        return;

    if (audioPathHasExtension(entry->soundUrl, ".ogg") || audioPathHasExtension(entry->soundUrl, ".pcm"))
    {
        PcMusicStream::start(entry->soundUrl);
        s_musicChannel = -1;
        return;
    }

    auto pcm = loadSound(entry->soundUrl);
    s_musicChannel = playPCM(std::move(pcm), 1.0f, false, AudioBus::Music);
}

void SoundManager::setListenerPosition(EntityLiving *entityliving, float f)
{
    if (!loaded || !options || options->soundVolume == 0.0f) return;
    updateAudioListener(s_listener, entityliving, f);
}

void SoundManager::playStreaming(const jstring &s, float f, float f1, float f2, float f3, float f4)
{
    if (!loaded || !options || (options->soundVolume == 0.0f && !s.empty())) return;

    // Java uses a distinct source named "streaming".
    if (s_streamingChannel >= 0)
        stopChannel(s_streamingChannel, AudioBus::Streaming);
    s_streamingChannel = -1;

    if (s.empty()) return;

    SoundPoolEntry *entry = soundPoolStreaming.getRandomSoundFromSoundPool(s);
    if (entry != nullptr && f3 > 0.0f)
    {
        const float attenuation = audioStreamingAttenuation(s_listener, f, f1, f2);
        if (attenuation <= 0.0f)
            return;

        // A record/stream temporarily replaces ambient background music.
        PcMusicStream::stop();
        if (s_musicChannel >= 0)
            stopChannel(s_musicChannel, AudioBus::Music);
        s_musicChannel = -1;
        ticksBeforeMusic = rand.nextInt(1200) + 600;

        auto pcm = loadSound(entry->soundUrl, f4);
        s_streamingChannel = playPCM(std::move(pcm), 0.5f * attenuation, false, AudioBus::Streaming);
    }
}

#ifdef VITA_PLATFORM
// First sound request after the pools are filled: decode the sets that fire
// during digging, walking and combat before they are asked for. step.*/dig.*
// are 4-6 short variants per surface; the rest are one-shots. Longer sounds
// (mob.*, ambient.*) stay on demand.
static void prewarmSoundPoolEntries(const SoundPool &pool)
{
    if (s_prewarmQueued)
        return;
    s_prewarmQueued = true;
    for (const auto &group : pool.nameToSoundPoolEntriesMapping)
    {
        const std::string &name = group.first;
        if (name.rfind("step.", 0) != 0 && name.rfind("dig.", 0) != 0 &&
            name != "random.pop" && name != "random.click" && name != "random.drr")
            continue;
        for (const SoundPoolEntry *entry : group.second)
            queueDecode(entry->soundUrl, 0.0f, false);
    }
}
#define prewarmSoundPool() prewarmSoundPoolEntries(soundPoolSounds)
#endif

void SoundManager::playSound(const jstring &s, float f, float f1, float f2, float f3, float f4)
{
#ifdef VITA_PLATFORM
    PlatformTickPhaseScope sndPhase("sndPlay"); // game-thread cost of a sound request
#endif
    if (!loaded || !options || options->soundVolume == 0.0f) return;

    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (entry != nullptr && f3 > 0.0f)
    {
        const float attenuation = audioSpatialAttenuation(s_listener, f, f1, f2, f3);
        if (attenuation <= 0.0f)
            return;

        const float vol = std::min(f3, 1.0f) * attenuation;
#ifdef VITA_PLATFORM
        prewarmSoundPool();
        PcmBuffer pcm = cachedSound(entry->soundUrl);
        if (!pcm)
        {
            queueDecode(entry->soundUrl, vol, true);
            return;
        }
        playPCM(std::move(pcm), vol, false, AudioBus::Sound);
#else
        auto pcm = loadSound(entry->soundUrl, f4);
        playPCM(std::move(pcm), vol, false, AudioBus::Sound);
#endif
    }
}

void SoundManager::playSoundFX(const jstring &s, float f, float f1)
{
#ifdef VITA_PLATFORM
    PlatformTickPhaseScope sndPhase("sndPlay");
#endif
    if (!loaded || !options || options->soundVolume == 0.0f) return;

    SoundPoolEntry *entry = soundPoolSounds.getRandomSoundFromSoundPool(s);
    if (entry != nullptr)
    {
        float vol = (f > 1.0f ? 1.0f : f) * 0.25f;
#ifdef VITA_PLATFORM
        prewarmSoundPool();
        PcmBuffer pcm = cachedSound(entry->soundUrl);
        if (!pcm)
        {
            queueDecode(entry->soundUrl, vol, true);
            return;
        }
        playPCM(std::move(pcm), vol, false, AudioBus::Sound);
#else
        auto pcm = loadSound(entry->soundUrl, f1);
        playPCM(std::move(pcm), vol, false, AudioBus::Sound);
#endif
    }
}

#endif
