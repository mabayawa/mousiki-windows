#include "library_view.h"
#include "terminal_ui.h"
#include "tiny_test.h"

using namespace muisc;
using namespace muisc::test;

namespace {

SpotifyLibraryItem item(const char* name, const char* owner) {
    SpotifyLibraryItem it;
    it.id = name;
    it.name = name;
    it.owner = owner;
    return it;
}

int left_sum(const LeftCols& c, int sep_w) {
    return c.idx + c.kind + sep_w + c.name + (c.owner ? sep_w + c.owner : 0) +
           (c.count ? sep_w + c.count : 0);
}
int right_sum(const RightCols& c, int sep_w) {
    return c.idx + c.mark + sep_w + c.title + (c.artist ? sep_w + c.artist : 0) +
           (c.dur ? sep_w + c.dur : 0);
}

// Raw UTF-8 rather than u-escapes, the same way disk_art.cpp embeds its braille:
// four double-width CJK glyphs, eight display columns.
const char* kCjk = "電光石火";

} // namespace

// --- the width invariant ---------------------------------------------------
// The test that matters most here. A row whose columns do not sum to the box
// inner width is the bug class app.cpp carries five separate warning comments
// about, and it is otherwise only findable by resizing a terminal by hand.

TEST(left_columns_never_overrun_the_box_at_any_width) {
    for (int inner = 1; inner <= 300; ++inner) {
        for (int sep_w = 2; sep_w <= 3; ++sep_w) {
            LeftCols c = left_pane_columns(inner, sep_w, 1);
            CHECK(c.name >= 4);
            CHECK(c.owner == 0 || c.owner >= 6);
            CHECK(c.count == 0 || c.count == 5);
            // Wide enough for the mandatory columns means the sum is EXACT, so
            // no spare column is left for a border to drift into.
            if (inner >= 3 + 1 + sep_w + 4) CHECK_EQ(left_sum(c, sep_w), inner);
        }
    }
}

TEST(right_columns_never_overrun_the_box_at_any_width) {
    for (int inner = 1; inner <= 300; ++inner) {
        for (int sep_w = 2; sep_w <= 3; ++sep_w) {
            RightCols c = right_pane_columns(inner, sep_w);
            CHECK(c.title >= 4);
            CHECK(c.artist == 0 || c.artist >= 6);
            CHECK(c.dur == 0 || c.dur == 5);
            if (inner >= 3 + 1 + sep_w + 4) CHECK_EQ(right_sum(c, sep_w), inner);
        }
    }
}

TEST(columns_degrade_least_important_first) {
    // W=40 (pane 20, inner 16): the name only. Two panes at 40 columns cannot
    // hold more than the thing that identifies the row.
    LeftCols n = left_pane_columns(16, 2, 1);
    CHECK_EQ(n.owner, 0);
    CHECK_EQ(n.count, 0);
    CHECK_EQ(n.name, 10);
    // W=80 (inner 36): the count earns its five columns, the owner does not.
    LeftCols m = left_pane_columns(36, 2, 1);
    CHECK_EQ(m.owner, 0);
    CHECK_EQ(m.count, 5);
    CHECK_EQ(m.name, 23);
    // W=120 (inner 56): everything fits.
    LeftCols w = left_pane_columns(56, 2, 1);
    CHECK_EQ(w.owner, 14);
    CHECK_EQ(w.count, 5);
    CHECK_EQ(w.name, 27);
    // W=200 (inner 96): the name absorbs the slack rather than the owner column
    // growing to fill it.
    LeftCols x = left_pane_columns(96, 2, 1);
    CHECK_EQ(x.owner, 14);
    CHECK_EQ(x.name, 67);
}

TEST(right_columns_match_the_same_breakpoints) {
    CHECK_EQ(right_pane_columns(16, 2).title, 10);
    CHECK_EQ(right_pane_columns(36, 2).artist, 0);
    CHECK_EQ(right_pane_columns(36, 2).dur, 5);
    CHECK_EQ(right_pane_columns(56, 2).artist, 14);
    CHECK_EQ(right_pane_columns(56, 2).title, 27);
    CHECK_EQ(right_pane_columns(96, 2).artist, 16);
    CHECK_EQ(right_pane_columns(96, 2).title, 65);
}

TEST(a_wider_separator_costs_the_name_not_the_box) {
    LeftCols one = left_pane_columns(56, 2, 1);
    LeftCols two = left_pane_columns(56, 3, 1);
    CHECK(two.name < one.name);
    CHECK_EQ(left_sum(two, 3), 56);
}

TEST(a_cjk_name_pads_to_exactly_the_column_width) {
    // The boundary case: utf8_take stops a column early when the next glyph is
    // double-width, and pad_right fills the gap. If the two disagree the row is
    // one column wrong and the border moves.
    const std::string cjk = std::string(kCjk) + kCjk;
    for (int w = 1; w <= 20; ++w) {
        CHECK_EQ(display_width(pad_right(truncate_str(cjk, w), w)), w);
    }
}

TEST(an_empty_filter_returns_everything) {
    std::vector<SpotifyLibraryItem> v{item("a", "x"), item("b", "y")};
    CHECK_EQ(static_cast<int>(library_filter_indices(v, "").size()), 2);
}

TEST(filtering_is_case_insensitive_and_matches_the_owner_too) {
    std::vector<SpotifyLibraryItem> v{item("Chill Vibes", "mabayawa"),
                                      item("gym", "mabayawa"),
                                      item("In Rainbows", "Radiohead")};
    CHECK_EQ(static_cast<int>(library_filter_indices(v, "chi").size()), 1);
    CHECK_EQ(library_filter_indices(v, "CHI")[0], 0);
    CHECK_EQ(library_filter_indices(v, "radio")[0], 2);
    CHECK(library_filter_indices(v, "zzz").empty());
}

TEST(filtering_matches_non_ascii_names_byte_wise) {
    std::vector<SpotifyLibraryItem> v{item(kCjk, "Vaundy")};
    CHECK_EQ(static_cast<int>(library_filter_indices(v, kCjk).size()), 1);
}

TEST(scroll_follows_the_cursor_down_then_back_up) {
    int cur = 0, scroll = 0;
    clamp_cursor_scroll(cur, scroll, 100, 9);
    CHECK_EQ(scroll, 0);
    cur = 9;
    clamp_cursor_scroll(cur, scroll, 100, 9);
    CHECK_EQ(scroll, 1);
    cur = 0;
    clamp_cursor_scroll(cur, scroll, 100, 9);
    CHECK_EQ(scroll, 0);
}

TEST(a_cursor_past_the_end_clamps) {
    int cur = 500, scroll = 400;
    clamp_cursor_scroll(cur, scroll, 10, 9);
    CHECK_EQ(cur, 9);
    CHECK_EQ(scroll, 1);
}

TEST(an_empty_list_resets_both) {
    int cur = 5, scroll = 3;
    clamp_cursor_scroll(cur, scroll, 0, 9);
    CHECK_EQ(cur, 0);
    CHECK_EQ(scroll, 0);
}

TEST(a_shrinking_list_pulls_the_window_back) {
    // What happens while a filter is being typed: the list gets shorter under a
    // scrolled window, which would otherwise render a pane of blank rows with
    // the cursor nowhere in sight.
    int cur = 0, scroll = 90;
    clamp_cursor_scroll(cur, scroll, 5, 9);
    CHECK_EQ(scroll, 0);
}

TEST(zero_visible_rows_does_not_crash) {
    int cur = 4, scroll = 2;
    clamp_cursor_scroll(cur, scroll, 10, 0);
    CHECK_EQ(scroll, 0);
}

TEST(overflow_footer_counts_only_what_is_off_screen) {
    CHECK_EQ(overflow_footer(100, 0, 9), std::string("( 91 more )"));
    CHECK_EQ(overflow_footer(10, 1, 9), std::string(""));
    CHECK_EQ(overflow_footer(9, 0, 9), std::string(""));
    CHECK_EQ(overflow_footer(0, 0, 9), std::string(""));
}

TEST(a_long_label_leaves_room_for_the_closing_corner) {
    // box_top ends in pad_right(s, total_width), so an over-wide label loses the
    // corner glyph rather than its own tail. The label has to be short first.
    const std::string huge(200, 120);
    for (int w = 20; w <= 120; w += 20) {
        CHECK(display_width(library_label(huge, w)) <= w - 10);
    }
    CHECK_EQ(library_label("short", 62), std::string("short"));
    CHECK_EQ(library_label("anything", 4), std::string(""));
}

TEST(durations_skip_unknown_entries_rather_than_subtracting_them) {
    OnlineResult a, b, c;
    a.duration_sec = 120.0;
    b.duration_sec = -1.0;
    c.duration_sec = 60.0;
    CHECK_EQ(total_duration_sec({a, b, c}), 180.0);
}

TEST(long_form_durations_read_as_hours_and_minutes) {
    CHECK_EQ(fmt_duration_long(0.0), std::string("--"));
    CHECK_EQ(fmt_duration_long(-1.0), std::string("--"));
    CHECK_EQ(fmt_duration_long(59.0), std::string("0m"));
    CHECK_EQ(fmt_duration_long(90.0), std::string("1m"));
    CHECK_EQ(fmt_duration_long(420.0), std::string("7m"));
    CHECK_EQ(fmt_duration_long(312.0 * 60.0), std::string("5h 12m"));
}
