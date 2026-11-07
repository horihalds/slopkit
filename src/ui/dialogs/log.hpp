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
}

namespace slopkit::ui::dialogs
{
    class LogDialog : public QDialog
    {
        Q_OBJECT

    public:
        LogDialog();

        void set_dialog_directory(const QString& directory);

        void save_to(const QString& path);

    signals:
        void logSaved(const QString& path);

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void drain();
        void render();
        void clear();
        void save_as();
        void update_status(int shown);

        widgets::ScrollingComboBox* level_filter_ {};
        QLineEdit*                  text_filter_ {};
        QPlainTextEdit*             view_ {};
        widgets::StatusLabel*       status_ {};
        LogNotifier                 notifier_;
        std::vector<log::Record>    records_;
        QString                     dialog_directory_;
        bool                        seeded_ {false};
    };

}
