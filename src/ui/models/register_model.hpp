#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <QAbstractTableModel>
#include <QString>

namespace slopkit::ui::models
{

    // One register's rendered value. `read` is false until the debugger supplies
    // a real value, which the pane paints as a muted em dash; `pending` marks a
    // cell whose write has been submitted but not yet re-read.
    struct RegisterValue
    {
        std::string name;
        QString     value;
        bool        read {};
        bool        pending {};
    };

    // The register table of the Debugger pane: a fixed x86-64 name list whose
    // values are supplied by the debug session. While a stop is live the value
    // cells are editable and a commit is handed to the controller; the model
    // never touches the target.
    class RegisterModel : public QAbstractTableModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            name,
            value,
            column_count,
        };

        // Called with a register name and its parsed value; false refuses the
        // edit, so the cell keeps its old text.
        using CommitHandler = std::function<bool(const std::string& name, std::uint64_t value)>;

        explicit RegisterModel(QObject* parent = nullptr);

        // Seeds the values of the registers named in `values`; a name that is
        // not in the fixed list is ignored. An empty span restores the
        // placeholders. Clears every pending mark.
        void set_values(std::span<const RegisterValue> values);

        void               set_commit_handler(CommitHandler handler);
        // Value cells are editable only while the session is stopped.
        void               set_editable(bool editable);
        [[nodiscard]] bool editable() const noexcept;

        // Parses "0x2000" / "2000" (hex when it carries hex digits) / "8192"
        // (decimal). Nothing when the text is not a number.
        [[nodiscard]] static std::optional<std::uint64_t> parse_value(std::string_view text);

        [[nodiscard]] int           rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int           columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant      data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        bool                        setData(const QModelIndex& index, const QVariant& value, int role) override;

    private:
        std::vector<RegisterValue> values_;
        CommitHandler              commit_;
        bool                       editable_ {false};
    };

} // namespace slopkit::ui::models
