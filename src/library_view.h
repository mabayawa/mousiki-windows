#pragma once
#include <string>
#include <vector>

#include "online_source.h"
#include "spotify_source.h"

// The arithmetic behind the Spotify library overlay's two panes, kept out of
// app.cpp so it can be tested at every terminal width instead of by resizing a
// window by hand.
//
// A mis-sized column is the bug class this codebase keeps fighting -- there are
// five separate comments in app.cpp warning that display_width counts ANSI
// escape bytes as columns, all of them scars from a pad that leaked reverse
// video or truncated through a reset. So the widths are computed here, asserted
// to sum to exactly the box's inner width, and app.cpp only builds strings.
namespace muisc {

// Column widths for one row. Every field is a display-column count, and for any
// inner wide enough to hold the mandatory columns they sum to EXACTLY `inner`:
// the name/title column absorbs the remainder, so there is never a spare column
// for a border to drift into.
struct LeftCols {
    int idx = 0;    // row number
    int kind = 0;   // the playlist/album glyph
    int name = 0;
    int owner = 0;  // 0 = column omitted at this width
    int count = 0;  // 0 = column omitted at this width
};
struct RightCols {
    int idx = 0;
    int mark = 0;   // the "already queued" glyph
    int title = 0;
    int artist = 0; // 0 = column omitted
    int dur = 0;    // 0 = column omitted
};

// `sep_w` is the separator plus its trailing space, measured rather than assumed
// because settings_.list_separator is user-configurable -- a two-character
// separator would otherwise shift every column right by one.
//
// Columns drop least-important-first as the pane narrows (count, then owner),
// and a column is only added if the name still keeps a readable share. That is
// the same principle as the queue panel's responsive artist column: a narrow
// pane loses a column rather than crushing the one that identifies the row.
LeftCols  left_pane_columns(int inner, int sep_w, int kind_w);
RightCols right_pane_columns(int inner, int sep_w);

// Indices of the items whose name or owner contains `needle`, case-insensitively.
//
// ASCII case folding only. A Turkish dotless i or a Greek final sigma will not
// fold correctly, and that is a deliberate limit rather than an oversight: doing
// it properly needs a case-mapping table this tree does not have, and substring
// matching on the raw bytes still works for every non-ASCII name -- you just
// have to match its case. An empty needle returns every index.
std::vector<int> library_filter_indices(const std::vector<SpotifyLibraryItem>& items,
                                        const std::string& needle);

// Cursor-follows-window scrolling, the same rule the list and queue panels use.
// Clamps the cursor into [0, count) first, then moves `scroll` the minimum
// distance needed to keep the cursor on screen. Both are in/out.
void clamp_cursor_scroll(int& cursor, int& scroll, int count, int visible);

// The "( N more )" the box bottom border carries when rows are off-screen, or ""
// when everything fits.
std::string overflow_footer(int total, int scroll, int visible);

// A collection name shortened so it still fits a box top border once the corner,
// the leading dash, the padding spaces and a trailing count are accounted for.
//
// Necessary because box_top() ends in pad_right(s, total_width), which for an
// over-wide label calls utf8_take and cuts off the CLOSING CORNER GLYPH -- the
// box loses its right-hand edge rather than the label losing its tail.
std::string library_label(const std::string& name, int total_width);

// Sums durations, skipping unknown (negative) ones rather than subtracting them.
double total_duration_sec(const std::vector<OnlineResult>& items);

// "5h 12m", "7m", or "--" when there is nothing known to show. For the pane
// header, where a track-level mm:ss would be meaningless.
std::string fmt_duration_long(double sec);

} // namespace muisc
