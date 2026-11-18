#pragma once
// dd/ui_markdown.hpp — an answer's markdown, laid out on a canvas.
//
// Headings, emphasis, strikethrough, code spans, links, block quotes (and GitHub's [!NOTE] alerts), nested and
// task lists, GFM tables, rules and fenced code. A fence is boxed, coloured by the grammar files exactly as a
// tool result is, and carries its own copy badge: the CopySpot says where it sits and what it copies (the code,
// without the fence). One pass, no library: CommonMark where a model's output needs it, GFM's tables, task lists
// and strikethrough, and a single newline is a line break, as a model means it.
//
// `streaming`: the text is still arriving. An open fence or an unclosed ** at the tail renders as what it will
// be, not as raw markers that flip when the closer lands. `max_rows` > 0 keeps the first rows and ends on an
// ellipsis row.

#include <string>
#include <vector>

#include "dd/ui.hpp"

namespace dd {
namespace ui {

struct MdOut {
    Canvas canvas;                 // at least one row
    std::vector<CopySpot> spots;   // the code blocks' badges, in canvas cells
    int hidden = 0;                // rows max_rows cut
};

MdOut render_markdown(const std::string& md, int width, const Theme& th, const GrammarSet* grammars,
                      bool streaming = false, int max_rows = 0);

}  // namespace ui
}  // namespace dd
