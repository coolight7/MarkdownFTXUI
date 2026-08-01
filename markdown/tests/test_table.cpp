#include "test_helper.hpp"
#include "markdown/dom_builder.hpp"
#include "markdown/parser.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

using namespace markdown;

// 剥离 ANSI 转义序列 (SGR 颜色/样式), 便于纯文本断言。
static std::string strip_ansi(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] != 'm') ++i;
            if (i < s.size()) ++i; // skip 'm'
        } else {
            out += s[i++];
        }
    }
    return out;
}

// 渲染 markdown 并返回 Screen 文本输出 (已剥离 ANSI 转义)。
static std::string render(std::string_view md, int width = 80, int height = 30) {
    auto parser  = make_cmark_parser();
    auto ast     = parser->parse(md);
    DomBuilder builder;
    auto element = builder.build(ast);
    auto screen  = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                         ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, element);
    return strip_ansi(screen.ToString());
}

int main() {
    // ---- Parser tests ----
    {
        // 基本表格: 表头 + 两行数据, 左对齐
        MarkdownAST ast;
        auto parser = make_cmark_parser();
        bool ok = parser->parse(
            "| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n", ast);
        ASSERT_TRUE(ok);
        ASSERT_EQ(ast.type, NodeType::Document);
        ASSERT_EQ(ast.children.size(), 1u);
        auto const& table = ast.children[0];
        ASSERT_EQ(table.type, NodeType::Table);
        ASSERT_EQ(table.columns, 2);
        ASSERT_EQ(table.alignments, "ll");
        ASSERT_EQ(table.children.size(), 3u); // 1 header + 2 body rows

        // 表头行
        auto const& header = table.children[0];
        ASSERT_EQ(header.type, NodeType::TableRow);
        ASSERT_TRUE(header.is_header);
        ASSERT_EQ(header.children.size(), 2u);
        ASSERT_EQ(header.children[0].type, NodeType::TableCell);
        ASSERT_EQ(header.children[1].type, NodeType::TableCell);

        // 表体行
        auto const& body = table.children[1];
        ASSERT_EQ(body.type, NodeType::TableRow);
        ASSERT_TRUE(!body.is_header);
        ASSERT_EQ(body.children.size(), 2u);
        ASSERT_EQ(body.children[0].children[0].text, "1");
    }

    {
        // 对齐: 左/中/右
        auto ast = make_cmark_parser()->parse(
            "| a | b | c |\n|:--|:-:|--:|\n| 1 | 2 | 3 |\n");
        auto const& table = ast.children[0];
        ASSERT_EQ(table.type, NodeType::Table);
        ASSERT_EQ(table.columns, 3);
        ASSERT_EQ(table.alignments, "lcr");
    }

    {
        // 无对齐标记 (分隔线没有 ':') -> 全部左对齐
        auto ast = make_cmark_parser()->parse(
            "| a | b |\n|---|---|\n| 1 | 2 |\n");
        ASSERT_EQ(ast.children[0].alignments, "ll");
    }

    {
        // 单列表格
        auto ast = make_cmark_parser()->parse(
            "| x |\n|---|\n| 1 |\n");
        auto const& table = ast.children[0];
        ASSERT_EQ(table.type, NodeType::Table);
        ASSERT_EQ(table.columns, 1);
        ASSERT_EQ(table.children.size(), 2u);
        ASSERT_TRUE(table.children[0].is_header);
        ASSERT_TRUE(!table.children[1].is_header);
    }

    {
        // 表格单元格内支持富文本
        auto ast = make_cmark_parser()->parse(
            "| **bold** | `code` |\n|---|---|\n| *it* | [lnk](https://x) |\n");
        auto const& table = ast.children[0];
        auto const& h0    = table.children[0].children[0];
        ASSERT_EQ(h0.type, NodeType::TableCell);
        ASSERT_EQ(h0.children.size(), 1u);
        ASSERT_EQ(h0.children[0].type, NodeType::Strong);
    }

    {
        // 非表格 (缺分隔线) 仍按普通段落解析
        auto ast = make_cmark_parser()->parse("| a | b |\n| 1 | 2 |\n");
        ASSERT_EQ(ast.children[0].type, NodeType::Paragraph);
    }

    // ---- DOM builder tests ----
    {
        // 渲染表格: 应出现边框字符与所有单元格文本
        auto out = render(
            "| a | b |\n|---|---|\n| 1 | 2 |\n");
        ASSERT_CONTAINS(out, "\u250C"); // ┌
        ASSERT_CONTAINS(out, "\u2510"); // ┐
        ASSERT_CONTAINS(out, "\u2514"); // └
        ASSERT_CONTAINS(out, "\u2518"); // ┘
        ASSERT_CONTAINS(out, "\u2502"); // │
        ASSERT_CONTAINS(out, " a ");
        ASSERT_CONTAINS(out, " b ");
        ASSERT_CONTAINS(out, " 1 ");
        ASSERT_CONTAINS(out, " 2 ");
    }

    {
        // 富文本单元格渲染
        auto out = render(
            "| **bold** | *it* |\n|---|---|\n| `code` | x |\n");
        ASSERT_CONTAINS(out, "bold");
        ASSERT_CONTAINS(out, "it");
        ASSERT_CONTAINS(out, "code");
        ASSERT_CONTAINS(out, "x");
    }

    {
        // 长内容按列宽对齐, 不崩溃
        auto out = render(
            "| name | value |\n|------|-------|\n"
            "| a_very_long_column_value | 1 |\n");
        ASSERT_CONTAINS(out, "a_very_long_column_value");
        ASSERT_CONTAINS(out, "1");
    }

    {
        // 空单元格与缺失单元格不崩溃
        auto out = render(
            "| a | b |\n|---|---|\n|  |   |\n");
        ASSERT_CONTAINS(out, "\u2502");
    }

    {
        // 单元格行内换行 (cmark 宽容处理) 不崩溃
        auto out = render(
            "| a | b |\n|---|---|\n| 1 |\n| 2 | 3 |\n");
        ASSERT_CONTAINS(out, "1");
        ASSERT_CONTAINS(out, "2");
        ASSERT_CONTAINS(out, "3");
    }

    {
        // 与前后段落混排
        auto out = render(
            "before\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\nafter\n");
        ASSERT_CONTAINS(out, "before");
        ASSERT_CONTAINS(out, "after");
        ASSERT_CONTAINS(out, "\u250C");
    }

    return 0;
}
