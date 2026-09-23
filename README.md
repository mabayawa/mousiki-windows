# Mousiki for Windows 🎵

A native Windows port of [itzender5820/mousiki](https://github.com/itzender5820/mousiki) — a fast, keyboard-driven terminal music player with spectrum visualizers, synced lyrics and online streaming.

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-orange.svg)](https://github.com/mabayawa/mousiki-windows)
[![Platform](https://img.shields.io/badge/Platform-Windows_%7C_Linux_%7C_macOS-brightgreen.svg)](https://github.com/mabayawa/mousiki-windows)

> **This is a fork.** All of the design, the TUI, the visualizers and the audio
> architecture are [ender](https://github.com/itzender5820)'s work. Upstream targets
> POSIX and lists Windows as *"Unverified Support — I don't have the hardware to test
> and debug on that operating system."* This fork is that missing half: a real
> `mousiki.exe` built against the Win32 API and WASAPI. No WSL, no MSYS runtime, no
> POSIX emulation layer.
>
> The Linux, macOS and Android builds are kept working — every change is either
> cross-platform or behind `#ifdef _WIN32`.

## ✨ Features

- **Local Music Playback:** Instantly browse and play your local music files.
- **Online Search & Streaming:** Search and stream tracks directly from online sources.
- **Synced Lyrics:** Real-time, word-by-word active lyrics highlighting as the song plays.
- **Visualizers:** Real-time FFT spectrum, waveform rendering, and spinning disk art.
- **Queue Management:** Effortless queueing, shuffling, and repeating.
- **Highly Configurable:** Tweak colors, visualizer fluidity, animations, and hotkeys.

## 🚀 Getting Started

### Requirements

| | |
|---|---|
| **Windows Terminal** | Required. The UI is ANSI escape sequences; mousiki refuses to start on a host without `ENABLE_VIRTUAL_TERMINAL_PROCESSING` rather than printing garbage. |
| **MSVC C++ toolset** | Visual Studio 2022 Build Tools or any Visual Studio with *Desktop development with C++*. MSYS2/UCRT64 also works — see below. |
| **CMake** | ≥ 3.16 |
| **FFmpeg** | `ffmpeg` and `ffprobe`, for decoding and metadata. |
| **yt-dlp** | Optional — online search and streaming only. |
| **Python 3 + `requests`** | Optional — synced lyrics only. |

Local playback works with nothing but FFmpeg.

### Install

```powershell
git clone https://github.com/mabayawa/mousiki-windows.git
cd mousiki-windows
.\setup.ps1
```

`setup.ps1` installs every dependency through `winget`, makes sure the MSVC C++
workload is actually present, seeds your config, writes the yt-dlp extractor
config, builds, and then verifies each tool is callable before claiming success.
It is re-runnable and leaves anything already installed alone.

If PowerShell refuses to run it:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass; .\setup.ps1
```

Useful flags: `-SkipDeps` (build only), `-SkipBuild` (dependencies only),
`-Generator Ninja`.

### Run

```powershell
.\build\Release\mousiki.exe
```

To launch it by name, add a function to your PowerShell profile (`$PROFILE`):

```powershell
function mousiki { & 'C:\path\to\mousiki-windows\build\Release\mousiki.exe' }
```

### Building with MinGW instead

```bash
# MSYS2 UCRT64 shell
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The build links `-static-libgcc -static-libstdc++`, so the result runs without
the GCC runtime DLLs beside it.

## ⌨️ Default Keybindings

Configurable in `%USERPROFILE%\.config\mousiki\config.txt`.

### Search & Playback
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Local Search** | `/` | Filter and search local library |
| **Online Stream Search** | `/s: <query>` | Search and stream music online |
| **Download Stream** | `y` | Download currently streaming track |
| **Play / Pause** | `p` (or `ENTER`) | Toggle playback |
| **Next / Previous Track** | `n` / `b` | Skip between songs |
| **Seek** | `ARROW_LEFT` / `ARROW_RIGHT` | Seek backward / forward |
| **Volume** | `1` / `2` | Decrease / Increase volume |
| **Shuffle / Repeat** | `m` / `r` | Toggle shuffle or repeat mode |

### Navigation & Queue
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Navigate** | `ARROW_UP` / `ARROW_DOWN` | Move selection |
| **Switch Tabs/Cards** | `TAB` | Cycle between UI panels |
| **Add to Queue** | `a` | Enqueue selected track |
| **Remove from Queue** | `d` | Dequeue selected track |
| **Filter by Folder** | `f` | Apply folder filter |
| **Clear Filter** | `c` | Reset active search/filters |
| **Quit** | `q` | Exit application |

## ⚙️ Configuration

| | |
|---|---|
| Config | `%USERPROFILE%\.config\mousiki\config.txt` |
| Cache | `%USERPROFILE%\.cache\mousiki\` |
| Log | `%USERPROFILE%\.cache\mousiki\logs\console.log` |
| Session | `%USERPROFILE%\.cache\mousiki\snapshot\snapshot.json` |

The dotted layout matches the other platforms deliberately, rather than using
`%APPDATA%` — the same config file works on all of them.

By default your **Music** folder is scanned via the Windows Known Folder API, so
a Music folder relocated to another drive or into OneDrive is found correctly.
Add more with as many `LocalMusicPath` entries as you like:

```ini
LocalMusicPath=D:\Music
LocalMusicPath=C:/Users/you/Downloads/albums
LocalMusicPath=~/Music
```

Windows and forward slashes both work, and `~` expands.

## 🔧 What the port required

Upstream's POSIX surface turned out to be small and well isolated — the 174 KB
`app.cpp` is nearly portable already. Four areas needed real work.

**Terminal I/O** (`src/win_compat.cpp`, `src/terminal_ui.cpp`)
`termios`, `ioctl(TIOCGWINSZ)` and `read(STDIN_FILENO)` become `SetConsoleMode`,
`GetConsoleScreenBufferInfo` and `ReadConsoleInputW`. Input uses `INPUT_RECORD`
rather than `ENABLE_VIRTUAL_TERMINAL_INPUT`, which is documented to drop modifier
information. Output installs a `std::streambuf` that converts UTF-8 to UTF-16 and
writes through `WriteConsoleW` — `SetConsoleOutputCP(CP_UTF8)` alone is not
reliable across console hosts. `wcwidth` has no Windows implementation at all, so
it ships its own table, taking a `uint32_t` rather than a 16-bit `wchar_t`.

**Subprocesses** (`src/process_util.cpp`)
`run_capture()` took a shell command string and ran it through `sh -c`. It now
takes an `argv` on every platform. Windows uses `CreateProcessW` with an explicit
`PROC_THREAD_ATTRIBUTE_HANDLE_LIST` — handle inheritance is process-wide, and
mousiki spawns ffprobe, yt-dlp and lyrics concurrently, so a blanket
`bInheritHandles` leaks pipes into the wrong child and the reader waits forever
for an EOF that never comes. Children run with `CREATE_NO_WINDOW` and live in a
job object, so nothing is orphaned when you quit mid-decode.

**Non-ASCII filenames**
MSVC's `std::filesystem::path::string()` converts through the process ANSI code
page and *throws* on anything it cannot represent, so one Cyrillic filename
aborted the library scan. Every conversion now goes through `path_utf8()`.
miniaudio opens files via `ma_decoder_init_file_w`.

**Audio** (`src/player.cpp`, `src/app.cpp`)
miniaudio's WASAPI backend was already wired up upstream. The problem was
threading: a fresh `std::thread` per track is fine on ALSA and CoreAudio, but
miniaudio initialises COM on the thread that creates the device, and COM
interfaces are apartment-affine — when that thread exits, the `IAudioClient`
belongs to a thread that no longer exists. One persistent worker now owns the
device for the whole session.

### Bugs fixed along the way

These are upstream bugs the port exposed, fixed here and worth sending back:

- **Shell injection.** `online_source.cpp` and `youtube_source.cpp` interpolated
  the raw search query into an `sh -c` command line. Typing `` `cmd` `` or `$( )`
  into the search box executed it. The argv refactor removes the whole class.
- **Non-Latin tracks shared one cache file.** The cache filename sanitizer kept
  only `[A-Za-z0-9]` applied byte-by-byte to UTF-8, so every CJK, Cyrillic or
  Greek title collapsed to `untitled` — meaning those tracks overwrote each other
  and silently played whichever was downloaded first.
- **Comma-decimal locales corrupted all timing.** `setlocale(LC_ALL, "")` adopts
  the OS numeric conventions, so on a German or French system `std::stod` stopped
  at the first `.` and every ffprobe duration and lyric timestamp was silently
  truncated. `LC_NUMERIC` is now pinned to `"C"`.
- **Ctrl+C left the terminal broken.** No signal handling anywhere meant the
  alternate screen stayed active and the cursor hidden. Now handled.
- **Lyrics progress messages went to stdout**, the same stream the JSON result is
  parsed from.
- **Unpinned dependency downloads.** CMake fetched miniaudio and kissfft from a
  moving `master` with no integrity check; now pinned with `EXPECTED_HASH`.
- `.MP3` and `.FLAC` were invisible to the library scanner (case-sensitive match).

## 🔒 A note on what this program does

Worth stating plainly, since it runs external tools and talks to the network.

mousiki's C++ does no networking at all. Everything that leaves your machine goes
through `yt-dlp` or the two Python helpers, and it is only ever the song metadata
you searched for:

| Destination | What is sent |
|---|---|
| `lyrics-api.boidu.dev` | song title, artist |
| `lrclib.net` | song title, artist |
| `youtube.com` (search + yt-dlp) | your search query |

No telemetry, no analytics, no phone-home, no credential or environment
harvesting. Files are written only under `%USERPROFILE%\.config\mousiki`,
`%USERPROFILE%\.cache\mousiki`, a `.lrc` sidecar next to a track, and your Music
folder when you press `y` to save a stream. The vendored `third_party/` sources
are byte-identical to upstream miniaudio v0.11.25 and kissfft.

## ⚠️ Known limitations

- **Windows Terminal is required.** A legacy console without VT processing is
  refused with a clear message rather than rendered as garbage.
- **Braille glyphs need a capable font.** The visualizers, waveform and spinning
  disk all draw with U+2800–U+28FF. Cascadia Mono (the Windows Terminal default)
  is fine; a raster font is not.
- **Devanagari and other Indic conjuncts may mis-measure.** The width logic
  assumes the terminal shapes conjuncts into a single cell, which most Linux
  terminals do and Windows Terminal does not reliably. Latin, Cyrillic, Greek and
  CJK are correct.
- **The library is scanned once at startup** — inherited from upstream. Add music
  and restart.
- **ARM64 is untested.** The build targets x64.

## 🙏 Attribution

Mousiki was created by **[ender (itzender5820)](https://github.com/itzender5820)** —
this fork exists only because the original is worth running on another platform.

- **[miniaudio](https://github.com/mackron/miniaudio)** — single-file audio playback (public domain / MIT-0)
- **[kissfft](https://github.com/mborgerding/kissfft)** — real-input FFT behind the spectrum visualizer (BSD-3-Clause)
- **[yt-dlp](https://github.com/yt-dlp/yt-dlp)** — online search and streaming (Unlicense)
- **[FFmpeg](https://ffmpeg.org/)** — decoding and metadata
- **requests** — used by `scripts/lrc.py` to fetch synced lyrics from Better Lyrics and [LRCLIB](https://lrclib.net)

## 📜 License

[Apache License 2.0](LICENSE), the same as upstream. This fork modifies the
original; the changes are described above and in the commit history.
