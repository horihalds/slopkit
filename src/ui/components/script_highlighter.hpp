#pragma once

#include <QSyntaxHighlighter>

#include <QString>

namespace slopkit::ui::components
{
    class ScriptHighlighter : public QSyntaxHighlighter
    {
        Q_OBJECT

    public:
        explicit ScriptHighlighter(QTextDocument* document);

    protected:
        void highlightBlock(const QString& text) override;
    };

}
