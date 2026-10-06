#include "ui/table_file.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

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

    QString table_name_for_target(std::string_view target_name)
    {
        const QString name =
            QString::fromUtf8(target_name.data(), static_cast<qsizetype>(target_name.size())).trimmed();

        // A process name can carry path separators or control characters; none of
        // them may reach the file system, so each is replaced by an underscore.
        QString sanitised;
        sanitised.reserve(name.size());
        for (const QChar character : name)
        {
            if (!character.isPrint() || character == QLatin1Char('/') || character == QLatin1Char('\\'))
            {
                sanitised.append(QLatin1Char('_'));
            }
            else
            {
                sanitised.append(character);
            }
        }

        if (sanitised.isEmpty() || sanitised == QStringLiteral(".") || sanitised == QStringLiteral(".."))
        {
            return default_table_path();
        }
        return sanitised + QStringLiteral(".skt");
    }

    QString default_table_directory()
    {
        QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (documents.isEmpty())
        {
            documents = QDir::homePath();
        }
        return QDir(documents).filePath(QStringLiteral("slopkit"));
    }

    bool ensure_table_directory(const QString& directory)
    {
        if (directory.isEmpty())
        {
            return false;
        }
        return QDir().mkpath(directory);
    }

} // namespace slopkit::ui
