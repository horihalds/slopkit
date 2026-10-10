#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <QAbstractListModel>
#include <QString>
#include <QStyledItemDelegate>
#include <QWidget>

class QListView;

namespace slopkit::ui::components
{
    class ScriptEditor;

    // One row of the completion list; `group` orders the list (0 API, 1 the
    // document, 2 the Lua library) and `function` decides whether accepting the
    // name also inserts `()`.
    struct CompletionItem
    {
        QString name;
        QString signature;
        QString summary;
        int     group {0};
        bool    function {false};
    };

    // The rows the completion popup shows: the API, then the names the edited
    // text defines, then the Lua library, alphabetical inside each group.
    class ScriptCompletionModel : public QAbstractListModel
    {
        Q_OBJECT

    public:
        enum Role
        {
            NameRole = Qt::UserRole + 1,
            SignatureRole,
            SummaryRole,
            GroupRole,
        };

        using QAbstractListModel::QAbstractListModel;

        // Re-filters from `script::api_catalog`, the `document_text` and the Lua
        // library lists, for a dotted `receiver` ("mem") and a `prefix`.
        void set_context(const QString& receiver, const QString& prefix, const QString& document_text);

        [[nodiscard]] QString  name_at(int row) const;
        [[nodiscard]] bool     function_at(int row) const;
        [[nodiscard]] int      rowCount(const QModelIndex& parent = {}) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

    private:
        std::vector<CompletionItem> items_;
    };

    // Paints a row's name and its muted signature (and summary) on two lines from
    // the active theme.
    class ScriptCompletionDelegate : public QStyledItemDelegate
    {
        Q_OBJECT

    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
        [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    };

    // A frameless, focus-less popup the editor drives: it never takes focus, so
    // the editor keeps its caret and every key.
    class ScriptCompletionPopup : public QWidget
    {
        Q_OBJECT

    public:
        explicit ScriptCompletionPopup(ScriptEditor* editor);

        // Re-filters for the caret's context and shows the list under
        // `caret_rect` (viewport coordinates); hides itself when nothing matches.
        void show_at(const QRect& caret_rect, const QString& receiver, const QString& prefix);
        void hide_popup();

        [[nodiscard]] bool                   is_open() const;
        [[nodiscard]] std::optional<QString> selected_name() const;
        [[nodiscard]] bool                   selected_is_function() const;

        // Moves the highlight by `delta` rows (Up/Down); returns true when it
        // moved, so the editor knows the key was consumed.
        bool move_selection(int delta);

    protected:
        void changeEvent(QEvent* event) override;

    private:
        void apply_theme();

        ScriptEditor*          editor_;
        ScriptCompletionModel* model_;
        QListView*             view_;
    };

    // The frameless, focus-less signature hint the editor shows while the caret
    // sits inside a call's parentheses.
    class ScriptCallTip : public QWidget
    {
        Q_OBJECT

    public:
        explicit ScriptCallTip(ScriptEditor* editor);

        // Shows `signature` with parameter `active_parameter` emphasised and
        // `summary` muted underneath, at the caret line (flipped above it when
        // there is no room below).
        void show_at(const QRect&   caret_rect,
                     const QString& signature,
                     std::size_t    active_parameter,
                     const QString& summary);
        void hide_tip();

        [[nodiscard]] bool    is_visible() const;
        [[nodiscard]] QString signature() const;
        [[nodiscard]] int     active_parameter() const;

    protected:
        void paintEvent(QPaintEvent* event) override;
        void changeEvent(QEvent* event) override;

    private:
        void apply_theme();

        ScriptEditor* editor_;
        QString       signature_;
        QString       summary_;
        std::size_t   active_parameter_ {0};
    };
} // namespace slopkit::ui::components
