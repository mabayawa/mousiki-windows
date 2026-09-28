#include "library_view.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "terminal_ui.h"

namespace muisc {

namespace {

// The narrowest a name/title column may be before it stops identifying the row.
constexpr int kMinName = 4;
// A secondary column has to leave the name at least this much, or it is not
// worth the space it costs. This is what makes an 80-column terminal spend its
// width on names rather than on a cramped owner column nobody can read.
constexpr int kNameBudgetForExtras = 16;
// Below this a secondary column is too narrow to hold anything but an ellipsis.
constexpr int kMinSecondary = 6;

constexpr int kIdxW = 3;
constexpr int kMarkW = 1;
constexpr int kCountW = 5;   // up to "99999" playlist tracks
constexpr int kDurW = 5;     // "mm:ss"

std::string ascii_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        // Cast through unsigned char: tolower on a negative char is UB, and a
        // UTF-8 continuation byte is exactly that on a signed-char platform.
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

LeftCols left_pane_columns(int inner, int sep_w, int kind_w) {
    LeftCols c;
    c.idx = kIdxW;
    c.kind = kind_w;
    // Everything up to and including the separator that precedes the name.
    int rest = inner - (c.idx + c.kind + sep_w);

    c.count = (rest - (sep_w + kCountW) >= kMinName) ? kCountW : 0;
    int after_count = rest - (c.count ? sep_w + c.count : 0);

    int want_owner = std::min(14, inner / 4);
    c.owner = (want_owner >= kMinSecondary &&
               after_count - (sep_w + want_owner) >= kNameBudgetForExtras)
                  ? want_owner : 0;

    c.name = after_count - (c.owner ? sep_w + c.owner : 0);
    // Only reachable on a pane too narrow for the mandatory columns, where the
    // sum then exceeds `inner` and box_line()'s truncate_str is the backstop.
    if (c.name < kMinName) c.name = kMinName;
    return c;
}

RightCols right_pane_columns(int inner, int sep_w) {
    RightCols c;
    c.idx = kIdxW;
    c.mark = kMarkW;
    int rest = inner - (c.idx + c.mark + sep_w);

    c.dur = (rest - (sep_w + kDurW) >= kMinName) ? kDurW : 0;
    int after_dur = rest - (c.dur ? sep_w + c.dur : 0);

    int want_artist = std::min(16, inner / 4);
    c.artist = (want_artist >= kMinSecondary &&
                after_dur - (sep_w + want_artist) >= kNameBudgetForExtras)
                   ? want_artist : 0;

    c.title = after_dur - (c.artist ? sep_w + c.artist : 0);
    if (c.title < kMinName) c.title = kMinName;
    return c;
}

std::vector<int> library_filter_indices(const std::vector<SpotifyLibraryItem>& items,
                                        const std::string& needle) {
    std::vector<int> out;
    out.reserve(items.size());
    if (needle.empty()) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) out.push_back(i);
        return out;
    }
    const std::string n = ascii_lower(needle);
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        // Owner is matched too, so "radiohead" finds the albums as well as a
        // playlist with the word in its name.
        if (ascii_lower(items[i].name).find(n) != std::string::npos ||
            ascii_lower(items[i].owner).find(n) != std::string::npos) {
            out.push_back(i);
        }
    }
    return out;
}

void clamp_cursor_scroll(int& cursor, int& scroll, int count, int visible) {
    if (count <= 0) { cursor = 0; scroll = 0; return; }
    cursor = std::clamp(cursor, 0, count - 1);
    if (visible <= 0) { scroll = 0; return; }
    if (cursor >= scroll + visible) scroll = cursor - visible + 1;
    if (cursor < scroll) scroll = cursor;
    // A shrinking list (a filter being typed) can leave scroll past the end,
    // which would render a pane of blank rows with the cursor nowhere visible.
    scroll = std::clamp(scroll, 0, std::max(0, count - visible));
}

std::string overflow_footer(int total, int scroll, int visible) {
    int remaining = total - (scroll + visible);
    if (remaining <= 0) return std::string();
    return "( " + std::to_string(remaining) + " more )";
}

std::string library_label(const std::string& name, int total_width) {
    // corner + dash + the two spaces around the label, plus room for a short
    // " (123)" count the caller appends.
    int room = total_width - 10;
    if (room <= 0) return std::string();
    return truncate_str(name, room);
}

double total_duration_sec(const std::vector<OnlineResult>& items) {
    double total = 0.0;
    for (const auto& r : items) {
        if (r.duration_sec > 0.0) total += r.duration_sec;
    }
    return total;
}

std::string fmt_duration_long(double sec) {
    if (sec <= 0.0) return "--";
    long long mins = static_cast<long long>(sec) / 60;
    long long h = mins / 60;
    long long m = mins % 60;
    if (h > 0) return std::to_string(h) + "h " + std::to_string(m) + "m";
    return std::to_string(m) + "m";
}

} // namespace muisc
