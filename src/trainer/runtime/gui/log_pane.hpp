#pragma once

#include <span>

#include <QPlainTextEdit>

#include "core/log.hpp"

namespace slopkit::trainer::gui
{
    class LogPane : public QPlainTextEdit
    {
        Q_OBJECT

    public:
        explicit LogPane(QWidget* parent = nullptr);

        void append_records(std::span<const log::Record> records);
    };
}
