// An answer's markdown on a canvas: blocks parsed off the lines, inline spans off each block's text, rows wrapped
// by display width, then painted. Every block renders to styled rows (Out) so a quote bar or a list marker is one
// prefix on rows that were already laid out, and a code badge rides those prefixes to its final cell.
#include "dd/ui_markdown.hpp"

#include "dd/tools/ansi_strip.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace dd {
namespace ui {
namespace {

constexpr size_t npos = std::string::npos;

struct SC {
    char32_t ch;
    Style st;
};
using Line = std::vector<SC>;

struct Out {   // laid-out rows and the code badges on them
    std::vector<Line> lines;
    std::vector<CopySpot> spots;
};

// The markdown palette, off the theme: the purple and gold the rest of the deck wears.
struct Pal {
    Style text, code, link, url, rule, bar, bullet, number, table, head_cell, fence, lang, badge, done, todo;
    Style head[6];
};

Pal palette(const Theme& th) {
    Pal p;
    p.text = Style{th.text};
    p.code = Style{th.code_ty, th.select_bg};
    p.link = Style{th.accent, kNone, kUnder};
    p.url = Style{th.muted};
    p.rule = Style{th.dim};
    p.bar = Style{th.accent};
    p.bullet = Style{th.hot, kNone, kBold};
    p.number = Style{th.accent, kNone, kBold};
    p.table = Style{th.dim};
    p.head_cell = Style{th.gold, kNone, kBold};
    p.fence = Style{th.dim};
    p.lang = Style{th.muted, kNone, kItalic};
    p.badge = Style{th.accent, kNone, kBold};
    p.done = Style{th.ok, kNone, kBold};
    p.todo = Style{th.muted};
    p.head[0] = Style{th.gold, kNone, kBold};
    p.head[1] = Style{th.title, kNone, kBold};
    p.head[2] = Style{th.hot, kNone, kBold};
    p.head[3] = Style{th.accent, kNone, kBold};
    p.head[4] = Style{th.muted, kNone, kBold | kItalic};
    p.head[5] = Style{th.muted, kNone, kItalic};
    return p;
}

int width_of(const Line& l) {
    int w = 0;
    for (const SC& c : l) w += cp_width(c.ch);
    return w;
}

Line text_line(const std::string& utf8, Style st) {
    Line l;
    for (char32_t c : from_utf8(utf8)) l.push_back({c, st});
    return l;
}

Line rep(char32_t c, int n, Style st) { return Line(static_cast<size_t>(std::max(0, n)), SC{c, st}); }

// ---------------- inline: spans off one block's text ----------------

bool is_space(char32_t c) { return c == U' ' || c == U'\t' || c == U'\n'; }
bool is_word(char32_t c) {
    return (c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_' || c >= 0x80;
}
bool is_punct(char32_t c) { return c < 0x80 && std::ispunct(static_cast<int>(c)); }

bool starts_at(const std::u32string& s, size_t i, std::u32string_view w) { return s.compare(i, w.size(), w) == 0; }

size_t run_of(const std::u32string& s, size_t i, size_t b, char32_t c) {
    size_t j = i;
    while (j < b && s[j] == c) ++j;
    return j - i;
}

size_t close_ticks(const std::u32string& s, size_t from, size_t b, size_t n) {
    for (size_t j = from; j < b;) {
        if (s[j] != U'`') {
            ++j;
            continue;
        }
        const size_t r = run_of(s, j, b, U'`');
        if (r == n) return j;
        j += r;
    }
    return npos;
}

// Past a code span or an escape that starts at j; j itself when neither does. A closer inside either is literal.
size_t skip_atom(const std::u32string& s, size_t j, size_t b) {
    if (s[j] == U'\\' && j + 1 < b) return j + 2;
    if (s[j] == U'`') {
        const size_t r = run_of(s, j, b, U'`');
        const size_t e = close_ticks(s, j + r, b, r);
        return e == npos ? j + r : e + r;
    }
    return j;
}

// The closer of a k-run of `c` opened before `from`: a run at least k long with no space before it (and, for _,
// no letter after it). The run's LAST k close, so "**bold *italic***" leaves the inner "*" to the italic.
size_t close_emph(const std::u32string& s, size_t from, size_t b, char32_t c, size_t k) {
    for (size_t j = from; j < b;) {
        if (const size_t n = skip_atom(s, j, b); n != j) {
            j = n;
            continue;
        }
        if (s[j] != c) {
            ++j;
            continue;
        }
        const size_t r = run_of(s, j, b, c);
        const bool right = j > from && !is_space(s[j - 1]);
        const bool after = c != U'_' || j + r >= b || !is_word(s[j + r]);
        if (r >= k && right && after) return j + (r - k);
        j += r;
    }
    return npos;
}

size_t close_bracket(const std::u32string& s, size_t i, size_t b) {   // s[i] == '['
    int depth = 0;
    for (size_t j = i + 1; j < b;) {
        if (const size_t n = skip_atom(s, j, b); n != j) {
            j = n;
            continue;
        }
        if (s[j] == U'[') ++depth;
        else if (s[j] == U']' && depth-- == 0) return j;
        ++j;
    }
    return npos;
}

size_t close_paren(const std::u32string& s, size_t i, size_t b) {   // s[i] == '('
    int depth = 0;
    for (size_t j = i + 1; j < b; ++j) {
        if (s[j] == U'\\') ++j;
        else if (s[j] == U'\n') return npos;
        else if (s[j] == U'(') ++depth;
        else if (s[j] == U')' && depth-- == 0) return j;
    }
    return npos;
}

// "&amp;" and its kin at i: the character and how much it eats, or {0, 0}.
std::pair<char32_t, size_t> entity(const std::u32string& s, size_t i, size_t b) {
    const size_t e = s.find(U';', i);
    if (e == npos || e >= b || e - i > 10 || e == i + 1) return {0, 0};
    const std::u32string name = s.substr(i + 1, e - i - 1);
    static const std::pair<std::u32string_view, char32_t> kNamed[] = {
        {U"amp", U'&'}, {U"lt", U'<'}, {U"gt", U'>'}, {U"quot", U'"'}, {U"apos", U'\''}, {U"nbsp", U' '},
        {U"mdash", U'—'}, {U"ndash", U'–'}, {U"hellip", U'…'}, {U"rarr", U'→'}, {U"larr", U'←'}, {U"copy", U'©'}};
    for (const auto& [n, c] : kNamed)
        if (name == n) return {c, e - i + 1};
    if (name[0] != U'#' || name.size() < 2) return {0, 0};
    const bool hex = name[1] == U'x' || name[1] == U'X';
    unsigned long v = 0;
    for (size_t k = hex ? 2 : 1; k < name.size(); ++k) {
        const char32_t d = name[k];
        const int x = d >= U'0' && d <= U'9'               ? static_cast<int>(d - U'0')
                      : hex && d >= U'a' && d <= U'f'      ? static_cast<int>(d - U'a' + 10)
                      : hex && d >= U'A' && d <= U'F'      ? static_cast<int>(d - U'A' + 10)
                                                           : -1;
        if (x < 0 || (v = v * (hex ? 16 : 10) + static_cast<unsigned long>(x)) > 0x10FFFF) return {0, 0};
    }
    if (v == 0 || name.size() == (hex ? 2u : 1u)) return {0, 0};
    return {static_cast<char32_t>(v), e - i + 1};
}

// An inline HTML tag at i (a name the model actually writes): how much it eats, 0 when it is prose ("a < b").
size_t html_tag(const std::u32string& s, size_t i, size_t b, bool& br) {
    size_t j = i + 1;
    if (j < b && s[j] == U'/') ++j;
    const size_t n0 = j;
    while (j < b && ((s[j] >= U'a' && s[j] <= U'z') || (s[j] >= U'A' && s[j] <= U'Z') || (j > n0 && s[j] >= U'0' && s[j] <= U'9'))) ++j;
    if (j == n0) return 0;
    std::u32string name = s.substr(n0, j - n0);
    for (char32_t& c : name)
        if (c >= U'A' && c <= U'Z') c += 32;
    static const std::u32string_view kTags[] = {U"br", U"b", U"i", U"em", U"strong", U"code", U"kbd", U"sub", U"sup", U"u",
                                                U"s", U"del", U"span", U"mark", U"small", U"details", U"summary", U"p",
                                                U"div", U"a", U"img", U"hr"};
    if (std::find(std::begin(kTags), std::end(kTags), name) == std::end(kTags)) return 0;
    const size_t e = s.find(U'>', j);
    if (e == npos || e >= b) return 0;
    for (size_t k = j; k < e; ++k)
        if (s[k] == U'\n' || s[k] == U'<') return 0;
    br = name == U"br" || name == U"p" || name == U"div" || name == U"hr";
    return e - i + 1;
}

void put_text(Line& out, const std::u32string& s, size_t a, size_t b, Style st) {
    for (size_t k = a; k < b; ++k) out.push_back({s[k], st});
}

// s[a, b) as styled characters onto `out`. `heal`: this is the tail of a text still streaming, so an opener with
// no closer yet styles the rest instead of showing its raw markers.
void inl(const std::u32string& s, size_t a, size_t b, Style st, const Pal& p, Line& out, bool heal) {
    size_t i = a;
    while (i < b) {
        const char32_t c = s[i];
        if (c == U'\\' && i + 1 < b && is_punct(s[i + 1])) {
            out.push_back({s[i + 1], st});
            i += 2;
            continue;
        }
        if (c == U'`') {
            const size_t r = run_of(s, i, b, U'`');
            size_t e = close_ticks(s, i + r, b, r);
            if (e == npos && !heal) {
                put_text(out, s, i, i + r, st);
                i += r;
                continue;
            }
            const bool open = e == npos;
            if (open) e = b;
            size_t x = i + r, y = e;
            if (y - x >= 2 && s[x] == U' ' && s[y - 1] == U' ' && s.find_first_not_of(U' ', x) < y) {
                ++x;   // one space each side is padding, so `` ` `` can show a backtick
                --y;
            }
            Style cs = p.code;
            cs.at |= st.at & (kBold | kItalic | kStrike);
            for (size_t k = x; k < y; ++k) out.push_back({s[k] == U'\n' ? U' ' : s[k], cs});
            i = open ? b : e + r;
            continue;
        }
        if (c == U'*' || c == U'_' || c == U'~') {
            const size_t r = run_of(s, i, b, c);
            const size_t k = c == U'~' ? 2 : std::min<size_t>(r, 3);
            const bool opens = (c == U'~' ? r == 2 : r <= 3) && i + r < b && !is_space(s[i + r]) &&
                               (c != U'_' || i == a || !is_word(s[i - 1]));
            if (opens) {
                Style in = st;
                in.at |= c == U'~' ? kStrike : k == 1 ? kItalic : k == 2 ? kBold : (kBold | kItalic);
                if (const size_t e = close_emph(s, i + k, b, c, k); e != npos) {
                    inl(s, i + k, e, in, p, out, false);
                    i = e + k;
                    continue;
                }
                if (heal) {
                    inl(s, i + k, b, in, p, out, true);
                    i = b;
                    continue;
                }
            }
            put_text(out, s, i, i + r, st);
            i += r;
            continue;
        }
        if ((c == U'[' || (c == U'!' && i + 1 < b && s[i + 1] == U'['))) {
            const bool image = c == U'!';
            const size_t lb = image ? i + 1 : i;
            const size_t rb = close_bracket(s, lb, b);
            if (rb != npos && rb + 1 < b && s[rb + 1] == U'(') {
                if (const size_t rp = close_paren(s, rb + 1, b); rp != npos) {
                    std::u32string url = s.substr(rb + 2, rp - rb - 2);
                    url.erase(0, url.find_first_not_of(U' '));
                    if (const size_t sp = url.find(U' '); sp != npos) url.resize(sp);   // drop a "title"
                    if (url.size() >= 2 && url.front() == U'<' && url.back() == U'>') url = url.substr(1, url.size() - 2);
                    if (image) {
                        Style im = p.url;
                        im.at |= kItalic;
                        for (char32_t x : std::u32string_view(U"[image: ")) out.push_back({x, im});
                        inl(s, lb + 1, rb, im, p, out, false);
                        out.push_back({U']', im});
                    } else {
                        Style ls = p.link;
                        ls.at |= st.at & (kBold | kItalic);
                        const size_t before = out.size();
                        inl(s, lb + 1, rb, ls, p, out, false);
                        std::u32string shown;
                        for (size_t k = before; k < out.size(); ++k) shown += out[k].ch;
                        if (!url.empty() && url != shown && url != U"mailto:" + shown) {   // no hover in a terminal: say where
                            out.push_back({U' ', st});
                            out.push_back({U'(', p.url});
                            for (char32_t x : url) out.push_back({x, p.url});
                            out.push_back({U')', p.url});
                        }
                    }
                    i = rp + 1;
                    continue;
                }
            }
            if (!image && rb != npos && rb + 1 < b && s[rb + 1] == U'[') {   // [text][ref]: the text, as a link
                if (const size_t re = s.find(U']', rb + 2); re != npos && re < b) {
                    inl(s, lb + 1, rb, p.link, p, out, false);
                    i = re + 1;
                    continue;
                }
            }
        }
        if (c == U'<') {
            if (const size_t e = s.find(U'>', i); e != npos && e < b) {
                const std::u32string in = s.substr(i + 1, e - i - 1);
                const bool url = !in.empty() && in.find_first_of(U" \n<") == npos &&
                                 (starts_at(in, 0, U"http://") || starts_at(in, 0, U"https://") || starts_at(in, 0, U"mailto:") ||
                                  (in.find(U'@') != npos && in.find(U'@') > 0));
                if (url) {
                    put_text(out, in, 0, in.size(), p.link);
                    i = e + 1;
                    continue;
                }
            }
            bool br = false;
            if (const size_t n = html_tag(s, i, b, br)) {
                if (br) out.push_back({U'\n', st});
                i += n;
                continue;
            }
        }
        if (c == U'&') {
            if (const auto [ch, n] = entity(s, i, b); n) {
                out.push_back({ch, st});
                i += n;
                continue;
            }
        }
        if (c == U'h' && (i == a || !is_word(s[i - 1])) && (starts_at(s, i, U"https://") || starts_at(s, i, U"http://"))) {
            size_t j = i;
            while (j < b && !is_space(s[j]) && s[j] != U'<' && s[j] != U'>' && s[j] != U'`' && s[j] != U'"') ++j;
            while (j > i && std::u32string_view(U".,;:!?'*_").find(s[j - 1]) != std::u32string_view::npos) --j;
            if (j > i && s[j - 1] == U')') {   // a ')' that closes the prose around the url is not part of it
                int bal = 0;
                for (size_t k = i; k < j; ++k) bal += s[k] == U'(' ? 1 : s[k] == U')' ? -1 : 0;
                if (bal < 0) --j;
            }
            Style ls = p.link;
            ls.at |= st.at & kBold;
            put_text(out, s, i, j, ls);
            i = j;
            continue;
        }
        out.push_back({c, st});
        ++i;
    }
}

// Word wrap by display width; '\n' is a line break. A word wider than the row is cut where the row ends.
std::vector<Line> wrap_line(const Line& in, int width) {
    width = std::max(1, width);
    std::vector<Line> rows;
    Line cur;
    int cw = 0;
    const auto flush = [&] {
        while (!cur.empty() && cur.back().ch == U' ') cur.pop_back();
        rows.push_back(std::move(cur));
        cur.clear();
        cw = 0;
    };
    for (size_t i = 0; i < in.size();) {
        if (in[i].ch == U'\n') {
            flush();
            ++i;
            continue;
        }
        if (in[i].ch == U' ') {
            if (cw > 0 && cw + 1 <= width) {
                cur.push_back(in[i]);
                ++cw;
            }
            ++i;
            continue;
        }
        size_t j = i;
        int ww = 0;
        while (j < in.size() && in[j].ch != U' ' && in[j].ch != U'\n') ww += cp_width(in[j++].ch);
        if (cw > 0 && cw + ww > width) flush();
        for (size_t k = i; k < j; ++k) {
            const int w1 = cp_width(in[k].ch);
            if (w1 == 0) continue;
            if (cw > 0 && cw + w1 > width) flush();
            cur.push_back(in[k]);
            cw += w1;
        }
        i = j;
    }
    if (!cur.empty() || rows.empty()) flush();
    return rows;
}

// ---------------- blocks: the lines parsed ----------------

struct Item;
struct Block {
    enum class K { Para, Heading, Code, Quote, List, Table, Rule } k = K::Para;
    int level = 0;
    std::string text, lang, alert;
    std::vector<std::string> lines;   // a fence's body
    std::vector<Block> kids;          // a quote's
    bool ordered = false, loose = false;
    long start = 1;
    std::vector<Item> items;
    std::vector<std::string> head;
    std::vector<int> align;   // 0 left, 1 right, 2 centre
    std::vector<std::vector<std::string>> rows;
};
struct Item {
    std::vector<Block> blocks;
    int task = -1;        // -1 no box, 0 open, 1 done
    bool spaced = false;  // a blank line between the item's own blocks
};

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}
int indent_of(const std::string& l) {
    int n = 0;
    while (n < static_cast<int>(l.size()) && l[static_cast<size_t>(n)] == ' ') ++n;
    return n;
}
bool blank(const std::string& l) { return l.find_first_not_of(" \t") == npos; }

bool fence_open(const std::string& l, char& ch, size_t& len, std::string& info) {
    const size_t i = static_cast<size_t>(indent_of(l));
    if (i > 3 || i >= l.size() || (l[i] != '`' && l[i] != '~')) return false;
    ch = l[i];
    size_t j = i;
    while (j < l.size() && l[j] == ch) ++j;
    len = j - i;
    info = trim(l.substr(j));
    return len >= 3 && (ch != '`' || info.find('`') == npos);
}
bool fence_close(const std::string& l, char ch, size_t len) {
    const size_t i = static_cast<size_t>(indent_of(l));
    if (i > 3) return false;
    size_t j = i;
    while (j < l.size() && l[j] == ch) ++j;
    return j - i >= len && blank(l.substr(j));
}
int heading_of(const std::string& l, std::string& text) {
    const size_t i = static_cast<size_t>(indent_of(l));
    if (i > 3) return 0;
    size_t j = i;
    while (j < l.size() && l[j] == '#') ++j;
    const int lv = static_cast<int>(j - i);
    if (lv < 1 || lv > 6 || (j < l.size() && l[j] != ' ' && l[j] != '\t')) return 0;
    std::string t = trim(l.substr(j));
    size_t e = t.size();
    while (e > 0 && t[e - 1] == '#') --e;
    if (e < t.size() && (e == 0 || t[e - 1] == ' ')) t = trim(t.substr(0, e));   // closing #s
    text = t;
    return lv;
}
bool rule_of(const std::string& l) {
    if (indent_of(l) > 3) return false;
    char32_t c = 0;
    int n = 0;
    for (char32_t x : from_utf8(l)) {
        if (x == U' ' || x == U'\t') continue;
        if (!c) c = x;
        if (x != c) return false;
        ++n;
    }
    return n >= 3 && (c == U'-' || c == U'*' || c == U'_' || c == U'─' || c == U'━' || c == U'═');
}
bool setext_of(const std::string& l, char c) {
    if (indent_of(l) > 3) return false;
    const std::string t = trim(l);
    return !t.empty() && t.find_first_not_of(c) == npos;
}
bool quote_of(const std::string& l, std::string& rest) {
    const size_t i = static_cast<size_t>(indent_of(l));
    if (i > 3 || i >= l.size() || l[i] != '>') return false;
    rest = l.substr(i + 1 < l.size() && l[i + 1] == ' ' ? i + 2 : i + 1);
    return true;
}

struct Marker {
    int indent = 0, content = 0;
    bool ordered = false;
    long number = 1;
};
bool list_of(const std::string& l, Marker& m) {
    const size_t ind = static_cast<size_t>(indent_of(l));
    size_t i = ind;
    if (i >= l.size()) return false;
    if (l[i] == '-' || l[i] == '*' || l[i] == '+') {
        m.ordered = false;
        ++i;
    } else {
        size_t j = i;
        while (j < l.size() && j - i < 9 && std::isdigit(static_cast<unsigned char>(l[j]))) ++j;
        if (j == i || j >= l.size() || (l[j] != '.' && l[j] != ')')) return false;
        m.ordered = true;
        m.number = std::stol(l.substr(i, j - i));
        i = j + 1;
    }
    if (i < l.size() && l[i] != ' ' && l[i] != '\t') return false;   // "-x" and "2.5" are prose
    size_t k = i;
    while (k < l.size() && l[k] == ' ') ++k;
    m.indent = static_cast<int>(ind);
    m.content = static_cast<int>(k == l.size() || k - i > 4 ? i + 1 : k);   // an empty item, or code-indented content
    return true;
}

// Cells of a table row, split on '|' outside code spans; "\|" is a literal pipe.
std::vector<std::string> cells_of(const std::string& l) {
    std::string t = trim(l);
    if (!t.empty() && t.front() == '|') t.erase(0, 1);
    if (!t.empty() && t.back() == '|' && !(t.size() >= 2 && t[t.size() - 2] == '\\')) t.pop_back();
    std::vector<std::string> out;
    std::string cur;
    size_t ticks = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cur += '|';
            ++i;
        } else if (c == '`') {
            size_t r = 1;
            while (i + r < t.size() && t[i + r] == '`') ++r;
            if (!ticks) ticks = r;
            else if (r == ticks) ticks = 0;
            cur.append(r, '`');
            i += r - 1;
        } else if (c == '|' && !ticks) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trim(cur));
    return out;
}

// The delimiter row under a table's head; a model that draws it in box glyphs (├───┼───┤) means the same row.
bool delim_of(const std::string& l, std::vector<int>& align) {
    std::string a;
    for (char32_t c : from_utf8(l)) {
        if (c == U'─' || c == U'━' || c == U'═') a += '-';
        else if (c == U'├' || c == U'┼' || c == U'┤' || c == U'│' || c == U'╞' || c == U'╪' || c == U'╡') a += '|';
        else if (c < 0x80) a += static_cast<char>(c);
        else return false;
    }
    if (a.find('-') == npos || a.find('|') == npos) return false;
    align.clear();
    for (const std::string& c : cells_of(a)) {
        if (c.empty()) return false;
        const bool L = c.front() == ':', R = c.size() > 1 && c.back() == ':';
        const std::string mid = c.substr(L ? 1 : 0, c.size() - (L ? 1 : 0) - (R ? 1 : 0));
        if (mid.empty() || mid.find_first_not_of('-') != npos) return false;
        align.push_back(L && R ? 2 : R ? 1 : 0);
    }
    return !align.empty();
}

bool table_at(const std::vector<std::string>& L, size_t i, std::vector<int>& align) {
    return L[i].find('|') != npos && i + 1 < L.size() && delim_of(L[i + 1], align) && cells_of(L[i]).size() == align.size();
}

bool starts_block(const std::vector<std::string>& L, size_t i) {
    const std::string& l = L[i];
    char ch;
    size_t len;
    std::string t;
    Marker m;
    std::vector<int> al;
    return fence_open(l, ch, len, t) || heading_of(l, t) || rule_of(l) || quote_of(l, t) || list_of(l, m) || table_at(L, i, al);
}

std::vector<Block> parse(const std::vector<std::string>& L);

Block parse_list(const std::vector<std::string>& L, size_t& i, const Marker& first) {
    Block b;
    b.k = Block::K::List;
    b.ordered = first.ordered;
    b.start = first.number;
    const int base = first.indent;
    while (i < L.size()) {
        Marker m;
        if (!list_of(L[i], m) || m.ordered != b.ordered || m.indent < base || rule_of(L[i])) break;
        std::vector<std::string> content{L[i].substr(std::min(static_cast<size_t>(m.content), L[i].size()))};
        ++i;
        bool saw_blank = false, inner_blank = false;
        while (i < L.size()) {
            const std::string& x = L[i];
            if (blank(x)) {
                saw_blank = true;
                content.emplace_back();
                ++i;
                continue;
            }
            const int ind = indent_of(x);
            Marker mm;
            if (list_of(x, mm) && mm.indent <= base) break;   // a sibling, or an item of an outer list
            if (ind > base) {                                // indented past the marker: this item's
                if (saw_blank) inner_blank = true;
                content.push_back(x.substr(static_cast<size_t>(std::min(ind, m.content))));
                ++i;
                continue;
            }
            if (saw_blank || starts_block(L, i)) break;   // lazy continuation of the item's paragraph, or the end
            content.push_back(trim(x));
            ++i;
        }
        while (!content.empty() && blank(content.back())) content.pop_back();
        Item it;
        it.spaced = inner_blank;
        it.blocks = parse(content);
        if (!it.blocks.empty() && it.blocks.front().k == Block::K::Para) {
            std::string& t = it.blocks.front().text;
            if (t.size() >= 3 && t[0] == '[' && t[2] == ']' && (t.size() == 3 || t[3] == ' ') &&
                (t[1] == ' ' || t[1] == 'x' || t[1] == 'X')) {
                it.task = t[1] == ' ' ? 0 : 1;
                t = trim(t.substr(3));
            }
        }
        b.items.push_back(std::move(it));
        Marker next;
        if (saw_blank && i < L.size() && list_of(L[i], next) && next.ordered == b.ordered && next.indent >= base) b.loose = true;
    }
    return b;
}

std::vector<Block> parse(const std::vector<std::string>& L) {
    std::vector<Block> out;
    size_t i = 0;
    while (i < L.size()) {
        const std::string& l = L[i];
        if (blank(l)) {
            ++i;
            continue;
        }
        char fch;
        size_t flen;
        std::string t;
        if (fence_open(l, fch, flen, t)) {   // an open fence at the end (a stream mid-block) runs to the end
            Block b;
            b.k = Block::K::Code;
            b.lang = t.substr(0, t.find_first_of(" \t{"));
            const int ind = indent_of(l);
            for (++i; i < L.size() && !fence_close(L[i], fch, flen); ++i)
                b.lines.push_back(L[i].substr(static_cast<size_t>(std::min(ind, indent_of(L[i])))));
            if (i < L.size()) ++i;
            out.push_back(std::move(b));
            continue;
        }
        if (const int lv = heading_of(l, t)) {
            Block b;
            b.k = Block::K::Heading;
            b.level = lv;
            b.text = t;
            out.push_back(std::move(b));
            ++i;
            continue;
        }
        if (rule_of(l)) {
            Block b;
            b.k = Block::K::Rule;
            out.push_back(std::move(b));
            ++i;
            continue;
        }
        if (quote_of(l, t)) {
            std::vector<std::string> inner;
            while (i < L.size() && quote_of(L[i], t)) {
                inner.push_back(t);
                ++i;
            }
            Block b;
            b.k = Block::K::Quote;
            if (const std::string a = trim(inner.front()); a.size() > 3 && a.compare(0, 2, "[!") == 0 && a.back() == ']') {
                std::string kind = a.substr(2, a.size() - 3);
                for (char& c : kind) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (kind == "NOTE" || kind == "TIP" || kind == "IMPORTANT" || kind == "WARNING" || kind == "CAUTION") {
                    b.alert = kind;
                    inner.erase(inner.begin());
                }
            }
            b.kids = parse(inner);
            out.push_back(std::move(b));
            continue;
        }
        Marker m;
        if (list_of(l, m)) {
            out.push_back(parse_list(L, i, m));
            continue;
        }
        std::vector<int> al;
        if (table_at(L, i, al)) {
            Block b;
            b.k = Block::K::Table;
            b.head = cells_of(l);
            b.align = al;
            for (i += 2; i < L.size() && !blank(L[i]) && L[i].find('|') != npos; ++i) {
                std::vector<std::string> r = cells_of(L[i]);
                r.resize(b.head.size());
                b.rows.push_back(std::move(r));
            }
            out.push_back(std::move(b));
            continue;
        }
        Block b;   // a paragraph: every line its own row, as a model writes it
        while (i < L.size() && !blank(L[i])) {
            if (!b.text.empty()) {
                if (setext_of(L[i], '=') || setext_of(L[i], '-')) {
                    b.k = Block::K::Heading;
                    b.level = setext_of(L[i], '=') ? 1 : 2;
                    ++i;
                    break;
                }
                if (starts_block(L, i)) break;
            }
            std::string x = trim(L[i]);
            if (!x.empty() && x.back() == '\\') x.pop_back();   // a hard break: every newline already is one
            b.text += (b.text.empty() ? "" : "\n") + x;
            ++i;
        }
        out.push_back(std::move(b));
    }
    return out;
}

// ---------------- render: blocks to rows ----------------

struct Ctx {
    const Theme& th;
    const GrammarSet* g;
    Pal p;
};

Line inline_line(const std::string& text, Style st, const Ctx& cx, bool heal) {
    const std::u32string u = from_utf8(text);
    Line out;
    inl(u, 0, u.size(), st, cx.p, out, heal);
    return out;
}

// `src` onto `dst`, its first row led by `first` and every later one by `rest` (as wide): a bullet, a quote bar.
void append(Out& dst, Out&& src, const Line& first, const Line& rest) {
    const int pw = width_of(first);
    const int y0 = static_cast<int>(dst.lines.size());
    for (size_t k = 0; k < src.lines.size(); ++k) {
        Line l = k == 0 ? first : rest;
        l.insert(l.end(), src.lines[k].begin(), src.lines[k].end());
        dst.lines.push_back(std::move(l));
    }
    for (CopySpot& sp : src.spots) dst.spots.push_back({sp.x + pw, sp.y + y0, std::move(sp.text)});
}

Out blocks(const std::vector<Block>& bs, int width, Style base, int depth, bool tight, bool tail, const Ctx& cx);

void code_block(const Block& b, int width, const Ctx& cx, Out& o) {
    std::string body;
    for (size_t k = 0; k < b.lines.size(); ++k) body += (k ? "\n" : "") + b.lines[k];
    const bool boxed = width >= 8;
    const int inner = boxed ? width - 4 : width, x0 = boxed ? 2 : 0;
    struct Seg {
        std::string text;
        bool cont;
    };
    std::vector<Seg> segs;   // rows cut by display width; a continued row is marked, the copy keeps the real line
    for (const std::string& raw : b.lines) {
        const std::u32string u = from_utf8(raw);
        if (u.empty()) segs.push_back({"", false});
        for (size_t i = 0; i < u.size();) {
            const bool cont = i > 0;
            const int room = std::max(1, cont ? inner - 2 : inner);
            int w = 0;
            size_t j = i;
            while (j < u.size() && w + cp_width(u[j]) <= room) w += cp_width(u[j++]);
            if (j == i) ++j;
            segs.push_back({to_utf8(u.substr(i, j - i)), cont});
            i = j;
        }
    }
    if (segs.empty()) segs.push_back({"", false});
    const int h = static_cast<int>(segs.size()) + (boxed ? 2 : 0);
    Canvas c(width, h);
    if (boxed) {
        c.box(0, 0, width, h, cx.p.fence, true);
        if (!b.lang.empty() && width > 14) c.title(2, 0, clip(b.lang, width - 10), cx.p.lang, cx.p.fence);
        c.put(width - 1, 0, kCopyGlyph, cx.p.badge);
    }
    CodeCtx ctx;
    ctx.fence = true;
    ctx.set = cx.g;
    ctx.lang = cx.g && !b.lang.empty() ? cx.g->find(b.lang) : nullptr;
    for (size_t k = 0; k < segs.size(); ++k) {
        const int y = static_cast<int>(k) + (boxed ? 1 : 0);
        if (segs[k].cont && inner > 2) {
            c.put(x0, y, U'↪', Style{cx.th.dim});
            paint_row(c, x0 + 2, y, segs[k].text, cx.th, ctx, cx.p.text, inner - 2);
        } else {
            paint_row(c, x0, y, segs[k].text, cx.th, ctx, cx.p.text, inner);
        }
    }
    const int y0 = static_cast<int>(o.lines.size());
    for (int y = 0; y < h; ++y) {
        Line l;
        for (int x = 0; x < width; ++x)
            if (const Cell& cell = c.at(x, y); cell.ch != 0) l.push_back({cell.ch, cell.st});
        o.lines.push_back(std::move(l));
    }
    if (boxed) o.spots.push_back({width - 1, y0, std::move(body)});
}

void table_block(const Block& b, int width, Style base, const Ctx& cx, Out& o) {
    const size_t n = b.head.size();
    std::vector<Line> head(n);
    for (size_t c = 0; c < n; ++c) head[c] = inline_line(b.head[c], cx.p.head_cell, cx, false);
    std::vector<std::vector<Line>> rows;
    for (const auto& r : b.rows) {
        std::vector<Line> lr(n);
        for (size_t c = 0; c < n; ++c) lr[c] = inline_line(c < r.size() ? r[c] : "", base, cx, false);
        rows.push_back(std::move(lr));
    }
    // Each column wants its widest line and needs its longest word (capped): the room is shared by that need.
    const auto measure = [](const Line& l, bool words) {
        int best = 0, cur = 0;
        for (const SC& x : l) {
            if (x.ch == U'\n' || (words && x.ch == U' ')) {
                best = std::max(best, cur);
                cur = 0;
            } else {
                cur += cp_width(x.ch);
            }
        }
        return std::max(best, cur);
    };
    std::vector<int> want(n, 1), need(n, 1);
    const auto feed = [&](const std::vector<Line>& r) {
        for (size_t c = 0; c < n; ++c) {
            want[c] = std::max(want[c], measure(r[c], false));
            need[c] = std::max(need[c], std::min(measure(r[c], true), 24));
        }
    };
    feed(head);
    for (const auto& r : rows) feed(r);
    const int avail = width - (3 * static_cast<int>(n) + 1);
    int floor = 0;   // columns narrower than a short word read as letter soup
    for (size_t c = 0; c < n; ++c) floor += std::max(3, std::min(need[c], 8));
    if (avail < floor) {   // no room for columns: one record per row, "head: cell"
        for (size_t k = 0; k < rows.size(); ++k) {
            if (k) o.lines.emplace_back();
            for (size_t c = 0; c < n; ++c) {
                Line l = head[c];
                l.push_back({U':', cx.p.head_cell});
                l.push_back({U' ', base});
                l.insert(l.end(), rows[k][c].begin(), rows[k][c].end());
                for (Line& w : wrap_line(l, width)) o.lines.push_back(std::move(w));
            }
        }
        return;
    }
    const auto sum = [](const std::vector<int>& v) {
        int s = 0;
        for (int x : v) s += x;
        return s;
    };
    std::vector<int> cw = want;
    if (sum(want) > avail) {
        cw = need;
        if (sum(cw) > avail) {   // even the words do not fit: shrink them in proportion, three columns at least
            const int total = sum(cw);
            for (size_t c = 0; c < n; ++c) cw[c] = std::max(3, cw[c] * avail / std::max(1, total));
            for (size_t c = 0; sum(cw) > avail; c = (c + 1) % n)
                if (cw[c] > 3) --cw[c];
        }
        int spare = avail - sum(cw), short_by = 0;
        for (size_t c = 0; c < n; ++c) short_by += std::max(0, want[c] - cw[c]);
        if (spare > 0 && short_by > 0) {
            int given = 0;
            for (size_t c = 0; c < n; ++c) {
                const int add = static_cast<int>(static_cast<long>(std::max(0, want[c] - cw[c])) * spare / short_by);
                cw[c] += add;
                given += add;
            }
            for (size_t c = 0; given < spare && c < n; ++c)
                if (cw[c] < want[c]) {
                    ++cw[c];
                    ++given;
                }
        }
    }
    const Style bs = cx.p.table;
    const auto rule = [&](char32_t l, char32_t m, char32_t r) {
        Line x{{l, bs}};
        for (size_t c = 0; c < n; ++c) {
            for (int k = 0; k < cw[c] + 2; ++k) x.push_back({U'─', bs});
            x.push_back({c + 1 < n ? m : r, bs});
        }
        return x;
    };
    std::vector<std::vector<std::vector<Line>>> laid;   // per row, per cell, its wrapped lines
    bool tall = false;
    const auto lay = [&](const std::vector<Line>& r) {
        std::vector<std::vector<Line>> cells(n);
        for (size_t c = 0; c < n; ++c) cells[c] = wrap_line(r[c], cw[c]);
        return cells;
    };
    for (const auto& r : rows) {
        laid.push_back(lay(r));
        for (const auto& cell : laid.back()) tall = tall || cell.size() > 1;
    }
    const auto emit = [&](const std::vector<std::vector<Line>>& cells) {
        size_t h = 1;
        for (const auto& cell : cells) h = std::max(h, cell.size());
        for (size_t y = 0; y < h; ++y) {
            Line x{{U'│', bs}};
            for (size_t c = 0; c < n; ++c) {
                const Line cell = y < cells[c].size() ? cells[c][y] : Line{};
                const int pad = std::max(0, cw[c] - width_of(cell));
                const int left = b.align[c] == 2 ? pad / 2 : b.align[c] == 1 ? pad : 0;
                x.push_back({U' ', base});
                x.insert(x.end(), static_cast<size_t>(left), SC{U' ', base});
                x.insert(x.end(), cell.begin(), cell.end());
                x.insert(x.end(), static_cast<size_t>(pad - left), SC{U' ', base});
                x.push_back({U' ', base});
                x.push_back({U'│', bs});
            }
            o.lines.push_back(std::move(x));
        }
    };
    o.lines.push_back(rule(U'╭', U'┬', U'╮'));
    emit(lay(head));
    o.lines.push_back(rule(U'├', U'┼', U'┤'));
    for (size_t k = 0; k < laid.size(); ++k) {
        if (k > 0 && tall) o.lines.push_back(rule(U'├', U'┼', U'┤'));   // rows of several lines need a line between them
        emit(laid[k]);
    }
    o.lines.push_back(rule(U'╰', U'┴', U'╯'));
}

Out block(const Block& b, int width, Style base, int depth, bool tail, const Ctx& cx) {
    Out o;
    width = std::max(1, width);
    switch (b.k) {
    case Block::K::Para:
        o.lines = wrap_line(inline_line(b.text, base, cx, tail), width);
        break;
    case Block::K::Heading: {
        const int lv = std::clamp(b.level, 1, 6);
        o.lines = wrap_line(inline_line(b.text, cx.p.head[lv - 1], cx, tail), width);
        int tw = 0;
        for (const Line& l : o.lines) tw = std::max(tw, width_of(l));
        if (lv == 1) o.lines.push_back(rep(U'━', width, Style{mix(cx.th.gold, cx.th.dim, 0.55)}));
        else if (lv == 2) o.lines.push_back(rep(U'─', std::max(1, tw), cx.p.rule));
        break;
    }
    case Block::K::Rule:
        o.lines.push_back(rep(U'─', width, cx.p.rule));
        break;
    case Block::K::Quote: {
        Style text{cx.th.muted, base.bg, (base.at | kItalic)};
        Style bar = cx.p.bar;
        Out inner;
        if (!b.alert.empty()) {   // GitHub's alerts: a coloured bar and a label instead of a grey quote
            struct A {
                const char* kind;
                const char* label;
                uint32_t col;
            };
            const A kinds[] = {{"NOTE", "ℹ Note", cx.th.code_ty},       {"TIP", "✦ Tip", cx.th.ok},
                               {"IMPORTANT", "❖ Important", cx.th.accent}, {"WARNING", "▲ Warning", cx.th.warn},
                               {"CAUTION", "✖ Caution", cx.th.error}};
            for (const A& a : kinds)
                if (b.alert == a.kind) {
                    bar = Style{a.col, kNone, kBold};
                    inner.lines.push_back(text_line(a.label, bar));
                }
            text = base;
        }
        append(inner, blocks(b.kids, width - 2, text, depth, false, tail, cx), {}, {});
        const Line pre{{U'▎', bar}, {U' ', base}};
        if (inner.lines.empty()) inner.lines.emplace_back();
        append(o, std::move(inner), pre, pre);
        break;
    }
    case Block::K::List: {
        static constexpr char32_t kBullets[] = {U'•', U'◦', U'▪', U'▫'};
        int numw = 1;
        if (b.ordered)
            for (size_t k = 0; k < b.items.size(); ++k)
                numw = std::max(numw, static_cast<int>(std::to_string(b.start + static_cast<long>(k)).size()) + 1);
        for (size_t k = 0; k < b.items.size(); ++k) {
            const Item& it = b.items[k];
            Line mark;
            if (it.task >= 0) {
                mark.push_back({it.task ? U'☑' : U'☐', it.task ? cx.p.done : cx.p.todo});
            } else if (b.ordered) {
                const std::string num = std::to_string(b.start + static_cast<long>(k)) + ".";
                mark = rep(U' ', numw - static_cast<int>(num.size()), base);
                const Line t = text_line(num, cx.p.number);
                mark.insert(mark.end(), t.begin(), t.end());
            } else {
                mark.push_back({kBullets[depth % 4], cx.p.bullet});
            }
            mark.push_back({U' ', base});
            const int mw = width_of(mark);
            Out body = blocks(it.blocks, width - mw, base, depth + 1, !(b.loose || it.spaced), tail && k + 1 == b.items.size(), cx);
            if (body.lines.empty()) body.lines.emplace_back();
            if (b.loose && k > 0) o.lines.emplace_back();
            append(o, std::move(body), mark, rep(U' ', mw, base));
        }
        break;
    }
    case Block::K::Table:
        table_block(b, width, base, cx, o);
        break;
    case Block::K::Code:
        code_block(b, width, cx, o);
        break;
    }
    return o;
}

Out blocks(const std::vector<Block>& bs, int width, Style base, int depth, bool tight, bool tail, const Ctx& cx) {
    Out o;
    for (size_t k = 0; k < bs.size(); ++k) {
        if (k > 0 && !tight && bs[k - 1].k != Block::K::Heading) o.lines.emplace_back();   // a heading sits on its text
        append(o, block(bs[k], width, base, depth, tail && k + 1 == bs.size(), cx), {}, {});
    }
    return o;
}

std::string expand_tabs(const std::string& l) {
    if (l.find('\t') == npos) return l;
    std::string o;
    int col = 0;
    for (size_t i = 0; i < l.size();) {
        const unsigned char c = static_cast<unsigned char>(l[i]);
        if (c == '\t') {
            const int n = 4 - col % 4;
            o.append(static_cast<size_t>(n), ' ');
            col += n;
            ++i;
            continue;
        }
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        o.append(l, i, len);
        ++col;
        i += len;
    }
    return o;
}

}  // namespace

MdOut render_markdown(const std::string& md, int width, const Theme& th, const GrammarSet* grammars, bool streaming,
                      int max_rows) {
    width = std::max(1, width);
    const std::string clean = dd::tools::sanitize_display_text(md);
    std::vector<std::string> lines;
    for (size_t i = 0; i <= clean.size();) {
        size_t e = clean.find('\n', i);
        if (e == npos) e = clean.size();
        lines.push_back(expand_tabs(clean.substr(i, e - i)));
        i = e + 1;
    }
    const Ctx cx{th, grammars, palette(th)};
    Out o = blocks(parse(lines), width, cx.p.text, 0, false, streaming, cx);
    while (!o.lines.empty() && o.lines.back().empty()) o.lines.pop_back();
    MdOut out;
    if (max_rows > 0 && static_cast<int>(o.lines.size()) > max_rows) {
        const int keep = max_rows - 1;
        out.hidden = static_cast<int>(o.lines.size()) - keep;
        o.lines.resize(static_cast<size_t>(keep));
        o.lines.push_back(Line{{U'…', Style{th.muted}}});
        o.spots.erase(std::remove_if(o.spots.begin(), o.spots.end(), [&](const CopySpot& s) { return s.y >= keep; }), o.spots.end());
    }
    out.canvas = Canvas(width, std::max(1, static_cast<int>(o.lines.size())));
    for (size_t y = 0; y < o.lines.size(); ++y) {
        int x = 0;
        for (const SC& c : o.lines[y]) {
            const int cw = cp_width(c.ch);
            if (cw == 0) continue;
            if (x + cw > width) break;
            out.canvas.put(x, static_cast<int>(y), c.ch, c.st);
            x += cw;
        }
    }
    out.spots = std::move(o.spots);
    return out;
}

}  // namespace ui
}  // namespace dd
