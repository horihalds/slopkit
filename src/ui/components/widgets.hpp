#pragma once

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

    // The colour a status kind is drawn with.
    [[nodiscard]] QColor status_color(StatusKind kind);

    // Muted title label used to head a section or panel.
    [[nodiscard]] QLabel* section_header(const QString& text, QWidget* parent = nullptr);

    // Muted, word-wrapped helper text placed under a field or panel.
    [[nodiscard]] QLabel* hint_text(const QString& text, QWidget* parent = nullptr);

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

    // Combo box that keeps a long popup usable: it shows at most
    // `max_visible_items` rows and scrolls the rest. The Fusion style reports
    // `SH_ComboBox_Popup` for every non-editable combo box, and Qt then sizes the
    // popup to the whole item list - `QComboBox::maxVisibleItems` is ignored and
    // only the screen bounds the popup, which a long list would cover.
    class ScrollingComboBox : public QComboBox
    {
        Q_OBJECT

    public:
        explicit ScrollingComboBox(int max_visible_items = 10, QWidget* parent = nullptr);

        void showPopup() override;

    private:
        int max_visible_items_ {10};
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

    // UI action glyphs; each maps to the generated
    // `:/icons/actions/<N>x<N>/<name>.png` resource set and mirrors the
    // `assets/icons/<name>.svg` artwork one-to-one.
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

    // Multi-size action icon for a menu or button; never null for a known kind.
    [[nodiscard]] QIcon action_icon(ActionIcon which);

} // namespace slopkit::ui::widgets
