#pragma once

#include <optional>

#include <QPlainTextEdit>
#include <QRect>
#include <QSize>
#include <QWidget>

class QKeyEvent;
class QPaintEvent;
class QResizeEvent;

namespace slopkit::ui::components
{

    class ScriptEditor;
    class ScriptHighlighter;
    class ScriptCompletionPopup;
    class ScriptCallTip;

    // The line-number gutter of a ScriptEditor: it paints the numbers and the
    // error marker and owns no state of its own.
    class ScriptEditorGutter : public QWidget
    {
        Q_OBJECT

    public:
        explicit ScriptEditorGutter(ScriptEditor* editor);

        [[nodiscard]] QSize sizeHint() const override;

    protected:
        void paintEvent(QPaintEvent* event) override;

    private:
        ScriptEditor* editor_ {};
    };

    // A Lua editing surface: the mono font, four-character tab stops,
    // auto-indent, a line-number gutter, the caret's current line and a marked
    // error line, and a matched-bracket highlight. Every colour comes from the
    // active theme, and the source is highlighted by a ScriptHighlighter.
    class ScriptEditor : public QPlainTextEdit
    {
        Q_OBJECT

    public:
        explicit ScriptEditor(QWidget* parent = nullptr);

        // The editor opens wide enough for a hook script's longest line, capped
        // at kMaxColumns, and 18 lines tall. minimumSizeHint() is fixed at
        // kMinColumns x kMinLines and never depends on the text, so typing does
        // not resize a window that is already shown.
        [[nodiscard]] QSize sizeHint() const override;
        [[nodiscard]] QSize minimumSizeHint() const override;

        // Marks `line` (1-based) in the gutter and with a band; 0 clears the
        // mark and a line outside the document is ignored.
        void              set_error_line(int line);
        [[nodiscard]] int error_line() const noexcept;

        // The gutter's current width, in logical pixels; the gutter calls
        // paint_gutter() from its own paint event.
        [[nodiscard]] int gutter_width() const noexcept;
        void              paint_gutter(QPaintEvent* event);

    protected:
        void resizeEvent(QResizeEvent* event) override;
        void changeEvent(QEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        void focusOutEvent(QFocusEvent* event) override;
        void hideEvent(QHideEvent* event) override;

    private:
        void update_gutter_width();
        void update_bands();
        void indent_lines(int direction);
        void insert_newline();

        // Completion: opens the list at the caret (for a typed prefix or on
        // Ctrl+Space), refreshes it while it is open and inserts the accepted
        // name inside one edit block so a single undo reverts it.
        void                update_completion(bool allow_open);
        void                open_completion();
        void                accept_completion();
        [[nodiscard]] QRect caret_rect() const;

        // The argument hint: shows the signature of the call the caret sits
        // inside, or hides it when there is none or the completion list is open.
        void refresh_hints();

        // The document's longest line in column units, with a tab advancing to
        // the next four-column stop; cached and invalidated on any text change.
        [[nodiscard]] int   longest_line_columns() const;
        [[nodiscard]] int   column_width() const;
        [[nodiscard]] int   line_height() const;
        [[nodiscard]] int   content_margin() const;
        [[nodiscard]] QSize size_for_lines(int columns, int lines) const;

        ScriptEditorGutter*        gutter_ {};
        ScriptHighlighter*         highlighter_ {};
        ScriptCompletionPopup*     completion_ {};
        ScriptCallTip*             tip_ {};
        int                        error_line_ {0};
        int                        gutter_width_ {0};
        mutable std::optional<int> longest_columns_ {};
    };

} // namespace slopkit::ui::components
