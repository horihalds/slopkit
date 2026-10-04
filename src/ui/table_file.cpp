#include "ui/table_file.hpp"

#include <QFileInfo>

namespace slopkit::ui
{

    QString table_file_path(const QString& typed)
    {
        if (typed.isEmpty())
        {
            return default_table_path();
        }

        // Only a name without any suffix at all gets the extension; a leading
        // dot (a dotfile) does not count as a suffix.
        const QString name = QFileInfo(typed).fileName();
        if (name.lastIndexOf(QLatin1Char('.')) > 0)
        {
            return typed;
        }
        return typed + QStringLiteral(".skt");
    }

    QString default_table_path()
    {
        return QStringLiteral("untitled.skt");
    }

} // namespace slopkit::ui
