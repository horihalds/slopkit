#include "ui/components/neutral_scroller.hpp"

#include <utility>

#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>

namespace slopkit::ui::components
{
    namespace
    {
        // The thumb's neutral middle: positive moves scroll down, negative up.
        constexpr int kScrollNeutral = 1'000'000;
    } // namespace

    NeutralScroller::NeutralScroller(QAbstractScrollArea& view, std::function<void(long long)> row_delta)
        : bar_(view.verticalScrollBar()), row_delta_(std::move(row_delta))
    {
        bar_->setRange(0, 2 * kScrollNeutral);
        bar_->setSingleStep(1);
        bar_->setTracking(true);
        {
            const QSignalBlocker block(bar_);
            bar_->setValue(kScrollNeutral);
        }
        QObject::connect(bar_,
                         &QScrollBar::valueChanged,
                         &view,
                         [this](int value)
                         {
                             on_value_changed(value);
                         });
    }

    void NeutralScroller::note_visible_rows(std::size_t rows) noexcept
    {
        if (bar_ != nullptr)
        {
            bar_->setPageStep(static_cast<int>(rows));
        }
    }

    void NeutralScroller::recenter() noexcept
    {
        if (bar_ == nullptr)
        {
            return;
        }
        const QSignalBlocker block(bar_);
        bar_->setValue(kScrollNeutral);
    }

    void NeutralScroller::on_value_changed(int value)
    {
        const int delta = value - kScrollNeutral;
        if (delta == 0)
        {
            return;
        }
        recenter();
        if (row_delta_)
        {
            row_delta_(delta);
        }
    }

} // namespace slopkit::ui::components
