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
    struct RegisterValue
    {
        std::string name;
        QString     value;
        bool        read {};
        bool        pending {};
    };

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

        using CommitHandler = std::function<bool(const std::string& name, std::uint64_t value)>;

        explicit RegisterModel(QObject* parent = nullptr);

        void set_values(std::span<const RegisterValue> values);

        void               set_commit_handler(CommitHandler handler);
        void               set_editable(bool editable);
        [[nodiscard]] bool editable() const noexcept;

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

}
