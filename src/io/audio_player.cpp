#include "audio_player.h"

#include <mfapi.h>
#include <mfplay.h>
#include <windows.h>
#include <wrl/client.h>

#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace meguri::io {

namespace {

// mfplay.lib は SDK に無い環境があるため、mfplay.dll を動的に読む
using MFPCreateMediaPlayerFn = HRESULT(WINAPI*)(LPCWSTR, BOOL, MFP_CREATION_OPTIONS,
                                                IMFPMediaPlayerCallback*, HWND,
                                                IMFPMediaPlayer**);

MFPCreateMediaPlayerFn load_mfplay() {
    static MFPCreateMediaPlayerFn fn = [] {
        HMODULE mod = LoadLibraryExW(L"mfplay.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!mod) return static_cast<MFPCreateMediaPlayerFn>(nullptr);
        return reinterpret_cast<MFPCreateMediaPlayerFn>(
            GetProcAddress(mod, "MFPCreateMediaPlayer"));
    }();
    return fn;
}

}  // namespace

struct AudioPlayer::Impl {
    ComPtr<IMFPMediaPlayer> player;
};

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() { close(); }

bool AudioPlayer::open(const std::wstring& path) {
    close();
    MFPCreateMediaPlayerFn create = load_mfplay();
    if (!create) return false;

    // コールバック無し = 同期モード。映像は自分で描いているので使わない
    ComPtr<IMFPMediaPlayer> player;
    if (FAILED(create(nullptr, FALSE, MFP_OPTION_NONE, nullptr, nullptr, &player))) {
        return false;
    }
    ComPtr<IMFPMediaItem> item;
    if (FAILED(player->CreateMediaItemFromURL(path.c_str(), TRUE, 0, &item)) || !item) {
        return false;
    }
    BOOL has_audio = FALSE, audio_selected = FALSE;
    if (FAILED(item->HasAudio(&has_audio, &audio_selected)) || !has_audio) {
        return false;  // 音声ストリームが無い
    }
    // 映像ストリームを外す (音声のみ再生)
    DWORD stream_count = 0;
    if (SUCCEEDED(item->GetNumberOfStreams(&stream_count))) {
        for (DWORD i = 0; i < stream_count; ++i) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(item->GetStreamAttribute(i, MF_MT_MAJOR_TYPE, &var)) &&
                var.vt == VT_CLSID && var.puuid && *var.puuid == MFMediaType_Video) {
                item->SetStreamSelection(i, FALSE);
            }
            PropVariantClear(&var);
        }
    }
    if (FAILED(player->SetMediaItem(item.Get()))) return false;
    impl_->player = player;
    return true;
}

void AudioPlayer::close() {
    if (impl_->player) {
        impl_->player->Shutdown();
        impl_->player.Reset();
    }
}

bool AudioPlayer::is_open() const { return impl_->player != nullptr; }

void AudioPlayer::play() {
    if (impl_->player) impl_->player->Play();
}

void AudioPlayer::pause() {
    if (impl_->player) impl_->player->Pause();
}

void AudioPlayer::seek_ms(int64_t position_ms) {
    if (!impl_->player) return;
    if (position_ms < 0) position_ms = 0;
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = position_ms * 10000;  // ms -> 100ns
    impl_->player->SetPosition(MFP_POSITIONTYPE_100NS, &var);
    PropVariantClear(&var);
}

int64_t AudioPlayer::position_ms() const {
    if (!impl_->player) return -1;
    PROPVARIANT var;
    PropVariantInit(&var);
    if (FAILED(impl_->player->GetPosition(MFP_POSITIONTYPE_100NS, &var)) ||
        var.vt != VT_I8) {
        PropVariantClear(&var);
        return -1;
    }
    const int64_t ms = var.hVal.QuadPart / 10000;
    PropVariantClear(&var);
    return ms;
}

void AudioPlayer::set_volume(float volume) {
    if (!impl_->player) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    impl_->player->SetVolume(volume);
}

void AudioPlayer::set_mute(bool mute) {
    if (impl_->player) impl_->player->SetMute(mute ? TRUE : FALSE);
}

}  // namespace meguri::io
