#include "ui/components/widgets.hpp"

#include <string_view>
#include <utility>

#include <QAbstractItemView>
#include <QEvent>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QVBoxLayout>

#include "ui/datatype_palette.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::widgets
{
    namespace
    {
        constexpr int kIconSizes[] = {16, 24, 32, 48, 64, 128, 256, 512};

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
    }

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
        auto* label = new StatusLabel(parent);
        label->set_status(StatusKind::info, text);
        QFont font = label->font();
        font.setWeight(QFont::DemiBold);
        label->setFont(font);
        return label;
    }

    QLabel* hint_text(const QString& text, QWidget* parent)
    {
        auto* label = new StatusLabel(parent);
        label->set_status(StatusKind::info, text);
        return label;
    }

    void chain_tab_order(std::initializer_list<QWidget*> widgets)
    {
        QWidget* previous = nullptr;
        for (QWidget* widget : widgets)
        {
            if (widget == nullptr)
            {
                continue;
            }
            if (previous != nullptr)
            {
                QWidget::setTabOrder(previous, widget);
            }
            previous = widget;
        }
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
        if (applying_)
        {
            return;
        }
        applying_ = true;

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

    QIcon datatype_icon(reclass::NodeKind kind)
    {
        const std::string_view name = datatype_icon_name(kind);
        if (name.empty())
        {
            return {};
        }

        const QColor  colour = datatype_colour(kind);
        const QString stem   = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
        const QString key    = stem + QLatin1Char('#') + QString::number(colour.rgba(), 16);

        static QHash<QString, QIcon> cache;
        if (const auto it = cache.constFind(key); it != cache.constEnd())
        {
            return *it;
        }

        QIcon icon;
        for (const int size : kActionIconSizes)
        {
            QPixmap pixmap(QStringLiteral(":/icons/datatypes/%1x%1/%2.png").arg(size).arg(stem));
            if (pixmap.isNull())
            {
                continue;
            }
            QPainter painter(&pixmap);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(pixmap.rect(), colour);
            painter.end();
            icon.addPixmap(pixmap);
        }
        cache.insert(key, icon);
        return icon;
    }

}
