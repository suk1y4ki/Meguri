// ズーム再生用の音声プレーヤー。同じファイルを MFPlay (映像ストリーム無効) で開き、
// 音声だけを再生する。呼び出し側 (UI スレッド) が映像クロックに合わせて
// seek_ms で同期を取る。音声ストリームが無いファイルは open が false を返す。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace meguri::io {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // 音声ストリームが無い・MFPlay が使えない環境では false
    bool open(const std::wstring& path);
    void close();
    bool is_open() const;

    void play();
    void pause();
    void seek_ms(int64_t position_ms);
    int64_t position_ms() const;  // 取得失敗時 -1

    void set_volume(float volume);  // 0.0〜1.0
    void set_mute(bool mute);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace meguri::io
