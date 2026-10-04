#include "ui/components/widgets.hpp"

#include <string_view>
#include <utility>

#include <QAbstractItemView>
#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QVBoxLayout>

#include "ui/theme.hpp"

namespace slopkit::ui::widgets
{

    namespace
    {
        // Every icon size generated for the desktop entry.
        constexpr int kIconSizes[] = {16, 24, 32, 48, 64, 128, 256, 512};

        // Every icon size generated for the action glyphs; menus paint 16
        // logical px, the rest cover HiDPI/fractional scaling.
        constexpr int kActionIconSizes[] = {16, 24, 32, 48, 64};

        constexpr std::string_view action_icon_name(ActionIcon which)
        {
            switch (which)
            {
            case ActionIcon::cancel:
                return "cancel";
            case ActionIcon::checkmark:
                return "checkmark";
            case ActionIcon::chip:
                return "chip";
            case ActionIcon::diskette:
                return "diskette";
            case ActionIcon::error:
                return "error";
            case ActionIcon::file:
                return "file";
            case ActionIcon::folder:
                return "folder";
            case ActionIcon::home:
                return "home";
            case ActionIcon::info:
                return "info";
            case ActionIcon::ok:
                return "ok";
            case ActionIcon::search:
                return "search";
            case ActionIcon::settings:
                return "settings";
            case ActionIcon::target:
                return "target";
            case ActionIcon::warning:
                return "warning";
            }
            std::unreachable();
        }
    } // namespace

    QColor status_color(StatusKind kind)
    {
        const Theme& theme = active_theme();
        switch (kind)
        {
        case StatusKind::info:
            break;
        case StatusKind::success:
            return theme.success;
        case StatusKind::warning:
            return theme.warning;
        case StatusKind::error:
            return theme.error;
        }
        return theme.text_muted;
    }

    QLabel* section_header(const QString& text, QWidget* parent)
    {
        // A status label in the muted "info" colour, so the title follows the
        // theme without a hard-coded colour.
        auto* label = new StatusLabel(parent);
        label->set_status(StatusKind::info, text);
        QFont font = label->font();
        font.setWeight(QFont::DemiBold);
        label->setFont(font);
        return label;
    }

    QLabel* hint_text(const QString& text, QWidget* parent)
    {
        // A status label in the muted "info" colour, so the hint follows the
        // theme without a hard-coded colour.
        auto* label = new StatusLabel(parent);
        label->set_status(StatusKind::info, text);
        return label;
    }

    PrimaryButton::PrimaryButton(const QString& text, QWidget* parent) : QPushButton(text, parent)
    {
        apply_palette();
    }

    void PrimaryButton::changeEvent(QEvent* event)
    {
        QPushButton::changeEvent(event);
        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        {
            apply_palette();
        }
    }

    void PrimaryButton::apply_palette()
    {
        // setPalette() reports a palette change; without the guard that would
        // recurse forever.
        if (applying_)
        {
            return;
        }
        applying_ = true;

        QPalette     palette = QGuiApplication::palette();
        const Theme& theme   = active_theme();
        palette.setColor(QPalette::Button, theme.accent);
        palette.setColor(QPalette::ButtonText, theme.on_accent);
        setPalette(palette);

        applying_ = false;
    }

    QPushButton* secondary_button(const QString& text, QWidget* parent)
    {
        return new QPushButton(text, parent);
    }

    ScrollingComboBox::ScrollingComboBox(int max_visible_items, QWidget* parent)
        : QComboBox(parent), max_visible_items_(max_visible_items)
    {
        setMaxVisibleItems(max_visible_items);
    }

    void ScrollingComboBox::showPopup()
    {
        QComboBox::showPopup();

        // Trim the popup to the visible rows so its list scrolls instead of
        // growing over the screen. The chrome - frame, margins and scroll arrows
        // - is whatever the popup spends beyond the list itself.
        QAbstractItemView* view       = this->view();
        QWidget*           popup      = view != nullptr ? view->window() : nullptr;
        const int          row_height = view != nullptr ? view->sizeHintForRow(0) : 0;
        if (popup == nullptr || popup == this || row_height <= 0)
        {
            return;
        }

        const int height = max_visible_items_ * row_height + (popup->height() - view->height());
        if (popup->height() <= height)
        {
            return;
        }

        popup->resize(popup->width(), height);
        // A popup opened above the combo box has to follow the shorter list.
        if (popup->y() < mapToGlobal(QPoint(0, 0)).y())
        {
            popup->move(popup->x(), mapToGlobal(QPoint(0, 0)).y() - height);
        }
        view->scrollTo(view->currentIndex(), QAbstractItemView::PositionAtCenter);
    }

    Panel::Panel(const QString& title, QWidget* parent) : QFrame(parent)
    {
        setFrameShape(QFrame::StyledPanel);
        setFrameShadow(QFrame::Plain);
        // The surface shade comes from a palette role, so it follows the theme.
        setBackgroundRole(QPalette::Base);
        setAutoFillBackground(true);

        body_ = new QVBoxLayout(this);
        body_->setContentsMargins(8, 8, 8, 8);
        body_->setSpacing(6);
        if (!title.isEmpty())
        {
            body_->addWidget(section_header(title, this));
        }
    }

    QVBoxLayout* Panel::body() const noexcept
    {
        return body_;
    }

    StatusLabel::StatusLabel(QWidget* parent) : QLabel(parent)
    {
        setWordWrap(true);
        apply_color();
    }

    void StatusLabel::set_status(StatusKind kind, const QString& text)
    {
        kind_ = kind;
        setText(text);
        apply_color();
    }

    void StatusLabel::clear_status()
    {
        set_status(StatusKind::info, QString());
    }

    void StatusLabel::changeEvent(QEvent* event)
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        {
            apply_color();
        }
    }

    void StatusLabel::apply_color()
    {
        // setPalette() reports a palette change; without the guard that would
        // recurse forever.
        if (applying_)
        {
            return;
        }
        applying_ = true;

        // The application palette is the base, so a later theme switch is picked
        // up again by the palette-change handler.
        QPalette palette = QGuiApplication::palette();
        palette.setColor(QPalette::WindowText, status_color(kind_));
        setPalette(palette);

        applying_ = false;
    }

    QIcon application_icon()
    {
        QIcon icon;
        for (const int size : kIconSizes)
        {
            icon.addFile(QStringLiteral(":/icons/%1x%1/apps/slopkit.png").arg(size));
        }
        return icon;
    }

    QIcon action_icon(ActionIcon which)
    {
        const std::string_view name = action_icon_name(which);
        const QString          stem = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
        QIcon                  icon;
        for (const int size : kActionIconSizes)
        {
            icon.addFile(QStringLiteral(":/icons/actions/%1x%1/%2.png").arg(size).arg(stem));
        }
        return icon;
    }

} // namespace slopkit::ui::widgets
