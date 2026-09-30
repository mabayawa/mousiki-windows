# Mousiki for Windows 🎵

A fork of [**mousiki**](https://github.com/itzender5820/mousiki) by
[ender (itzender5820)](https://github.com/itzender5820), a fast, keyboard-driven
terminal music player with spectrum visualizers, synced lyrics and online
streaming. This fork adds native Windows support and Spotify.

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-orange.svg)](https://github.com/mabayawa/mousiki-windows)
[![Platform](https://img.shields.io/badge/Platform-Windows_%7C_Linux_%7C_macOS-brightgreen.svg)](https://github.com/mabayawa/mousiki-windows)

> **This is a fork, not the original project.** Mousiki was created by
> [ender](https://github.com/itzender5820); the design, the TUI, the visualizers and
> the audio architecture are their work. For the original, see
> [itzender5820/mousiki](https://github.com/itzender5820/mousiki).
>
> Upstream targets POSIX and lists Windows as *"Unverified Support — I don't have
> the hardware to test and debug on that operating system."* This fork started as
> that missing half: a real `mousiki.exe` built against the Win32 API and WASAPI,
> with no WSL, no MSYS runtime and no POSIX emulation layer. It has since grown
> Spotify integration, stereo playback and a reworked queue. All of these are listed
> under [What this fork adds](#-what-this-fork-adds).
>
> The Linux, macOS and Android builds are kept working. Every change is either
> cross-platform or behind `#ifdef _WIN32`.

## ✨ Features

From upstream:

- **Local Music Playback:** Instantly browse and play your local music files.
- **Online Search & Streaming:** Search and stream tracks directly from online sources.
- **Synced Lyrics:** Real-time, word-by-word active lyrics highlighting as the song plays.
- **Visualizers:** Real-time FFT spectrum, waveform rendering, and spinning disk art.
- **Queue Management:** Effortless queueing, shuffling, and repeating.
- **Highly Configurable:** Tweak colors, visualizer fluidity, animations, and hotkeys.

Added in this fork:

- **Native Windows:** a Win32/WASAPI build with a one-command `setup.ps1` installer.
- **Spotify Search:** `/sp: <query>` searches Spotify from inside mousiki.
- **Real Spotify Audio:** plays the actual Spotify stream through a Connect device
  or through [librespot](https://github.com/librespot-org/librespot), and falls back
  to YouTube when neither is available. See [Spotify](#-spotify).
- **Your Spotify Library:** Browse your own playlists, the ones you follow and your saved albums with `o`, and queue a track or a whole playlist from there.
- **Stereo Playback:** upstream folds everything to mono; this fork plays stereo end to end.
- **A Queue You Build From Searches:** the queue survives new searches and restarts, `a` adds from either panel, and `ENTER` on the queue plays that item.
- **Clean Track Switching:** the old track stops when you press the key, and the UI moves to the new track only once its audio has actually started.
- **Unit Tests:** a C++ test target and Python tests for the Spotify helper. See [Tests](#-tests).

## 🚀 Getting Started

### Requirements

| | |
|---|---|
| **Windows Terminal** | Required. The UI is ANSI escape sequences; mousiki refuses to start on a host without `ENABLE_VIRTUAL_TERMINAL_PROCESSING` rather than printing garbage. |
| **MSVC C++ toolset** | Visual Studio 2022 Build Tools or any Visual Studio with *Desktop development with C++*. MSYS2/UCRT64 also works — see below. |
| **CMake** | ≥ 3.16 |
| **FFmpeg** | `ffmpeg` and `ffprobe`, for decoding and metadata. |
| **yt-dlp** | Optional — online search and streaming only. |
| **Python 3 + `requests`** | Optional — synced lyrics and Spotify. |
| **Spotify app Client ID** | Optional — Spotify search and library. Premium is needed for Spotify audio. See [Spotify](#-spotify). |
| **librespot** | Optional — Spotify audio without the Spotify desktop app. |

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
| **Online Stream Search** | `/s: <query>` | Search and stream music online (YouTube) |
| **Spotify Search** | `/sp: <query>` | Search Spotify (needs `SpotifyClientId`) |
| **Download Stream** | `y` | Download currently streaming track |
| **Play / Pause** | `p` (or `ENTER`) | Toggle playback |
| **Next / Previous Track** | `n` / `b` | Skip between songs |
| **Seek** | `ARROW_LEFT` / `ARROW_RIGHT` | Seek backward / forward |
| **Volume** | `1` / `2` | Increase / decrease volume — also sets the Spotify Connect device volume when playing that way |
| **Shuffle / Repeat** | `m` / `r` | Toggle shuffle or repeat mode |

### Navigation & Queue
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Navigate** | `ARROW_UP` / `ARROW_DOWN` | Move selection (in whichever panel has focus) |
| **Switch Tabs/Cards** | `TAB` | Move focus between the list and the queue |
| **Add to Queue** | `a` | Enqueue the track hovered in the list — works from either panel |
| **Remove from Queue** | `d` | Drop the hovered queue item |
| **Play a Queue Item** | `ENTER` | With the queue focused, plays that item instead of the list row |
| **Reorder Queue** | `u` / `ARROW_LEFT` | Move the hovered queue item up / down (queue focused) |
| **Queue a Playlist** | `g` | Paste a YouTube playlist link and queue all of it |
| **Filter by Folder** | `f` | Apply folder filter |
| **Clear Filter** | `c` | Reset active search/filters |
| **Quit** | `q` | Exit application |

The queue is yours, not a view of the search results: a new `/s:` or `/sp:`
search replaces the result list but leaves the queue alone, so you can search
several times over and collect a track from each. A `•` beside a result row
means that track is already in the queue. The queue also survives a restart —
see `snapshot.json` under Configuration below.

### Spotify Library

Press `o` for a full-screen browser over your own Spotify library: the playlists
you created, the ones you follow, and your saved albums. The library pane is on
the left, the tracks of whichever row the cursor is on are on the right.

| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Open / close** | `o` | Opens the browser; pressing it again closes it, as `t` does for the console |
| **Switch pane** | `TAB` / `ARROW_LEFT` / `ARROW_RIGHT` | Move focus between the library and its tracks |
| **Navigate** | `ARROW_UP` / `ARROW_DOWN` | Move the cursor in whichever pane has focus |
| **Filter** | `/` | Filter the library by name or owner as you type; `ENTER` keeps it, `ESC` abandons it |
| **Queue everything** | `a` | Queue every track in the selected playlist or album |
| **Queue one track** | `ENTER` | With the tracks pane focused, queue the hovered track. From the library pane it jumps to the tracks instead |
| **Clear filter** | `c` | Drop an active filter without leaving the browser |
| **Reload** | `r` | Refetch from Spotify, bypassing the session cache |
| **Close** | `ESC` | Clears an active filter first, then closes on the second press |

`♫` marks a playlist, `▤` a saved album, and `•` a track already in your queue.
Playlists you created are listed first, then the ones you follow, then your
albums — in the order Spotify returns them within each group, since that is the
order you arranged them in. "Mine" is decided by comparing Spotify user **ids**,
not display names: a display name is neither unique nor guaranteed to exist, so
a playlist made by someone sharing yours would otherwise be listed as your own.

Queueing is instant however large the playlist. mousiki's queue is local, and
Spotify's own queue endpoint is used only for a one-track gapless lookahead, so
a 300-track album costs no extra network calls — the only wait is fetching the
track list itself, which happens on a background thread.

The library is fetched once per session and then cached in memory; `r` forces a
refetch. Moving the cursor does not fetch anything until it has been still for a
moment, so holding `ARROW_DOWN` through a large library costs one request rather
than one per row. Nothing is written to disk.

A playlist can show a track count larger than the list beneath it. Spotify counts
things in that total that cannot be streamed here — local files, and tracks
unavailable in your country — so they are dropped and the pane says how many.
On one real library that is 33 of 572 saved tracks.

Separately, if the browser header says "plan unknown", your saved login predates
the `user-read-private` scope and Spotify will not report your plan or your
region to it. Re-running `python scripts\spotify.py --client-id <your id> login`
fixes that. It does **not** affect which tracks are hidden — availability is
decided from your account either way. Below 72 columns only the focused pane is
drawn, at full width, and `TAB` switches between the two views instead.

Needs `SpotifyClientId` in `config.txt`, the same as `/sp:` search — see
Configuration below. No extra permissions: the scopes this uses are already
requested by the existing login.

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

## 🟢 Spotify

Spotify is optional and off by default. Leave `SpotifyClientId` blank and mousiki
behaves exactly like upstream.

### Setup

1. Register a free app at <https://developer.spotify.com/dashboard> and add
   exactly this redirect URI to it: `http://127.0.0.1:8888/callback`. It must be
   `127.0.0.1`, because Spotify rejects `localhost`.
2. Paste the app's Client ID into `config.txt`:
   ```ini
   SpotifyClientId=<your client id>
   ```
3. Log in once. This opens your browser:
   ```powershell
   python scripts\spotify.py --client-id <your client id> login
   ```

The Client ID is not a secret. Login uses PKCE, so mousiki never asks for or
stores a client secret. The token is saved to
`%USERPROFILE%\.cache\mousiki\spotify_token.json`.

### Where the audio comes from

The Web API only provides metadata, so when you play a Spotify track mousiki
tries these sources in order:

1. **A Spotify Connect device**, such as the Spotify desktop app. mousiki drives
   it as a remote, and `1`/`2` set that device's volume. The audio comes out of
   the device, so the visualizers have no samples to draw.
2. **librespot**, which decrypts the stream and pipes raw PCM into mousiki's own
   player. The visualizers, waveform and exact position all work. mousiki looks
   for it at `SpotifyLibrespotPath`, then on `PATH`, then in
   `%USERPROFILE%\.cache\mousiki\bin\`.
3. **YouTube**, found by searching for the Spotify title and artist. This is the
   fallback when neither of the above is available, and it works without Premium.

Both Spotify transports need a Premium account.

librespot publishes no prebuilt binaries. You can get one in either of two ways:

- Build it locally:
  ```powershell
  cargo install librespot --locked --no-default-features --features native-tls
  ```
  and point `SpotifyLibrespotPath` at the result.
- Run the `librespot` workflow in this repository (`.github/workflows/librespot.yml`).
  It builds and checksums a binary and attaches it to a release. Pin its URL and
  sha256 in `LIBRESPOT_BUILDS` in `scripts/spotify.py`, and mousiki will download
  and verify it. The table ships empty, so nothing is downloaded until you do this.

`SpotifyPrefetch=1` (the default) hands librespot the next track before the
current one ends, so playback continues without a gap.

## 🧩 What this fork adds

Apart from the Windows port (next section), the fork also adds:

- **Spotify:** PKCE login, `/sp:` search, the `o` library browser, and real audio
  through Connect or librespot. All Spotify networking happens in
  `scripts/spotify.py`, so the C++ still makes no network calls itself.
- **Stereo end to end.** Upstream opens the device with one channel and passes
  `-ac 1` to ffmpeg, so local FLAC and MP3 were flattened to mono too. The
  spectrum analyser is still fed a mono sum; the device gets full stereo.
- **A bounded PCM ring** (`src/pcm_ring.h`). Before, each track's buffer was sized
  from its duration, about 100 MB for a five-minute stereo track, and a stream of
  unknown length such as librespot's could not be stored at all. The ring is a
  fixed 8 MiB lock-free buffer that keeps about 12 s of history, so seeking back
  5 s is still instant.
- **Clean track switching.** Pressing next used to leave the old song audible
  while the progress bar and lyrics had already moved on. Now the old track ends
  when you press the key, and the UI switches only when the new track's audio has
  actually started.
- **The queue.** It now works as a list you build up across several searches
  (see *Navigation & Queue* above). Bulk-adding a YouTube playlist moved from `a`
  to `g`.

## 🧪 Tests

The C++ unit tests are built by default (`MOUSIKI_BUILD_TESTS=ON`) and have no
framework dependency:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The Spotify helper's pagination and response handling are covered in Python,
with no network access:

```powershell
python -m unittest discover -s tests -p "test_*.py"
```

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
| `accounts.spotify.com` | only when you use Spotify: the PKCE login, and token refreshes |
| `api.spotify.com` | only when you use Spotify: your search query, and — for `o` — a read of your own profile, playlists, saved albums and their track lists |

The Spotify reads are exactly that: reads. Nothing is written to your Spotify
account, no playlist is created or modified, and the queue you build lives only
in mousiki. The only write the app ever makes is to the playback transport of a
device you already own (play, pause, seek, next, volume), and the one-track
lookahead that makes the handover gapless.

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
- **Opening the Spotify browser takes a second or two.** `o` runs three separate
  helper invocations (profile, playlists, saved albums), each a Python start plus
  a TLS handshake. It is fetched once per session and cached after that; `r`
  refetches deliberately.
- **Spotify audio needs Premium.** Without it, Spotify tracks play through the
  YouTube fallback.
- **ARM64 is untested.** The build targets x64.

## 🙏 Attribution

Mousiki was created by **[ender (itzender5820)](https://github.com/itzender5820)**.
This repository is a fork of [itzender5820/mousiki](https://github.com/itzender5820/mousiki).
It exists only because the original is worth running on another platform. Bugs
in the Windows port and the Spotify features belong to this fork, so report them
here and not upstream.

- **[librespot](https://github.com/librespot-org/librespot)** — optional Spotify audio backend (MIT)

- **[miniaudio](https://github.com/mackron/miniaudio)** — single-file audio playback (public domain / MIT-0)
- **[kissfft](https://github.com/mborgerding/kissfft)** — real-input FFT behind the spectrum visualizer (BSD-3-Clause)
- **[yt-dlp](https://github.com/yt-dlp/yt-dlp)** — online search and streaming (Unlicense)
- **[FFmpeg](https://ffmpeg.org/)** — decoding and metadata
- **requests** — used by `scripts/lrc.py` to fetch synced lyrics from Better Lyrics and [LRCLIB](https://lrclib.net)

## 📜 License

[Apache License 2.0](LICENSE), the same as upstream. This fork modifies the
original; the changes are described above and in the commit history.
