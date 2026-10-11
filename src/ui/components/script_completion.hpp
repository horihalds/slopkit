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

    struct CompletionItem
    {
        QString name;
        QString signature;
        QString summary;
        int     group {0};
        bool    function {false};
    };

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

        void set_context(const QString& receiver, const QString& prefix, const QString& document_text);

        [[nodiscard]] QString  name_at(int row) const;
        [[nodiscard]] bool     function_at(int row) const;
        [[nodiscard]] int      rowCount(const QModelIndex& parent = {}) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

    private:
        std::vector<CompletionItem> items_;
    };

    class ScriptCompletionDelegate : public QStyledItemDelegate
    {
        Q_OBJECT

    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
        [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    };

    class ScriptCompletionPopup : public QWidget
    {
        Q_OBJECT

    public:
        explicit ScriptCompletionPopup(ScriptEditor* editor);

        void show_at(const QRect& caret_rect, const QString& receiver, const QString& prefix);
        void hide_popup();

        [[nodiscard]] bool                   is_open() const;
        [[nodiscard]] std::optional<QString> selected_name() const;
        [[nodiscard]] bool                   selected_is_function() const;

        bool move_selection(int delta);

    protected:
        void changeEvent(QEvent* event) override;

    private:
        void apply_theme();

        ScriptEditor*          editor_;
        ScriptCompletionModel* model_;
        QListView*             view_;
    };

    class ScriptCallTip : public QWidget
    {
        Q_OBJECT

    public:
        explicit ScriptCallTip(ScriptEditor* editor);

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
}
