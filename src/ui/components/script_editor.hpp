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

    class ScriptEditor : public QPlainTextEdit
    {
        Q_OBJECT

    public:
        explicit ScriptEditor(QWidget* parent = nullptr);

        [[nodiscard]] QSize sizeHint() const override;
        [[nodiscard]] QSize minimumSizeHint() const override;

        void              set_error_line(int line);
        [[nodiscard]] int error_line() const noexcept;

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

        void                update_completion(bool allow_open);
        void                open_completion();
        void                accept_completion();
        [[nodiscard]] QRect caret_rect() const;

        void refresh_hints();

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

}
