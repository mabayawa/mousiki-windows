#!/usr/bin/env python3
"""Spotify helper for mousiki.

Why a Python helper rather than C++: mousiki does no networking in C++ at all
-- every byte that leaves the machine already goes through yt-dlp or a script
in this directory. Adding an HTTP stack to the C++ would mean WinHTTP on
Windows and libcurl everywhere else, which is a new dependency and a second
code path. lyrics_fetcher.cpp already proves the subprocess pattern works, and
these calls are user-initiated (opening a playlist), not per-frame, so the
spawn cost is irrelevant.

Every subcommand prints a single JSON object on stdout. Progress and errors go
to stderr, because stdout is parsed. Exit code is 0 whenever JSON was emitted,
including for handled errors -- the caller reads "ok".

Auth is the Authorization Code flow with PKCE. There is deliberately no client
secret: a desktop app cannot keep one, which is the entire reason PKCE exists.
"""

import base64
import hashlib
import http.server
import json
import os
import pathlib
import secrets
import sys
import threading
import time
import urllib.parse
import webbrowser

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except (ValueError, OSError):
            pass

try:
    import requests
except ImportError:
    print(json.dumps({
        "ok": False, "error": "MODULE_MISSING",
        "detail": "the 'requests' package is not available (pip install requests)",
    }))
    sys.exit(0)

AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"
API = "https://api.spotify.com/v1"

REDIRECT_PORT = 8888
REDIRECT_URI = "http://127.0.0.1:%d/callback" % REDIRECT_PORT

# playlist-*      : your playlists, including private and collaborative ones
# user-library-*  : Liked Songs
# user-*-playback : see and control what is playing, i.e. point librespot at a
#                   track once it has registered itself as a Connect device
# streaming       : required for Connect playback control
SCOPES = " ".join([
    "playlist-read-private",
    "playlist-read-collaborative",
    "user-library-read",
    "user-read-playback-state",
    "user-modify-playback-state",
    "streaming",
])


def emit(obj):
    print(json.dumps(obj, ensure_ascii=False))
    sys.exit(0)


def fail(error, detail):
    emit({"ok": False, "error": error, "detail": detail})


def cache_path():
    # Mirrors the layout the C++ uses: $HOME/.cache/mousiki, where win_compat
    # points HOME at %USERPROFILE%.
    home = os.environ.get("HOME") or os.environ.get("USERPROFILE") or "."
    d = pathlib.Path(home) / ".cache" / "mousiki"
    d.mkdir(parents=True, exist_ok=True)
    return d / "spotify_token.json"


def load_tokens():
    p = cache_path()
    if not p.is_file():
        return None
    try:
        with open(p, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def save_tokens(tok):
    p = cache_path()
    tmp = p.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(tok, fh)
    os.replace(tmp, p)
    # This file is a bearer credential for the user's account. On POSIX that
    # means 0600; on Windows the per-user profile directory is already the
    # protection boundary and chmod is a no-op, so do not pretend otherwise.
    if os.name == "posix":
        try:
            os.chmod(p, 0o600)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# PKCE login
# ---------------------------------------------------------------------------

class _CallbackHandler(http.server.BaseHTTPRequestHandler):
    result = {}

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/callback":
            self.send_response(404)
            self.end_headers()
            return
        q = urllib.parse.parse_qs(parsed.query)
        _CallbackHandler.result = {k: v[0] for k, v in q.items()}
        ok = "code" in _CallbackHandler.result
        body = (
            "<html><body style='font-family:system-ui;padding:3rem;text-align:center'>"
            "<h2>%s</h2><p>%s</p></body></html>"
            % (
                "mousiki is connected" if ok else "Authorisation failed",
                "You can close this tab and go back to the terminal."
                if ok else _CallbackHandler.result.get("error", "unknown error"),
            )
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass  # stdout is JSON; the default handler logs to stderr noisily


def do_login(client_id, timeout=180):
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(64)).decode().rstrip("=")
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode()).digest()
    ).decode().rstrip("=")
    state = secrets.token_urlsafe(16)

    params = {
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPES,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
        "state": state,
    }
    url = AUTH_URL + "?" + urllib.parse.urlencode(params)

    try:
        server = http.server.HTTPServer(("127.0.0.1", REDIRECT_PORT), _CallbackHandler)
    except OSError as e:
        fail("PORT_BUSY", "cannot listen on 127.0.0.1:%d (%s). Close whatever is "
                          "using it and try again." % (REDIRECT_PORT, e))

    _CallbackHandler.result = {}
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()

    print("Opening your browser to authorise mousiki...", file=sys.stderr)
    print(url, file=sys.stderr)
    try:
        webbrowser.open(url)
    except Exception:
        pass

    deadline = time.time() + timeout
    while not _CallbackHandler.result and time.time() < deadline:
        time.sleep(0.2)
    server.shutdown()

    res = _CallbackHandler.result
    if not res:
        fail("TIMEOUT", "no response from the browser within %ds" % timeout)
    if "error" in res:
        fail("DENIED", "Spotify returned: %s" % res["error"])
    if res.get("state") != state:
        # Guards against a different page on the machine hitting our callback.
        fail("STATE_MISMATCH", "the callback did not match this login attempt")

    r = requests.post(TOKEN_URL, timeout=20, data={
        "client_id": client_id,
        "grant_type": "authorization_code",
        "code": res["code"],
        "redirect_uri": REDIRECT_URI,
        "code_verifier": verifier,
    })
    if r.status_code != 200:
        fail("TOKEN_EXCHANGE", "HTTP %d: %s" % (r.status_code, r.text[:300]))

    tok = r.json()
    tok["client_id"] = client_id
    tok["expires_at"] = time.time() + tok.get("expires_in", 3600) - 60
    save_tokens(tok)
    emit({"ok": True, "detail": "authorised"})


def access_token(client_id):
    tok = load_tokens()
    if not tok:
        return None, "not logged in"
    if tok.get("expires_at", 0) > time.time():
        return tok["access_token"], None
    refresh = tok.get("refresh_token")
    if not refresh:
        return None, "session expired and no refresh token"
    r = requests.post(TOKEN_URL, timeout=20, data={
        "client_id": client_id or tok.get("client_id"),
        "grant_type": "refresh_token",
        "refresh_token": refresh,
    })
    if r.status_code != 200:
        return None, "refresh failed (HTTP %d) -- log in again" % r.status_code
    new = r.json()
    # Spotify does not always return a new refresh token; keep the old one.
    tok.update(new)
    tok.setdefault("refresh_token", refresh)
    tok["expires_at"] = time.time() + new.get("expires_in", 3600) - 60
    save_tokens(tok)
    return tok["access_token"], None


def api(token, method, path, **kw):
    r = requests.request(method, API + path, timeout=20,
                         headers={"Authorization": "Bearer " + token}, **kw)
    return r


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------

def cmd_status(client_id):
    tok, err = access_token(client_id)
    if not tok:
        emit({"ok": False, "error": "NO_AUTH", "detail": err})
    r = api(tok, "GET", "/me")
    if r.status_code != 200:
        emit({"ok": False, "error": "API", "detail": "HTTP %d" % r.status_code})
    me = r.json()
    emit({
        "ok": True,
        "user": me.get("display_name") or me.get("id"),
        "product": me.get("product"),          # "premium" / "free"
        "country": me.get("country"),
    })


def cmd_playlists(client_id):
    tok, err = access_token(client_id)
    if not tok:
        emit({"ok": False, "error": "NO_AUTH", "detail": err})
    out, url = [], "/me/playlists?limit=50"
    while url:
        r = api(tok, "GET", url)
        if r.status_code != 200:
            emit({"ok": False, "error": "API", "detail": "HTTP %d" % r.status_code})
        d = r.json()
        for p in d.get("items", []):
            if not p:
                continue
            # Spotify moved the count: a playlist object's "tracks" field is
            # now null and the total lives under "items". Read both so this
            # keeps working whichever shape the API returns.
            count = (p.get("items") or p.get("tracks") or {}).get("total", 0)
            out.append({
                "id": p.get("id", ""),
                "name": p.get("name", ""),
                "owner": (p.get("owner") or {}).get("display_name", ""),
                "tracks": count,
                "uri": p.get("uri", ""),
            })
        nxt = d.get("next")
        url = nxt[len(API):] if nxt and nxt.startswith(API) else None
    emit({"ok": True, "playlists": out})


def _unwrap(entry):
    """A playlist/library entry wraps the track object. That wrapper key used
    to be "track"; on /playlists/{id}/items it is now "item". Accept either."""
    if not entry:
        return None
    return entry.get("item") or entry.get("track")


def _track_obj(t):
    if not t or t.get("is_local"):
        return None
    artists = ", ".join(a.get("name", "") for a in (t.get("artists") or []) if a)
    return {
        "id": t.get("id", ""),
        "uri": t.get("uri", ""),
        "title": t.get("name", ""),
        "artist": artists,
        "album": (t.get("album") or {}).get("name", ""),
        "duration_sec": round((t.get("duration_ms") or 0) / 1000.0, 3),
    }


def cmd_tracks(client_id, playlist_id):
    tok, err = access_token(client_id)
    if not tok:
        emit({"ok": False, "error": "NO_AUTH", "detail": err})
    out = []
    # /tracks returns 403 Forbidden now -- the endpoint was renamed to /items.
    url = "/playlists/%s/items?limit=100" % urllib.parse.quote(playlist_id)
    while url:
        r = api(tok, "GET", url)
        if r.status_code != 200:
            emit({"ok": False, "error": "API", "detail": "HTTP %d" % r.status_code})
        d = r.json()
        for item in d.get("items", []):
            obj = _track_obj(_unwrap(item))
            if obj and obj["uri"]:
                out.append(obj)
        nxt = d.get("next")
        url = nxt[len(API):] if nxt and nxt.startswith(API) else None
    emit({"ok": True, "tracks": out})


def cmd_saved(client_id):
    tok, err = access_token(client_id)
    if not tok:
        emit({"ok": False, "error": "NO_AUTH", "detail": err})
    out, url = [], "/me/tracks?limit=50"
    while url:
        r = api(tok, "GET", url)
        if r.status_code != 200:
            emit({"ok": False, "error": "API", "detail": "HTTP %d" % r.status_code})
        d = r.json()
        for item in d.get("items", []):
            obj = _track_obj(_unwrap(item))
            if obj and obj["uri"]:
                out.append(obj)
        nxt = d.get("next")
        url = nxt[len(API):] if nxt and nxt.startswith(API) else None
    emit({"ok": True, "tracks": out})


# Spotify caps /search at 10 results per request. limit=20 and limit=50 are
# both rejected outright with "Invalid limit", despite paging limits of 50-100
# elsewhere in the API.
SEARCH_LIMIT = 10


def cmd_search(client_id, query, limit=SEARCH_LIMIT):
    tok, err = access_token(client_id)
    if not tok:
        emit({"ok": False, "error": "NO_AUTH", "detail": err})
    r = api(tok, "GET", "/search?" + urllib.parse.urlencode(
        {"q": query, "type": "track", "limit": min(limit, SEARCH_LIMIT)}))
    if r.status_code != 200:
        emit({"ok": False, "error": "API", "detail": "HTTP %d" % r.status_code})
    items = ((r.json().get("tracks") or {}).get("items") or [])
    out = [o for o in (_track_obj(t) for t in items) if o and o["uri"]]
    emit({"ok": True, "tracks": out})


def cmd_devices(client_id):
    tok, err = access_token(client_id)
    if not tok:
        emit({"ok": False, "error": "NO_AUTH", "detail": err})
    r = api(tok, "GET", "/me/player/devices")
    if r.status_code != 200:
        emit({"ok": False, "error": "API", "detail": "HTTP %d" % r.status_code})
    out = [{
        "id": d.get("id", ""),
        "name": d.get("name", ""),
        "type": d.get("type", ""),
        "active": bool(d.get("is_active")),
    } for d in (r.json().get("devices") or [])]
    emit({"ok": True, "devices": out})


def cmd_play(client_id, device_id, uri):
    tok, err = access_token(client_id)
    if not tok:
        emit({"ok": False, "error": "NO_AUTH", "detail": err})
    body = {"uris": [uri]} if uri.startswith("spotify:track:") else {"context_uri": uri}
    r = api(tok, "PUT", "/me/player/play?" + urllib.parse.urlencode({"device_id": device_id}),
            json=body)
    # 204 is success; 202 means the device is still waking up.
    if r.status_code in (200, 202, 204):
        emit({"ok": True, "detail": "playing"})
    if r.status_code == 404:
        emit({"ok": False, "error": "NO_DEVICE",
              "detail": "Spotify does not see that device. Is librespot running?"})
    if r.status_code == 403:
        emit({"ok": False, "error": "FORBIDDEN",
              "detail": "Spotify refused playback -- Premium is required for this."})
    emit({"ok": False, "error": "API", "detail": "HTTP %d: %s" % (r.status_code, r.text[:200])})


def main():
    args = sys.argv[1:]
    if not args:
        fail("USAGE", "spotify.py <login|status|playlists|tracks|saved|search|devices|play> ...")

    client_id = os.environ.get("MOUSIKI_SPOTIFY_CLIENT_ID", "")
    if "--client-id" in args:
        i = args.index("--client-id")
        if i + 1 >= len(args):
            fail("USAGE", "--client-id needs a value")
        client_id = args[i + 1]
        del args[i:i + 2]

    cmd = args[0]
    rest = args[1:]

    if cmd != "status" and not client_id and cmd == "login":
        fail("NO_CLIENT_ID",
             "no Spotify client id. Set SpotifyClientId in config.txt, or pass "
             "--client-id, or set MOUSIKI_SPOTIFY_CLIENT_ID.")

    try:
        if cmd == "login":
            do_login(client_id)
        elif cmd == "status":
            cmd_status(client_id)
        elif cmd == "playlists":
            cmd_playlists(client_id)
        elif cmd == "tracks":
            if not rest:
                fail("USAGE", "tracks <playlist_id>")
            cmd_tracks(client_id, rest[0])
        elif cmd == "saved":
            cmd_saved(client_id)
        elif cmd == "search":
            if not rest:
                fail("USAGE", "search <query>")
            cmd_search(client_id, " ".join(rest))
        elif cmd == "devices":
            cmd_devices(client_id)
        elif cmd == "play":
            if len(rest) < 2:
                fail("USAGE", "play <device_id> <spotify_uri>")
            cmd_play(client_id, rest[0], rest[1])
        else:
            fail("USAGE", "unknown subcommand: %s" % cmd)
    except requests.RequestException as e:
        fail("NETWORK", str(e))


if __name__ == "__main__":
    main()
