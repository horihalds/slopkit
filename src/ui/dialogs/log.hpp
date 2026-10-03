#pragma once

#include <vector>

#include <QDialog>

#include "core/log.hpp"
#include "ui/log_notifier.hpp"

class QLineEdit;
class QPlainTextEdit;

namespace slopkit::ui::widgets
{
    class ScrollingComboBox;
    class StatusLabel;
} // namespace slopkit::ui::widgets

namespace slopkit::ui::dialogs
{

    // A non-modal, live view of log::Logger's records with a level filter, a text
    // filter, Clear and Save As. It is seeded from the logger's retained history
    // the first time it is shown.
    class LogDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit LogDialog(QWidget* parent = nullptr);

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void drain();   // notifier records -> records_
        void render();  // records_ filtered -> view
        void clear();   // empties the view and the retained history
        void save_as(); // writes the visible lines to a chosen file
        void update_status(int shown);

        widgets::ScrollingComboBox* level_filter_ {};
        QLineEdit*                  text_filter_ {};
        QPlainTextEdit*             view_ {};
        widgets::StatusLabel*       status_ {};
        LogNotifier                 notifier_;
        std::vector<log::Record>    records_;
        bool                        seeded_ {false};
    };

} // namespace slopkit::ui::dialogs
