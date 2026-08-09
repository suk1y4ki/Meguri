# Meguri

English | [日本語](README.md)

Meguri is a Windows desktop tool for quickly reviewing many short WEBP
animations, MP4/WMV/AVI videos, PNG images, and JPEG images, then moving
unwanted files to the Recycle Bin.

![Meguri preview](assets/preview.gif)

## Features

- Open a folder with the folder picker, a command-line argument, or drag and drop
- Play WEBP animations and MP4/WMV/AVI videos together in a justified grid layout
- Show PNG/JPEG images in the same grid with the same select, zoom, delete, and restore operations
- Select with click, Ctrl+click, Shift+click, and Ctrl+A
- Move selected files to the Recycle Bin with Del, and restore the last delete batch with Ctrl+Z
- Open a single item in zoom view with double-click or Enter
- Seek, play/pause, move between items, delete-and-advance, and play audio in zoom view
- Optional experimental audio playback for videos visible in the grid
- File type filters for WEBP, MP4, WMV, AVI, PNG, and JPG, plus optional subfolder scanning (WMV/AVI are off by default)
- Sort by name, modified time, or file size
- Switch tile sizes or adjust them continuously with Ctrl+wheel
- Dark mode by default, light mode, Japanese/English UI, and Per-Monitor V2 DPI support
- GPU zero-copy video playback on supported systems, with automatic software fallback
- Portable settings and probe cache next to the EXE by default, with an AppData storage option

## Requirements

- Windows
- Visual Studio 2022 Community or Build Tools with MSVC
- CMake 3.25 or later

MP4/WMV/AVI support uses Windows Media Foundation. FFmpeg and external codec
packages are not required.

## Create a Distribution Folder

Run from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\make_dist.ps1
```

This runs a Release build, tests, and install, then collects the minimal
distribution into `dist\Meguri`. Third-party license files are copied to
`dist\Meguri\licenses`.

## Development Build

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dev_build.ps1 -Release
```

The script configures, builds, and runs tests with the Visual Studio 2022 CMake
generator. Build outputs stay under `build\vs2022`; use `dist\Meguri` for files
you intend to package or publish.

Debug build:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dev_build.ps1
```

Manual CMake commands:

```powershell
cmake --preset vs2022
cmake --build --preset build-release
ctest --preset test-release --output-on-failure
```

## Usage

Start `Meguri.exe`, then open or drop a media folder.

| Operation | Action |
| --- | --- |
| Click | Select |
| Ctrl+click | Toggle selection |
| Shift+click | Select a range |
| Ctrl+A | Select all visible items |
| Del | Move selected items to the Recycle Bin |
| Ctrl+Z | Restore the last delete batch |
| Double-click / Enter | Open zoom view |
| Esc / double-click in zoom view | Return to grid view |
| Arrow keys / wheel in zoom view | Move to the previous or next item |
| Space in zoom view | Play / pause |
| Ctrl+wheel | Adjust tile size |

You can also pass a folder path as the first argument:

```powershell
build\vs2022\apps\meguri_gui\Release\Meguri.exe <folder>
```

## CLI

`Meguri_CLI.exe` is a helper tool for validation, sample generation, and
benchmarks.

```powershell
Meguri_CLI.exe scan <folder> [--no-recursive]
Meguri_CLI.exe info <file|folder>
Meguri_CLI.exe decode <file> [--out <dir>] [--max <n>]
Meguri_CLI.exe bench <folder> [--threads <n>] [--limit <px>]
Meguri_CLI.exe gensample <folder> [--webp <n>] [--mp4 <n>] [--large]
```

Example:

```powershell
$cli = "build\vs2022\apps\meguri_cli\Release\Meguri_CLI.exe"
& $cli gensample tmp\samples --webp 12 --mp4 12
& $cli info tmp\samples
```

## More Information

- [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md): third-party library notices

## License

MIT License. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for
third-party libraries.
