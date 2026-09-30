import json
import os
import re
import sys
import threading

# lrc.py (same directory) does the actual fetching: Better Lyrics first
# (word-level TTML, converted to enhanced LRC here in lrc.py itself),
# falling back to LRCLIB (line-synced only) if Better Lyrics has nothing.
# This script's only job is to call it and translate the result into the
# flat JSON object lyrics_fetcher.cpp already knows how to parse --
# {"ok", "enhanced", "lrc", "source"} on success, {"ok": false, "error",
# "detail"} on failure -- so nothing on the C++ side had to change to
# pick up the new source.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Windows has no LANG/LC_ALL. When stdout is a pipe -- which it always is here,
# because mousiki captures it -- Python picks the process ANSI code page for
# the pipe encoding, so printing a non-Latin track title raises
# UnicodeEncodeError and kills the script outright. Forcing UTF-8 on both
# streams matches what the C++ side already decodes.
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except (ValueError, OSError):
            pass


_watchdog = None


def emit(obj):
    if _watchdog is not None:
        _watchdog.cancel()
    print(json.dumps(obj))
    sys.exit(0)


def _find_pipx_site_packages(package_name):
    """Best-effort: if `package_name` was installed into an isolated pipx
    venv rather than a normal pip/pip3 environment, find that venv's
    site-packages directory so it can be spliced onto sys.path.

    This mirrors what setup.sh's own pipx detection does at install time
    -- but doesn't rely on anything setup.sh sets, since setup.sh only
    ever copies the built binary to somewhere on PATH (no wrapper script,
    no persistent env export), so there is no path by which an env var
    it sets could ever reach this script when the running `mousiki`
    binary later spawns it as a subprocess. This discovers the venv
    fresh, every call, using whatever `pipx` (or MOUSIKI_PIPX_PATH, kept
    as an override for any future wrapper that does choose to set it)
    is available in *this* process's environment/PATH.
    """
    import glob
    import shutil
    import subprocess as sp

    pipx_bin = os.environ.get("MOUSIKI_PIPX_PATH") or shutil.which("pipx")
    if not pipx_bin:
        return None

    try:
        pipx_home = sp.check_output(
            [pipx_bin, "environment", "--value", "PIPX_HOME"],
            stderr=sp.DEVNULL, text=True,
        ).strip()

        # Don't hardcode $PIPX_HOME/venvs/<package> -- that's a pipx
        # layout assumption. Instead, look for any directory whose name
        # starts with the package name under the venvs dir, and ask its
        # venv Python for the real site-packages path.
        venvs_dir = os.path.join(pipx_home, "venvs")
        candidates = sorted(glob.glob(os.path.join(venvs_dir, package_name + "*")))

        for venv_dir in candidates:
            # pipx puts the interpreter in Scripts\python.exe on Windows and
            # bin/python everywhere else.
            venv_python = os.path.join(venv_dir, "Scripts", "python.exe")
            if not os.path.isfile(venv_python):
                venv_python = os.path.join(venv_dir, "bin", "python")
            if not os.path.isfile(venv_python):
                continue

            site_packages = sp.check_output(
                [venv_python, "-c", "import site; print(site.getsitepackages()[0])"],
                stderr=sp.DEVNULL, text=True,
            ).strip()

            if site_packages and os.path.isdir(site_packages):
                return site_packages

    except (OSError, sp.SubprocessError):
        pass

    return None


def load_lrc_module():
    # First: normal Python environment -- pip/pip3 install (regular or
    # --user), a system package, or `requests` already importable for any
    # other reason. lrc.py itself does `import requests` at module load
    # time, so this fails with ImportError exactly when requests isn't
    # reachable, same as trying to import requests directly would.
    try:
        import lrc
        return lrc
    except ImportError:
        pass

    # Second: requests might be sitting in an isolated pipx venv (e.g.
    # `pipx install requests`, which setup.sh offers as one of its three
    # install options) rather than on the normal import path. Find it and
    # splice its site-packages onto sys.path, then retry the import.
    site_packages = _find_pipx_site_packages("requests")
    if site_packages:
        sys.path.insert(0, site_packages)
        try:
            import lrc
            return lrc
        except ImportError:
            pass

    return None


def has_word_timestamps(lrc_text):
    """Check if LRC text actually contains word-level <mm:ss.xx> timestamps."""
    if not lrc_text:
        return False
    for line in lrc_text.split("\n"):
        bracket_end = line.find("]")
        if bracket_end < 0:
            continue
        rest = line[bracket_end + 1:]
        if "<" in rest and ">" in rest:
            return True
    return False


def clean_youtube_title(text):
    if '|' in text:
        text = text.split('|')[0]
    text = re.sub(r'\(.*?\)', '', text)
    text = re.sub(r'\[.*?\]', '', text)
    return text.strip()


def main():
    if len(sys.argv) < 2:
        emit({
            "ok": False,
            "error": "EXCEPTION",
            "detail": "usage: fetch_lyrics.py <title> [artist]"
        })

    raw_title = sys.argv[1]
    title = clean_youtube_title(raw_title)
    artist = sys.argv[2] if len(sys.argv) > 2 else ""

    lrc = load_lrc_module()
    if lrc is None:
        emit({
            "ok": False,
            "error": "MODULE_MISSING",
            "detail": "the 'requests' package is not available (pip install requests)"
        })

    # Hard 35-second backstop: two sources (Better Lyrics, then LRCLIB), each
    # with its own 10s network timeout inside lrc.py, times up to two
    # attempts (original query, then swapped title/artist below) -- worst
    # case that's up to ~40s of genuine network waiting.
    #
    # This used to be signal.alarm(35), which does not exist on Windows at all
    # (it lives behind a try/except AttributeError, so the backstop was simply
    # absent there) and which, even on POSIX, had no SIGALRM handler installed
    # -- so firing it killed the process and the C++ side saw an empty capture
    # rather than a diagnosis. A daemon timer thread behaves identically on
    # every platform and can report why it gave up.
    global _watchdog

    def _on_timeout():
        try:
            sys.stdout.write(json.dumps({
                "ok": False,
                "error": "TIMEOUT",
                "detail": "lyrics lookup exceeded 35s",
            }) + "\n")
            sys.stdout.flush()
        finally:
            os._exit(0)

    _watchdog = threading.Timer(35.0, _on_timeout)
    _watchdog.daemon = True
    _watchdog.start()

    def do_fetch(song, performer):
        return lrc.get_lyrics(song, performer)

    result = None
    last_error = None

    try:
        result = do_fetch(title, artist)
    except Exception as e:
        last_error = str(e)

    # Retry with swapped Title/Artist if it failed and the title looks
    # like a YouTube-style "Artist - Title" string with no artist tag of
    # its own -- same heuristic the old syncedlyrics-backed version used.
    if result is None and "-" in title and not artist:
        parts = [p.strip() for p in title.split("-", 1)]
        if len(parts) == 2 and parts[0] and parts[1]:
            try:
                result = do_fetch(parts[1], parts[0])
            except Exception as e:
                last_error = str(e)

    if result is None:
        detail = last_error or f"no lyrics found for: {title} {artist}".strip()
        error = "EXCEPTION" if last_error else "NOT_FOUND"
        emit({"ok": False, "error": error, "detail": detail})

    # lrc.py hands back {"source", "ttml", "lrc", "enhanced_lrc"}. Prefer
    # the word-level enhanced LRC (only Better Lyrics provides one); fall
    # back to the plain line-synced LRC (LRCLIB, or Better Lyrics if its
    # TTML somehow had no per-word spans). Re-checking for actual "<...>"
    # timestamps rather than trusting which field was non-empty mirrors
    # the same "don't trust the flag, check the text" caution the old
    # script applied to syncedlyrics' `enhanced` request flag.
    lrc_text = result.get("enhanced_lrc") or result.get("lrc") or ""
    if not lrc_text:
        emit({
            "ok": False,
            "error": "NOT_FOUND",
            "detail": f"no lyrics found for: {title} {artist}".strip()
        })

    emit({
        "ok": True,
        "enhanced": has_word_timestamps(lrc_text),
        "lrc": lrc_text,
        "source": result.get("source", "lrc.py"),
    })


if __name__ == "__main__":
    main()
