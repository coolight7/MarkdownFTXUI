#include "markdown/dom_builder.hpp"
#include "markdown/text_utils.hpp"

#include <string_view>

#include <ftxui/dom/flexbox_config.hpp>

namespace markdown {
namespace {

constexpr int kMaxDepth = 40;

using Links = std::vector<LinkTarget>;

// Iteratively collect all text from a subtree (no recursion — safe at any
// depth).  Used as the plain-text fallback when nesting exceeds kMaxDepth.
std::string collect_text(ASTNode const& root) {
    std::string result;
    std::vector<ASTNode const*> stack{&root};
    while (!stack.empty()) {
        auto* n = stack.back();
        stack.pop_back();
        if (!n->text.empty()) result += n->text;
        if (n->type == NodeType::SoftBreak) result += ' ';
        if (n->type == NodeType::HardBreak) result += '\n';
        // Push children in reverse so leftmost is processed first.
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) {
            stack.push_back(&*it);
        }
    }
    return normalize_emoji_width(result);
}

// Returns true if the next link to be inserted matches focused_link.
bool is_next_link_focused(Links const& links, int focused_link) {
    return static_cast<int>(links.size()) == focused_link;
}

// Compute the decorator for a link based on whether it is focused.
ftxui::Decorator link_style(bool is_focused, ftxui::Decorator base,
                            Theme const& theme) {
    if (is_focused) return base | ftxui::underlined | ftxui::inverted;
    return base | ftxui::underlined | theme.link;
}

// Register a link: create a LinkTarget, wrap each element in elems[from..]
// with reflect for click detection, and apply focus to the first element.
void register_link(Links& links, ftxui::Elements& elems, size_t from,
                   std::string const& url, bool is_focused) {
    links.emplace_back(LinkTarget{.url = url});
    auto& target = links.back();
    size_t count = elems.size() - from;
    target.boxes.resize(count);
    for (size_t i = from; i < elems.size(); ++i) {
        elems[i] = elems[i] | ftxui::reflect(target.boxes[i - from]);
    }
    if (is_focused && count > 0) {
        elems[from] = elems[from] | ftxui::focus;
    }
}

ftxui::Element build_node(ASTNode const& node, int depth, int qd, int mqd,
                          Links& links, int focused_link,
                          Theme const& theme);

ftxui::Elements build_children(ASTNode const& node, int depth, int qd,
                               int mqd, Links& links, int focused_link,
                               Theme const& theme) {
    ftxui::Elements result;
    for (auto const& child : node.children) {
        result.push_back(build_node(child, depth, qd, mqd, links,
                                    focused_link, theme));
    }
    return result;
}

// Collect inline children into a single hbox (for paragraphs, etc.)
ftxui::Element build_inline_container(ASTNode const& node, int depth, int qd,
                                      int mqd, Links& links, int focused_link,
                                      Theme const& theme) {
    ftxui::Elements parts;
    for (auto const& child : node.children) {
        parts.push_back(build_node(child, depth, qd, mqd, links,
                                   focused_link, theme));
    }
    if (parts.empty()) {
        return ftxui::text("");
    }
    if (parts.size() == 1) {
        return std::move(parts[0]);
    }
    return ftxui::hbox(std::move(parts));
}

// Recursively collect words from inline AST nodes, preserving decorators.
// Each word becomes a separate flexbox item so wrapping works at word
// boundaries even inside bold/italic/link runs.
void collect_inline_words(ASTNode const& node, int depth, int qd, int mqd,
                          ftxui::Elements& words,
                          ftxui::Decorator style,
                          Links& links, int focused_link,
                          Theme const& theme) {
    if (depth > kMaxDepth) {
        auto text = collect_text(node);
        if (!text.empty()) words.push_back(ftxui::text(text) | style);
        return;
    }
    for (auto const& child : node.children) {
        switch (child.type) {
        case NodeType::Text: {
            auto t = normalize_emoji_width(child.text);
            size_t pos = 0;
            while (pos < t.size()) {
                size_t space_start = pos;
                while (pos < t.size() && t[pos] == ' ') ++pos;
                if (pos >= t.size()) {
                    // Trailing spaces: emit separator for next sibling.
                    if (space_start < pos && !words.empty()) {
                        words.push_back(ftxui::text(" ") | style);
                    }
                    break;
                }
                auto end = t.find(' ', pos);
                if (end == std::string::npos) end = t.size();
                std::string word;
                bool needs_space = (space_start < pos);
                word.reserve((end - pos) + (needs_space ? 1 : 0));
                if (needs_space) word += ' ';
                word.append(t.data() + pos, end - pos);
                words.push_back(ftxui::text(std::move(word)) | style);
                pos = end;
            }
            break;
        }
        case NodeType::SoftBreak:
            words.push_back(ftxui::text(" ") | style);
            break;
        case NodeType::HardBreak:
            break; // handled by build_wrapping_container
        case NodeType::Strong:
            collect_inline_words(child, depth + 1, qd, mqd, words,
                                 style | ftxui::bold, links, focused_link,
                                 theme);
            break;
        case NodeType::Emphasis:
            collect_inline_words(child, depth + 1, qd, mqd, words,
                                 style | ftxui::italic, links, focused_link,
                                 theme);
            break;
        case NodeType::Link: {
            bool is_focused = is_next_link_focused(links, focused_link);
            auto ls = link_style(is_focused, style, theme);
            size_t before = words.size();
            collect_inline_words(child, depth + 1, qd, mqd, words,
                                 ls, links, focused_link, theme);
            register_link(links, words, before, child.url, is_focused);
            break;
        }
        case NodeType::CodeInline:
            words.push_back(ftxui::text(normalize_emoji_width(child.text))
                            | theme.code_inline | style);
            break;
        default:
            words.push_back(build_node(child, depth, qd, mqd, links,
                                       focused_link, theme) | style);
            break;
        }
    }
}

// Check if a paragraph node contains only plain text (Text + SoftBreak).
bool is_plain_text_paragraph(ASTNode const& node) {
    for (auto const& child : node.children) {
        if (child.type != NodeType::Text && child.type != NodeType::SoftBreak) {
            return false;
        }
    }
    return true;
}

// Check if a node contains any HardBreak children.
bool has_hard_break(ASTNode const& node) {
    for (auto const& child : node.children) {
        if (child.type == NodeType::HardBreak) return true;
    }
    return false;
}

// Build a flexbox row from a flat list of word elements.
ftxui::Element words_to_element(ftxui::Elements& words) {
    static const auto wrap_config = ftxui::FlexboxConfig().SetGap(0, 0);
    if (words.empty()) return ftxui::text("");
    // Always use flexbox — even for a single element.  Without this,
    // a lone underlined link stretches to the full vbox width and its
    // underline extends across the entire line (looks like a separator).
    return ftxui::flexbox(std::move(words), wrap_config);
}

// Wrapping version of build_inline_container for block-level paragraphs.
// Splits all inline content into word-level flexbox items for line wrapping.
// HardBreak nodes force a new line by splitting into separate flexbox rows.
ftxui::Element build_wrapping_container(ASTNode const& node, int depth, int qd,
                                        int mqd, Links& links,
                                        int focused_link,
                                        Theme const& theme) {
    // Fast path: plain text paragraphs use ftxui::paragraph() directly,
    // avoiding per-word flexbox overhead.
    if (is_plain_text_paragraph(node)) {
        std::string combined;
        for (auto const& child : node.children) {
            if (child.type == NodeType::Text) {
                if (!combined.empty() && combined.back() != ' ') {
                    combined += ' ';
                }
                combined += child.text;
            } else if (child.type == NodeType::SoftBreak) {
                if (!combined.empty() && combined.back() != ' ') {
                    combined += ' ';
                }
            }
        }
        return ftxui::paragraph(normalize_emoji_width(combined));
    }

    // If no hard breaks, single flexbox row (common case).
    if (!has_hard_break(node)) {
        ftxui::Elements words;
        collect_inline_words(node, depth, qd, mqd, words, ftxui::nothing,
                             links, focused_link, theme);
        return words_to_element(words);
    }

    // Split at HardBreak boundaries: each segment becomes its own row.
    // Build a temporary ASTNode per segment and collect words from it.
    ftxui::Elements rows;
    ASTNode segment{.type = node.type};
    auto flush_segment = [&] {
        if (segment.children.empty()) {
            rows.push_back(ftxui::text(""));
            return;
        }
        ftxui::Elements words;
        collect_inline_words(segment, depth, qd, mqd, words, ftxui::nothing,
                             links, focused_link, theme);
        rows.push_back(words_to_element(words));
        segment.children.clear();
    };

    for (auto const& child : node.children) {
        if (child.type == NodeType::HardBreak) {
            flush_segment();
        } else {
            segment.children.push_back(child);
        }
    }
    flush_segment();

    if (rows.size() == 1) return std::move(rows[0]);
    return ftxui::vbox(std::move(rows));
}

// Build a ListItem: first Paragraph gets bullet/number prefix,
// subsequent children (nested lists) rendered below with indentation.
ftxui::Element build_list_item(ASTNode const& node, int depth, int qd,
                               int mqd, std::string const& prefix,
                               Links& links, int focused_link,
                               Theme const& theme) {
    std::string indent(depth * 2, ' ');

    ftxui::Elements rows;
    bool first_para = true;
    for (auto const& child : node.children) {
        if (first_para && (child.type == NodeType::Paragraph ||
                           child.type == NodeType::Text)) {
            // First paragraph: render with wrapping, bullet/number prefix
            auto content = build_wrapping_container(child, depth, qd, mqd,
                                                    links, focused_link, theme);
            rows.push_back(ftxui::hbox({
                ftxui::text(indent + prefix),
                content | ftxui::flex,
            }));
            first_para = false;
        } else {
            // Nested lists or additional paragraphs
            rows.push_back(build_node(child, depth, qd, mqd, links,
                                      focused_link, theme));
        }
    }
    if (rows.empty()) {
        return ftxui::text(indent + prefix);
    }
    if (rows.size() == 1) {
        return std::move(rows[0]);
    }
    return ftxui::vbox(std::move(rows));
}

ftxui::Element build_document(ASTNode const& node, int depth, int qd, int mqd,
                              Links& links, int focused_link,
                              Theme const& theme) {
    auto children = build_children(node, depth, qd, mqd, links, focused_link,
                                   theme);
    if (children.empty()) return ftxui::text("");
    ftxui::Elements spaced;
    for (size_t i = 0; i < children.size(); ++i) {
        if (i > 0) spaced.push_back(ftxui::text(""));
        spaced.push_back(std::move(children[i]));
    }
    return ftxui::vbox(std::move(spaced));
}

ftxui::Element build_heading(ASTNode const& node, int depth, int qd, int mqd,
                             Links& links, int focused_link,
                             Theme const& theme) {
    auto content = build_wrapping_container(node, depth, qd, mqd, links,
                                            focused_link, theme);
    if (node.level == 1) return content | theme.heading1;
    if (node.level == 2) return content | theme.heading2;
    return content | theme.heading3;
}

ftxui::Element build_link(ASTNode const& node, int depth, int qd, int mqd,
                          Links& links, int focused_link,
                          Theme const& theme) {
    bool is_focused = is_next_link_focused(links, focused_link);
    auto el = build_inline_container(node, depth, qd, mqd, links,
                                     focused_link, theme)
        | link_style(is_focused, ftxui::nothing, theme);
    ftxui::Elements elems;
    elems.push_back(std::move(el));
    register_link(links, elems, 0, node.url, is_focused);
    return std::move(elems[0]);
}

ftxui::Element build_bullet_list(ASTNode const& node, int depth, int qd,
                                 int mqd, Links& links, int focused_link,
                                 Theme const& theme) {
    ftxui::Elements items;
    for (auto const& child : node.children) {
        items.push_back(build_list_item(child, depth + 1, qd, mqd, "\u2022 ",
                                        links, focused_link, theme));
    }
    return ftxui::vbox(std::move(items));
}

ftxui::Element build_ordered_list(ASTNode const& node, int depth, int qd,
                                  int mqd, Links& links, int focused_link,
                                  Theme const& theme) {
    ftxui::Elements items;
    int num = node.list_start;
    for (auto const& child : node.children) {
        items.push_back(build_list_item(child, depth + 1, qd, mqd,
                                        std::to_string(num++) + ". ",
                                        links, focused_link, theme));
    }
    return ftxui::vbox(std::move(items));
}

ftxui::Element build_blockquote(ASTNode const& node, int depth, int qd,
                                int mqd, Links& links, int focused_link,
                                Theme const& theme) {
    auto content = ftxui::vbox(build_children(node, depth, qd + 1, mqd, links,
                                              focused_link, theme));
    // Cap visual indentation at max_quote_depth; content still renders.
    if (qd >= mqd) {
        return content | theme.blockquote;
    }
    return ftxui::hbox({
        ftxui::text("\u2502 "),
        content | theme.blockquote,
    });
}

ftxui::Element build_code_block(ASTNode const& node, Theme const& theme) {
    auto sanitized_code = normalize_emoji_width(node.text);
    std::string_view code = sanitized_code;
    if (!code.empty() && code.back() == '\n') code.remove_suffix(1);
    ftxui::Elements lines;
    size_t start = 0;
    while (start <= code.size()) {
        auto end = code.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(ftxui::text(std::string(code.substr(start))));
            break;
        }
        lines.push_back(
            ftxui::text(std::string(code.substr(start, end - start))));
        start = end + 1;
    }
    if (lines.empty()) lines.push_back(ftxui::text(""));
    auto content = ftxui::vbox(std::move(lines)) | theme.code_block;
    if (!node.info.empty()) {
        return ftxui::window(ftxui::text(" " + node.info + " ") | ftxui::dim,
                             content);
    }
    return content | ftxui::border;
}

ftxui::Element build_image(ASTNode const& node, int depth, int qd, int mqd,
                           Links& links, int focused_link,
                           Theme const& theme) {
    auto alt = build_inline_container(node, depth, qd, mqd, links,
                                      focused_link, theme);
    return ftxui::hbox({
        ftxui::text("[IMG: ") | ftxui::dim,
        alt,
        ftxui::text("]") | ftxui::dim,
    });
}

// 收集单元格纯文本用于宽度测量: 换行折叠为空格, 宽度按终端显示列计算。
std::string cell_plain_text(ASTNode const& cell) {
    std::string plain = collect_text(cell);
    for (char& c : plain) {
        if (c == '\n') c = ' ';
    }
    return plain;
}

// 渲染 GFM 表格: 用 box-drawing 字符绘制边框, 按列最大内容宽度对齐各列,
// 支持左/中/右对齐 (来自分隔行的 ':' 位置) 与加粗表头。
ftxui::Element build_table(ASTNode const& node, int depth, int qd, int mqd,
                           Links& links, int focused_link,
                           Theme const& theme) {
    // 1. 逐行构建单元格元素, 同时统计每列最大内容宽度。
    std::vector<std::vector<ftxui::Element>> rows;
    std::vector<bool> row_is_header;
    std::vector<int> col_widths;
    for (auto const& row_node : node.children) {
        if (row_node.type != NodeType::TableRow) continue;
        std::vector<ftxui::Element> row;
        size_t ci = 0;
        for (auto const& cell : row_node.children) {
            if (cell.type != NodeType::TableCell) continue;
            auto el = build_inline_container(cell, depth, qd, mqd, links,
                                             focused_link, theme);
            int w = utf8_display_width(cell_plain_text(cell));
            if (ci >= col_widths.size()) col_widths.resize(ci + 1, 0);
            col_widths[ci] = std::max(col_widths[ci], w);
            row.push_back(std::move(el));
            ++ci;
        }
        rows.push_back(std::move(row));
        row_is_header.push_back(row_node.is_header);
    }
    if (rows.empty()) {
        // 空表或畸形 AST: 退化为空文本, 不渲染边框。
        return ftxui::text("");
    }

    // 2. 列宽兜底: 空列/未统计列最小宽度 1。
    size_t ncols = std::max<size_t>(node.columns, 1);
    for (auto& w : col_widths) w = std::max(w, 1);
    while (col_widths.size() < ncols) col_widths.push_back(1);
    if (col_widths.empty()) col_widths.push_back(1);

    // 3. 边框行: ┌─┬─┐ / ├─┼─┤ / └─┴─┘  (每列内容宽度 + 左右 1 空格内边距)
    // 注: box-drawing 字符是多字节 UTF-8, 因此用 string_view 而非 char。
    auto repeat = [](std::string_view sv, size_t n) {
        std::string out;
        out.reserve(sv.size() * n);
        for (size_t i = 0; i < n; ++i) out += sv;
        return out;
    };
    auto border_line = [&](std::string_view left, std::string_view mid,
                           std::string_view right, std::string_view h) {
        std::string s(left);
        for (size_t c = 0; c < col_widths.size(); ++c) {
            s += repeat(h, col_widths[c] + 2);
            s += (c + 1 < col_widths.size()) ? mid : right;
        }
        return ftxui::text(std::move(s)) | theme.table_border;
    };

    // 4. 数据行: 单元格按列宽固定, 并按对齐方式放置内容。
    auto render_row = [&](std::vector<ftxui::Element> const& row,
                          bool is_header) {
        ftxui::Elements parts;
        parts.push_back(ftxui::text("\u2502") | theme.table_border);
        for (size_t c = 0; c < col_widths.size(); ++c) {
            ftxui::Element content
                = (c < row.size()) ? row[c] : ftxui::text("");
            char align = (c < node.alignments.size()) ? node.alignments[c] : 'l';
            ftxui::Element aligned;
            switch (align) {
                case 'c':
                    aligned = ftxui::hbox(
                        {ftxui::filler(), std::move(content), ftxui::filler()});
                    break;
                case 'r':
                    aligned = ftxui::hbox({ftxui::filler(), std::move(content)});
                    break;
                default: // 'l'
                    aligned = ftxui::hbox({std::move(content), ftxui::filler()});
                    break;
            }
            if (is_header) aligned = aligned | theme.table_header;
            parts.push_back(ftxui::text(" "));
            parts.push_back(
                std::move(aligned)
                | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, col_widths[c]));
            parts.push_back(ftxui::text(" "));
            parts.push_back(ftxui::text("\u2502") | theme.table_border);
        }
        return ftxui::hbox(std::move(parts));
    };

    // 5. 组装: 上边框 + 各行 (行间分隔线) + 下边框。
    ftxui::Elements lines;
    lines.push_back(border_line("\u250C", "\u252C", "\u2510", "\u2500"));
    for (size_t r = 0; r < rows.size(); ++r) {
        lines.push_back(render_row(rows[r], row_is_header[r]));
        if (r + 1 < rows.size()) {
            lines.push_back(
                border_line("\u251C", "\u253C", "\u2524", "\u2500"));
        }
    }
    lines.push_back(border_line("\u2514", "\u2534", "\u2518", "\u2500"));
    return ftxui::vbox(std::move(lines));
}

ftxui::Element build_node(ASTNode const& node, int depth, int qd, int mqd,
                          Links& links, int focused_link,
                          Theme const& theme) {
    // Depth guard: fall back to plain text to prevent stack overflow.
    if (depth + qd > kMaxDepth) {
        return ftxui::paragraph(collect_text(node));
    }

    switch (node.type) {
    case NodeType::Document:
        return build_document(node, depth, qd, mqd, links, focused_link,
                              theme);
    case NodeType::Heading:
        return build_heading(node, depth, qd, mqd, links, focused_link,
                             theme);
    case NodeType::Paragraph:
        return build_wrapping_container(node, depth, qd, mqd, links,
                                        focused_link, theme);
    case NodeType::Strong:
        return build_inline_container(node, depth, qd, mqd, links,
                                      focused_link, theme) | ftxui::bold;
    case NodeType::Emphasis:
        return build_inline_container(node, depth, qd, mqd, links,
                                      focused_link, theme) | ftxui::italic;
    case NodeType::Link:
        return build_link(node, depth, qd, mqd, links, focused_link, theme);
    case NodeType::BulletList:
        return build_bullet_list(node, depth, qd, mqd, links, focused_link,
                                 theme);
    case NodeType::OrderedList:
        return build_ordered_list(node, depth, qd, mqd, links, focused_link,
                                  theme);
    case NodeType::ListItem:
        return build_list_item(node, depth, qd, mqd, "\u2022 ", links,
                               focused_link, theme);
    case NodeType::BlockQuote:
        return build_blockquote(node, depth, qd, mqd, links, focused_link,
                                theme);
    case NodeType::CodeInline:
        return ftxui::text(normalize_emoji_width(node.text)) | theme.code_inline;
    case NodeType::CodeBlock:
        return build_code_block(node, theme);
    case NodeType::ThematicBreak:
        return ftxui::separator();
    case NodeType::Image:
        return build_image(node, depth, qd, mqd, links, focused_link, theme);
    case NodeType::Table:
        return build_table(node, depth, qd, mqd, links, focused_link, theme);
    case NodeType::TableRow:
    case NodeType::TableCell:
        // 行/单元格仅在 Table 内部渲染, 单独出现时退化为空文本。
        return ftxui::text("");
    case NodeType::Text:
        return ftxui::text(normalize_emoji_width(node.text));
    case NodeType::SoftBreak:
        return ftxui::text(" ");
    case NodeType::HardBreak:
        return ftxui::text("");
    default:
        return ftxui::text(normalize_emoji_width(node.text));
    }
}

} // namespace

ftxui::Element DomBuilder::build(MarkdownAST const& ast, int focused_link,
                                 Theme const& theme) {
    _link_targets.clear();
    auto result = build_node(ast, 0, 0, _max_quote_depth, _link_targets,
                             focused_link, theme);

    // Build flat index for click detection.  Stores pointers into
    // LinkTarget::boxes — reflect() fills them during layout, so the
    // pointers stay valid and always have fresh coordinates.
    _flat_boxes.clear();
    for (int i = 0; i < static_cast<int>(_link_targets.size()); ++i) {
        for (auto const& box : _link_targets[i].boxes) {
            _flat_boxes.push_back({&box, i});
        }
    }

    return result;
}

} // namespace markdown
