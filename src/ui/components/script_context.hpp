#pragma once

#include <functional>
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

    // One field of a class, as the completion list shows it: the field's name
    // and how its type reads.
    struct ClassField
    {
        QString name;
        QString type;
    };

    // The fields of one named class, from whatever owns the class definitions;
    // an unknown class gets nothing back rather than an error.
    using ClassFieldSource = std::function<std::vector<ClassField>(const QString& class_name)>;

    [[nodiscard]] bool is_code_at(const QString& text, int position);

    [[nodiscard]] std::optional<Identifier> identifier_at(const QString& text, int position);

    [[nodiscard]] std::optional<Call> enclosing_call(const QString& text, int position);

    [[nodiscard]] std::vector<DocumentName> document_names(const QString& text);

    [[nodiscard]] int matching_bracket(const QString& text, int position);

    // The class an instance variable holds at `position` (the caret), as the
    // text shows it: `local p = structs.get("Player"):at(...)` binds `p` to
    // `Player`, and so does a class handle held in its own variable (`local
    // Player = structs.get("Player")`, `p = Player:at(...)`) or one instance
    // variable assigned from another. A `structs.get` with no `:at(...)` on it
    // is a handle, not an instance. The last assignment the text writes at or
    // before `position` decides, so a variable reassigned to something else
    // names nothing. Nothing when the text shows no such binding.
    [[nodiscard]] std::optional<QString> instance_class(const QString& text, int position, const QString& variable);
}
