#include <catch2/catch.hpp>

#include <QAbstractTableModel>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QTableView>

#include "support/ui_helpers.hpp"
#include "ui/components/validating_delegate.hpp"

namespace
{
    using slopkit::ui::components::ValidatingDelegate;

    // A one-cell model that takes one byte and refuses every other text the way
    // a validating table's models do: false, and a reason on its own status
    // signal.
    class PickyModel : public QAbstractTableModel
    {
    public:
        QString text;
        int     accepted {0};

        [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override
        {
            return parent.isValid() ? 0 : 1;
        }

        [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override
        {
            return parent.isValid() ? 0 : 1;
        }

        [[nodiscard]] QVariant data(const QModelIndex&, int role) const override
        {
            return role == Qt::DisplayRole || role == Qt::EditRole ? QVariant {text} : QVariant {};
        }

        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override
        {
            return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable : Qt::NoItemFlags;
        }

        bool setData(const QModelIndex& index, const QVariant& value, int role) override
        {
            if (role != Qt::EditRole || !index.isValid() || value.toString() != QStringLiteral("1A"))
            {
                return false;
            }
            text = value.toString();
            ++accepted;
            emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
            return true;
        }
    };
}

TEST_CASE("a cell editor applies on Enter only when the model takes its text", "[ui][components][delegate]")
{
    application();

    PickyModel model;
    model.text = QStringLiteral("11");

    QTableView table;
    table.setModel(&model);
    table.setItemDelegate(new ValidatingDelegate(&table));
    table.resize(200, 80);
    table.show();
    QCoreApplication::processEvents();

    const QModelIndex cell = model.index(0, 0);
    table.edit(cell);

    // The view owns the editor and destroys it when it closes, so the test holds
    // a pointer that follows it down.
    QPointer<QLineEdit> editor = table.findChild<QLineEdit*>();
    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->text() == QStringLiteral("11"));

    // A text the model turns down keeps the editor open, with the text still
    // selected so the correction is one edit away.
    editor->setText(QStringLiteral("nope"));
    QKeyEvent enter {QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    REQUIRE(editor != nullptr);
    CHECK(editor->isVisible());
    CHECK(editor->selectedText() == QStringLiteral("nope"));
    CHECK(model.text == QStringLiteral("11"));
    CHECK(model.accepted == 0);

    // A text the model takes applies and takes the editor down with it.
    editor->setText(QStringLiteral("1A"));
    QApplication::sendEvent(editor, &enter);
    QCoreApplication::processEvents();

    CHECK(editor == nullptr);
    CHECK(model.text == QStringLiteral("1A"));
    CHECK(model.accepted == 1);

    // Escape still cancels a correction without writing through the model.
    table.edit(cell);
    editor = table.findChild<QLineEdit*>();
    REQUIRE(editor != nullptr);
    REQUIRE(editor->isVisible());

    editor->setText(QStringLiteral("nope"));
    QKeyEvent escape {QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier};
    QApplication::sendEvent(editor, &escape);
    QCoreApplication::processEvents();

    CHECK(editor == nullptr);
    CHECK(model.text == QStringLiteral("1A"));
    CHECK(model.accepted == 1);
}
