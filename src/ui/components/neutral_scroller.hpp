#pragma once

#include <cstddef>
#include <functional>

class QAbstractScrollArea;
class QScrollBar;

namespace slopkit::ui::components
{
    class NeutralScroller
    {
    public:
        NeutralScroller(QAbstractScrollArea& view, std::function<void(long long)> row_delta);

        NeutralScroller(const NeutralScroller&)            = delete;
        NeutralScroller& operator=(const NeutralScroller&) = delete;

        void note_visible_rows(std::size_t rows) noexcept;
        void recenter() noexcept;

    private:
        void on_value_changed(int value);

        QScrollBar*                    bar_ {nullptr};
        std::function<void(long long)> row_delta_;
    };

}
