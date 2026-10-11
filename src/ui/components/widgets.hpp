#pragma once

#include <initializer_list>

#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QWidget>

class QVBoxLayout;

namespace slopkit::ui::widgets
{
    enum class StatusKind
    {
        info,
        success,
        warning,
        error,
    };

    [[nodiscard]] QColor status_color(StatusKind kind);

    [[nodiscard]] QLabel* section_header(const QString& text, QWidget* parent = nullptr);

    [[nodiscard]] QLabel* hint_text(const QString& text, QWidget* parent = nullptr);

    void chain_tab_order(std::initializer_list<QWidget*> widgets);

    class PrimaryButton : public QPushButton
    {
        Q_OBJECT

    public:
        explicit PrimaryButton(const QString& text, QWidget* parent = nullptr);

    protected:
        void changeEvent(QEvent* event) override;

    private:
        void apply_palette();

        bool applying_ {false};
    };

    [[nodiscard]] QPushButton* secondary_button(const QString& text, QWidget* parent = nullptr);

    class Panel : public QFrame
    {
        Q_OBJECT

    public:
        explicit Panel(const QString& title = QString(), QWidget* parent = nullptr);

        [[nodiscard]] QVBoxLayout* body() const noexcept;

    private:
        QVBoxLayout* body_ {};
    };

    class ScrollingComboBox : public QComboBox
    {
        Q_OBJECT

    public:
        explicit ScrollingComboBox(int max_visible_items = 10, QWidget* parent = nullptr);

        void showPopup() override;

    private:
        int max_visible_items_ {10};
    };

    class StatusLabel : public QLabel
    {
        Q_OBJECT

    public:
        explicit StatusLabel(QWidget* parent = nullptr);

        void set_status(StatusKind kind, const QString& text);
        void clear_status();

    protected:
        void changeEvent(QEvent* event) override;

    private:
        void apply_color();

        StatusKind kind_ {StatusKind::info};
        bool       applying_ {false};
    };

    [[nodiscard]] QIcon application_icon();

    enum class ActionIcon
    {
        cancel,
        checkmark,
        chip,
        diskette,
        error,
        file,
        folder,
        home,
        info,
        ok,
        search,
        settings,
        target,
        warning,
    };

    [[nodiscard]] QIcon action_icon(ActionIcon which);

}
