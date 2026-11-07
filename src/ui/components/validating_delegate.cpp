#include "ui/components/validating_delegate.hpp"

#include <QAbstractItemDelegate>
#include <QAbstractItemModel>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QModelIndex>
#include <QStyleOptionViewItem>
#include <QWidget>

namespace slopkit::ui::components
{
    ValidatingDelegate::ValidatingDelegate(QObject* parent) : ElidedTooltipDelegate(parent) {}

    QWidget* ValidatingDelegate::createEditor(QWidget*                    parent,
                                              const QStyleOptionViewItem& option,
                                              const QModelIndex&          index) const
    {
        // The cell an editor stands for: the base filter reports a commit but
        // never names the cell it belonged to, and keeping the editor open on a
        // refusal means writing the cell in the filter itself.
        index_ = QPersistentModelIndex(index);
        return ElidedTooltipDelegate::createEditor(parent, option, index);
    }

    bool ValidatingDelegate::eventFilter(QObject* editor, QEvent* event)
    {
        // Only the stock line editor is verified here; an editor a subclass
        // creates for another data type keeps the base's own handling.
        auto* line = qobject_cast<QLineEdit*>(editor);
        if (line == nullptr || event->type() != QEvent::KeyPress)
        {
            return ElidedTooltipDelegate::eventFilter(editor, event);
        }

        const auto* key = static_cast<const QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape)
        {
            emit closeEditor(line, QAbstractItemDelegate::RevertModelCache);
            return true;
        }
        if (key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter)
        {
            return ElidedTooltipDelegate::eventFilter(editor, event);
        }

        // The base filter would report the commit and close the editor whatever
        // the model answered, so a refusal has to be caught before it.
        if (!commit(line))
        {
            return true;
        }
        // The cell already holds the text, so the editor only has to go away
        // without committing the same text a second time.
        emit closeEditor(line, QAbstractItemDelegate::NoHint);
        return true;
    }

    bool ValidatingDelegate::commit(QWidget* editor)
    {
        auto* line = qobject_cast<QLineEdit*>(editor);
        if (line == nullptr || !index_.isValid())
        {
            return true;
        }
        // A persistent index hands out a const model, while committing the
        // editor's text is exactly the write it exists for.
        const QModelIndex index = index_;
        auto*             model = const_cast<QAbstractItemModel*>(index.model());
        if (model == nullptr || model->setData(index, line->text(), Qt::EditRole))
        {
            return true;
        }

        // The model refused the text and named its reason on the status line;
        // the text stays selected so the correction is one edit away.
        line->selectAll();
        return false;
    }

}
