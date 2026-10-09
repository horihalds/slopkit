#pragma once

#include <QSyntaxHighlighter>

#include <QString>

namespace slopkit::ui::components
{

    // Colours the Lua source of the script editor: keywords, strings (short and
    // long), comments (line and long), numbers and the script API globals. Every
    // colour is read from the active theme while highlighting, so a later
    // rehighlight() follows a theme switch. It never executes or parses Lua —
    // it is a lexer, not a compiler.
    class ScriptHighlighter : public QSyntaxHighlighter
    {
        Q_OBJECT

    public:
        explicit ScriptHighlighter(QTextDocument* document);

    protected:
        void highlightBlock(const QString& text) override;
    };

} // namespace slopkit::ui::components
