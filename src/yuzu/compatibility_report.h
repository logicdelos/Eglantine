// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QList>
#include <QString>

class QComboBox;
class QPlainTextEdit;

namespace Core {
class System;
}

namespace CompatibilityReporting {

struct Addon {
    QString name;
    QString version;
    bool enabled;
};

/// "Name vVersion, Name2" for the enabled add-ons, "none" when there are none.
QString SummarizeEnabledAddons(const QList<Addon>& addons);

/// What the report states about the game and the computer it ran on.
struct Session {
    quint64 program_id{};
    QString title;
    QString build;
    QString game_version;
    QString cpu;
    QString gpu;
    QString driver;
    QString os;
    QString ram;
    QString graphics_api;
    QString settings;
    QString mods;
    QString updates;
};

/// The running game and this computer, as far as the emulator knows them. Reads nothing from the
/// network and nothing that identifies the user (the settings text has no credentials).
[[nodiscard]] Session CurrentSession(Core::System& system);

/// Help > Compatibility Report. The user fills in how the game runs, reads the whole report and
/// copies it or saves it to a file. Eglantine sends nothing: there is no upload.
class Dialog final : public QDialog {
    Q_OBJECT

public:
    explicit Dialog(const Session& session, QWidget* parent = nullptr);
    ~Dialog() override;

    /// The report as shown in the preview: plain text, ready to paste.
    [[nodiscard]] QString ReportText() const;

private:
    void Refresh();
    void Copy();
    void Save();

    Session session;
    QComboBox* outcome{};
    QPlainTextEdit* works{};
    QPlainTextEdit* issues{};
    QPlainTextEdit* preview{};
};

} // namespace CompatibilityReporting
