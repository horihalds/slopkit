#pragma once

#include <cstddef>
#include <functional>

class QAbstractScrollArea;
class QScrollBar;

namespace slopkit::ui::components
{
    // Owns a custom-painted row view's vertical bar: the range is a fixed band
    // parked at the middle, so the bar only ever reports the direction and size
    // of a move and never has to model an unbounded address space in its int
    // range. Every move is re-centred, so the thumb never drifts to an end.
    // `row_delta` runs the view's own row step with the signed row count.
    class NeutralScroller
    {
    public:
        NeutralScroller(QAbstractScrollArea& view, std::function<void(long long)> row_delta);

        NeutralScroller(const NeutralScroller&)            = delete;
        NeutralScroller& operator=(const NeutralScroller&) = delete;

        // The page step follows the row fit whenever the view re-fits.
        void note_visible_rows(std::size_t rows) noexcept;
        // Parks the thumb back at the middle after a jump or a re-window.
        void recenter() noexcept;

    private:
        void on_value_changed(int value);

        QScrollBar*                    bar_ {nullptr};
        std::function<void(long long)> row_delta_;
    };

} // namespace slopkit::ui::components
