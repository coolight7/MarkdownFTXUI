// Round-trip: content set via set_content() vs. content pre-parsed by a
// standalone MarkdownParser and handed in via set_ast() produces the
// same rendered output.
//
// That's the contract async callers need — parsing off-thread and then
// installing the AST must be equivalent to letting the Viewer parse
// internally.

#include "test_helper.hpp"
#include "markdown/viewer.hpp"
#include "markdown/parser.hpp"

#include <ftxui/screen/screen.hpp>
#include <ftxui/dom/elements.hpp>

using namespace markdown;

namespace {

std::string render(Viewer& viewer, int w = 80, int h = 10) {
    auto comp = viewer.component();
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, comp->Render());
    return screen.ToString();
}

} // namespace

int main() {
    const std::string_view md =
        "# Hello\n\n"
        "A paragraph with **bold**, *italic*, and a [link](https://example.com).\n\n"
        "> Quote line\n\n"
        "- item 1\n"
        "- item 2\n";

    // Baseline: synchronous path.
    std::string baseline;
    {
        Viewer v(make_cmark_parser());
        v.set_content(md);
        baseline = render(v);
    }

    // Async path: parse separately, install via set_ast().
    std::string async_output;
    {
        auto parser = make_cmark_parser();   // separate parser instance
        MarkdownAST ast;
        parser->parse(md, ast);              // would run on a worker IRL

        Viewer v(make_cmark_parser());       // viewer's internal parser stays unused
        v.set_ast(std::move(ast));
        async_output = render(v);
    }

    ASSERT_EQ(async_output, baseline);

    // set_content after set_ast: the later call wins (fresh parse).
    {
        auto parser = make_cmark_parser();
        MarkdownAST ast;
        parser->parse("## Stale", ast);

        Viewer v(make_cmark_parser());
        v.set_ast(std::move(ast));
        v.set_content(md);  // supersedes the AST
        ASSERT_EQ(render(v), baseline);
    }

    // set_ast after set_content: AST wins.
    {
        Viewer v(make_cmark_parser());
        v.set_content("## Stale");

        auto parser = make_cmark_parser();
        MarkdownAST ast;
        parser->parse(md, ast);
        v.set_ast(std::move(ast));
        ASSERT_EQ(render(v), baseline);
    }

    std::cout << "test passed successfully.\n";
    return 0;
}
