# 技術メモ

README を公開向けに短く保つため、構成、検証、実装上の判断、性能メモをこのファイルに分けています。

## リポジトリ構成

依存方向は一方向です。

```text
apps -> app -> io -> core
```

| パス | 内容 | 依存 |
| --- | --- | --- |
| `src/core` | レイアウト、仮想化スケジューラ、選択、再生タイミング、フィルタの純ロジック | 標準 C++ のみ |
| `src/io` | フォルダ走査、WEBP/MP4/画像デコード、ゴミ箱操作、サンプル生成 | `core`、Windows API、vendored ライブラリ |
| `src/app` | GUI/CLI 共通の設定とプローブキャッシュ | `io`、`nlohmann/json` |
| `apps/meguri_cli` | 検証、ベンチ、フレーム出力、サンプル生成 | `app`、`stb_image_write` |
| `apps/meguri_gui` | Win32 + Direct2D のデスクトップ UI | `app` |
| `tests` | core ロジックのテスト | `meguri_core` |
| `third_party` | libwebp、nlohmann/json、stb の vendored ファイル | 各ライセンス |

## CMake プリセット

| プリセット | 用途 | 出力先 |
| --- | --- | --- |
| `windows-msvc-debug` | Debug configure | `build/windows-msvc-debug` |
| `windows-msvc-release` | Release configure | `build/windows-msvc-release` |
| `build-debug` | Debug build | `build/windows-msvc-debug` |
| `build-release` | Release build | `build/windows-msvc-release` |
| `test-debug` | Debug test | `build/windows-msvc-debug` |
| `test-release` | Release test | `build/windows-msvc-release` |

`scripts/dev_build.ps1` と `scripts/make_dist.ps1` は、MSVC が `PATH` に無い場合に
`Launch-VsDevShell.ps1 -Arch amd64` を自動で読み込みます。

## 検証コマンド

サンプル生成とメタデータ確認:

```powershell
$cli = "build\windows-msvc-debug\apps\meguri_cli\Meguri_CLI.exe"
& $cli gensample samples\input --webp 12 --mp4 12
& $cli info samples\input
& $cli decode samples\input\sample_00.webp --out samples\output --max 3
& $cli bench samples\input
```

GUI 起動:

```powershell
build\windows-msvc-debug\apps\meguri_gui\Meguri.exe samples\input
```

GUI の内部レンダー結果を PNG 保存:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\capture_app.ps1 -Out shot.png
```

選択、削除、ゴミ箱、復元の自動テスト:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\e2e_delete_test.ps1
```

## 処理モデル

Meguri は CLI ファーストで作られています。デコーダの正しさ、フレームの向き、タイミング、
サンプル生成は CLI で確認し、その上に GUI を載せています。

GUI の再生は pull 型です。約 60 fps の描画タイックで、表示期限が来たタイルだけ
デコードジョブを積みます。タイルごとの未処理ジョブは常に 1 つまでなので、重いファイルは
そのタイルだけフレームが落ち、一覧全体は止まりません。

一覧は仮想化されています。スクロール位置とパフォーマンスモードからアクティブ範囲を決め、
範囲外のタイルはデコーダ、フレームバッファ、Direct2D ビットマップを解放します。
先読み済みフレームは保持し、画面に入った瞬間に描画できるようにしています。

| モード | 内容 |
| --- | --- |
| 標準 | 可視タイル + 前後 1 画面分を先読み、同時デコード上限 96 |
| 大量 | 可視タイルのみ、同時デコード上限 48 |
| 全再生 | 全ファイルを常時アクティブ化。小規模フォルダ向け |

巨大ウィンドウで下部のタイルが永久に読み込まれない状況を避けるため、可視タイルは
通常の同時数上限を超えても全数アクティブ化します。

## GPU 再生

対応環境では、MP4 を Chrome 型の GPU パイプラインで再生します。

```text
DXVA で NV12 テクスチャへデコード
-> 共有 VideoProcessor で BGRA へ変換・縮小
-> 同じ D3D11 デバイス上の Direct2D 1.1 で描画
```

デコードから表示まで CPU へのピクセルコピーを避けます。同時 GPU デコーダ数は
`QueryVideoMemoryInfo` の VRAM 予算から次の目安で計算します。

```text
VRAM 予算 * 設定割合 / 128 MB
```

非対応環境、リソース不足、実行時失敗ではソフトウェアデコード + CPU 転送に自動フォールバックします。

| 環境変数 | 内容 |
| --- | --- |
| `MEGURI_NO_GPU=1` | MP4 を強制的にソフトウェアデコード |
| `MEGURI_NO_ZEROCOPY=1` | 従来の HwndRenderTarget + CPU 転送描画を使用 |

## 実行時失敗への対応

同時 GPU デコーダの真の上限は事前に正確には問い合わせできません。ドライバによっては
open が成功した後、`ReadSample` が `E_OUTOFMEMORY` で失敗します。

対策:

1. 失敗したタイルはソフトウェアデコードで開き直す
2. 失敗発生時の同時数付近まで実効 GPU 上限を下げる
3. 上限降下は失敗バーストごとに 1 回だけにして、全タイルが CPU 化するスパイラルを避ける
4. 一定時間失敗が無ければ上限を段階的に戻す
5. GPU 枠が空いたら、ソフトウェアデコード中の可視タイルを少しずつ GPU に昇格する
6. 画面外に出たタイルの失敗状態はリセットし、再訪時に GPU から再試行する

## 音声

一覧再生では、MP4 音声は既定で無効です。拡大表示では同じファイルを Media Foundation の
音声プレイヤーで並走させ、映像クロックに同期しながら再生します。

実験的な一覧音声オプションでは、画面に見えている動画の音声を最大 10 件まで同時再生します。
ホバー中のタイルを優先します。

## 設定とプローブキャッシュ

フォルダを開いたとき、レイアウトに必要な解像度、再生時間、フレーム数、形式を先に取得します。
このプローブ処理ではフレーム自体はデコードしません。

結果は設定ファイルと同じ保存先の `probe_cache.json` に保存されます。ファイルサイズと更新日時が
変わった項目だけ再取得し、画面に見えている付近を優先して処理します。

既定の保存先は EXE と同じフォルダです。

- `settings.json`
- `probe_cache.json`

GUI から `%APPDATA%\Meguri` に切り替えられます。読み込みは EXE 側を優先し、EXE フォルダに
書き込めない場合は AppData に自動退避します。

## 描画とキャプチャのメモ

- 共有 D3D デバイスでは `Present(1)` の vsync 待ちがデコード/変換ワーカーを止めるため、
  GUI は非ブロックの `Present(0)` を使います。
- フレーム期限は消費時刻ではなく予定時刻から進め、パイプライン遅延が再生周期に蓄積しないようにします。
- セッションロック中は `PrintWindow(PW_RENDERFULLCONTENT)` でも Direct2D 領域が黒くなるため、
  E2E キャプチャは WM_COPYDATA 経由で同じシーンを WIC PNG に描画します。
- `d2d1.h` は Win32 の `DrawText` マクロの影響を受けるため、Unicode ビルドでは
  `ID2D1RenderTarget::DrawText` が `DrawTextW` として見える場合があります。
- `.ico` 差し替え時の再ビルド漏れを避けるため、`.rc` に `assets/app.ico` の
  `OBJECT_DEPENDS` を設定しています。

## 性能メモ

Release / 32 スレッド環境での過去の実測値です。

- 小サイズ 200 ファイル、12,558 フレームの CLI フルデコードが 1.9 秒、約 6,600 frames/s
- 1080p/720p MP4 24 件:
  ソフトウェア原寸 372 frames/s、GPU 原寸 533 frames/s、GPU + 450px 縮小 1,417 frames/s
- 同フォルダの GUI 同時再生:
  GPU 縮小パスは約 0.66 CPU コア / 436 MB、ソフトウェアパスは約 16.6 CPU コア / 3.6 GB
- GUI 200 ファイル:
  標準モードで約 100-130 MB、全再生モードで約 230 MB

## 文字コード

ソースは UTF-8 を前提にしています。PowerShell 5.1 対応のため、`.ps1` は BOM 付き UTF-8 を
維持する運用です。Git for Windows の iconv 互換性を考慮し、`.gitattributes` では
`working-tree-encoding=UTF-8-BOM` を使っていません。
