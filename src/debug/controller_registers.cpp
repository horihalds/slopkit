#include "debug/controller.hpp"

#include <utility>

namespace slopkit::debug
{

    std::span<const RegisterValue> Controller::registers() const noexcept
    {
        return registers_;
    }

    std::span<const Frame> Controller::backtrace() const noexcept
    {
        return backtrace_;
    }

    void Controller::refresh()
    {
        if (state_ != State::stopped)
        {
            return;
        }
        const std::uint32_t tid = active_tid();
        if (tid == 0)
        {
            return;
        }
        {
            const JobId id = worker_.next_job_id();
            worker_.submit_registers(id,
                                     tid,
                                     [this](JobResult&& result)
                                     {
                                         apply_registers(std::move(result));
                                     });
        }
        {
            const JobId id = worker_.next_job_id();
            worker_.submit_backtrace(id,
                                     tid,
                                     [this](JobResult&& result)
                                     {
                                         apply_backtrace(std::move(result));
                                     });
        }
    }

    void Controller::apply_registers(JobResult&& result)
    {
        const auto* registers = std::get_if<RegistersResult>(&result);
        if (registers == nullptr)
        {
            return;
        }
        if (registers->error)
        {
            notify(MessageKind::warning,
                   QStringLiteral("Cannot read the registers: %1").arg(failure_text(*registers->error)));
            return;
        }
        registers_ = registers->values;

        // The first read after attach is also what tells the listing where to go.
        if (last_stop_.address == 0)
        {
            last_stop_.address = rip();
            last_stop_.tid     = active_tid();
            emit stopped();
        }
        emit registersChanged();
    }

    void Controller::apply_backtrace(JobResult&& result)
    {
        const auto* frames = std::get_if<BacktraceResult>(&result);
        if (frames == nullptr)
        {
            return;
        }
        if (frames->error)
        {
            backtrace_.clear();
            emit backtraceChanged();
            return;
        }
        backtrace_ = frames->frames;
        emit backtraceChanged();
    }

    void Controller::write_register(std::string_view name, std::uint64_t value)
    {
        if (state_ != State::stopped)
        {
            notify(MessageKind::warning, QStringLiteral("Registers can only be edited while the target is stopped."));
            return;
        }
        const JobId id = worker_.next_job_id();
        worker_.submit_set_register(
            id,
            active_tid(),
            std::string(name),
            value,
            [this](JobResult&& result)
            {
                const auto* written = std::get_if<VoidResult>(&result);
                if (written != nullptr && written->error)
                {
                    notify(MessageKind::warning,
                           QStringLiteral("Cannot write the register: %1").arg(failure_text(*written->error)));
                    return;
                }
                refresh();
            });
    }

    std::uint64_t Controller::rip() const noexcept
    {
        for (const RegisterValue& value : registers_)
        {
            if (value.name == "RIP")
            {
                return value.value;
            }
        }
        return last_stop_.address;
    }

} // namespace slopkit::debug
