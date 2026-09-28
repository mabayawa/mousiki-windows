"""Tests for the parts of the Spotify helper that only Python can cover.

Pagination lives in scripts/spotify.py, not in C++: the helper follows every
`next` link and hands the C++ side one flat array, so the C++ tests cannot see
it at all. These cover that, plus the response shapes the parser never sees
because the helper has already normalised them.

No network. spotify.access_token and spotify.api are monkeypatched with a
table-driven fake keyed by request path.

Run: python -m unittest discover -s tests -p "test_*.py"
"""
import io
import json
import os
import sys
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))

try:
    import spotify
except ImportError as exc:                                   # pragma: no cover
    raise unittest.SkipTest("scripts/spotify.py needs requests: %s" % exc)


class FakeResponse:
    def __init__(self, payload, status=200):
        self._payload = payload
        self.status_code = status

    def json(self):
        return self._payload


class HelperTest(unittest.TestCase):
    """Base: fake the token and the HTTP layer, capture the one JSON object."""

    def setUp(self):
        self._real_token = spotify.access_token
        self._real_api = spotify.api
        spotify.access_token = lambda client_id: ("fake-token", None)
        self.pages = {}
        self.requested = []

    def tearDown(self):
        spotify.access_token = self._real_token
        spotify.api = self._real_api

    def serve(self, pages, status=200):
        """pages maps an API path to the payload returned for it."""
        self.pages = pages

        def fake_api(token, method, path, **kw):
            self.requested.append(path)
            if path not in self.pages:
                raise AssertionError("unexpected request path: %r" % path)
            return FakeResponse(self.pages[path], status)

        spotify.api = fake_api

    def run_cmd(self, fn, *args):
        """Every subcommand prints one JSON object and exits 0, even on error."""
        buf = io.StringIO()
        with redirect_stdout(buf):
            with self.assertRaises(SystemExit) as caught:
                fn("client-id", *args)
        self.assertEqual(caught.exception.code, 0)
        raw = buf.getvalue()
        obj = json.loads(raw)
        # Exactly ONE object, with nothing trailing it.
        self.assertEqual(raw.strip(), json.dumps(obj, ensure_ascii=False))
        return obj, raw


class TestPlaylists(HelperTest):
    def test_follows_next_across_pages_and_concatenates(self):
        api = spotify.API
        self.serve({
            "/me/playlists?limit=50": {
                "items": [{"id": "p1", "name": "one",
                           "owner": {"display_name": "me", "id": "u1"},
                           "tracks": {"total": 3}, "uri": "spotify:playlist:p1"}],
                "next": api + "/me/playlists?offset=50&limit=50",
            },
            "/me/playlists?offset=50&limit=50": {
                "items": [{"id": "p2", "name": "two",
                           "owner": {"display_name": "you", "id": "u2"},
                           "items": {"total": 7}, "uri": "spotify:playlist:p2"}],
                "next": None,
            },
        })
        obj, _ = self.run_cmd(spotify.cmd_playlists)
        self.assertTrue(obj["ok"])
        self.assertEqual([p["id"] for p in obj["playlists"]], ["p1", "p2"])
        self.assertEqual(len(self.requested), 2)
        # Both count shapes: Spotify moved the total from "tracks" to "items".
        self.assertEqual(obj["playlists"][0]["tracks"], 3)
        self.assertEqual(obj["playlists"][1]["tracks"], 7)

    def test_emits_owner_id_and_kind(self):
        self.serve({"/me/playlists?limit=50": {
            "items": [{"id": "p1", "name": "one",
                       "owner": {"display_name": "me", "id": "u1"},
                       "tracks": {"total": 1}, "uri": "spotify:playlist:p1"}],
            "next": None}})
        obj, _ = self.run_cmd(spotify.cmd_playlists)
        self.assertEqual(obj["playlists"][0]["owner_id"], "u1")
        self.assertEqual(obj["playlists"][0]["kind"], "playlist")

    def test_null_display_name_becomes_empty_string(self):
        self.serve({"/me/playlists?limit=50": {
            "items": [{"id": "p1", "name": "one",
                       "owner": {"display_name": None, "id": "u1"},
                       "tracks": {"total": 1}, "uri": ""}],
            "next": None}})
        obj, _ = self.run_cmd(spotify.cmd_playlists)
        self.assertEqual(obj["playlists"][0]["owner"], "")
        self.assertEqual(obj["playlists"][0]["owner_id"], "u1")

    def test_a_null_item_is_skipped(self):
        self.serve({"/me/playlists?limit=50": {
            "items": [None, {"id": "p1", "name": "one", "owner": {"id": "u1"},
                             "tracks": {"total": 1}, "uri": ""}],
            "next": None}})
        obj, _ = self.run_cmd(spotify.cmd_playlists)
        self.assertEqual(len(obj["playlists"]), 1)


class TestStatus(HelperTest):
    def test_emits_raw_id_alongside_display_name(self):
        self.serve({"/me": {"display_name": "Bono", "id": "u1",
                            "product": "premium", "country": "PH"}})
        obj, _ = self.run_cmd(spotify.cmd_status)
        self.assertEqual(obj["id"], "u1")        # the classification key
        self.assertEqual(obj["user"], "Bono")    # unchanged for existing callers

    def test_null_product_is_passed_through_not_defaulted(self):
        self.serve({"/me": {"display_name": "Bono", "id": "u1",
                            "product": None, "country": "PH"}})
        obj, _ = self.run_cmd(spotify.cmd_status)
        self.assertIsNone(obj["product"])        # unknown, never "free"


class TestAlbums(HelperTest):
    def test_unwraps_the_album_and_joins_artists(self):
        self.serve({"/me/albums?limit=50": {
            "items": [{"added_at": "x", "album": {
                "id": "a1", "name": "In Rainbows",
                "artists": [{"name": "Radiohead"}, {"name": "Guest"}],
                "tracks": {"total": 10}, "uri": "spotify:album:a1"}}],
            "next": None}})
        obj, _ = self.run_cmd(spotify.cmd_albums)
        row = obj["albums"][0]
        self.assertEqual(row["name"], "In Rainbows")
        self.assertEqual(row["owner"], "Radiohead, Guest")
        self.assertEqual(row["owner_id"], "")    # never classifiable as mine
        self.assertEqual(row["kind"], "album")
        self.assertEqual(row["tracks"], 10)

    def test_paginates(self):
        api = spotify.API
        self.serve({
            "/me/albums?limit=50": {
                "items": [{"album": {"id": "a1", "name": "one", "artists": [],
                                     "tracks": {"total": 1}, "uri": ""}}],
                "next": api + "/me/albums?offset=50&limit=50"},
            "/me/albums?offset=50&limit=50": {
                "items": [{"album": {"id": "a2", "name": "two", "artists": [],
                                     "tracks": {"total": 2}, "uri": ""}}],
                "next": None},
        })
        obj, _ = self.run_cmd(spotify.cmd_albums)
        self.assertEqual([a["id"] for a in obj["albums"]], ["a1", "a2"])

    def test_a_null_album_is_skipped(self):
        self.serve({"/me/albums?limit=50": {
            "items": [{"added_at": "x", "album": None}], "next": None}})
        obj, _ = self.run_cmd(spotify.cmd_albums)
        self.assertEqual(obj["albums"], [])


class TestAlbumTracks(HelperTest):
    def test_fills_the_album_name_into_simplified_tracks(self):
        # /albums/{id} track objects are SIMPLIFIED: no album field at all.
        self.serve({"/albums/a1": {
            "name": "In Rainbows",
            "tracks": {"items": [{"id": "t1", "uri": "spotify:track:t1", "name": "Nude",
                                  "artists": [{"name": "Radiohead"}],
                                  "duration_ms": 254000}],
                       "next": None}}})
        obj, _ = self.run_cmd(spotify.cmd_album_tracks, "a1")
        track = obj["tracks"][0]
        self.assertEqual(track["album"], "In Rainbows")
        self.assertEqual(track["artist"], "Radiohead")
        self.assertEqual(track["duration_sec"], 254.0)

    def test_follows_the_nested_next(self):
        api = spotify.API
        self.serve({
            "/albums/a1": {"name": "Alb", "tracks": {
                "items": [{"id": "t1", "uri": "u1", "name": "one",
                           "artists": [], "duration_ms": 1000}],
                "next": api + "/albums/a1/tracks?offset=50"}},
            "/albums/a1/tracks?offset=50": {
                "items": [{"id": "t2", "uri": "u2", "name": "two",
                           "artists": [], "duration_ms": 2000}],
                "next": None},
        })
        obj, _ = self.run_cmd(spotify.cmd_album_tracks, "a1")
        self.assertEqual([t["uri"] for t in obj["tracks"]], ["u1", "u2"])
        self.assertEqual([t["album"] for t in obj["tracks"]], ["Alb", "Alb"])

    def test_uses_one_request_for_a_short_album(self):
        # The whole reason for reading /albums/{id} instead of
        # /albums/{id}/tracks: the name and the first page arrive together.
        self.serve({"/albums/a1": {"name": "Alb", "tracks": {"items": [], "next": None}}})
        self.run_cmd(spotify.cmd_album_tracks, "a1")
        self.assertEqual(self.requested, ["/albums/a1"])


class TestTrackObject(unittest.TestCase):
    def test_local_files_are_dropped(self):
        self.assertIsNone(spotify._track_obj({"is_local": True, "uri": "x", "name": "y"}))

    def test_unwrap_accepts_both_wrapper_keys(self):
        # The key was "track"; on /playlists/{id}/items it is now "item".
        self.assertEqual(spotify._unwrap({"item": {"id": "a"}}), {"id": "a"})
        self.assertEqual(spotify._unwrap({"track": {"id": "b"}}), {"id": "b"})
        self.assertIsNone(spotify._unwrap(None))

    def test_missing_album_falls_back_to_the_supplied_name(self):
        obj = spotify._track_obj({"uri": "u", "name": "n", "artists": []}, "Fallback")
        self.assertEqual(obj["album"], "Fallback")
        # A real album field still wins over the fallback.
        obj = spotify._track_obj({"uri": "u", "name": "n", "artists": [],
                                  "album": {"name": "Real"}}, "Fallback")
        self.assertEqual(obj["album"], "Real")

    def test_existing_call_sites_still_work_without_the_new_argument(self):
        obj = spotify._track_obj({"uri": "u", "name": "n", "artists": [],
                                  "album": {"name": "Real"}})
        self.assertEqual(obj["album"], "Real")


class TestErrorEnvelope(HelperTest):
    def test_http_401_yields_one_object_and_exit_zero(self):
        self.serve({"/me/albums?limit=50": {}}, status=401)
        obj, _ = self.run_cmd(spotify.cmd_albums)
        self.assertFalse(obj["ok"])
        self.assertEqual(obj["error"], "API")

    def test_album_tracks_reports_the_same_way(self):
        self.serve({"/albums/a1": {}}, status=404)
        obj, _ = self.run_cmd(spotify.cmd_album_tracks, "a1")
        self.assertFalse(obj["ok"])


class TestEnsureAsciiFalse(HelperTest):
    def test_cjk_titles_are_emitted_as_raw_utf8(self):
        # Load-bearing: src/tiny_json.h cannot decode u-escapes, so an escaped
        # response would silently mangle every non-ASCII title. This asserts the
        # contract from the PRODUCING side; the C++ suite asserts it from the
        # reading side.
        cjk = "\u96fb\u5149\u77f3\u706b"
        self.serve({"/albums/a1": {"name": "strobo", "tracks": {
            "items": [{"id": "t1", "uri": "u1", "name": cjk,
                       "artists": [], "duration_ms": 1000}], "next": None}}})
        obj, raw = self.run_cmd(spotify.cmd_album_tracks, "a1")
        self.assertEqual(obj["tracks"][0]["title"], cjk)
        self.assertIn(cjk, raw)
        self.assertNotIn("u96fb", raw)


if __name__ == "__main__":
    unittest.main()
