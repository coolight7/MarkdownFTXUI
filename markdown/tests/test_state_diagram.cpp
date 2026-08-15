#include "test_helper.hpp"
#include "markdown/dom_builder.hpp"
#include "markdown/parser.hpp"
#include "markdown/state_diagram.hpp"
#include "markdown/theme.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>
#include <string_view>

using namespace markdown;

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// 将 markdown 渲染到固定画布并取回文本
std::string renderMarkdownToString(std::string_view src, int width = 80, int height = 40) {
    auto parser  = make_cmark_parser();
    auto ast     = parser->parse(src);
    auto builder = DomBuilder();
    auto el      = builder.build(ast, -1, theme_default());
    auto screen  = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height)
    );
    ftxui::Render(screen, el);
    return screen.ToString();
}

/// 将状态图渲染到固定画布并取回文本
std::string renderDiagramToString(const MermaidStateDiagram& dg, int maxWidth = 0) {
    auto el     = renderMermaidStateDiagram(dg, maxWidth);
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(160), ftxui::Dimension::Fixed(160)
    );
    ftxui::Render(screen, el);
    return screen.ToString();
}

} // namespace

int main() {
    // Test 1: 解析线性链路 (含起始/结束伪状态与边标签)
    {
        auto dg = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> A
    A --> B: go
    B --> [*]
)");
        ASSERT_EQ(dg.nodes.size(), 4u); // [*] 起 + [*] 终 + A + B
        ASSERT_EQ(dg.edges.size(), 3u);
        bool hasLabel = false;
        for (const auto& e : dg.edges) {
            if (e.label == "go") {
                hasLabel = true;
            }
        }
        ASSERT_TRUE(hasLabel);
        ASSERT_TRUE(!dg.directionLR);
    }

    // Test 2: direction LR / 容错 (未知语法不崩溃)
    {
        auto dg  = parseMermaidStateDiagram("direction LR\nA --> B\n%% comment\nnote right of A\nignored");
        ASSERT_TRUE(dg.directionLR);
        ASSERT_EQ(dg.nodes.size(), 2u);
        ASSERT_EQ(dg.edges.size(), 1u);
        auto dg2 = parseMermaidStateDiagram("state C {\n state inner\n}\nA --> B\n");
        // 复合状态体 C 计入节点, 其内容 (state inner) 忽略
        ASSERT_EQ(dg2.nodes.size(), 3u);
        ASSERT_EQ(dg2.edges.size(), 1u);
    }

    // Test 3: 状态图直接渲染 (盒线/箭头/标签)
    {
        auto dg  = parseMermaidStateDiagram("stateDiagram-v2\n[*] --> A\nA --> B: go\nB --> [*]");
        auto out = renderDiagramToString(dg);
        ASSERT_TRUE(contains(out, "[*]"));
        ASSERT_TRUE(contains(out, "A"));
        ASSERT_TRUE(contains(out, "B"));
        ASSERT_TRUE(contains(out, "go"));
        ASSERT_TRUE(contains(out, "v"));  // TB 箭头
        ASSERT_TRUE(contains(out, "┌"));  // 盒子上边框
    }

    // Test 4: 空图渲染为空元素
    {
        auto dg  = parseMermaidStateDiagram("stateDiagram-v2\n%% only comment\n");
        auto out = renderDiagramToString(dg);
        ASSERT_TRUE(!contains(out, "[*]"));
        ASSERT_TRUE(!contains(out, "v"));
    }

    // Test 5: DomBuilder 将 ```mermaid 代码块渲染为状态图 (替代代码块样式)
    {
        auto out = renderMarkdownToString(R"(
before

```mermaid
stateDiagram-v2
    [*] --> 1_reproduce_bug
    1_reproduce_bug --> 1_in_progress: start
    1_in_progress --> 1_completed: reproduced
    1_completed --> [*]
```

after
)");
        ASSERT_TRUE(contains(out, "before"));
        ASSERT_TRUE(contains(out, "after"));
        // 状态图内容: 节点/箭头/盒线
        ASSERT_TRUE(contains(out, "1_reproduce_bug"));
        ASSERT_TRUE(contains(out, "1_in_progress"));
        ASSERT_TRUE(contains(out, "1_completed"));
        ASSERT_TRUE(contains(out, "start"));
        ASSERT_TRUE(contains(out, "reproduced"));
        ASSERT_TRUE(contains(out, "v"));
        // 不应显示围栏原样 (代码块样式被状态图替代)
        ASSERT_TRUE(!contains(out, "```"));
        ASSERT_TRUE(!contains(out, "stateDiagram-v2"));
    }

    // Test 6: 大小写不敏感的围栏语言 (```MERMAID) 同样渲染为状态图
    {
        auto out = renderMarkdownToString("```MERMAID\nA --> B\n```");
        ASSERT_TRUE(contains(out, "A"));
        ASSERT_TRUE(contains(out, "B"));
        ASSERT_TRUE(!contains(out, "MERMAID"));
    }

    // Test 7: 普通代码块不受影响 (仍渲染代码 + 语言标签)
    {
        auto out = renderMarkdownToString("```python\nprint(1)\n```");
        ASSERT_TRUE(contains(out, "python"));
        ASSERT_TRUE(contains(out, "print(1)"));
    }

    // Test 8: 节点状态后缀着色回调
    {
        auto&    theme = theme_default();
        auto     cb    = diagramNodeColor(theme);
        ASSERT_TRUE(cb("x_in_progress") == theme.diagram_running);
        ASSERT_TRUE(cb("x_completed") == theme.diagram_done);
        ASSERT_TRUE(cb("x_failed") == theme.diagram_failed);
        ASSERT_TRUE(cb("x_pending") == theme.diagram_pending);
        ASSERT_TRUE(cb("plain") == ftxui::Color::Default);
    }

    // Test 9: maxWidth 截断 (过宽时标签带 "…", 不崩溃)
    {
        auto dg  = parseMermaidStateDiagram("stateDiagram-v2\n[*] --> very_long_node_name_here\nvery_long_node_name_here --> [*]");
        auto out = renderDiagramToString(dg, 20);
        ASSERT_TRUE(contains(out, "…"));
        ASSERT_TRUE(contains(out, "[*]"));
    }

    return 0;
}
