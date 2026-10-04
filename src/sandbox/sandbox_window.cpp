#include "sandbox/sandbox_window.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
#include "sandbox/sandbox_values.hpp"
#include "scan/types.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::sandbox
{
    namespace
    {
        // The animation runs at 2 Hz, the rows re-read the values at 10 Hz: light
        // enough that the sandbox stays idle at well under a percent of a core.
        constexpr int kRefreshIntervalMs   = 100;
        constexpr int kAnimationIntervalMs = 500;

        QString value_type_name(scan::ValueType type)
        {
            return QString::fromUtf8(scan::kValueTypeNames[static_cast<std::size_t>(type)]);
        }

        QString hex_dump(std::span<const std::byte> bytes)
        {
            std::string text;
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                if (index != 0)
                {
                    text += ' ';
                }
                text += std::format("{:02X}", std::to_integer<unsigned>(bytes[index]));
            }
            return QString::fromStdString(text);
        }

        QString banner_text()
        {
            const auto& banner = values().banner;
            const auto  end    = std::ranges::find(banner, '\0');
            return QString::fromUtf8(banner.data(), static_cast<int>(end - banner.begin()));
        }

        void set_banner(const QString& text)
        {
            const QByteArray utf8   = text.toUtf8();
            auto&            banner = values().banner;
            std::fill(banner.begin(), banner.end(), '\0');
            const auto count = std::min<std::size_t>(static_cast<std::size_t>(utf8.size()), banner.size() - 1);
            std::copy_n(utf8.constData(), count, banner.begin());
        }

        QString pattern_text()
        {
            const auto& pattern = values().pattern;
            return hex_dump(std::span(reinterpret_cast<const std::byte*>(pattern.data()), pattern.size()));
        }

        QString heap_marker_text()
        {
            return hex_dump(heap_marker());
        }
    } // namespace

    SandboxWindow::SandboxWindow(QWidget* parent) : QWidget(parent)
    {
        build_body();

        refresh_timer_ = new QTimer(this);
        refresh_timer_->setObjectName(QStringLiteral("refresh_timer"));
        refresh_timer_->setInterval(kRefreshIntervalMs);
        connect(refresh_timer_, &QTimer::timeout, this, &SandboxWindow::refresh);
        refresh_timer_->start();

        animation_timer_ = new QTimer(this);
        animation_timer_->setObjectName(QStringLiteral("animation_timer"));
        animation_timer_->setInterval(kAnimationIntervalMs);
        connect(animation_timer_, &QTimer::timeout, this, &SandboxWindow::animate);
        animation_timer_->start();

        refresh();
    }

    void SandboxWindow::build_body()
    {
        setWindowTitle(tr("slopkit sandbox"));
        setWindowIcon(ui::widgets::application_icon());
        setMinimumSize(460, 400);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        layout->addWidget(ui::widgets::section_header(tr("Exposed values"), this));
        layout->addWidget(ui::widgets::hint_text(tr("Each row is a live view over this process' memory."), this));

        // Column headings shared by every panel's grid.
        const auto headers = [this](QGridLayout* grid)
        {
            grid->addWidget(ui::widgets::section_header(tr("Value"), this), 0, 0);
            grid->addWidget(ui::widgets::section_header(tr("Type"), this), 0, 1);
            grid->addWidget(ui::widgets::section_header(tr("Current"), this), 0, 2);
        };

        // Integers.
        {
            auto* panel = new ui::widgets::Panel(tr("Integers"), this);
            auto* grid  = new QGridLayout();
            grid->setHorizontalSpacing(12);
            grid->setVerticalSpacing(6);
            grid->setColumnStretch(2, 1);
            headers(grid);

            int row = 1;
            add_row(
                grid,
                row++,
                QStringLiteral("byte_value"),
                value_type_name(scan::ValueType::byte),
                []
                {
                    return QString::fromStdString(std::format("{}", static_cast<int>(values().byte_value)));
                },
                [](const QString& text)
                {
                    bool       ok     = false;
                    const uint parsed = text.toUInt(&ok);
                    if (ok && parsed <= 0xFF)
                    {
                        values().byte_value = static_cast<std::uint8_t>(parsed);
                    }
                });
            add_row(
                grid,
                row++,
                QStringLiteral("small_value"),
                value_type_name(scan::ValueType::int16),
                []
                {
                    return QString::fromStdString(std::format("{}", static_cast<int>(values().small_value)));
                },
                [](const QString& text)
                {
                    bool      ok     = false;
                    const int parsed = text.toInt(&ok);
                    if (ok && parsed >= std::numeric_limits<std::int16_t>::min()
                        && parsed <= std::numeric_limits<std::int16_t>::max())
                    {
                        values().small_value = static_cast<std::int16_t>(parsed);
                    }
                });
            add_row(
                grid,
                row++,
                QStringLiteral("health"),
                value_type_name(scan::ValueType::int32),
                []
                {
                    return QString::fromStdString(std::format("{}", values().health));
                },
                [](const QString& text)
                {
                    bool      ok     = false;
                    const int parsed = text.toInt(&ok);
                    if (ok)
                    {
                        values().health = parsed;
                    }
                });
            add_row(
                grid,
                row++,
                QStringLiteral("score"),
                value_type_name(scan::ValueType::int64),
                []
                {
                    return QString::fromStdString(std::format("{}", values().score));
                },
                [](const QString& text)
                {
                    bool            ok     = false;
                    const qlonglong parsed = text.toLongLong(&ok);
                    if (ok)
                    {
                        values().score = static_cast<std::int64_t>(parsed);
                    }
                });

            panel->body()->addLayout(grid);
            layout->addWidget(panel);
        }

        // Floating point and text.
        {
            auto* panel = new ui::widgets::Panel(tr("Floating point and text"), this);
            auto* grid  = new QGridLayout();
            grid->setHorizontalSpacing(12);
            grid->setVerticalSpacing(6);
            grid->setColumnStretch(2, 1);
            headers(grid);

            int row = 1;
            add_row(
                grid,
                row++,
                QStringLiteral("speed"),
                value_type_name(scan::ValueType::float32),
                []
                {
                    return QString::fromStdString(std::format("{:.3f}", values().speed));
                },
                [](const QString& text)
                {
                    bool        ok     = false;
                    const float parsed = text.toFloat(&ok);
                    if (ok)
                    {
                        values().speed = parsed;
                    }
                });
            add_row(
                grid,
                row++,
                QStringLiteral("precision"),
                value_type_name(scan::ValueType::float64),
                []
                {
                    return QString::fromStdString(std::format("{:.6f}", values().precision));
                },
                [](const QString& text)
                {
                    bool         ok     = false;
                    const double parsed = text.toDouble(&ok);
                    if (ok)
                    {
                        values().precision = parsed;
                    }
                });
            add_row(
                grid,
                row++,
                QStringLiteral("drift"),
                value_type_name(scan::ValueType::float32),
                []
                {
                    return QString::fromStdString(std::format("{:.3f}", values().drift));
                },
                [](const QString& text)
                {
                    bool        ok     = false;
                    const float parsed = text.toFloat(&ok);
                    if (ok)
                    {
                        values().drift = parsed;
                    }
                });
            add_row(
                grid,
                row++,
                QStringLiteral("banner"),
                value_type_name(scan::ValueType::string),
                []
                {
                    return banner_text();
                },
                [](const QString& text)
                {
                    set_banner(text);
                });

            panel->body()->addLayout(grid);
            layout->addWidget(panel);
        }

        // Byte patterns.
        {
            auto* panel = new ui::widgets::Panel(tr("Byte patterns"), this);
            auto* grid  = new QGridLayout();
            grid->setHorizontalSpacing(12);
            grid->setVerticalSpacing(6);
            grid->setColumnStretch(2, 1);
            headers(grid);

            int row = 1;
            add_row(
                grid,
                row++,
                QStringLiteral("pattern"),
                value_type_name(scan::ValueType::byte_array),
                []
                {
                    return pattern_text();
                },
                {},
                true);
            add_row(
                grid,
                row++,
                QStringLiteral("heap_marker"),
                value_type_name(scan::ValueType::byte_array),
                []
                {
                    return heap_marker_text();
                },
                {},
                true);

            grid->addWidget(new QLabel(tr("Address"), this), row, 0);
            heap_marker_address_ = new QLabel(this);
            heap_marker_address_->setObjectName(QStringLiteral("heap_marker_address"));
            heap_marker_address_->setFont(ui::mono_font());
            heap_marker_address_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            grid->addWidget(heap_marker_address_, row, 1, 1, 2);

            panel->body()->addLayout(grid);
            layout->addWidget(panel);
        }

        auto* controls = new QHBoxLayout();
        pause_check_   = new QCheckBox(tr("Pause changes"), this);
        pause_check_->setObjectName(QStringLiteral("pause_check"));
        auto* reset_button = ui::widgets::secondary_button(tr("Reset values"), this);
        reset_button->setObjectName(QStringLiteral("reset_button"));
        controls->addWidget(pause_check_);
        controls->addWidget(reset_button);
        controls->addStretch(1);
        layout->addLayout(controls);
        layout->addStretch(1);

        connect(pause_check_, &QCheckBox::toggled, this, &SandboxWindow::set_paused);
        connect(reset_button,
                &QPushButton::clicked,
                this,
                [this]
                {
                    reset_values();
                });
    }

    QLineEdit* SandboxWindow::add_row(QGridLayout*                        grid,
                                      int                                 row,
                                      const QString&                      name,
                                      const QString&                      type_name,
                                      std::function<QString()>            read,
                                      std::function<void(const QString&)> write,
                                      bool                                read_only)
    {
        auto* name_label = new QLabel(name, this);
        name_label->setObjectName(QStringLiteral("name_%1").arg(name));
        grid->addWidget(name_label, row, 0);

        auto* type_label = new QLabel(type_name, this);
        type_label->setObjectName(QStringLiteral("type_%1").arg(name));
        grid->addWidget(type_label, row, 1);

        auto* edit = new QLineEdit(this);
        edit->setObjectName(QStringLiteral("value_%1").arg(name));
        edit->setFont(ui::mono_font());
        edit->setReadOnly(read_only);
        grid->addWidget(edit, row, 2);

        rows_.push_back(Row {edit, std::move(read), std::move(write)});
        if (rows_.back().write)
        {
            std::function<void(const QString&)> writer = rows_.back().write;
            connect(edit,
                    &QLineEdit::editingFinished,
                    this,
                    [writer, edit]
                    {
                        writer(edit->text());
                    });
        }
        return edit;
    }

    void SandboxWindow::refresh()
    {
        for (const Row& row : rows_)
        {
            // Leave a focused editor alone so the refresh never clobbers what the
            // user is typing; signals are blocked so re-reading a value cannot
            // bounce back as an edit.
            if (row.edit->hasFocus())
            {
                continue;
            }
            const QSignalBlocker blocker(row.edit);
            row.edit->setText(row.read());
        }

        const std::span<std::byte> marker = heap_marker();
        heap_marker_address_->setText(
            QString::fromStdString(std::format("0x{:016X}", reinterpret_cast<std::uintptr_t>(marker.data()))));
    }

    void SandboxWindow::set_paused(bool paused)
    {
        paused_ = paused;
        if (paused)
        {
            animation_timer_->stop();
        }
        else
        {
            animation_timer_->start();
        }
        if (pause_check_ != nullptr)
        {
            const QSignalBlocker blocker(pause_check_);
            pause_check_->setChecked(paused);
        }
        log::debug(log::category::ui, paused ? "sandbox animation paused" : "sandbox animation resumed");
    }

    bool SandboxWindow::is_paused() const noexcept
    {
        return paused_;
    }

    void SandboxWindow::animate()
    {
        if (paused_)
        {
            return;
        }
        advance();
        refresh();
    }

    void SandboxWindow::reset_values()
    {
        reset();
        refresh();
        log::info(log::category::ui, "sandbox values reset");
    }

} // namespace slopkit::sandbox
