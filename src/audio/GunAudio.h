#pragma once

#include <xaudio2.h>
#include <xaudio2fx.h>
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
//
// ---- Bus graph -------------------------------------------------------------
//
//   source voices --> category submix --> mastering voice
//                 \-> reverb submix   -/
//
// Category buses (weapons / voices / ambience / UI) exist so a whole class of
// sound can be mixed, ducked or muted without touching call sites, and so a
// master volume has one place to live. The reverb submix is what makes
// X3DAUDIO_CALCULATE_REVERB meaningful: it computes a send level, which needs
// somewhere to send to. Without this graph both that flag and CALCULATE_DELAY
// produce numbers nothing reads.
enum class AudioBus : int {
    Weapons = 0,   // gunfire, explosions, impacts
    Voices,        // enemy shouts, pain, death
    Ambience,      // footsteps, fire loops, rotor wash
    UI,            // menu and HUD feedback; never spatialised
    Music,         // menu and deployment score; never spatialised
    Count
};

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
        BuildBuses(s);
        return true;
    }

    static IXAudio2* Engine() { return Get().engine; }
    static UINT32 OutputChannels() { return Get().outputChannels; }
    static bool SpatialReady() { return Get().handleReady && Get().listenerValid; }
    static const X3DAUDIO_HANDLE& Handle() { return Get().handle; }
    static const X3DAUDIO_LISTENER& Listener() { return Get().listener; }

    static IXAudio2SubmixVoice* BusVoice(AudioBus bus) {
        const int index = static_cast<int>(bus);
        if (index < 0 || index >= static_cast<int>(AudioBus::Count))
            return nullptr;
        return Get().buses[index];
    }
    static IXAudio2SubmixVoice* ReverbVoice() { return Get().reverb; }
    static bool ReverbReady() { return Get().reverb != nullptr; }

    // ---- Mix control -------------------------------------------------------
    //
    // Master rides the mastering voice, so it scales everything including the
    // reverb tail. Per-bus volumes are independent of it and of each other.
    static void SetMasterVolume(float volume) {
        State& s = Get();
        s.masterVolume = Clamp01(volume);
        if (s.master) s.master->SetVolume(s.masterVolume);
    }
    static float MasterVolume() { return Get().masterVolume; }

    static void SetBusVolume(AudioBus bus, float volume) {
        const int index = static_cast<int>(bus);
        if (index < 0 || index >= static_cast<int>(AudioBus::Count)) return;
        State& s = Get();
        s.busVolume[index] = Clamp01(volume);
        if (s.buses[index]) s.buses[index]->SetVolume(s.busVolume[index]);
    }
    static float BusVolume(AudioBus bus) {
        const int index = static_cast<int>(bus);
        if (index < 0 || index >= static_cast<int>(AudioBus::Count)) return 0.0f;
        return Get().busVolume[index];
    }

    // Wet level of the reverb return. 0 leaves the graph intact but silent, so
    // reverb can be switched off without rebuilding anything.
    static void SetReverbVolume(float volume) {
        State& s = Get();
        s.reverbVolume = Clamp01(volume);
        if (s.reverb) s.reverb->SetVolume(s.reverbVolume);
    }
    static float ReverbVolume() { return Get().reverbVolume; }

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
    //
    // Order matters and is the reverse of construction: source voices are gone
    // by now, then the submixes that fed the master, then the master, then the
    // engine. Destroying a submix while something still routes to it is a
    // use-after-free inside XAudio2's mixer thread.
    static void Shutdown() {
        State& s = Get();
        for (IXAudio2SubmixVoice*& bus : s.buses) {
            if (bus) { bus->DestroyVoice(); bus = nullptr; }
        }
        if (s.reverb) { s.reverb->DestroyVoice(); s.reverb = nullptr; }
        if (s.master) { s.master->DestroyVoice(); s.master = nullptr; }
        if (s.engine) { s.engine->Release(); s.engine = nullptr; }
        s.handleReady = false;
        s.listenerValid = false;
        s.outputChannels = 0;
    }

private:
    static float Clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    struct State {
        IXAudio2* engine = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SubmixVoice* buses[static_cast<int>(AudioBus::Count)] = {};
        IXAudio2SubmixVoice* reverb = nullptr;
        X3DAUDIO_HANDLE handle = {};
        X3DAUDIO_LISTENER listener = {};
        UINT32 outputChannels = 0;
        float masterVolume = 1.0f;
        // One entry per AudioBus, in enum order. Music sits under the rest by
        // default: a score that competes with gunfire and callouts is a score
        // the player turns off.
        float busVolume[static_cast<int>(AudioBus::Count)] = {
            1.0f, 1.0f, 1.0f, 1.0f, 0.55f };
        // Conservative default: enough to place sounds in a space, not enough
        // to smear a rifle crack into a cathedral.
        float reverbVolume = 0.22f;
        bool handleReady = false;
        bool listenerValid = false;
        bool failed = false;
    };
    static State& Get() { static State state; return state; }

    // Reverb submix first, then the category buses. Built once, from Ensure.
    static void BuildBuses(State& s) {
        // The reverb submix runs at the mastering voice's channel count so its
        // output needs no rematrixing. ProcessingStage 1 puts it after the
        // category buses (stage 0), which is required: XAudio2 processes stages
        // in ascending order, and a reverb fed from a bus processed later would
        // be a frame behind its own input.
        if (SUCCEEDED(s.engine->CreateSubmixVoice(
                &s.reverb, s.outputChannels, 44100, 0, 1, nullptr, nullptr))) {
            XAUDIO2_EFFECT_DESCRIPTOR effect = {};
            IUnknown* reverbEffect = nullptr;
            if (SUCCEEDED(XAudio2CreateReverb(&reverbEffect, 0))) {
                effect.pEffect = reverbEffect;
                effect.InitialState = TRUE;
                effect.OutputChannels = s.outputChannels;
                XAUDIO2_EFFECT_CHAIN chain = {};
                chain.EffectCount = 1;
                chain.pEffectDescriptors = &effect;
                if (SUCCEEDED(s.reverb->SetEffectChain(&chain))) {
                    // A mid-sized outdoor-ish space. The island is mostly open,
                    // so a short decay reads as air rather than as a room.
                    XAUDIO2FX_REVERB_PARAMETERS params = {};
                    XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3dl2 =
                        XAUDIO2FX_I3DL2_PRESET_STONECORRIDOR;
                    ReverbConvertI3DL2ToNative(&i3dl2, &params);
                    s.reverb->SetEffectParameters(0, &params, sizeof(params));
                }
                // SetEffectChain AddRefs the effect; release our reference
                // whether or not it was accepted.
                reverbEffect->Release();
            }
            s.reverb->SetVolume(s.reverbVolume);
        }

        // Category buses feed the master directly, and additionally send to the
        // reverb when it exists. Each source voice picks its bus at creation.
        for (int i = 0; i < static_cast<int>(AudioBus::Count); ++i) {
            IXAudio2SubmixVoice* bus = nullptr;
            if (FAILED(s.engine->CreateSubmixVoice(
                    &bus, s.outputChannels, 44100, 0, 0, nullptr, nullptr)))
                continue;
            // Category buses stay DRY -- they feed the master only. The reverb
            // send is made per source voice instead (see CreateRoutedVoice),
            // because X3DAudio computes a different wet level for every emitter
            // based on its distance. Sending the whole bus as well would apply
            // reverb twice and flatten that per-source difference.
            bus->SetVolume(s.busVolume[i]);
            s.buses[i] = bus;
        }
    }
};

#define STB_VORBIS_HEADER_ONLY
#include "../../thirdparty/stb/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
#include "../../thirdparty/dr_libs/dr_mp3.h"

// Small overlapping-voice player for short PCM/float WAV effects.
class GunAudio {
public:
    // Upper bound on Play()'s gain. Unity is the normal ceiling; values above
    // it amplify, for sources authored quieter than the rest of the mix.
    static constexpr float kMaxPlayGain = 4.0f;

    ~GunAudio() { Shutdown(); }

    // `bus` decides which category submix this effect's voices feed, and so
    // which volume slider moves it. Weapons is the default because most of the
    // effects in this game are gunfire, impacts and explosions.
    bool Initialize(const std::string& relativePath,
                    AudioBus bus = AudioBus::Weapons) {
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
        bus_ = bus;
        std::cout << "Audio loaded: " << path << "\n";
        return true;
    }

    void Play(float volume = 1.0f, float pitch = 1.0f) {
        if (!AudioDevice::Engine() || samples_.empty()) return;
        ReclaimFinished();
        IXAudio2SourceVoice* voice = CreateRoutedVoice(0);
        if (!voice) return;

        XAUDIO2_BUFFER buffer = {};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(samples_.size());
        buffer.pAudioData = samples_.data();
        // Ceiling above unity so a quiet source can be lifted deliberately.
        // XAudio2 treats gain > 1 as amplification, which clips if the sample
        // already peaks near full scale -- kMaxPlayGain keeps that within a
        // range quiet dialogue survives rather than leaving it unbounded.
        voice->SetVolume((std::max)(0.0f, (std::min)(kMaxPlayGain, volume)));
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
        // Whether this voice carries a reverb send has to be decided before it
        // is created -- XAUDIO2_VOICE_SENDS is fixed at creation.
        const bool wetPath = AudioDevice::ReverbReady() &&
                             AudioDevice::BusVoice(bus_) != nullptr;
        // USEFILTER is required for the low-pass X3DAudio computes below; a
        // voice created without it silently ignores SetFilterParameters.
        IXAudio2SourceVoice* voice =
            CreateRoutedVoice(XAUDIO2_VOICE_USEFILTER, wetPath);
        if (!voice) return;

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
        //   REVERB      -- send level into the reverb submix, rising with
        //                  distance so far sources sit back in the space
        //                  instead of sitting on top of the listener. This is
        //                  only meaningful now that a reverb bus exists to
        //                  receive it.
        //
        // DELAY is still not requested: it yields an interaural delay pair that
        // needs a per-voice delay effect to consume, which this graph does not
        // have. The matrix already carries the level difference between ears.
        UINT32 calculateFlags = X3DAUDIO_CALCULATE_MATRIX |
                                X3DAUDIO_CALCULATE_LPF_DIRECT;
        if (wetPath) calculateFlags |= X3DAUDIO_CALCULATE_REVERB;
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
        // attenuation from the curve above as well. Send 0 is the dry path into
        // the category bus (see CreateRoutedVoice).
        voice->SetOutputMatrix(AudioDevice::BusVoice(bus_), 1, channels, matrix);

        // Send 1 is the reverb. ReverbLevel rises with distance, so a far shot
        // arrives mostly as room rather than as direct sound. Applied as a flat
        // per-channel gain -- the reverb submix does its own spatial smearing,
        // and panning the wet signal as well would undo that.
        if (wetPath) {
            float wetMatrix[8] = {};
            const float wet = (std::max)(0.0f, (std::min)(1.0f, dsp.ReverbLevel));
            for (UINT32 c = 0; c < channels; ++c) wetMatrix[c] = wet;
            voice->SetOutputMatrix(AudioDevice::ReverbVoice(), 1, channels,
                                   wetMatrix);
        }
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
            loopVoice_ = CreateRoutedVoice(0);
            if (!loopVoice_) return;
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
    // Creates a source voice already routed to this effect's category bus.
    // Every voice in this class goes through here so nothing can accidentally
    // bypass the graph and play straight into the master.
    // `wet` adds a direct send to the reverb submix alongside the category bus,
    // so PlayAt can drive the per-source reverb level X3DAudio computes. A dry
    // voice routes to its bus only.
    IXAudio2SourceVoice* CreateRoutedVoice(UINT32 flags, bool wet = false) const {
        IXAudio2* engine = AudioDevice::Engine();
        if (!engine) return nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        IXAudio2SubmixVoice* bus = AudioDevice::BusVoice(bus_);
        if (bus) {
            // Send 0 is the dry path, send 1 the reverb. PlayAt sets the level
            // on send 1 by index, so this order is part of the contract.
            XAUDIO2_SEND_DESCRIPTOR sends[2] = {
                { 0, bus }, { 0, AudioDevice::ReverbVoice() } };
            const UINT32 sendCount =
                (wet && AudioDevice::ReverbReady()) ? 2u : 1u;
            XAUDIO2_VOICE_SENDS sendList = { sendCount, sends };
            if (FAILED(engine->CreateSourceVoice(&voice, &format_, flags, 2.0f,
                                                 nullptr, &sendList)))
                return nullptr;
            return voice;
        }
        // No bus (device came up before the graph, or creation failed): fall
        // back to the default routing rather than dropping the sound.
        if (FAILED(engine->CreateSourceVoice(&voice, &format_, flags, 2.0f)))
            return nullptr;
        return voice;
    }

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
    AudioBus bus_ = AudioBus::Weapons;
    std::vector<uint8_t> samples_;
    std::vector<IXAudio2SourceVoice*> voices_;
    IXAudio2SourceVoice* loopVoice_ = nullptr;
};
