#pragma once

#include <QColor>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QWidget>

class QAction;
class QToolButton;
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

    // The colour a status kind is drawn with.
    [[nodiscard]] QColor status_color(StatusKind kind);

    // Muted title label used to head a section or panel.
    [[nodiscard]] QLabel* section_header(const QString& text, QWidget* parent = nullptr);

    // Accent-filled button for the primary action. Fusion derives the hover and
    // pressed shades from the button colour, and the palette is re-applied when
    // the application palette changes, so a theme switch keeps the accent.
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

    // Surface-coloured button for secondary actions; the base palette already
    // paints it that way.
    [[nodiscard]] QPushButton* secondary_button(const QString& text, QWidget* parent = nullptr);

    // Card-like surface with an optional title; add the content to body().
    class Panel : public QFrame
    {
        Q_OBJECT

    public:
        explicit Panel(const QString& title = QString(), QWidget* parent = nullptr);

        [[nodiscard]] QVBoxLayout* body() const noexcept;

    private:
        QVBoxLayout* body_ {};
    };

    // Collapsible themed section; add the content to body().
    class CollapsibleSection : public QWidget
    {
        Q_OBJECT

    public:
        explicit CollapsibleSection(const QString& title, bool expanded = true, QWidget* parent = nullptr);

        [[nodiscard]] QVBoxLayout* body() const noexcept;

    private:
        void set_expanded(bool expanded);

        QToolButton* toggle_ {};
        QWidget*     content_ {};
        QVBoxLayout* body_ {};
    };

    // Wrapped status line coloured by kind; keeps its colour across theme
    // switches.
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

    // Multi-size application icon assembled from the generated
    // `:/icons/<N>x<N>/apps/slopkit.png` resources.
    [[nodiscard]] QIcon application_icon();

    // Toolbar action: the toolbar shows `text`, the menus show `menu_text`
    // (which carries the ellipsis where the command opens something).
    [[nodiscard]] QAction* toolbar_action(const QString& text, const QString& menu_text, QWidget* parent);

} // namespace slopkit::ui::widgets
