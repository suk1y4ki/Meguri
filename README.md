# Meguri

Meguri は、短い WEBP アニメーション、MP4 動画、PNG/JPEG 画像を一覧で同時に確認し、
不要なファイルをすばやくゴミ箱へ送るための Windows デスクトップツールです。

![Meguri icon](assets/icon.png)

## 機能

- フォルダを開く、起動引数で渡す、またはドラッグ＆ドロップで読み込み
- WEBP アニメーションと MP4 動画をジャスティファイドレイアウトで同時再生
- PNG/JPEG 画像も同じ一覧で表示し、選択・拡大・削除・復元を共通操作で実行
- クリック、Ctrl+クリック、Shift+クリック、Ctrl+A による選択
- Del で選択ファイルをゴミ箱へ移動、Ctrl+Z で直前の削除バッチを復元
- ダブルクリックまたは Enter で 1 件を拡大表示
- 拡大表示中のシーク、再生/一時停止、前後移動、削除して次を表示、音声再生
- 一覧表示中の動画音声を同時再生する実験的オプション
- WEBP / MP4 / PNG / JPG の種類別フィルタとサブフォルダ走査
- 名前、更新日時、サイズでの並び替え
- タイルサイズの切り替えと Ctrl+ホイールによる連続調整
- ダークモード既定、ライトモード、日本語/英語 UI、Per-Monitor V2 DPI 対応
- 対応環境では MP4 を GPU ゼロコピー再生し、失敗時は自動でソフトウェア再生にフォールバック
- 設定とプローブキャッシュは既定で EXE と同じフォルダに保存し、AppData 保存にも切り替え可能

## 必要環境

- Windows
- Visual Studio 2022 Community または Build Tools の MSVC
- Visual Studio 同梱の Ninja
- CMake 3.25 以上

MP4 の処理には Windows 標準の Media Foundation を使用します。FFmpeg や外部コーデックの同梱は不要です。

## ビルド

リポジトリ直下で実行します。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dev_build.ps1 -Release
```

このスクリプトは configure、build、test をまとめて実行します。`cl` や `ninja` が
`PATH` に無い場合は Visual Studio の開発者シェルを自動で読み込みます。

主な出力先:

- `build\windows-msvc-release\apps\meguri_gui\Meguri.exe`
- `build\windows-msvc-release\apps\meguri_cli\Meguri_CLI.exe`

Debug ビルド:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dev_build.ps1
```

手動で実行する場合:

```powershell
cmake --preset windows-msvc-release
cmake --build --preset build-release
ctest --preset test-release --output-on-failure
```

## 配布フォルダ作成

```powershell
powershell -ExecutionPolicy Bypass -File scripts\make_dist.ps1
```

Release ビルド、テスト、install を実行し、`dist\Meguri` に最小構成を集約します。
サードパーティのライセンス全文は `dist\Meguri\licenses` にコピーされます。

## 使い方

`Meguri.exe` を起動し、表示したいメディアフォルダを開くかドロップします。

| 操作 | 内容 |
| --- | --- |
| クリック | 選択 |
| Ctrl+クリック | 選択の切り替え |
| Shift+クリック | 範囲選択 |
| Ctrl+A | 表示中の項目を全選択 |
| Del | 選択項目をゴミ箱へ移動 |
| Ctrl+Z | 直前の削除バッチを復元 |
| ダブルクリック / Enter | 拡大表示 |
| Esc / 拡大表示中のダブルクリック | 一覧へ戻る |
| 拡大表示中の矢印キー / ホイール | 前後の項目へ移動 |
| 拡大表示中の Space | 再生 / 一時停止 |
| Ctrl+ホイール | タイルサイズ調整 |

起動引数でフォルダを渡すこともできます。

```powershell
build\windows-msvc-release\apps\meguri_gui\Meguri.exe samples\input
```

## CLI

`Meguri_CLI.exe` は検証、サンプル生成、ベンチマーク向けの補助ツールです。

```powershell
Meguri_CLI.exe scan <folder> [--no-recursive]
Meguri_CLI.exe info <file|folder>
Meguri_CLI.exe decode <file> [--out <dir>] [--max <n>]
Meguri_CLI.exe bench <folder> [--threads <n>] [--limit <px>]
Meguri_CLI.exe gensample <folder> [--webp <n>] [--mp4 <n>] [--large]
```

例:

```powershell
$cli = "build\windows-msvc-release\apps\meguri_cli\Meguri_CLI.exe"
& $cli gensample samples\input --webp 12 --mp4 12
& $cli info samples\input
```

## 詳細情報

- [TECHNICAL.md](TECHNICAL.md): 構成、検証コマンド、GPU 再生、性能メモ
- [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md): サードパーティライブラリの表記

## ライセンス

MIT License。サードパーティライブラリについては
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) を参照してください。
