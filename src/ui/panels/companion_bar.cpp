#include "ui/panels/companion_bar.hpp"

#include <QHBoxLayout>
#include <QPushButton>

#include "ui/components/widgets.hpp"

namespace slopkit::ui::panels
{
    CompanionBar::CompanionBar(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setSpacing(6);

        memory_view_button_ = widgets::secondary_button(tr("Memory View"), this);
        memory_view_button_->setToolTip(tr("Open the Memory Viewer at the main module's entry point (or its base)"));
        memory_view_button_->setEnabled(false);

        reclass_button_ = widgets::secondary_button(tr("ReClass"), this);
        reclass_button_->setToolTip(tr("Open the ReClass window"));

        breakpoints_button_ = widgets::secondary_button(tr("Breakpoints"), this);
        breakpoints_button_->setToolTip(tr("Open the Breakpoints window"));

        access_watch_button_ = widgets::secondary_button(tr("Access Watch"), this);
        access_watch_button_->setToolTip(tr("Open the Access Watch window"));

        log_button_ = widgets::secondary_button(tr("Log"), this);
        log_button_->setToolTip(tr("Open the Log window"));

        settings_button_ = widgets::secondary_button(tr("Settings"), this);
        settings_button_->setToolTip(tr("Open the Settings window"));

        layout->addWidget(memory_view_button_);
        layout->addWidget(reclass_button_);
        layout->addWidget(breakpoints_button_);
        layout->addWidget(access_watch_button_);
        layout->addWidget(log_button_);
        layout->addWidget(settings_button_);
        layout->addStretch(1);

        connect(memory_view_button_, &QPushButton::clicked, this, &CompanionBar::memoryViewRequested);
        connect(reclass_button_, &QPushButton::clicked, this, &CompanionBar::reclassRequested);
        connect(breakpoints_button_, &QPushButton::clicked, this, &CompanionBar::breakpointsRequested);
        connect(access_watch_button_, &QPushButton::clicked, this, &CompanionBar::accessWatchWindowRequested);
        connect(log_button_, &QPushButton::clicked, this, &CompanionBar::logRequested);
        connect(settings_button_, &QPushButton::clicked, this, &CompanionBar::settingsRequested);

        widgets::chain_tab_order({memory_view_button_,
                                  reclass_button_,
                                  breakpoints_button_,
                                  access_watch_button_,
                                  log_button_,
                                  settings_button_});
    }

    void CompanionBar::set_target_attached(bool attached)
    {
        memory_view_button_->setEnabled(attached);
    }

    QWidget* CompanionBar::tab_order_first() const noexcept
    {
        return memory_view_button_;
    }

    QWidget* CompanionBar::tab_order_last() const noexcept
    {
        return settings_button_;
    }

}
