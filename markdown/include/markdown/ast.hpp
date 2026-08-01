#pragma once

#include <string>
#include <vector>

namespace markdown {

enum class NodeType {
    Document,
    Heading,
    Paragraph,
    Text,
    Emphasis,
    Strong,
    Link,
    ListItem,
    BulletList,
    OrderedList,
    CodeInline,
    CodeBlock,
    BlockQuote,
    SoftBreak,
    HardBreak,
    ThematicBreak,
    Image,
    Table,
    TableRow,
    TableCell,
};

struct ASTNode {
    NodeType type = NodeType::Document;
    std::string text;
    std::string url;
    std::string info;       // code block language (e.g. "python")
    int level = 0;
    int list_start = 1;
    int columns = 0;        // Table: number of columns
    std::string alignments; // Table: per-column alignment, one of 'l'/'c'/'r' (default 'l')
    bool is_header = false; // TableRow: true for the header row
    std::vector<ASTNode> children;
};

using MarkdownAST = ASTNode;

} // namespace markdown
