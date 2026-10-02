#include "ui/fonts.hpp"

#include <cstddef>

#include <QByteArray>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStringList>

#include "slopkit_notosans_regular.h"
#include "slopkit_notosansmono_regular.h"

namespace slopkit::ui
{

    namespace
    {
        // Cached by register_embedded_fonts(); empty until then.
        QString g_ui_family;
        QString g_mono_family;

        // Registers one embedded face and returns its family name, or an empty
        // string on failure.
        QString register_face(const unsigned char* data, std::size_t size)
        {
            const QByteArray bytes(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));
            const int        font_id = QFontDatabase::addApplicationFontFromData(bytes);
            if (font_id < 0)
            {
                return {};
            }

            const QStringList families = QFontDatabase::applicationFontFamilies(font_id);
            return families.isEmpty() ? QString() : families.first();
        }

        QFont make_font(const QString& family, const QString& fallback)
        {
            QFont font(family.isEmpty() ? fallback : family);
            font.setPixelSize(kBaseFontSize);
            return font;
        }
    } // namespace

    bool register_embedded_fonts()
    {
        g_ui_family   = register_face(slopkit::generated::slopkit_notosans_regular,
                                      slopkit::generated::slopkit_notosans_regular_size);
        g_mono_family = register_face(slopkit::generated::slopkit_notosansmono_regular,
                                      slopkit::generated::slopkit_notosansmono_regular_size);
        if (g_ui_family.isEmpty() || g_mono_family.isEmpty())
        {
            return false;
        }

        QGuiApplication::setFont(ui_font());
        return true;
    }

    QFont ui_font()
    {
        return make_font(g_ui_family, QStringLiteral("Noto Sans"));
    }

    QFont mono_font()
    {
        return make_font(g_mono_family, QStringLiteral("Noto Sans Mono"));
    }

} // namespace slopkit::ui
