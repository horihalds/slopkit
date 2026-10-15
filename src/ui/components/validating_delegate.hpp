#pragma once

#include <QPersistentModelIndex>

#include "ui/components/elided_tooltip_delegate.hpp"

class QEvent;
class QModelIndex;
class QStyleOptionViewItem;
class QWidget;

namespace slopkit::ui::components
{
    // A cell editor that applies its text only when the model accepts it: Enter
    // writes through the model and takes the editor down, while a refusal keeps
    // the editor open with the text selected and leaves the reason to the
    // model's own statusChanged signal. It also carries the elided cell's
    // full-text tooltip, so a table that had one keeps it.
    class ValidatingDelegate : public widgets::ElidedTooltipDelegate
    {
        Q_OBJECT

    public:
        explicit ValidatingDelegate(QObject* parent = nullptr);

        QWidget*
        createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

        bool eventFilter(QObject* editor, QEvent* event) override;

    private:
        // Writes the editor's text into the cell it was opened for; false keeps
        // the editor open, the reason already on the owning status line.
        [[nodiscard]] bool commit(QWidget* editor);

        // The cell the open editor stands for: the delegate filters the editor
        // rather than the view, so the index a commit belongs to is remembered
        // when the editor is created.
        mutable QPersistentModelIndex index_;
    };

}
