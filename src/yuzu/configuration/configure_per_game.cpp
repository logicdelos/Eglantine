// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include "common/fs/fs_util.h"
#include "common/game_presets.h"
#include "common/settings_enums.h"
#include "common/settings_input.h"
#include "configuration/shared_widget.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/xts_archive.h"
#include "core/loader/loader.h"
#include "frontend_common/config.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "qt_common/util/vk.h"
#include "ui_configure_per_game.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_applets.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/configure_frame_generation.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/configure_input_per_game.h"
#include "yuzu/configuration/configure_network.h"
#include "yuzu/configuration/configure_per_game.h"
#include "yuzu/configuration/configure_per_game_addons.h"
#include "yuzu/configuration/configure_per_game_tweaks.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/util/util.h"

ConfigurePerGame::ConfigurePerGame(QWidget* parent, u64 title_id_, const std::string& file_name,
                                   std::vector<VkDeviceInfo::Record>& vk_device_records_,
                                   Core::System& system_)
    : QDialog(parent), ui(std::make_unique<Ui::ConfigurePerGame>()),
      file_path(std::filesystem::path(Common::FS::ToU8String(file_name))), title_id{title_id_},
      system{system_},
      builder{std::make_unique<ConfigurationShared::Builder>(this, !system_.IsPoweredOn())},
      tab_group{std::make_shared<std::vector<ConfigurationShared::Tab*>>()},
      vk_device_records{vk_device_records_} {
    const auto config_file_name = title_id == 0 ? Common::FS::PathToUTF8String(file_path.filename())
                                                : fmt::format("{:016X}", title_id);
    game_config = std::make_unique<QtConfig>(config_file_name, Config::ConfigType::PerGameConfig);
    addons_tab = std::make_unique<ConfigurePerGameAddons>(system_, this);
    input_tab = std::make_unique<ConfigureInputPerGame>(system_, game_config.get(), this);
    network_tab = std::make_unique<ConfigureNetwork>(system_, this);
    BuildSettingTabs();
    if (ConfigurePerGameTweaks::HasContent(title_id)) {
        tweaks_tab = std::make_unique<ConfigurePerGameTweaks>(title_id, system_, *builder, this);
        connect(tweaks_tab.get(), &ConfigurePerGameTweaks::ApplyRecommendedRequested, this,
                &ConfigurePerGame::ApplyRecommendedSettings);
    }

    ui->setupUi(this);

    // The info column scrolls on its own on short screens instead of growing the dialog.
    auto* const info_area = new QScrollArea(this);
    ui->horizontalLayout->replaceWidget(ui->gameInfo, info_area);
    info_area->setFrameShape(QFrame::NoFrame);
    info_area->setWidgetResizable(true);
    info_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    info_area->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    info_area->setWidget(ui->gameInfo);

    AddTabs();

    if (tweaks_tab != nullptr) {
        connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int index) {
            if (index >= 0 && tweaks_tab != nullptr && !system.IsPoweredOn()) {
                QWidget* page = ui->tabWidget->widget(index);
                if (auto* area = qobject_cast<QScrollArea*>(page)) {
                    page = area->widget();
                }
                if (page == tweaks_tab.get()) {
                    ApplySettingTabs();
                    tweaks_tab->RefreshRecommendations();
                }
            }
        });
    }

    setFocusPolicy(Qt::ClickFocus);
    setWindowTitle(tr("Properties"));

    addons_tab->SetTitleId(title_id);

    scene = new QGraphicsScene(this);
    ui->icon_view->setScene(scene);
    ui->button_detect_icon->setIcon(
        QIcon::fromTheme(QStringLiteral("edit-find"), QIcon::fromTheme(QStringLiteral("search"))));
    ui->button_upload_icon->setIcon(QIcon::fromTheme(
        QStringLiteral("document-open"), QIcon::fromTheme(QStringLiteral("insert-image"))));
    ui->button_remove_icon->setIcon(QIcon::fromTheme(QStringLiteral("edit-clear")));
    ui->button_remove_icon->setEnabled(false);
    connect(ui->button_upload_icon, &QPushButton::clicked, this, &ConfigurePerGame::OnUploadIcon);
    connect(ui->button_detect_icon, &QPushButton::clicked, this, &ConfigurePerGame::OnDetectIcon);
    connect(ui->button_remove_icon, &QPushButton::clicked, this,
            &ConfigurePerGame::OnRemoveCustomIcon);
    connect(&detect_watcher, &QFutureWatcher<IconDetectResult>::finished, this, [this] {
        // A detection the dialog cancelled (closing) shows nothing.
        if (!detect_cancel || detect_cancel->load()) {
            return;
        }
        ui->button_detect_icon->setEnabled(true);
        ui->button_detect_icon->setText(tr("Detect Icon"));
        ui->button_upload_icon->setEnabled(true);
        ui->display_name->setEnabled(true);
        ui->allow_online_detection->setEnabled(true);
        ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
        if (auto* apply = ui->buttonBox->button(QDialogButtonBox::Apply)) {
            apply->setEnabled(true);
        }
        UpdateRemoveIconButton();
        const IconDetectResult out = detect_watcher.result();
        QString message;
        switch (out.status) {
        case IconDetectStatus::Found:
            break;
        case IconDetectStatus::NoLocalMatch:
            message = tr("No matching local icon was found. Select Allow online lookup to search "
                         "online, or upload an image.");
            break;
        case IconDetectStatus::TimedOut:
            message = tr("Icon detection timed out.");
            break;
        case IconDetectStatus::NotFound:
            message = tr("Could not find a matching icon for \"%1\".")
                          .arg(QString::fromStdString(out.search_name));
            break;
        case IconDetectStatus::Failed:
            message = tr("Icon detection failed. Try uploading an image instead.");
            break;
        case IconDetectStatus::Cancelled:
            return;
        }
        if (out.status != IconDetectStatus::Found || out.icon_bytes.empty()) {
            if (!message.isEmpty()) {
                QMessageBox::information(this, tr("Detect Icon"), message);
            }
            return;
        }
        if (!out.resolved_title.empty()) {
            ui->display_name->setText(QString::fromStdString(out.resolved_title));
        }
        QPixmap map;
        if (map.loadFromData(out.icon_bytes.data(), static_cast<u32>(out.icon_bytes.size()))) {
            UpdateIconPreview(map);
        }
        pending_icon = out.icon_bytes;
        icon_modified = true;
        remove_custom_icon = false;
        UpdateRemoveIconButton();
    });

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigurePerGame::HandleApplyButtonClicked);
    }

    LoadConfiguration();
    info_area->setMinimumWidth(ui->gameInfo->sizeHint().width() +
                               info_area->verticalScrollBar()->sizeHint().width());
    ConfigurationShared::FitToScreen(this, QSize(1040, 760));
}

ConfigurePerGame::~ConfigurePerGame() {
    // Deleting the tab pages makes the tab widget switch tabs; don't run the handler on a
    // half-destroyed dialog.
    ui->tabWidget->blockSignals(true);
    if (detect_cancel) {
        detect_cancel->store(true);
    }
}

void ConfigurePerGame::done(int result) {
    // OK saves the picked artwork and name first; a failed save keeps the dialog open.
    if (result == QDialog::Accepted && (detect_watcher.isRunning() || !SaveCustomMetadata(true))) {
        return;
    }
    if (detect_cancel) {
        detect_cancel->store(true);
    }
    QDialog::done(result);
}

void ConfigurePerGame::BuildSettingTabs() {
    tab_group->clear();
    audio_tab = std::make_unique<ConfigureAudio>(system, tab_group, *builder, this);
    cpu_tab = std::make_unique<ConfigureCpu>(system, tab_group, *builder, this);
    graphics_advanced_tab =
        std::make_unique<ConfigureGraphicsAdvanced>(system, tab_group, *builder, this);
    graphics_extensions_tab =
        std::make_unique<ConfigureGraphicsExtensions>(system, tab_group, *builder, this);
    graphics_tab = std::make_unique<ConfigureGraphics>(
        system, vk_device_records, [&]() { graphics_advanced_tab->ExposeComputeOption(); },
        [](Settings::AspectRatio, Settings::ResolutionSetup) {}, tab_group, *builder, this);
    frame_gen_tab = std::make_unique<ConfigureFrameGeneration>(system, tab_group, *builder, this);
    system_tab = std::make_unique<ConfigureSystem>(system, tab_group, *builder, this);
    applets_tab = std::make_unique<ConfigureApplets>(system, tab_group, *builder, this);
}

void ConfigurePerGame::AddTabs() {
    ui->tabWidget->clear();
    const std::array<std::tuple<QWidget*, const char*, QString>, 12> tabs{{
        {addons_tab.get(), "puzzle", tr("Add-Ons")},
        {tweaks_tab.get(), "wand", tr("Tweaks")},
        {system_tab.get(), "system", tr("System")},
        {cpu_tab.get(), "cpu", tr("CPU")},
        {graphics_tab.get(), "monitor", tr("Graphics")},
        {graphics_advanced_tab.get(), "layers", tr("Adv. Graphics")},
        {graphics_extensions_tab.get(), "wand", tr("Ext. Graphics")},
        {frame_gen_tab.get(), "frame_generation", tr("Frame Gen.")},
        {audio_tab.get(), "audio", tr("Audio")},
        {input_tab.get(), "gamepad", tr("Input Profiles")},
        {network_tab.get(), "globe", tr("Network")},
        {applets_tab.get(), "apps", tr("Applets")},
    }};
    for (const auto& [tab, icon, name] : tabs) {
        if (tab == nullptr) {
            continue;
        }
        if (tab != addons_tab.get() && tab != input_tab.get()) {
            ConfigurationShared::UseToggles(tab);
        }
        ui->tabWidget->addTab(ConfigurationShared::ScrollTab(tab),
                              ConfigurationShared::PageIcon(QString::fromLatin1(icon)), name);
    }
}

void ConfigurePerGame::ApplySettingTabs() {
    for (const auto tab : *tab_group) {
        tab->ApplyConfiguration();
    }
}

void ConfigurePerGame::ApplyRecommendedSettings(const std::set<std::string>& keys) {
    const auto* preset = Settings::FindGamePreset(title_id);
    if (preset == nullptr || keys.empty() || system.IsPoweredOn()) {
        return;
    }
    // Keep the edits made in the other tabs, then rebuild them to show the new values.
    ApplySettingTabs();
    Settings::SetRecommendedSettings(Settings::values, *preset, false, &keys);

    const int current_index = ui->tabWidget->currentIndex();
    {
        [[maybe_unused]] const QSignalBlocker blocker(ui->tabWidget);
        ui->tabWidget->clear();
        BuildSettingTabs();
        AddTabs();
        if (current_index >= 0 && current_index < ui->tabWidget->count()) {
            ui->tabWidget->setCurrentIndex(current_index);
        }
    }
    tweaks_tab->RefreshRecommendations();
}

void ConfigurePerGame::ApplyConfiguration() {
    ApplySettingTabs();
    if (tweaks_tab != nullptr) {
        tweaks_tab->ApplyConfiguration();
    }
    addons_tab->ApplyConfiguration();
    input_tab->ApplyConfiguration();
    network_tab->ApplyConfiguration();
    applets_tab->ApplyConfiguration();

    if (Settings::IsDockedMode() && Settings::values.players.GetValue()[0].controller_type ==
                                        Settings::ControllerType::Handheld) {
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
        Settings::values.use_docked_mode.SetGlobal(true);
    }

    system.ApplySettings();
    Settings::LogSettings();

    game_config->SaveAllValues();
}

void ConfigurePerGame::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigurePerGame::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigurePerGame::HandleApplyButtonClicked() {
    if (detect_watcher.isRunning() || !SaveCustomMetadata(true)) {
        return;
    }
    UISettings::values.configuration_applied = true;
    ApplyConfiguration();
}

void ConfigurePerGame::LoadFromFile(FileSys::VirtualFile file_) {
    file = std::move(file_);
    LoadConfiguration();
}

void ConfigurePerGame::LoadConfiguration() {
    if (file == nullptr) {
        return;
    }

    addons_tab->LoadFromFile(file);

    ui->display_title_id->setText(
        QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char{'0'}).toUpper());

    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto control = pm.GetControlMetadata();
    const auto loader = Loader::GetLoader(system, file);
    // A NUP carries its own edited name and icon, so its loader is read before the base game's.
    const bool is_nup_file = loader != nullptr && loader->GetFileType() == Loader::FileType::NUP;

    // Custom artwork and name are stored per program ID outside the game file, so they work for
    // every format and while a game runs.
    ui->custom_icon_widget->setVisible(title_id != 0);
    ui->display_name->setReadOnly(title_id == 0);
    ui->display_name->setClearButtonEnabled(title_id != 0);
    ui->display_name->setToolTip(
        title_id != 0 ? tr("Name shown in the library. Clear it to use the game's own name.")
                      : QString{});

    if (!is_nup_file && control.first != nullptr) {
        ui->display_version->setText(QString::fromStdString(control.first->GetVersionString()));
        ui->display_name->setText(QString::fromStdString(control.first->GetApplicationName()));
        ui->display_developer->setText(QString::fromStdString(control.first->GetDeveloperName()));
    } else if (loader != nullptr) {
        std::string title;
        if (loader->ReadTitle(title) == Loader::ResultStatus::Success) {
            ui->display_name->setText(QString::fromStdString(title));
        } else if (is_nup_file) {
            ui->display_name->setText(
                QFileInfo(QString::fromStdString(file->GetName())).completeBaseName());
        }

        FileSys::NACP nacp;
        if (loader->ReadControlData(nacp) == Loader::ResultStatus::Success) {
            ui->display_developer->setText(QString::fromStdString(nacp.GetDeveloperName()));
            const auto version = nacp.GetVersionString();
            ui->display_version->setText(version.empty() ? QStringLiteral("1.0.0")
                                                         : QString::fromStdString(version));
        } else {
            ui->display_version->setText(QStringLiteral("1.0.0"));
        }
    }
    original_title = ui->display_name->text().simplified().toStdString();
    ui->display_name->setPlaceholderText(QString::fromStdString(original_title));
    if (const auto custom = QtCommon::Game::ReadCustomTitle(title_id); !custom.empty()) {
        ui->display_name->setText(QString::fromStdString(custom));
    }
    initial_title = ui->display_name->text().simplified().toStdString();
    icon_modified = false;
    pending_icon.clear();
    remove_custom_icon = false;

    file_icon = QPixmap{};
    if (!is_nup_file && control.second != nullptr) {
        const auto bytes = control.second->ReadAllBytes();
        file_icon.loadFromData(bytes.data(), static_cast<u32>(bytes.size()));
    } else if (loader != nullptr) {
        std::vector<u8> bytes;
        if (loader->ReadIcon(bytes) == Loader::ResultStatus::Success) {
            file_icon.loadFromData(bytes.data(), static_cast<u32>(bytes.size()));
        }
    }
    QPixmap custom_icon;
    if (const auto custom = QtCommon::Game::ReadCustomArtwork(title_id); !custom.empty()) {
        custom_icon.loadFromData(custom.data(), static_cast<u32>(custom.size()));
    }
    has_custom_icon = !custom_icon.isNull();
    UpdateIconPreview(has_custom_icon ? custom_icon : file_icon);
    UpdateRemoveIconButton();

    ui->display_filename->setText(QString::fromStdString(file->GetName()));

    if (loader != nullptr) {
        ui->display_format->setText(
            QString::fromStdString(Loader::GetFileTypeString(loader->GetFileType())));
    }

    const auto valueText = QtCommon::ReadableByteSize(file->GetSize());
    ui->display_size->setText(valueText);
}

void ConfigurePerGame::UpdateIconPreview(const QPixmap& map) {
    scene->clear();
    if (map.isNull()) {
        return;
    }
    scene->addPixmap(map.scaled(ui->icon_view->width(), ui->icon_view->height(),
                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

void ConfigurePerGame::OnUploadIcon() {
    if (title_id == 0 || detect_watcher.isRunning()) {
        return;
    }
    const QString default_dir =
        file_path.has_parent_path()
            ? QString::fromStdString(Common::FS::PathToUTF8String(file_path.parent_path()))
            : QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString selected = QtCommon::Frontend::GetOpenFileName(
        tr("Select Game Icon"), default_dir,
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*)"));
    if (selected.isEmpty()) {
        return;
    }
    std::vector<u8> icon = ReadSquareIconFile(selected);
    if (icon.empty()) {
        QMessageBox::warning(this, tr("Invalid Image"),
                             tr("Select a PNG, JPEG, BMP or WebP image up to 8 MiB, "
                                "8192 pixels per side and 16 megapixels."));
        return;
    }
    pending_icon = std::move(icon);
    QPixmap map;
    if (map.loadFromData(pending_icon.data(), static_cast<u32>(pending_icon.size()))) {
        UpdateIconPreview(map);
    }
    icon_modified = true;
    remove_custom_icon = false;
    UpdateRemoveIconButton();
}

void ConfigurePerGame::OnRemoveCustomIcon() {
    if (title_id == 0 || detect_watcher.isRunning()) {
        return;
    }
    // Drops a picked image that is not saved yet, and the saved artwork on Apply or OK.
    pending_icon.clear();
    icon_modified = false;
    remove_custom_icon = has_custom_icon;
    UpdateIconPreview(file_icon);
    UpdateRemoveIconButton();
}

void ConfigurePerGame::UpdateRemoveIconButton() {
    ui->button_remove_icon->setEnabled(icon_modified || (has_custom_icon && !remove_custom_icon));
}

void ConfigurePerGame::OnDetectIcon() {
    if (title_id == 0 || detect_watcher.isRunning()) {
        return;
    }
    QString query = ui->display_name->text().trimmed();
    if (query.isEmpty()) {
        query = QFileInfo(QString::fromStdString(Common::FS::PathToUTF8String(file_path)))
                    .completeBaseName();
    }
    if (query.size() > 1024) {
        QMessageBox::information(this, tr("Detect Icon"),
                                 tr("The game name is too long to search."));
        return;
    }

    ui->button_detect_icon->setEnabled(false);
    ui->button_detect_icon->setText(tr("Detecting..."));
    ui->button_upload_icon->setEnabled(false);
    ui->button_remove_icon->setEnabled(false);
    ui->display_name->setEnabled(false);
    ui->allow_online_detection->setEnabled(false);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    if (auto* apply = ui->buttonBox->button(QDialogButtonBox::Apply)) {
        apply->setEnabled(false);
    }

    detect_cancel = std::make_shared<std::atomic_bool>(false);
    // Nothing leaves this machine unless the user ticked Allow online lookup; SteamGridDB is asked
    // only with the user's own key.
    const bool allow_online = ui->allow_online_detection->isChecked();
    detect_watcher.setFuture(QtConcurrent::run(
        [name = query.toStdString(), tid = title_id, path = file_path, allow_online,
         key = UISettings::values.steamgriddb_api_key.GetValue(), cancel = detect_cancel] {
            return DetectGameIcon(name, tid, path, allow_online, key, *cancel);
        }));
}

bool ConfigurePerGame::SaveCustomMetadata(bool show_error) {
    if (remove_custom_icon) {
        if (!QtCommon::Game::RemoveCustomArtwork(title_id)) {
            if (show_error) {
                QMessageBox::warning(this, tr("Game Artwork"),
                                     tr("The artwork could not be removed."));
            }
            return false;
        }
        remove_custom_icon = false;
        has_custom_icon = false;
        UpdateRemoveIconButton();
        UISettings::values.is_game_list_reload_pending.exchange(true);
        emit CustomArtworkChanged(title_id);
    }
    if (icon_modified) {
        // Saved as custom artwork next to the user data: the game file is never rewritten.
        QImage image;
        image.loadFromData(pending_icon.data(), static_cast<u32>(pending_icon.size()));
        if (!QtCommon::Game::SaveCustomArtwork(title_id, image)) {
            if (show_error) {
                QMessageBox::warning(this, tr("Game Artwork"),
                                     tr("The artwork could not be saved."));
            }
            return false;
        }
        icon_modified = false;
        pending_icon.clear();
        has_custom_icon = true;
        UpdateRemoveIconButton();
        UISettings::values.is_game_list_reload_pending.exchange(true);
        emit CustomArtworkChanged(title_id);
    }
    const std::string new_title = ui->display_name->text().simplified().toStdString();
    if (title_id == 0 || new_title == initial_title) {
        return true;
    }
    // The game's own name, or an empty field, removes the custom name.
    const std::string custom_title = new_title == original_title ? std::string{} : new_title;
    if (!QtCommon::Game::SaveCustomTitle(title_id, custom_title)) {
        if (show_error) {
            QMessageBox::warning(this, tr("Game Name"), tr("The game name could not be saved."));
        }
        return false;
    }
    initial_title = new_title;
    title_changed = true;
    const std::string shown = custom_title.empty() ? original_title : custom_title;
    if (shown.empty()) {
        // No name to show in place: let the library read it again.
        UISettings::values.is_game_list_reload_pending.exchange(true);
    } else {
        renamed_title = QString::fromStdString(shown);
    }
    return true;
}
