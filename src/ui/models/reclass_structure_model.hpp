#pragma once

#include <cstddef>
#include <memory>
#include <set>
#include <vector>

#include <QAbstractItemModel>
#include <QVariant>

#include "reclass/project.hpp"

class QModelIndex;

namespace slopkit::ui::models
{
    // How deep an expanded chain of class references may nest before a row
    // stops inlining, so a pathological chain cannot explode the grid.
    inline constexpr std::size_t kMaxReclassBlockDepth = 8;

    // The Type cell's bracketed, editable suffix - a text node's length in
    // characters, an array's element count - which the grid anchors its inline
    // editor on: every other kind answers nothing and takes no run editor.
    inline constexpr int kExtentTextRole = Qt::UserRole + 3;

    // One grid row: a node of the selected class, or a node inlined from a
    // class a reference node names. `offset` is shown in the Offset column and
    // continues from the referencing node's offset, while `block` names the
    // stretch of memory the row's value is read from.
    struct ReclassRow
    {
        // The node the row shows, so a container's child needs no class lookup.
        const reclass::Node* node {};
        std::size_t          node_id {};
        std::size_t          class_index {};
        std::size_t          node_index {};
        std::size_t          offset {};
        std::size_t          block {};
        std::size_t          position {};
        bool                 unknown_class {};
        bool                 cycle {};
        bool                 too_deep {};
        // The node is hidden: the row is the collapsed `...` placeholder the
        // grid draws, and it inlines nothing.
        bool                 hidden {};

        ReclassRow*                              parent {};
        std::vector<std::unique_ptr<ReclassRow>> children;
    };

    // One stretch of memory the grid reads: the selected class, or the class an
    // expanded reference node names. `base_offset` is where the block's own
    // first byte sits in the display offsets, which is how the value model
    // locates the block's base inside its parent's bytes.
    struct ReclassBlock
    {
        std::size_t index {};
        std::size_t class_index {};
        std::size_t parent_block {};
        std::size_t base_offset {};
        std::size_t size {};
        bool        via_pointer {};
    };

    class ReclassStructureModel : public QAbstractItemModel
    {
        Q_OBJECT

    public:
        enum Column
        {
            offset,
            address,
            ascii,
            type,
            name,
            value,
            comment,
            hints,
            pointer,
            column_count,
        };

        explicit ReclassStructureModel(reclass::Project& project, QObject* parent = nullptr);

        void set_project(reclass::Project& project);
        void set_class(std::size_t index);
        void set_expanded(std::size_t node_id, bool expanded);

        // Re-reads the project: the rows, their offsets and the block list.
        void rebuild();

        // Renames the node the row shows; an empty name is refused with its
        // reason on statusChanged, the way the value model refuses a write.
        bool set_node_name(const ReclassRow& row, const QString& name);

        // Sets the node's note: the text is trimmed and an empty one clears the
        // note, so there is nothing to refuse.
        bool set_node_comment(const ReclassRow& row, const QString& text);

        // Sets the node's own number, the one the Type cell brackets: a text
        // node's length in characters or an array's element count. The text
        // must be a whole number of at least one; anything else is refused with
        // its reason on statusChanged, and so is a row with no node of its own.
        bool set_node_extent(const ReclassRow& row, const QString& text);

        [[nodiscard]] reclass::Project& project() noexcept;
        [[nodiscard]] std::size_t       selected_class() const noexcept;
        [[nodiscard]] bool              valid_class() const noexcept;

        [[nodiscard]] const ReclassRow*                row_for(const QModelIndex& index) const;
        [[nodiscard]] const ReclassRow*                row_at(const QModelIndex& parent, std::size_t row) const;
        [[nodiscard]] QModelIndex                      index_for_node(std::size_t node_id) const;
        [[nodiscard]] const std::vector<ReclassBlock>& blocks() const noexcept;

        // Whether the reference node with this id is inlined in the grid.
        [[nodiscard]] bool is_expanded(std::size_t node_id) const;

        // Whether the row names a class the grid could inline. A marked row - an
        // unknown class, a cycle or a chain past the depth cap - cannot.
        [[nodiscard]] bool expandable(const ReclassRow& row) const;

        // The reference nodes the grid currently inlines.
        [[nodiscard]] const std::set<std::size_t>& expanded_nodes() const noexcept;

        [[nodiscard]] QModelIndex   index(int row, int column, const QModelIndex& parent = {}) const override;
        [[nodiscard]] QModelIndex   parent(const QModelIndex& index) const override;
        [[nodiscard]] int           rowCount(const QModelIndex& parent = {}) const override;
        [[nodiscard]] int           columnCount(const QModelIndex& parent = {}) const override;
        [[nodiscard]] QVariant      data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
        [[nodiscard]] QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

    signals:
        void blocksChanged();

        // A command's outcome, reported on the window's status line.
        void statusChanged(const QString& message, bool error);

    private:
        void build_rows();

        void build_class(const reclass::Class&     type,
                         std::size_t               class_index,
                         std::size_t               block,
                         std::size_t               base_offset,
                         std::vector<std::size_t>& ancestors,
                         ReclassRow*               parent);

        // Appends the children of an expanded container: an array's elements
        // follow one another while a union's members share the union's address.
        void build_container(const reclass::Node& node,
                             std::size_t          class_index,
                             std::size_t          block,
                             std::size_t          base_offset,
                             ReclassRow*          parent);

        [[nodiscard]] ReclassRow* append_row(ReclassRow* parent, std::unique_ptr<ReclassRow> row);
        [[nodiscard]] std::size_t class_index_of(const reclass::Class* type) const;
        [[nodiscard]] QString     type_text(const ReclassRow& row, const reclass::Node& node) const;

        reclass::Project*                        project_ {};
        std::size_t                              selected_class_ {};
        bool                                     valid_class_ {};
        std::set<std::size_t>                    expanded_;
        std::vector<std::unique_ptr<ReclassRow>> roots_;
        std::vector<ReclassBlock>                blocks_;
    };

}
