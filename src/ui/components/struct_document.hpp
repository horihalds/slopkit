#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "process/access_worker.hpp"
#include "process/attachment.hpp"
#include "process/types.hpp"
#include "script/symbols.hpp"
#include "structs/definition.hpp"
#include "structs/layout.hpp"
#include "structs/types.hpp"
#include "ui/address_format.hpp"
#include "ui/live_values.hpp"
#include "ui/models/live_cells.hpp"

#include <QObject>
#include <QString>

namespace slopkit::ui::components
{
    // Decodes the fields of a `structs::Structs` straight out of a live target.
    // It is a `ui::LiveSurface`: the batched read pass asks it for one request
    // per interval, so no target access happens on the UI thread.
    class StructDocument : public QObject, public ui::LiveSurface
    {
        Q_OBJECT

    public:
        StructDocument(process::AccessWorker&   worker,
                       process::AttachedTarget& target,
                       structs::Structs&        definitions,
                       QObject*                 parent  = nullptr,
                       script::SymbolTable&     symbols = script::default_symbol_table());

        void               set_visible(bool visible);
        [[nodiscard]] bool visible() const noexcept;

        [[nodiscard]] std::size_t pointer_size() const noexcept;

        void                                 set_modules(std::vector<process::ModuleInfo> modules);
        void                                 set_address_mode(ui::AddressMode mode);
        [[nodiscard]] const ui::ModuleSpans& module_spans() const;
        [[nodiscard]] script::SymbolTable&   symbols() const;

        // Redefines what the next pass reads after the definitions, the modules
        // or the attached target changed.
        void refresh();

        [[nodiscard]] bool    followed(std::uint64_t id) const;
        void                  set_followed(std::uint64_t id, bool followed);
        [[nodiscard]] bool    can_follow(std::uint64_t id) const;
        [[nodiscard]] QString follow_refusal(std::uint64_t id) const;

        void               set_expanded(std::uint64_t id, bool expanded);
        [[nodiscard]] bool expanded(std::uint64_t id) const;

        [[nodiscard]] QString value_text(std::uint64_t id) const;
        [[nodiscard]] QString type_text(std::uint64_t id) const;
        [[nodiscard]] QString offset_text(std::uint64_t id) const;
        [[nodiscard]] QString describe(std::span<const std::uint64_t> ids) const;

        bool write_value(std::uint64_t id, const QString& text);

        [[nodiscard]] std::vector<ui::LiveRequest> next_live_request() override;
        void apply_live_readings(std::span<const ui::LiveReading> readings) override;

    signals:
        void valuesChanged();
        void statusChanged(const QString& message, bool is_error);

    private:
        [[nodiscard]] std::uint64_t                        root_base(std::uint64_t root) const noexcept;
        [[nodiscard]] std::vector<structs::FieldPlacement> current_placements() const;
        [[nodiscard]] std::vector<std::uint8_t>            visible_mask() const;
        [[nodiscard]] std::vector<std::uint64_t>           pointer_bases() const;
        [[nodiscard]] bool                                 fields_overlap(std::size_t row) const;
        [[nodiscard]] std::optional<QString>               follow_block(std::uint64_t id) const;

        void ensure_bases();
        void on_resolved(process::JobId id, process::JobResult&& result);

        process::AccessWorker&   worker_;
        process::AttachedTarget& target_;
        structs::Structs&        definitions_;
        script::SymbolTable&     symbols_;

        ui::ModuleSpans                        module_spans_;
        ui::AddressMode                        address_mode_ {ui::AddressMode::module_relative};
        std::size_t                            pointer_size_ {8};
        bool                                   visible_ {true};
        ui::models::LiveCells                  cells_;
        std::map<std::uint64_t, std::uint64_t> base_of_root_;
        std::map<std::uint64_t, std::uint64_t> pointer_value_;
        std::set<std::uint64_t>                followed_;
        std::set<std::uint64_t>                collapsed_;

        bool                          resolve_pending_ {true};
        std::optional<process::JobId> resolve_job_;
        std::optional<process::JobId> write_pending_;
        process::ProcessId            requested_pid_ {0};
    };

}
