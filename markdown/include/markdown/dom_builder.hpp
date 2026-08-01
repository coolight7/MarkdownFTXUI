#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include "markdown/ast.hpp"
#include "markdown/theme.hpp"

namespace markdown {

struct LinkTarget {
    std::vector<ftxui::Box> boxes;
    std::string url;
};

struct FlatLinkBox {
    ftxui::Box const* box;  // points into LinkTarget::boxes (filled by reflect)
    int link_index;
};

class DomBuilder {
public:
    ftxui::Element build(MarkdownAST const& ast, int focused_link = -1,
                         Theme const& theme = theme_default());
    std::vector<LinkTarget> const& link_targets() const { return _link_targets; }
    std::vector<FlatLinkBox> const& flat_link_boxes() const { return _flat_boxes; }

    void set_max_quote_depth(int d) { _max_quote_depth = d; }
    int max_quote_depth() const { return _max_quote_depth; }

    /// 设置可用渲染宽度 (终端列数)。
    /// 用于限制表格等宽元素的总宽度: 当表格自然宽度超过此值时,
    /// 列宽将按比例缩减并启用单元格内自动换行。
    /// <= 0 表示不限制 (默认行为)。
    void set_max_width(int w) { _max_width = w; }
    int max_width() const { return _max_width; }

private:
    std::vector<LinkTarget> _link_targets;
    std::vector<FlatLinkBox> _flat_boxes;
    int _max_quote_depth = 10;
    int _max_width = 0; // <= 0: 不限制
};

} // namespace markdown
