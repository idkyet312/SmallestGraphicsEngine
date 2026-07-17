#pragma once

#include <xaudio2.h>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define STB_VORBIS_HEADER_ONLY
#include "../thirdparty/stb/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
#include "../thirdparty/dr_libs/dr_mp3.h"

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
        HRESULT hr = XAudio2Create(&engine_);
        if (FAILED(hr) || FAILED(engine_->CreateMasteringVoice(&master_))) {
            Shutdown();
            std::cerr << "XAudio2 initialization failed\n";
            return false;
        }
        std::cout << "Audio loaded: " << path << "\n";
        return true;
    }

    void Play(float volume = 1.0f, float pitch = 1.0f) {
        if (!engine_ || samples_.empty()) return;
        ReclaimFinished();
        IXAudio2SourceVoice* voice = nullptr;
        if (FAILED(engine_->CreateSourceVoice(&voice, &format_, 0, 2.0f))) return;

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

    void Update() { ReclaimFinished(); }

    void Shutdown() {
        for (IXAudio2SourceVoice* voice : voices_) voice->DestroyVoice();
        voices_.clear();
        if (master_) { master_->DestroyVoice(); master_ = nullptr; }
        if (engine_) { engine_->Release(); engine_ = nullptr; }
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

    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    WAVEFORMATEX format_ = {};
    std::vector<uint8_t> samples_;
    std::vector<IXAudio2SourceVoice*> voices_;
};
