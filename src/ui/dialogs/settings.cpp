#include "ui/dialogs/settings.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include <QButtonGroup>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "core/version.hpp"
#include "ui/components/widgets.hpp"
#include "ui/fonts.hpp"

namespace slopkit::ui::dialogs
{

    namespace
    {
        bool parse_number(std::string_view text, std::uint64_t& out)
        {
            if (text.empty())
            {
                return false;
            }
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
            return error == std::errc {} && end == text.data() + text.size();
        }

        QString to_qstring(std::string_view text)
        {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        }
    } // namespace

    SettingsDialog::SettingsDialog(plugin::PluginHost& host, scan::ScanEngine& engine, QWidget* parent)
        : QDialog(parent), host_(host), engine_(engine)
    {
        setWindowTitle(tr("Settings"));
        resize(760, 520);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto* body = new QHBoxLayout();

        categories_ = new QListWidget(this);
        categories_->addItem(tr("Appearance"));
        categories_->addItem(tr("Scanning"));
        categories_->addItem(tr("Plugins"));
        categories_->addItem(tr("About"));
        categories_->setMaximumWidth(180);
        body->addWidget(categories_);

        pages_ = new QStackedWidget(this);
        pages_->addWidget(build_appearance_page());
        pages_->addWidget(build_scanning_page());
        pages_->addWidget(build_plugins_page());
        pages_->addWidget(build_about_page());
        body->addWidget(pages_, 1);

        layout->addLayout(body, 1);

        status_ = new widgets::StatusLabel(this);
        layout->addWidget(status_);

        connect(categories_,
                &QListWidget::currentRowChanged,
                this,
                [this](int row)
                {
                    if (row >= 0)
                    {
                        pages_->setCurrentIndex(row);
                    }
                });
        categories_->setCurrentRow(0);
    }

    QWidget* SettingsDialog::build_appearance_page()
    {
        auto* page   = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Appearance"), page));

        layout->addWidget(new QLabel(tr("Theme"), page));

        auto* theme_group = new QButtonGroup(page);
        auto* buttons_row = new QHBoxLayout();
        dark_button_      = new QRadioButton(tr("Dark"), page);
        light_button_     = new QRadioButton(tr("Light"), page);
        dark_button_->setChecked(true);
        theme_group->addButton(dark_button_, 0);
        theme_group->addButton(light_button_, 1);
        buttons_row->addWidget(dark_button_);
        buttons_row->addWidget(light_button_);
        buttons_row->addStretch(1);
        layout->addLayout(buttons_row);

        // idClicked fires only for user input, so programmatic updates (and the
        // palette re-application) cannot loop back into this signal.
        connect(theme_group,
                &QButtonGroup::idClicked,
                this,
                [this](int id)
                {
                    emit darkThemeChanged(id == 0);
                });

        auto* note = new QLabel(tr("The theme applies live and is not persisted between runs."), page);
        note->setWordWrap(true);
        layout->addWidget(note);
        layout->addStretch(1);
        return page;
    }

    QWidget* SettingsDialog::build_scanning_page()
    {
        auto* page   = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Scanning"), page));

        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

        alignment_edit_ = new QLineEdit(QStringLiteral("4"), page);
        alignment_edit_->setFont(mono_font());
        alignment_edit_->setMaximumWidth(160);
        alignment_edit_->setToolTip(tr("Used when the scan controls' alignment field is left blank"));
        form->addRow(tr("Default fast-scan alignment (bytes)"), alignment_edit_);

        result_cap_edit_ = new QLineEdit(QStringLiteral("1000000"), page);
        result_cap_edit_->setFont(mono_font());
        result_cap_edit_->setMaximumWidth(200);
        form->addRow(tr("Stored result cap"), result_cap_edit_);

        layout->addLayout(form);

        auto* apply_row = new QHBoxLayout();
        auto* apply     = new widgets::PrimaryButton(tr("Apply"), page);
        apply_row->addWidget(apply);
        apply_row->addStretch(1);
        layout->addLayout(apply_row);
        connect(apply, &QPushButton::clicked, this, &SettingsDialog::apply_scanning);

        auto* note = new QLabel(tr("Defaults apply to this session; nothing is persisted between runs."), page);
        note->setWordWrap(true);
        layout->addWidget(note);
        layout->addStretch(1);
        return page;
    }

    QWidget* SettingsDialog::build_plugins_page()
    {
        auto* page   = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Plugins"), page));

        plugins_view_ = new QPlainTextEdit(page);
        plugins_view_->setReadOnly(true);
        plugins_view_->setFont(mono_font());
        layout->addWidget(plugins_view_, 1);
        return page;
    }

    QWidget* SettingsDialog::build_about_page()
    {
        auto* page   = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("About"), page));

        about_title_ = new QLabel(tr("slopkit %1").arg(to_qstring(slopkit::version())), page);
        layout->addWidget(about_title_);

        auto* description =
            new QLabel(tr("A plugin-first memory scanner and debugger front end. The host never touches "
                          "another process directly: all access is provided by plugins."),
                       page);
        description->setWordWrap(true);
        layout->addWidget(description);

        auto* note = new QLabel(tr("Theme: dark/light, applied live from the Appearance category."), page);
        note->setWordWrap(true);
        layout->addWidget(note);

        about_plugins_ = new QLabel(page);
        layout->addWidget(about_plugins_);
        layout->addStretch(1);
        return page;
    }

    void SettingsDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        refresh_plugins();
    }

    void SettingsDialog::select_about()
    {
        categories_->setCurrentRow(3);
    }

    void SettingsDialog::set_dark_theme(bool dark)
    {
        if (dark)
        {
            dark_button_->setChecked(true);
        }
        else
        {
            light_button_->setChecked(true);
        }
    }

    void SettingsDialog::apply_scanning()
    {
        std::uint64_t alignment = 0;
        if (!parse_number(alignment_edit_->text().toStdString(), alignment) || alignment == 0)
        {
            status_->set_status(widgets::StatusKind::error, tr("Alignment must be a positive number."));
            return;
        }
        std::uint64_t cap = 0;
        if (!parse_number(result_cap_edit_->text().toStdString(), cap) || cap == 0)
        {
            status_->set_status(widgets::StatusKind::error, tr("The result cap must be a positive number."));
            return;
        }

        engine_.set_max_stored_hits(static_cast<std::size_t>(cap));
        emit alignmentChanged(static_cast<quint64>(alignment));
        status_->set_status(widgets::StatusKind::info, tr("Scanning defaults applied."));
    }

    void SettingsDialog::refresh_plugins()
    {
        const auto plugins = host_.plugins();

        QString text;
        if (plugins.empty())
        {
            status_->set_status(widgets::StatusKind::warning, tr("No plugins were loaded."));
        }
        for (const auto& plugin : plugins)
        {
            text += tr("%1 v%2 (precedence %3)")
                        .arg(to_qstring(plugin->id()))
                        .arg(to_qstring(plugin->version()))
                        .arg(plugin->precedence());
            text += QLatin1Char('\n');
            text += QStringLiteral("  ") + to_qstring(plugin->name()) + QLatin1Char('\n');
            text += QStringLiteral("  ") + to_qstring(plugin->path().string()) + QLatin1Char('\n');
            if (!plugin->description().empty())
            {
                text += QStringLiteral("  ") + to_qstring(plugin->description()) + QLatin1Char('\n');
            }
            text += QLatin1Char('\n');
        }

        if (!host_.diagnostics().empty())
        {
            text += tr("Diagnostics") + QLatin1Char('\n');
            for (const auto& diagnostic : host_.diagnostics())
            {
                text += to_qstring((diagnostic.path.string() + ": " + diagnostic.message)) + QLatin1Char('\n');
            }
        }
        plugins_view_->setPlainText(text);

        if (about_plugins_ != nullptr)
        {
            about_plugins_->setText(tr("Plugins loaded: %1").arg(static_cast<int>(plugins.size())));
        }
    }

} // namespace slopkit::ui::dialogs
