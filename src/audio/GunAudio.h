#pragma once

#include <xaudio2.h>
#include <x3daudio.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// One XAudio2 device, mastering voice and X3DAudio handle for the whole
// process, plus the listener every positional sound is mixed against.
//
// Each GunAudio used to create its own engine and mastering voice -- 16 devices
// for 16 sound effects, each with its own mixer thread and its own submix of the
// same output. They now all share this one. A GunAudio is just decoded samples
// and the voices playing them.
//
// Everything is left-handed to match the rest of the engine.
class AudioDevice {
public:
    // Brought up on the first Initialize; safe to call repeatedly.
    static bool Ensure() {
        State& s = Get();
        if (s.engine) return true;
        if (s.failed) return false;
        if (FAILED(XAudio2Create(&s.engine)) ||
            FAILED(s.engine->CreateMasteringVoice(&s.master))) {
            if (s.engine) { s.engine->Release(); s.engine = nullptr; }
            s.master = nullptr;
            s.failed = true;
            std::cerr << "XAudio2 initialization failed\n";
            return false;
        }
        XAUDIO2_VOICE_DETAILS details = {};
        s.master->GetVoiceDetails(&details);
        s.outputChannels = details.InputChannels;
        DWORD mask = 0;
        if (SUCCEEDED(s.master->GetChannelMask(&mask)) && mask != 0) {
            X3DAudioInitialize(mask, X3DAUDIO_SPEED_OF_SOUND, s.handle);
            s.handleReady = true;
        }
        return true;
    }

    static IXAudio2* Engine() { return Get().engine; }
    static UINT32 OutputChannels() { return Get().outputChannels; }
    static bool SpatialReady() { return Get().handleReady && Get().listenerValid; }
    static const X3DAUDIO_HANDLE& Handle() { return Get().handle; }
    static const X3DAUDIO_LISTENER& Listener() { return Get().listener; }

    // Written once per frame from the camera.
    static void SetListener(const float position[3], const float front[3],
                            const float up[3]) {
        State& s = Get();
        s.listener.Position = { position[0], position[1], position[2] };
        s.listener.OrientFront = { front[0], front[1], front[2] };
        s.listener.OrientTop = { up[0], up[1], up[2] };
        // Velocity stays zero: doppler on a walking player is inaudible, and a
        // teleporting camera (level load, leaving a vehicle) would produce a
        // violent pitch sweep if it were derived from position deltas.
        s.listener.Velocity = { 0.0f, 0.0f, 0.0f };
        s.listenerValid = true;
    }

    // Called once at shutdown, after every GunAudio has released its voices.
    static void Shutdown() {
        State& s = Get();
        if (s.master) { s.master->DestroyVoice(); s.master = nullptr; }
        if (s.engine) { s.engine->Release(); s.engine = nullptr; }
        s.handleReady = false;
        s.listenerValid = false;
        s.outputChannels = 0;
    }

private:
    struct State {
        IXAudio2* engine = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        X3DAUDIO_HANDLE handle = {};
        X3DAUDIO_LISTENER listener = {};
        UINT32 outputChannels = 0;
        bool handleReady = false;
        bool listenerValid = false;
        bool failed = false;
    };
    static State& Get() { static State state; return state; }
};

#define STB_VORBIS_HEADER_ONLY
#include "../../thirdparty/stb/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
#include "../../thirdparty/dr_libs/dr_mp3.h"

// Small overlapping-voice player for short PCM/float WAV effects.
class GunAudio {
public:
    ~GunAudio() { Shutdown(); }

    bool Initialize(const std::string& relativePath) {
        Shutdown();
        const std::string path = Resolve(relativePath);
        if (!LoadAudio(path)) {
            std::cerr << "Audio unavailable: " << path << "\n";
            return false;
        }
        if (!AudioDevice::Ensure()) {
            Shutdown();
            return false;
        }
        std::cout << "Audio loaded: " << path << "\n";
        return true;
    }

    void Play(float volume = 1.0f, float pitch = 1.0f) {
        IXAudio2* engine = AudioDevice::Engine();
        if (!engine || samples_.empty()) return;
        ReclaimFinished();
        IXAudio2SourceVoice* voice = nullptr;
        if (FAILED(engine->CreateSourceVoice(&voice, &format_, 0, 2.0f))) return;

        XAUDIO2_BUFFER buffer = {};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(samples_.size());
        buffer.pAudioData = samples_.data();
        voice->SetVolume((std::max)(0.0f, (std::min)(1.0f, volume)));
        voice->SetFrequencyRatio((std::max)(0.5f, (std::min)(2.0f, pitch)));
        if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
            voice->DestroyVoice();
            return;
        }
        voices_.push_back(voice);
    }

    // Positional one-shot. `x/y/z` is the world position of the sound; the
    // listener is whatever AudioListener was last given.
    //
    // `volume` is a pre-attenuation gain, so existing per-event mixing (a
    // silenced rifle being quieter than a loud one) still applies on top of the
    // distance falloff X3DAudio computes.
    //
    // `maxDistance` is where the sound reaches silence. X3DAudio's default curve
    // is inverse-square from the CurveDistanceScaler, which drops off far too
    // fast for gunfire meant to be heard across a firefight, so a custom linear
    // rolloff is supplied instead -- see kRolloff below.
    void PlayAt(float x, float y, float z, float volume = 1.0f,
                float pitch = 1.0f, float maxDistance = 60.0f) {
        IXAudio2* engine = AudioDevice::Engine();
        if (!engine || samples_.empty()) return;
        // No listener yet (menu, level load): fall back to a flat 2D play so a
        // sound is never silently dropped.
        if (!AudioDevice::SpatialReady() || AudioDevice::OutputChannels() == 0) {
            Play(volume, pitch);
            return;
        }
        ReclaimFinished();
        IXAudio2SourceVoice* voice = nullptr;
        // USEFILTER is required for the low-pass X3DAudio computes below; a
        // voice created without it silently ignores SetFilterParameters.
        if (FAILED(engine->CreateSourceVoice(&voice, &format_,
                                             XAUDIO2_VOICE_USEFILTER, 2.0f)))
            return;

        X3DAUDIO_EMITTER emitter = {};
        emitter.ChannelCount = 1;   // treated as a point source regardless of
                                    // the asset's own channel count
        emitter.CurveDistanceScaler = (std::max)(1.0f, maxDistance);
        emitter.DopplerScaler = 0.0f;
        emitter.Position = { x, y, z };
        emitter.OrientFront = { 0.0f, 0.0f, 1.0f };
        emitter.OrientTop = { 0.0f, 1.0f, 0.0f };
        emitter.Velocity = { 0.0f, 0.0f, 0.0f };
        // Without an inner radius X3DAudio treats the source as an infinitely
        // small point, and a sound passing near the listener snaps hard from one
        // speaker to the other. A small radius makes the pan sweep smoothly
        // through the middle instead, which is most of what "more 3D" means for
        // sources that move past you.
        emitter.InnerRadius = 2.0f;
        emitter.InnerRadiusAngle = X3DAUDIO_PI / 4.0f;
        // Linear-ish rolloff: full volume very close, then a steady fade to zero
        // at the scaler distance. Gunfire that vanished at 15 m made a firefight
        // feel small, which is what the default inverse-square curve produced.
        static const X3DAUDIO_DISTANCE_CURVE_POINT kRolloffPoints[] = {
            { 0.0f, 1.0f }, { 0.12f, 0.85f }, { 0.35f, 0.5f },
            { 0.7f, 0.18f }, { 1.0f, 0.0f }
        };
        static X3DAUDIO_DISTANCE_CURVE kRolloff = {
            const_cast<X3DAUDIO_DISTANCE_CURVE_POINT*>(kRolloffPoints),
            static_cast<UINT32>(std::size(kRolloffPoints))
        };
        emitter.pVolumeCurve = &kRolloff;

        // Matrix is [emitter channels] x [output channels]; emitter is mono.
        float matrix[8] = {};
        const UINT32 channels = (std::min)(AudioDevice::OutputChannels(), 8u);
        X3DAUDIO_DSP_SETTINGS dsp = {};
        dsp.SrcChannelCount = 1;
        dsp.DstChannelCount = channels;
        dsp.pMatrixCoefficients = matrix;

        // MATRIX alone only pans and attenuates, which is why distant sounds
        // read as "the same sound, quieter" rather than as far away. The extra
        // flags are what make a position audible:
        //
        //   LPF_DIRECT  -- air absorbs high frequencies over distance, and the
        //                  head shadows sounds arriving from behind. X3DAudio
        //                  returns a cutoff coefficient for both, so a shot
        //                  across the map goes dull instead of just quiet.
        //
        // REVERB and DELAY are deliberately NOT requested. Each only produces a
        // number for a submix to consume -- a reverb send level and an
        // interaural delay pair -- and this engine has no submix graph. Asking
        // for them would compute values nothing reads.
        const UINT32 calculateFlags = X3DAUDIO_CALCULATE_MATRIX |
                                      X3DAUDIO_CALCULATE_LPF_DIRECT;
        X3DAudioCalculate(AudioDevice::Handle(), &AudioDevice::Listener(),
                          &emitter, calculateFlags, &dsp);

        XAUDIO2_BUFFER buffer = {};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(samples_.size());
        buffer.pAudioData = samples_.data();
        voice->SetVolume((std::max)(0.0f, (std::min)(1.0f, volume)));
        voice->SetFrequencyRatio((std::max)(0.5f, (std::min)(2.0f, pitch)));

        // Distance/direction low-pass. LPFDirectCoefficient is 1 for a source
        // right on top of the listener and falls toward 0 as it recedes or moves
        // behind, which is exactly the cutoff a one-pole filter wants.
        //
        // Floored so a distant sound goes muffled rather than inaudible -- the
        // raw coefficient approaches zero at the edge of the curve, which
        // filters the sound out entirely instead of just dulling it.
        XAUDIO2_FILTER_PARAMETERS filter = {};
        filter.Type = LowPassFilter;
        filter.Frequency =
            (std::max)(0.28f, (std::min)(1.0f, dsp.LPFDirectCoefficient));
        filter.OneOverQ = 1.0f;
        voice->SetFilterParameters(&filter);

        // The panning matrix is what places the sound; it carries the distance
        // attenuation from the curve above as well.
        voice->SetOutputMatrix(nullptr, 1, channels, matrix);
        if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
            voice->DestroyVoice();
            return;
        }
        voices_.push_back(voice);
    }

    void SetLoop(bool enabled, float volume = 1.0f, float pitch = 1.0f) {
        IXAudio2* engine = AudioDevice::Engine();
        if (!enabled || !engine || samples_.empty()) {
            StopLoop();
            return;
        }
        if (!loopVoice_) {
            if (FAILED(engine->CreateSourceVoice(&loopVoice_, &format_, 0, 2.0f))) {
                loopVoice_ = nullptr;
                return;
            }
            XAUDIO2_BUFFER buffer = {};
            buffer.Flags = XAUDIO2_END_OF_STREAM;
            buffer.AudioBytes = static_cast<UINT32>(samples_.size());
            buffer.pAudioData = samples_.data();
            buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
            if (FAILED(loopVoice_->SubmitSourceBuffer(&buffer)) ||
                FAILED(loopVoice_->Start())) {
                loopVoice_->DestroyVoice();
                loopVoice_ = nullptr;
                return;
            }
        }
        loopVoice_->SetVolume((std::max)(0.0f, (std::min)(1.0f, volume)));
        loopVoice_->SetFrequencyRatio((std::max)(0.5f, (std::min)(2.0f, pitch)));
    }

    void StopLoop() {
        if (!loopVoice_) return;
        loopVoice_->Stop();
        loopVoice_->DestroyVoice();
        loopVoice_ = nullptr;
    }

    void Update() { ReclaimFinished(); }

    // Releases this effect's voices only. The device itself is shared, so it
    // outlives any single GunAudio -- AudioDevice::Shutdown tears it down once,
    // after every effect has been shut down.
    void Shutdown() {
        StopLoop();
        for (IXAudio2SourceVoice* voice : voices_) voice->DestroyVoice();
        voices_.clear();
        samples_.clear();
        format_ = {};
    }

private:
    static std::string Resolve(const std::string& relative) {
        for (const std::string& path : { relative, "build/" + relative,
                                        "../" + relative, "../../build/" + relative })
            if (std::filesystem::exists(path)) return path;
        return relative;
    }

    static uint32_t ReadU32(std::ifstream& file) {
        uint32_t value = 0;
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
        return value;
    }

    bool LoadAudio(const std::string& path) {
        const std::string extension = std::filesystem::path(path).extension().string();
        if (extension == ".ogg") return LoadOgg(path);
        if (extension == ".mp3") return LoadMp3(path);
        return LoadWav(path);
    }

    bool LoadMp3(const std::string& path) {
        drmp3_config config = {};
        drmp3_uint64 frameCount = 0;
        drmp3_int16* decoded = drmp3_open_file_and_read_pcm_frames_s16(
            path.c_str(), &config, &frameCount, nullptr);
        if (!decoded || frameCount == 0 || config.channels == 0 || config.sampleRate == 0)
            return false;

        const size_t byteCount = static_cast<size_t>(frameCount) *
                                 static_cast<size_t>(config.channels) * sizeof(drmp3_int16);
        samples_.assign(reinterpret_cast<const uint8_t*>(decoded),
                        reinterpret_cast<const uint8_t*>(decoded) + byteCount);
        drmp3_free(decoded, nullptr);

        format_ = {};
        format_.wFormatTag = WAVE_FORMAT_PCM;
        format_.nChannels = static_cast<WORD>(config.channels);
        format_.nSamplesPerSec = static_cast<DWORD>(config.sampleRate);
        format_.wBitsPerSample = 16;
        format_.nBlockAlign = static_cast<WORD>(config.channels * sizeof(drmp3_int16));
        format_.nAvgBytesPerSec = format_.nSamplesPerSec * format_.nBlockAlign;
        return true;
    }

    bool LoadOgg(const std::string& path) {
        int channels = 0, sampleRate = 0;
        short* decoded = nullptr;
        const int samplesPerChannel = stb_vorbis_decode_filename(
            path.c_str(), &channels, &sampleRate, &decoded);
        if (samplesPerChannel <= 0 || channels <= 0 || !decoded) return false;

        const size_t byteCount = static_cast<size_t>(samplesPerChannel) *
                                 static_cast<size_t>(channels) * sizeof(short);
        samples_.assign(reinterpret_cast<const uint8_t*>(decoded),
                        reinterpret_cast<const uint8_t*>(decoded) + byteCount);
        free(decoded);

        format_ = {};
        format_.wFormatTag = WAVE_FORMAT_PCM;
        format_.nChannels = static_cast<WORD>(channels);
        format_.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        format_.wBitsPerSample = 16;
        format_.nBlockAlign = static_cast<WORD>(channels * sizeof(short));
        format_.nAvgBytesPerSec = format_.nSamplesPerSec * format_.nBlockAlign;
        return true;
    }

    bool LoadWav(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        char id[4] = {};
        uint32_t riffSize = 0;
        char wave[4] = {};
        if (!file.read(id, 4) || std::string(id, 4) != "RIFF") return false;
        riffSize = ReadU32(file); (void)riffSize;
        if (!file.read(wave, 4) || std::string(wave, 4) != "WAVE") return false;

        bool haveFormat = false, haveData = false;
        while (file && (!haveFormat || !haveData)) {
            if (!file.read(id, 4)) break;
            const uint32_t size = ReadU32(file);
            if (std::string(id, 4) == "fmt ") {
                format_ = {};
                const uint32_t bytes = (std::min)(size, static_cast<uint32_t>(sizeof(format_)));
                file.read(reinterpret_cast<char*>(&format_), bytes);
                if (size > bytes) file.seekg(size - bytes, std::ios::cur);
                if (size == 16) format_.cbSize = 0;
                haveFormat = file.good();
            } else if (std::string(id, 4) == "data") {
                samples_.resize(size);
                file.read(reinterpret_cast<char*>(samples_.data()), size);
                haveData = file.good();
            } else {
                file.seekg(size, std::ios::cur);
            }
            if (size & 1u) file.seekg(1, std::ios::cur);
        }
        return haveFormat && haveData &&
            (format_.wFormatTag == WAVE_FORMAT_PCM ||
             format_.wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    }

    void ReclaimFinished() {
        auto it = voices_.begin();
        while (it != voices_.end()) {
            XAUDIO2_VOICE_STATE state = {};
            (*it)->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued == 0) {
                (*it)->DestroyVoice();
                it = voices_.erase(it);
            } else {
                ++it;
            }
        }
    }

    WAVEFORMATEX format_ = {};
    std::vector<uint8_t> samples_;
    std::vector<IXAudio2SourceVoice*> voices_;
    IXAudio2SourceVoice* loopVoice_ = nullptr;
};
