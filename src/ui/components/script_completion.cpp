#include "ui/components/script_completion.hpp"

#include <algorithm>
#include <string>

#include <QEvent>
#include <QFontMetrics>
#include <QListView>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QVBoxLayout>

#include "script/api_catalog.hpp"
#include "ui/components/script_context.hpp"
#include "ui/components/script_editor.hpp"
#include "ui/theme.hpp"

namespace slopkit::ui::components
{
    namespace
    {
        // The list never grows taller than this many rows; it scrolls instead.
        constexpr int kMaxRows = 12;

        [[nodiscard]] QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
        }

        struct ParameterSpan
        {
            int begin;
            int end;
        };

        // The [begin, end) span of every top-level parameter inside a
        // signature's parentheses; `[optional]` brackets are notation, not
        // nesting.
        [[nodiscard]] std::vector<ParameterSpan> parameter_spans(const QString& signature)
        {
            const int open = signature.indexOf(QLatin1Char('('));
            if (open < 0)
            {
                return {};
            }
            int close = signature.indexOf(QLatin1Char(')'), open);
            if (close < 0)
            {
                close = signature.size();
            }

            std::vector<ParameterSpan> spans;
            int                        depth = 0;
            int                        start = open + 1;
            for (int index = open + 1; index < close; ++index)
            {
                const QChar character = signature.at(index);
                if (character == QLatin1Char('(') || character == QLatin1Char('[') || character == QLatin1Char('{'))
                {
                    ++depth;
                }
                else if (character == QLatin1Char(')') || character == QLatin1Char(']')
                         || character == QLatin1Char('}'))
                {
                    --depth;
                }
                else if (character == QLatin1Char(',') && depth == 0)
                {
                    spans.push_back({start, index});
                    start = index + 1;
                }
            }
            if (close > open + 1)
            {
                spans.push_back({start, close});
            }
            return spans;
        }
    } // namespace

    void
    ScriptCompletionModel::set_context(const QString& receiver, const QString& prefix, const QString& document_text)
    {
        beginResetModel();
        items_.clear();

        const bool dotted = !receiver.isEmpty();

        // The API comes first (group 0).
        for (const script::ApiEntry* entry : script::matches(receiver.toStdString(), prefix.toStdString()))
        {
            CompletionItem item;
            const QString  full = to_qstring(entry->name);
            item.name           = dotted ? full.mid(receiver.size() + 1) : full;
            item.signature      = to_qstring(entry->signature);
            item.summary        = to_qstring(entry->summary);
            item.group          = 0;
            item.function =
                entry->kind == script::ApiKind::global_function || entry->kind == script::ApiKind::table_function;
            items_.push_back(item);
        }

        if (!dotted)
        {
            // The names the edited text defines (group 1), once each.
            QStringList seen;
            for (const script_context::DocumentName& name : script_context::document_names(document_text))
            {
                if (!name.name.startsWith(prefix, Qt::CaseInsensitive) || seen.contains(name.name))
                {
                    continue;
                }
                seen.append(name.name);
                CompletionItem item;
                item.name      = name.name;
                item.signature = name.signature;
                item.group     = 1;
                item.function  = name.function;
                items_.push_back(item);
            }
        }

        // The Lua library (group 2): the globals, or a `string`/`table`/`math`
        // member when the receiver is one of those tables.
        if (!dotted)
        {
            for (const std::string_view global : script::lua_standard_globals())
            {
                const QString name = to_qstring(global);
                if (!name.startsWith(prefix, Qt::CaseInsensitive))
                {
                    continue;
                }
                CompletionItem item;
                item.name  = name;
                item.group = 2;
                items_.push_back(item);
            }
        }
        else if (receiver == QLatin1String("string") || receiver == QLatin1String("table")
                 || receiver == QLatin1String("math"))
        {
            const QString stem = receiver + QLatin1Char('.');
            for (const std::string_view member : script::lua_library_members())
            {
                const QString full = to_qstring(member);
                if (!full.startsWith(stem))
                {
                    continue;
                }
                const QString name = full.mid(stem.size());
                if (!name.startsWith(prefix, Qt::CaseInsensitive))
                {
                    continue;
                }
                CompletionItem item;
                item.name      = name;
                item.signature = full;
                item.group     = 2;
                items_.push_back(item);
            }
        }

        std::stable_sort(items_.begin(),
                         items_.end(),
                         [](const CompletionItem& left, const CompletionItem& right)
                         {
                             if (left.group != right.group)
                             {
                                 return left.group < right.group;
                             }
                             return QString::compare(left.name, right.name, Qt::CaseInsensitive) < 0;
                         });

        endResetModel();
    }

    QString ScriptCompletionModel::name_at(int row) const
    {
        if (row < 0 || row >= static_cast<int>(items_.size()))
        {
            return {};
        }
        return items_[static_cast<std::size_t>(row)].name;
    }

    bool ScriptCompletionModel::function_at(int row) const
    {
        if (row < 0 || row >= static_cast<int>(items_.size()))
        {
            return false;
        }
        return items_[static_cast<std::size_t>(row)].function;
    }

    int ScriptCompletionModel::rowCount(const QModelIndex& parent) const
    {
        if (parent.isValid())
        {
            return 0;
        }
        return static_cast<int>(items_.size());
    }

    QVariant ScriptCompletionModel::data(const QModelIndex& index, int role) const
    {
        if (index.row() < 0 || index.row() >= static_cast<int>(items_.size()))
        {
            return {};
        }
        const CompletionItem& item = items_[static_cast<std::size_t>(index.row())];
        switch (role)
        {
        case Qt::DisplayRole:
        case NameRole:
            return item.name;
        case SignatureRole:
            return item.signature;
        case SummaryRole:
            return item.summary;
        case GroupRole:
            return item.group;
        default:
            return {};
        }
    }

    QSize ScriptCompletionDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        const QFontMetrics metrics(option.font);
        const int          width =
            std::max(metrics.horizontalAdvance(index.data(ScriptCompletionModel::NameRole).toString()),
                     metrics.horizontalAdvance(index.data(ScriptCompletionModel::SignatureRole).toString()));
        return QSize(width + 24, metrics.height() * 2 + 6);
    }

    void ScriptCompletionDelegate::paint(QPainter*                   painter,
                                         const QStyleOptionViewItem& option,
                                         const QModelIndex&          index) const
    {
        painter->save();

        const QRect     rect     = option.rect;
        const bool      selected = (option.state & QStyle::State_Selected) != 0;
        const QPalette& palette  = option.palette;

        painter->fillRect(rect, selected ? palette.color(QPalette::Highlight) : palette.color(QPalette::Base));

        QFont name_font = option.font;
        name_font.setBold(true);
        const QFontMetrics name_metrics(name_font);
        const QFontMetrics metrics(option.font);

        const QColor name_colour = selected ? palette.color(QPalette::HighlightedText) : palette.color(QPalette::Text);
        const QColor detail_colour = selected ? palette.color(QPalette::HighlightedText) : active_theme().text_muted;

        const int available = rect.width() - 16;
        if (available <= 0)
        {
            painter->restore();
            return;
        }

        painter->setFont(name_font);
        painter->setPen(name_colour);
        painter->drawText(
            QRect(rect.left() + 8, rect.top() + 3, available, name_metrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            name_metrics.elidedText(index.data(ScriptCompletionModel::NameRole).toString(), Qt::ElideRight, available));

        QString       detail  = index.data(ScriptCompletionModel::SignatureRole).toString();
        const QString summary = index.data(ScriptCompletionModel::SummaryRole).toString();
        if (!summary.isEmpty())
        {
            detail = detail.isEmpty() ? summary : detail + QStringLiteral("  —  ") + summary;
        }

        painter->setFont(option.font);
        painter->setPen(detail_colour);
        painter->drawText(QRect(rect.left() + 8, rect.top() + 3 + name_metrics.height(), available, metrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(detail, Qt::ElideRight, available));

        painter->restore();
    }

    ScriptCompletionPopup::ScriptCompletionPopup(ScriptEditor* editor)
        : QWidget(editor, Qt::ToolTip | Qt::FramelessWindowHint), editor_(editor),
          model_(new ScriptCompletionModel(this)), view_(new QListView(this))
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);

        view_->setModel(model_);
        view_->setItemDelegate(new ScriptCompletionDelegate(view_));
        view_->setFocusPolicy(Qt::NoFocus);
        view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view_->setSelectionMode(QAbstractItemView::SingleSelection);
        view_->setUniformItemSizes(true);
        view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view_->setFrameShape(QFrame::NoFrame);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(view_);

        apply_theme();
    }

    void ScriptCompletionPopup::apply_theme()
    {
        const QPalette palette = make_palette(active_theme());
        setPalette(palette);
        view_->setPalette(palette);
        view_->viewport()->setPalette(palette);
        update();
    }

    void ScriptCompletionPopup::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);

        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        {
            apply_theme();
        }
    }

    void ScriptCompletionPopup::show_at(const QRect& caret_rect, const QString& receiver, const QString& prefix)
    {
        model_->set_context(receiver, prefix, editor_->toPlainText());
        if (model_->rowCount() == 0)
        {
            hide_popup();
            return;
        }

        view_->setCurrentIndex(model_->index(0, 0));
        view_->scrollToTop();

        const QFontMetrics metrics(view_->font());
        int                width = 220;
        for (int row = 0; row < model_->rowCount(); ++row)
        {
            const QModelIndex index = model_->index(row, 0);
            width =
                std::max(width, metrics.horizontalAdvance(index.data(ScriptCompletionModel::NameRole).toString()) + 48);
            width = std::max(
                width, metrics.horizontalAdvance(index.data(ScriptCompletionModel::SignatureRole).toString()) + 48);
        }
        width = std::min(width, 520);

        const int row_height   = metrics.height() * 2 + 6;
        const int visible_rows = std::min(model_->rowCount(), kMaxRows);
        setFixedSize(width + 2, visible_rows * row_height + 2);

        const QPoint caret_top       = editor_->viewport()->mapToGlobal(caret_rect.topLeft());
        const QPoint caret_bottom    = editor_->viewport()->mapToGlobal(caret_rect.bottomLeft());
        const QPoint viewport_bottom = editor_->viewport()->mapToGlobal(QPoint(0, editor_->viewport()->height()));

        int y = caret_bottom.y() + 2;
        if (y + height() > viewport_bottom.y())
        {
            y = caret_top.y() - height() - 2; // flip above the caret line
        }
        move(QPoint(caret_bottom.x(), y));

        show();
        raise();
    }

    void ScriptCompletionPopup::hide_popup()
    {
        hide();
    }

    bool ScriptCompletionPopup::is_open() const
    {
        return isVisible();
    }

    std::optional<QString> ScriptCompletionPopup::selected_name() const
    {
        const QModelIndex index = view_->currentIndex();
        if (!index.isValid())
        {
            return std::nullopt;
        }
        return model_->name_at(index.row());
    }

    bool ScriptCompletionPopup::selected_is_function() const
    {
        const QModelIndex index = view_->currentIndex();
        if (!index.isValid())
        {
            return false;
        }
        return model_->function_at(index.row());
    }

    bool ScriptCompletionPopup::move_selection(int delta)
    {
        if (model_->rowCount() == 0)
        {
            return false;
        }
        const QModelIndex current = view_->currentIndex();
        const int         row     = current.isValid() ? current.row() : 0;
        const int         next    = std::clamp(row + delta, 0, model_->rowCount() - 1);
        if (next == row)
        {
            return false;
        }
        view_->setCurrentIndex(model_->index(next, 0));
        view_->scrollTo(model_->index(next, 0));
        return true;
    }

    ScriptCallTip::ScriptCallTip(ScriptEditor* editor)
        : QWidget(editor, Qt::ToolTip | Qt::FramelessWindowHint), editor_(editor)
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        apply_theme();
    }

    void ScriptCallTip::apply_theme()
    {
        setPalette(make_palette(active_theme()));
        update();
    }

    void ScriptCallTip::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);

        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        {
            apply_theme();
        }
    }

    void ScriptCallTip::show_at(const QRect&   caret_rect,
                                const QString& signature,
                                std::size_t    active_parameter,
                                const QString& summary)
    {
        signature_        = signature;
        summary_          = summary;
        active_parameter_ = active_parameter;

        const QFontMetrics metrics(font());
        const int          width  = metrics.horizontalAdvance(signature) + 16;
        const int          height = (summary.isEmpty() ? 1 : 2) * metrics.height() + 8;
        setFixedSize(width, height);

        const QPoint caret_top       = editor_->viewport()->mapToGlobal(caret_rect.topLeft());
        const QPoint caret_bottom    = editor_->viewport()->mapToGlobal(caret_rect.bottomLeft());
        const QPoint viewport_bottom = editor_->viewport()->mapToGlobal(QPoint(0, editor_->viewport()->height()));

        int y = caret_bottom.y() + 2;
        if (y + height > viewport_bottom.y())
        {
            y = caret_top.y() - height - 2; // flip above the caret line
        }
        move(QPoint(caret_top.x(), y));

        show();
        raise();
    }

    void ScriptCallTip::hide_tip()
    {
        hide();
    }

    bool ScriptCallTip::is_visible() const
    {
        return isVisible();
    }

    QString ScriptCallTip::signature() const
    {
        return signature_;
    }

    int ScriptCallTip::active_parameter() const
    {
        return static_cast<int>(active_parameter_);
    }

    void ScriptCallTip::paintEvent(QPaintEvent* event)
    {
        Q_UNUSED(event);

        QPainter     painter(this);
        const Theme& theme = active_theme();
        painter.fillRect(rect(), palette().color(QPalette::Base));

        const QFontMetrics metrics(font());
        QFont              bold = font();
        bold.setBold(true);

        const int                        baseline = metrics.ascent() + 4;
        const std::vector<ParameterSpan> spans    = parameter_spans(signature_);

        if (active_parameter_ < spans.size())
        {
            const ParameterSpan& span   = spans[active_parameter_];
            const QString        before = signature_.left(span.begin);
            const QString        active = signature_.mid(span.begin, span.end - span.begin);
            const QString        after  = signature_.mid(span.end);

            int x = 8;
            painter.setFont(font());
            painter.setPen(theme.text_muted);
            painter.drawText(QPoint(x, baseline), before);
            x += metrics.horizontalAdvance(before);

            const QFontMetrics bold_metrics(bold);
            painter.setFont(bold);
            painter.setPen(theme.accent);
            painter.drawText(QPoint(x, bold_metrics.ascent() + 4), active);
            x += bold_metrics.horizontalAdvance(active);

            painter.setFont(font());
            painter.setPen(theme.text_muted);
            painter.drawText(QPoint(x, baseline), after);
        }
        else
        {
            painter.setFont(font());
            painter.setPen(theme.text_muted);
            painter.drawText(QPoint(8, baseline), signature_);
        }

        if (!summary_.isEmpty())
        {
            painter.setFont(font());
            painter.setPen(theme.text_muted);
            painter.drawText(QPoint(8, metrics.height() + baseline), summary_);
        }
    }
} // namespace slopkit::ui::components
