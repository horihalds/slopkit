#pragma once

#include <span>
#include <string>
#include <vector>

#include <QAbstractTableModel>
#include <QString>

namespace slopkit::ui::models
{

    // One register's rendered value. `read` is false until the debugger supplies
    // a real value, which the placeholder pane paints as an em dash.
    struct RegisterValue
    {
        std::string name;
        QString     value;
        bool        read {};
    };

    // The read-only register table of the Memory Viewer's placeholder debugger
    // pane: a fixed x86-64 name list whose values stay em dashes until the
    // debugger fills them in. It never touches the target.
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

        explicit RegisterModel(QObject* parent = nullptr);

        // Seeds the values of the registers named in `values`; a name that is
        // not in the fixed list is ignored. An empty span restores the
        // placeholders. The future debugger calls this with live values.
        void set_values(std::span<const RegisterValue> values);

        [[nodiscard]] int           rowCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] int           columnCount(const QModelIndex& parent = QModelIndex()) const override;
        [[nodiscard]] QVariant      data(const QModelIndex& index, int role) const override;
        [[nodiscard]] QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

    private:
        std::vector<RegisterValue> values_;
    };

} // namespace slopkit::ui::models
