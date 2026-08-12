#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "markdown/dom_builder.hpp"
#include "markdown/theme.hpp"

namespace markdown {

/// Incrementally parses a markdown document that grows over time (streaming).
///
/// Re-parsing the entire accumulated text on every update is O(n) per update,
/// i.e. O(n^2) overall — this dominates CPU when streaming long messages.
///
/// IncrementalRenderer exploits the fact that markdown block structure is
/// append-only while streaming: once a top-level block is closed (a later
/// block has started), it can no longer change. The renderer therefore:
///   * keeps a long-lived cmark block parser (fed incrementally, so structure
///     discovery costs only the newly appended bytes),
///   * exposes each *stable* (closed) top-level block as an independently
///     cached renderable unit (element built exactly once),
///   * exposes the trailing (still-growing) block so it can be re-parsed cheaply
///     per update.
///
/// Total work over a whole stream is O(n) for the stable content plus
/// O(size of last block) per update, instead of O(n) per update. A consumer
/// that renders each stable block as its own scrollable item also avoids
/// re-*laying out* the whole document every frame (only the visible items and
/// the small trailing block are laid out).
///
/// Typical usage:
/// @code
///   IncrementalRenderer r;
///   r.append(newTokenText);                       // as tokens arrive
///   for (size_t i = 0; i < r.stableBlockCount(); ++i) {
///       ftxui::Element el = r.stableBlockElement(i);   // cached, stable
///   }
///   std::unique_ptr<DomBuilder> frontierBuilder;
///   ftxui::Element frontier = r.renderFrontier(theme, maxWidth, frontierBuilder);
///   // frontierBuilder (and `r`) must outlive `frontier` (link boxes point
///   // into builders); stable block builders are owned internally.
/// @endcode
///
/// Known limitation: reference-style links whose definition appears later
/// ("[text][ref]" + "[ref]: url") resolve at document level; a block cached
/// before its definition arrives may display the literal text until the
/// message is completely re-rendered (the TUI re-renders the finished message
/// through the regular full-parse path, so the final result is always correct).
/// Ordinary links "[text](url)" and autolinks are self-contained and unaffected.
class IncrementalRenderer {
public:
    IncrementalRenderer();
    ~IncrementalRenderer();

    IncrementalRenderer(IncrementalRenderer const&)            = delete;
    IncrementalRenderer& operator=(IncrementalRenderer const&) = delete;

    /// Append newly received text (call with the incremental suffix each time).
    void append(std::string_view newText);

    /// The full accumulated text so far.
    std::string_view text() const;

    /// Number of stable (closed) top-level blocks currently cached.
    size_t stableBlockCount() const;

    /// Byte offset in text() where the trailing (still-growing) part begins.
    size_t frontierStart() const;

    /// Source text of stable block `i` (for height estimation). Empty if OOR.
    std::string_view stableBlockSource(size_t i) const;

    /// Cached Element of stable block `i` (built once, uncolored). Empty if OOR.
    /// The renderer owns the builder; keep the renderer alive while using it.
    /// `theme`/`maxWidth` are the render params the block is built with; a
    /// change invalidates (and lazily rebuilds) the cached elements.
    ftxui::Element stableBlockElement(size_t i, Theme const& theme, int maxWidth);

    /// Render the trailing (still-growing) block.
    ///
    /// @param theme, maxWidth  Render params; a change invalidates cached stable
    ///                         block elements (they are rebuilt lazily).
    /// @param buildersOut      Receives the DomBuilder owning this frontier's link
    ///                         boxes; the caller must keep it alive alongside the
    ///                         returned Element.
    /// @return The frontier Element (empty if there is no trailing content).
    ftxui::Element renderFrontier(
        Theme const&               theme,
        int                        maxWidth,
        std::unique_ptr<DomBuilder>& buildersOut
    );

    /// Drop cached stable-block elements (e.g. after theme/width change). The
    /// accumulated text and block structure are kept; elements are rebuilt on
    /// next renderFrontier()/stableBlockElement().
    void invalidateCache();

    /// Clear everything and start a new document.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace markdown