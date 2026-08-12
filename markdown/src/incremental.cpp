#include "markdown/incremental.hpp"
#include "markdown/parser.hpp"

#include <cmark-gfm.h>
#include <cmark-gfm-core-extensions.h>

#include <optional>
#include <utility>

namespace markdown {

namespace {

/// One-shot parse + build of a source fragment; returns the element and the
/// DomBuilder that owns its link boxes (caller keeps the builder alive).
std::pair<ftxui::Element, std::unique_ptr<DomBuilder>>
parseAndBuild(std::string_view src, Theme const& theme, int maxWidth) {
    auto parser = make_cmark_parser();
    MarkdownAST ast;
    parser->parse(src, ast);
    auto builder = std::make_unique<DomBuilder>();
    if (maxWidth > 0) {
        builder->set_max_width(maxWidth);
    }
    auto el = builder->build(ast, -1, theme);
    return {std::move(el), std::move(builder)};
}

} // namespace

struct IncrementalRenderer::Impl {
    /// Long-lived block-structure parser, fed incrementally. Used only to
    /// discover top-level block boundaries cheaply (not to render inlines).
    cmark_parser* structParser = nullptr;

    /// Full accumulated text.
    std::string text;

    /// lineStarts[i] = byte offset of the start of the (i+1)-th line (1-based
    /// line i+1). lineStarts[0] == 0. Lazily extended by rescanLines().
    std::vector<size_t> lineStarts{0};
    /// How far `text` has been scanned for line starts.
    size_t lineScanPos = 0;
    /// True if `text` currently ends with a lone '\r' whose successor ('\n'?)
    /// is not yet known; rescan is deferred until more text arrives.
    bool trailingCR = false;

    /// Rendered elements + builders for stable (closed) top-level blocks.
    /// `begin`/`end` are byte ranges into `text`; `built` is set lazily once
    /// the element has been built with the current render params.
    struct StableBlock {
        size_t                begin  = 0;
        size_t                end    = 0;
        bool                  built  = false;
        ftxui::Element        element;
        std::unique_ptr<DomBuilder> builder;
    };
    std::vector<StableBlock> stable;

    /// Render params the (built) stable elements were built with; a change
    /// marks all stable blocks unbuilt so they are rebuilt lazily.
    int                  cachedWidth = -1;
    std::optional<Theme> cachedTheme;

    void ensureParser() {
        if (structParser) {
            return;
        }
        cmark_gfm_core_extensions_ensure_registered();
        structParser = cmark_parser_new(CMARK_OPT_DEFAULT);
        // Match the one-shot render parser: recognize GFM tables as single blocks.
        if (cmark_syntax_extension* table = cmark_find_syntax_extension("table")) {
            cmark_parser_attach_syntax_extension(structParser, table);
        }
    }

    void freeParser() {
        if (structParser) {
            cmark_parser_free(structParser);
            structParser = nullptr;
        }
    }

    /// Extend lineStarts to cover complete lines in `text`.
    void rescanLines() {
        size_t i = lineScanPos;
        size_t n = text.size();
        if (trailingCR) {
            if (i < n && text[i] == '\r') {
                if (i + 1 < n && text[i + 1] == '\n') {
                    lineStarts.push_back(i + 2);
                    i += 2;
                } else if (i + 1 < n) {
                    lineStarts.push_back(i + 1);
                    i += 1;
                } else {
                    return; // still trailing '\r'; wait for more text
                }
                trailingCR = false;
            } else {
                trailingCR = false;
            }
        }
        while (i < n) {
            char c = text[i];
            if (c == '\n') {
                lineStarts.push_back(i + 1);
                ++i;
            } else if (c == '\r') {
                if (i + 1 < n && text[i + 1] == '\n') {
                    lineStarts.push_back(i + 2);
                    i += 2;
                } else if (i + 1 < n) {
                    lineStarts.push_back(i + 1);
                    ++i;
                } else {
                    trailingCR = true;
                    break;
                }
            } else {
                ++i;
            }
        }
        lineScanPos = i;
    }

    /// Byte offset of the start of 1-based line `line` (clamped).
    size_t lineOffset(int line) const {
        if (line <= 1) {
            return 0;
        }
        size_t idx = static_cast<size_t>(line - 1);
        if (idx < lineStarts.size()) {
            return lineStarts[idx];
        }
        return text.size();
    }

    /// Start lines of the current top-level blocks (from the struct parser).
    std::vector<int> topLevelStartLines() const {
        std::vector<int> starts;
        if (!structParser) {
            return starts;
        }
        cmark_node* root = cmark_parser_get_root(structParser);
        for (cmark_node* c = cmark_node_first_child(root); c != nullptr;
             c           = cmark_node_next(c)) {
            starts.push_back(cmark_node_get_start_line(c));
        }
        return starts;
    }

    /// Harvest newly-stable top-level block ranges (no element building yet).
    void harvestStable() {
        std::vector<int> starts = topLevelStartLines();
        int const        count   = static_cast<int>(starts.size());
        int const        numStable = count > 0 ? count - 1 : 0;
        for (int i = static_cast<int>(stable.size()); i < numStable; ++i) {
            size_t begin = lineOffset(starts[i]);
            size_t end   = lineOffset(starts[i + 1]);
            if (end < begin) {
                end = begin;
            }
            stable.push_back(StableBlock{begin, end, false, {}, {}});
        }
    }

    /// Apply render params; on change, mark all stable blocks unbuilt.
    void setRenderParams(Theme const& theme, int maxWidth) {
        if (cachedWidth != maxWidth || !cachedTheme || cachedTheme->name != theme.name) {
            for (auto& sb : stable) {
                sb.built = false;
                sb.element = nullptr;
                sb.builder.reset();
            }
            cachedWidth = maxWidth;
            cachedTheme = theme;
        }
    }

    /// Build stable blocks that are not yet built with the current params.
    void ensureStableBuilt(Theme const& theme, int maxWidth) {
        for (auto& sb : stable) {
            if (sb.built) {
                continue;
            }
            std::string_view src{text.data() + sb.begin, sb.end - sb.begin};
            auto [el, builder] = parseAndBuild(src, theme, maxWidth);
            sb.element   = std::move(el);
            sb.builder   = std::move(builder);
            sb.built     = true;
        }
    }
};

IncrementalRenderer::IncrementalRenderer() :
    impl_(std::make_unique<Impl>()) {}

IncrementalRenderer::~IncrementalRenderer() {
    impl_->freeParser();
}

void IncrementalRenderer::append(std::string_view newText) {
    if (newText.empty()) {
        return;
    }
    impl_->ensureParser();
    impl_->text.append(newText.data(), newText.size());
    cmark_parser_feed(impl_->structParser, newText.data(), newText.size());
    impl_->rescanLines();
    impl_->harvestStable();
}

std::string_view IncrementalRenderer::text() const {
    return impl_->text;
}

size_t IncrementalRenderer::stableBlockCount() const {
    return impl_->stable.size();
}

size_t IncrementalRenderer::frontierStart() const {
    if (impl_->stable.empty()) {
        return 0;
    }
    return impl_->stable.back().end;
}

std::string_view IncrementalRenderer::stableBlockSource(size_t i) const {
    if (i >= impl_->stable.size()) {
        return {};
    }
    auto const& sb = impl_->stable[i];
    return {impl_->text.data() + sb.begin, sb.end - sb.begin};
}

ftxui::Element IncrementalRenderer::stableBlockElement(size_t i, Theme const& theme, int maxWidth) {
    if (i >= impl_->stable.size()) {
        return ftxui::text("");
    }
    impl_->setRenderParams(theme, maxWidth);
    impl_->ensureStableBuilt(theme, maxWidth);
    return impl_->stable[i].element;
}

ftxui::Element IncrementalRenderer::renderFrontier(
    Theme const&               theme,
    int                        maxWidth,
    std::unique_ptr<DomBuilder>& buildersOut
) {
    auto& d = *impl_;
    d.setRenderParams(theme, maxWidth);
    d.ensureStableBuilt(theme, maxWidth);

    const size_t start = d.stable.empty() ? 0 : d.stable.back().end;
    if (start >= d.text.size()) {
        return ftxui::text("");
    }
    std::string_view tail{d.text.data() + start, d.text.size() - start};
    if (tail.empty()) {
        return ftxui::text("");
    }
    // 独立解析尾部; 仅含引用定义/空行的尾部解析结果为空, 返回空元素
    auto parser = make_cmark_parser();
    MarkdownAST ast;
    parser->parse(tail, ast);
    if (ast.children.empty()) {
        return ftxui::text("");
    }
    auto builder = std::make_unique<DomBuilder>();
    if (maxWidth > 0) {
        builder->set_max_width(maxWidth);
    }
    auto el = builder->build(ast, -1, theme);
    buildersOut = std::move(builder);
    return el;
}

void IncrementalRenderer::invalidateCache() {
    for (auto& sb : impl_->stable) {
        sb.built = false;
        sb.element = nullptr;
        sb.builder.reset();
    }
    impl_->cachedWidth = -1;
    impl_->cachedTheme.reset();
}

void IncrementalRenderer::reset() {
    impl_->freeParser();
    impl_->structParser = nullptr;
    impl_->text.clear();
    impl_->lineStarts.assign(1, 0);
    impl_->lineScanPos = 0;
    impl_->trailingCR  = false;
    impl_->stable.clear();
    impl_->cachedWidth = -1;
    impl_->cachedTheme.reset();
}

} // namespace markdown