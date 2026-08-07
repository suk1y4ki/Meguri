# Third-party notices

Meguri は以下のサードパーティライブラリを vendoring して利用しています。
ライセンス全文は配布物の `licenses/` フォルダおよび `third_party/` 以下に同梱されています。

| ライブラリ | 用途 | ライセンス |
| --- | --- | --- |
| [libwebp](https://github.com/webmproject/libwebp) 1.5.0 | アニメーション WEBP のデコード / サンプル生成用エンコード | BSD-3-Clause |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 | 設定ファイル (JSON) の読み書き | MIT |
| [stb_image_write](https://github.com/nothings/stb) | CLI のフレーム PNG 出力 | MIT / Public Domain |

MP4 のデコード / サンプル生成には Windows 標準の Media Foundation を使用しており、追加の再配布物はありません。
