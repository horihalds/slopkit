#include "ui/components/widgets.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/theme.hpp"

namespace slopkit::ui::widgets
{

    namespace
    {
        // Every icon size generated for the desktop entry.
        constexpr int kIconSizes[] = {16, 24, 32, 48, 64, 128, 256, 512};
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

    CollapsibleSection::CollapsibleSection(const QString& title, bool expanded, QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        toggle_ = new QToolButton(this);
        toggle_->setText(title);
        toggle_->setCheckable(true);
        toggle_->setChecked(expanded);
        toggle_->setAutoRaise(true);
        toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        toggle_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        layout->addWidget(toggle_, 0, Qt::AlignLeft);

        content_ = new QWidget(this);
        content_->setVisible(expanded);
        body_ = new QVBoxLayout(content_);
        body_->setContentsMargins(12, 0, 0, 0);
        body_->setSpacing(6);
        layout->addWidget(content_);

        connect(toggle_, &QToolButton::toggled, this, &CollapsibleSection::set_expanded);
    }

    QVBoxLayout* CollapsibleSection::body() const noexcept
    {
        return body_;
    }

    void CollapsibleSection::set_expanded(bool expanded)
    {
        toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        content_->setVisible(expanded);
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

} // namespace slopkit::ui::widgets
