// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QDialog>
#include <QFutureWatcher>
#include <QList>
#include <QPixmap>

#include "configuration/shared_widget.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "frontend_common/config.h"
#include "qt_common/config/qt_config.h"
#include "qt_common/config/shared_translation.h"
#include "qt_common/util/vk.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/util/icon_search.h"

namespace Core {
class System;
}

namespace InputCommon {
class InputSubsystem;
}

class ConfigurePerGameAddons;
class ConfigureAudio;
class ConfigureCpu;
class ConfigureFrameGeneration;
class ConfigureGraphics;
class ConfigureGraphicsAdvanced;
class ConfigureGraphicsExtensions;
class ConfigureInputPerGame;
class ConfigureSystem;
class ConfigureNetwork;
class ConfigurePerGameTweaks;
class ConfigureApplets;

class QGraphicsScene;
class QStandardItem;
class QStandardItemModel;
class QTreeView;
class QVBoxLayout;

namespace Ui {
class ConfigurePerGame;
}

class ConfigurePerGame : public QDialog {
    Q_OBJECT

public:
    // Cannot use std::filesystem::path due to https://bugreports.qt.io/browse/QTBUG-73263
    explicit ConfigurePerGame(QWidget* parent, u64 title_id_, const std::string& file_name,
                              std::vector<VkDeviceInfo::Record>& vk_device_records,
                              Core::System& system_);
    ~ConfigurePerGame() override;

    /// Save all button configurations to settings file
    void ApplyConfiguration();

    void LoadFromFile(FileSys::VirtualFile file_);

    /// The name the library should show when the user renamed the game in this dialog: the custom
    /// name, or the game's own name after the field was cleared.
    std::optional<QString> RenamedTitle() const {
        return renamed_title;
    }

    /// True when a new custom name was saved or the custom name was removed, even when there is no
    /// name to show in place and the library reads it again.
    bool TitleChanged() const {
        return title_changed;
    }

signals:
    /// The custom artwork was saved or removed.
    void CustomArtworkChanged(u64 program_id);

private:
    void done(int result) override;
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void HandleApplyButtonClicked();

    void LoadConfiguration();
    void UpdateIconPreview(const QPixmap& map);
    void OnUploadIcon();
    void OnDetectIcon();
    void OnRemoveCustomIcon();
    void UpdateRemoveIconButton();
    /// Saves the picked artwork and name, which live outside the game file. False on failure.
    bool SaveCustomMetadata(bool show_error = false);

    // Creates the tabs built from settings widgets, and lists every tab in the tab widget.
    void BuildSettingTabs();
    void AddTabs();
    // Moves pending edits into the settings so the Tweaks tab compares against them.
    void ApplySettingTabs();
    void ApplyRecommendedSettings(const std::set<std::string>& keys);

    std::unique_ptr<Ui::ConfigurePerGame> ui;
    FileSys::VirtualFile file;
    std::filesystem::path file_path;
    u64 title_id;
    /// The game's own name, and the name shown when the dialog opened (custom or own).
    std::string original_title;
    std::string initial_title;
    std::optional<QString> renamed_title;
    bool title_changed{false};
    /// An image picked or detected in this dialog, saved as custom artwork on Apply or OK.
    std::vector<u8> pending_icon;
    bool icon_modified{false};
    /// The game's own icon, shown again when the custom artwork is removed.
    QPixmap file_icon;
    bool has_custom_icon{false};
    /// The saved custom artwork is deleted on Apply or OK.
    bool remove_custom_icon{false};
    QFutureWatcher<IconDetectResult> detect_watcher;
    std::shared_ptr<std::atomic_bool> detect_cancel;

    QGraphicsScene* scene;

    std::unique_ptr<QtConfig> game_config;

    Core::System& system;
    std::unique_ptr<ConfigurationShared::Builder> builder;
    std::shared_ptr<std::vector<ConfigurationShared::Tab*>> tab_group;

    std::unique_ptr<ConfigurePerGameAddons> addons_tab;
    std::unique_ptr<ConfigureAudio> audio_tab;
    std::unique_ptr<ConfigureCpu> cpu_tab;
    std::unique_ptr<ConfigureFrameGeneration> frame_gen_tab;
    std::unique_ptr<ConfigureGraphicsAdvanced> graphics_advanced_tab;
    std::unique_ptr<ConfigureGraphicsExtensions> graphics_extensions_tab;
    std::unique_ptr<ConfigureGraphics> graphics_tab;
    std::unique_ptr<ConfigureInputPerGame> input_tab;
    std::unique_ptr<ConfigureSystem> system_tab;
    std::unique_ptr<ConfigureNetwork> network_tab;
    std::unique_ptr<ConfigureApplets> applets_tab;
    std::unique_ptr<ConfigurePerGameTweaks> tweaks_tab;

    std::vector<VkDeviceInfo::Record>& vk_device_records;
};
