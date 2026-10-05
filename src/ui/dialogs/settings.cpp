#include "ui/dialogs/settings.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#include <QButtonGroup>
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "core/log.hpp"
#include "core/log_categories.hpp"
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

    SettingsDialog::SettingsDialog(plugin::PluginHost& host,
                                   scan::ScanEngine&   engine,
                                   SettingsController& settings,
                                   QWidget*            parent)
        : QDialog(parent), host_(host), engine_(engine), settings_(settings)
    {
        setWindowTitle(tr("Settings"));
        resize(760, 520);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto* body = new QHBoxLayout();

        categories_ = new QListWidget(this);
        categories_->addItem(tr("Appearance"));
        categories_->addItem(tr("Addresses"));
        categories_->addItem(tr("Live update"));
        categories_->addItem(tr("Tables"));
        categories_->addItem(tr("Scanning"));
        categories_->addItem(tr("Plugins"));
        categories_->addItem(tr("About"));
        categories_->setMaximumWidth(180);
        body->addWidget(categories_);

        pages_ = new QStackedWidget(this);
        pages_->addWidget(build_appearance_page());
        pages_->addWidget(build_addresses_page());
        pages_->addWidget(build_live_update_page());
        pages_->addWidget(build_tables_page());
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

        // The dialog is a view: it shows the persisted values and follows the
        // controller, so an external change keeps the radios in step.
        set_dark_theme(settings_.values().dark_theme);
        set_address_mode(settings_.values().address_mode);
        set_auto_load_last_table(settings_.values().auto_load_last_table);
        set_live_update_enabled(settings_.values().live_update_enabled);
        set_live_update_interval_ms(settings_.values().live_update_interval_ms);
        refresh_last_table();
        connect(&settings_, &SettingsController::darkThemeChanged, this, &SettingsDialog::set_dark_theme);
        connect(&settings_, &SettingsController::addressModeChanged, this, &SettingsDialog::set_address_mode);
        connect(
            &settings_, &SettingsController::autoLoadLastTableChanged, this, &SettingsDialog::set_auto_load_last_table);
        connect(&settings_, &SettingsController::liveUpdateChanged, this, &SettingsDialog::set_live_update_enabled);
        connect(&settings_,
                &SettingsController::liveUpdateIntervalChanged,
                this,
                &SettingsDialog::set_live_update_interval_ms);
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

        // idClicked fires only for user input, so the programmatic radio sync
        // cannot loop back into the controller.
        connect(theme_group,
                &QButtonGroup::idClicked,
                this,
                [this](int id)
                {
                    log::debug(log::category::ui,
                               id == 0 ? "settings: dark theme applied" : "settings: light theme applied");
                    settings_.set_dark_theme(id == 0);
                });

        auto* note = new QLabel(tr("The theme applies live and is remembered between runs."), page);
        note->setWordWrap(true);
        layout->addWidget(note);
        layout->addStretch(1);
        return page;
    }

    QWidget* SettingsDialog::build_addresses_page()
    {
        auto* page   = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Addresses"), page));

        layout->addWidget(new QLabel(tr("Static addresses"), page));

        auto* mode_group        = new QButtonGroup(page);
        auto* buttons_row       = new QHBoxLayout();
        module_relative_button_ = new QRadioButton(tr("Module + RVA"), page);
        absolute_button_        = new QRadioButton(tr("Absolute address"), page);
        module_relative_button_->setChecked(true);
        module_relative_button_->setToolTip(tr("libc.so.6+1A2B"));
        absolute_button_->setToolTip(tr("0x7F3A1B2C"));
        mode_group->addButton(module_relative_button_, 0);
        mode_group->addButton(absolute_button_, 1);
        buttons_row->addWidget(module_relative_button_);
        buttons_row->addWidget(absolute_button_);
        buttons_row->addStretch(1);
        layout->addLayout(buttons_row);

        // idClicked fires only for user input, so the programmatic radio sync
        // cannot loop back into the controller.
        connect(mode_group,
                &QButtonGroup::idClicked,
                this,
                [this](int id)
                {
                    log::debug(log::category::ui,
                               id == 0 ? "settings: module-relative addresses applied"
                                       : "settings: absolute addresses applied");
                    settings_.set_address_mode(id == 0 ? ui::AddressMode::module_relative : ui::AddressMode::absolute);
                });

        auto* note = new QLabel(tr("Applies live to the found results, the address list and the Memory Viewer; "
                                   "the choice is remembered between runs."),
                                page);
        note->setWordWrap(true);
        layout->addWidget(note);
        layout->addStretch(1);
        return page;
    }

    QWidget* SettingsDialog::build_live_update_page()
    {
        auto* page   = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Live update"), page));

        live_update_check_ = new QCheckBox(tr("Keep values in sync with live memory"), page);
        live_update_check_->setObjectName(QStringLiteral("live_update_enabled"));
        layout->addWidget(live_update_check_);

        auto* form            = new QFormLayout();
        live_update_interval_ = new QSpinBox(page);
        live_update_interval_->setObjectName(QStringLiteral("live_update_interval"));
        live_update_interval_->setRange(50, 5000);
        live_update_interval_->setSingleStep(50);
        live_update_interval_->setSuffix(tr(" ms"));
        form->addRow(tr("Refresh interval"), live_update_interval_);
        layout->addLayout(form);

        layout->addWidget(widgets::hint_text(tr("Refreshes the address list, the results list and the Memory Viewer "
                                                "while a target is attached; the choice is remembered between runs."),
                                             page));
        layout->addStretch(1);

        // clicked fires only for user input, so the programmatic sync cannot loop
        // back into the controller.
        connect(live_update_check_,
                &QCheckBox::clicked,
                this,
                [this](bool checked)
                {
                    live_update_interval_->setEnabled(checked);
                    log::debug(log::category::ui,
                               checked ? "settings: live update enabled" : "settings: live update disabled");
                    settings_.set_live_update_enabled(checked);
                });

        connect(live_update_interval_,
                &QSpinBox::valueChanged,
                this,
                [this](int interval_ms)
                {
                    settings_.set_live_update_interval_ms(interval_ms);
                });

        return page;
    }

    QWidget* SettingsDialog::build_tables_page()
    {
        auto* page   = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        layout->addWidget(widgets::section_header(tr("Tables"), page));

        auto_load_check_ = new QCheckBox(tr("Load the last table when slopkit starts"), page);
        auto_load_check_->setObjectName(QStringLiteral("auto_load_last_table"));
        layout->addWidget(auto_load_check_);

        last_table_label_ = new QLabel(page);
        last_table_label_->setFont(mono_font());
        last_table_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(last_table_label_);

        auto* note = new QLabel(tr("The switch, the last table and the file dialogs' directory are remembered "
                                   "between runs; the switch applies at the next start."),
                                page);
        note->setWordWrap(true);
        layout->addWidget(note);
        layout->addStretch(1);

        // clicked fires only for user input, so the programmatic sync cannot loop
        // back into the controller.
        connect(auto_load_check_,
                &QCheckBox::clicked,
                this,
                [this](bool checked)
                {
                    log::debug(log::category::ui,
                               checked ? "settings: auto-load last table enabled"
                                       : "settings: auto-load last table disabled");
                    settings_.set_auto_load_last_table(checked);
                });

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
        refresh_last_table();
    }

    void SettingsDialog::select_about()
    {
        categories_->setCurrentRow(categories_->count() - 1);
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

    void SettingsDialog::set_address_mode(ui::AddressMode mode)
    {
        if (mode == ui::AddressMode::module_relative)
        {
            module_relative_button_->setChecked(true);
        }
        else
        {
            absolute_button_->setChecked(true);
        }
    }

    void SettingsDialog::set_auto_load_last_table(bool enabled)
    {
        auto_load_check_->setChecked(enabled);
    }

    void SettingsDialog::set_live_update_enabled(bool enabled)
    {
        live_update_check_->setChecked(enabled);
        live_update_interval_->setEnabled(enabled);
    }

    void SettingsDialog::set_live_update_interval_ms(int interval_ms)
    {
        live_update_interval_->setValue(interval_ms);
    }

    void SettingsDialog::refresh_last_table()
    {
        const QString path = settings_.values().last_table_path;
        last_table_label_->setText(path.isEmpty() ? tr("Last table: none yet") : tr("Last table: %1").arg(path));
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
