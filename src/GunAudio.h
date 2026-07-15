#pragma once

#include <xaudio2.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Small overlapping-voice player for short PCM/float WAV effects.
class GunAudio {
public:
    ~GunAudio() { Shutdown(); }

    bool Initialize(const std::string& relativePath) {
        Shutdown();
        const std::string path = Resolve(relativePath);
        if (!LoadWav(path)) {
            std::cerr << "Gun audio unavailable: " << path << "\n";
            return false;
        }
        HRESULT hr = XAudio2Create(&engine_);
        if (FAILED(hr) || FAILED(engine_->CreateMasteringVoice(&master_))) {
            Shutdown();
            std::cerr << "XAudio2 initialization failed\n";
            return false;
        }
        std::cout << "Gun audio loaded: " << path << "\n";
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
