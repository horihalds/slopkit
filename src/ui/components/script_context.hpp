#pragma once

#include <optional>
#include <vector>

#include <QString>

namespace slopkit::ui::components::script_context
{
    struct Identifier
    {
        QString receiver;
        QString name;
        int     begin {0};
        int     end {0};
    };

    struct Call
    {
        QString receiver;
        QString name;
        int     open_paren {0};
        int     active_parameter {0};
    };

    struct DocumentName
    {
        QString name;
        QString signature;
        bool    function {false};
    };

    [[nodiscard]] bool is_code_at(const QString& text, int position);

    [[nodiscard]] std::optional<Identifier> identifier_at(const QString& text, int position);

    [[nodiscard]] std::optional<Call> enclosing_call(const QString& text, int position);

    [[nodiscard]] std::vector<DocumentName> document_names(const QString& text);

    [[nodiscard]] int matching_bracket(const QString& text, int position);
}
