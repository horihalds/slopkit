#pragma once

#include <QPlainTextEdit>
#include <QSize>
#include <QWidget>

class QKeyEvent;
class QPaintEvent;
class QResizeEvent;

namespace slopkit::ui::components
{

    class ScriptEditor;
    class ScriptHighlighter;

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

    private:
        void update_gutter_width();
        void update_bands();
        void indent_lines(int direction);
        void insert_newline();

        ScriptEditorGutter* gutter_ {};
        ScriptHighlighter*  highlighter_ {};
        int                 error_line_ {0};
        int                 gutter_width_ {0};
    };

} // namespace slopkit::ui::components
