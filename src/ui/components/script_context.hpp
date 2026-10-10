#pragma once

#include <optional>
#include <vector>

#include <QString>

// Lexical queries over a Lua document, shared by the script editor's bracket
// highlight and by the completion list and argument hints. Pure `QString`
// functions with no widget dependency, so strings, comments and long strings are
// classified in exactly one place.
namespace slopkit::ui::components::script_context
{
    // The word the caret touches, with the dotted receiver in front of it
    // ("mem.re" -> receiver "mem", name "re", begin/end the full word "re").
    struct Identifier
    {
        QString receiver;
        QString name;
        int     begin {0};
        int     end {0};
    };

    // The innermost call whose parentheses contain the caret:
    // `mem.read(0x1000, |` -> receiver "mem", name "read", open_paren at the
    // `(` and active_parameter 1.
    struct Call
    {
        QString receiver;
        QString name;
        int     open_paren {0};
        int     active_parameter {0};
    };

    // A name the document defines, for the completion list.
    struct DocumentName
    {
        QString name;
        QString signature; // "helper(a, b)", or empty for a `local` variable
        bool    function {false};
    };

    // True when the caret sits outside every comment and string; at the end of
    // the text, the character just before the caret decides.
    [[nodiscard]] bool is_code_at(const QString& text, int position);

    // The word the caret touches, or nothing on whitespace and inside a comment
    // or string. A caret right after a dot ("mem.") yields an empty name with the
    // receiver set, so the members can be listed.
    [[nodiscard]] std::optional<Identifier> identifier_at(const QString& text, int position);

    // The innermost call whose parentheses contain the caret, or nothing.
    [[nodiscard]] std::optional<Call> enclosing_call(const QString& text, int position);

    // Every `function name(...)`, `local function name(...)` and `local name`
    // the document defines in code, in source order.
    [[nodiscard]] std::vector<DocumentName> document_names(const QString& text);

    // The position of the bracket matching the one at `position`, skipping
    // brackets inside comments and strings; -1 when there is none or the
    // character there is not a bracket in code.
    [[nodiscard]] int matching_bracket(const QString& text, int position);
} // namespace slopkit::ui::components::script_context
