#include "ui/dialogs/add_script.hpp"

#include <cctype>
#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "script/engine.hpp"
#include "ui/components/script_editor.hpp"
#include "ui/components/widgets.hpp"
#include "ui/text.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        // The activate/deactivate skeleton a new script starts from; the address
        // list runs these two globals from the row's Active checkbox.
        constexpr std::string_view kScriptSkeleton = R"(-- Runs when this row's Active checkbox is ticked.
-- Return true once the target is ready, or false with an optional message to
-- refuse, e.g. return false, "why": a refused tick leaves the checkbox off.
function activate()
    return true
end

-- Runs when the checkbox is unticked; undo whatever activate() did.
function deactivate()
    return true
end
)";

        // The 1-based line a Lua compiler message points at, or 0 when it names
        // none. A chunk error reads `script:<line>: <message>`.
        [[nodiscard]] int error_line_from_message(std::string_view message)
        {
            for (std::size_t i = 0; i + 1 < message.size(); ++i)
            {
                if (message[i] != ':')
                {
                    continue;
                }

                const std::size_t start = i + 1;
                std::size_t       end   = start;
                while (end < message.size() && std::isdigit(static_cast<unsigned char>(message[end])))
                {
                    ++end;
                }
                if (end > start && end < message.size() && message[end] == ':')
                {
                    return std::stoi(std::string(message.substr(start, end - start)));
                }
            }
            return 0;
        }

        // A log line is one line, so a compiler message is flattened.
        [[nodiscard]] std::string single_line(std::string text)
        {
            for (char& ch : text)
            {
                if (ch == '\n' || ch == '\r')
                {
                    ch = ' ';
                }
            }
            return text;
        }
    } // namespace

    AddScriptDialog::AddScriptDialog(table::AddressTable& table, QWidget* parent) : QDialog(parent), table_(table)
    {
        setWindowTitle(tr("Add Script"));

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        description_edit_ = new QLineEdit(this);
        description_edit_->setObjectName(QStringLiteral("description_edit"));
        form->addRow(tr("Description"), description_edit_);
        layout->addLayout(form);

        script_edit_ = new components::ScriptEditor(this);
        script_edit_->setObjectName(QStringLiteral("script_edit"));
        layout->addWidget(script_edit_, 1);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        auto* buttons  = new QHBoxLayout();
        commit_button_ = new widgets::PrimaryButton(tr("Add"), this);
        commit_button_->setObjectName(QStringLiteral("commit_button"));
        verify_button_ = widgets::secondary_button(tr("Verify"), this);
        verify_button_->setObjectName(QStringLiteral("verify_button"));
        auto* close_button = widgets::secondary_button(tr("Close"), this);
        buttons->addWidget(commit_button_);
        buttons->addWidget(verify_button_);
        buttons->addWidget(close_button);
        buttons->addStretch(1);
        layout->addLayout(buttons);

        connect(commit_button_, &QPushButton::clicked, this, &AddScriptDialog::commit);
        connect(verify_button_, &QPushButton::clicked, this, &AddScriptDialog::verify);
        connect(close_button, &QPushButton::clicked, this, &QDialog::close);
        // Editing the source makes a previous verdict stale.
        connect(script_edit_,
                &QPlainTextEdit::textChanged,
                this,
                [this]
                {
                    script_edit_->set_error_line(0);
                    status_->clear_status();
                });

        reset_for_add();
    }

    void AddScriptDialog::keyPressEvent(QKeyEvent* event)
    {
        // The popups inside the editor consume `Esc` first; a key that reaches
        // here must not let QDialog's default reject() discard the edits.
        if (event->key() == Qt::Key_Escape)
        {
            event->accept();
            return;
        }

        QDialog::keyPressEvent(event);
    }

    void AddScriptDialog::reset_for_add()
    {
        editing_.reset();
        setWindowTitle(tr("Add Script"));
        commit_button_->setText(tr("Add"));
        description_edit_->setText(tr("New script"));
        script_edit_->setPlainText(to_qstring(kScriptSkeleton));
        script_edit_->set_error_line(0);
        status_->clear_status();
    }

    void AddScriptDialog::reset_for_add(const QString& description, const QString& source)
    {
        // The parameterless form seeds the skeleton and the default description;
        // this one overwrites both with the caller's own text.
        reset_for_add();
        description_edit_->setText(description);
        script_edit_->setPlainText(source);
        script_edit_->set_error_line(0);
        status_->clear_status();
    }

    void AddScriptDialog::edit_entry(std::size_t row)
    {
        if (!table_.valid_index(row) || table_.entries()[row].kind != table::EntryKind::script)
        {
            reset_for_add();
            return;
        }

        editing_ = row;
        setWindowTitle(tr("Edit Script"));
        commit_button_->setText(tr("Save"));
        description_edit_->setText(to_qstring(table_.entries()[row].description));
        script_edit_->setPlainText(to_qstring(table_.entries()[row].script));
        script_edit_->set_error_line(0);
        status_->clear_status();
    }

    void AddScriptDialog::verify()
    {
        const std::string                      source = script_edit_->toPlainText().toStdString();
        const std::expected<void, std::string> result = script::check_syntax(source);

        if (result.has_value())
        {
            script_edit_->set_error_line(0);
            status_->set_status(widgets::StatusKind::success, tr("Syntax OK."));
            log::info(log::category::script, "syntax check passed");
            return;
        }

        // The compiler's own message is the feedback; the line it names (if any)
        // is marked and the caret is moved to it.
        status_->set_status(widgets::StatusKind::error, to_qstring(result.error()));

        const int line = error_line_from_message(result.error());
        script_edit_->set_error_line(line);
        if (line > 0)
        {
            const QTextBlock block = script_edit_->document()->findBlockByNumber(line - 1);
            if (block.isValid())
            {
                script_edit_->setTextCursor(QTextCursor(block));
                script_edit_->centerCursor();
            }
        }

        log::warning(log::category::script, std::format("syntax check failed: {}", single_line(result.error())));
    }

    void AddScriptDialog::commit()
    {
        // An empty description would still sanitise to a member name, but a row
        // the user cannot tell apart is refused instead.
        if (description_edit_->text().trimmed().isEmpty())
        {
            log::warning(log::category::ui, "script rejected: empty description");
            status_->set_status(widgets::StatusKind::error, tr("Description must not be empty."));
            return;
        }

        const std::string description = description_edit_->text().toStdString();
        const std::string script      = script_edit_->toPlainText().toStdString();

        if (editing_.has_value() && table_.set_description(*editing_, description)
            && table_.set_script(*editing_, script))
        {
            log::info(log::category::ui,
                      std::format("script edited at index {} ({} byte(s))", *editing_, script.size()));
        }
        else
        {
            const std::size_t added = table_.add_script(description, script);
            log::info(log::category::ui, std::format("script added at index {} ({} byte(s))", added, script.size()));
        }

        // A confirmed add or edit closes the dialog; the row is the feedback.
        accept();
    }

} // namespace slopkit::ui::dialogs
