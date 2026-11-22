// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <thread>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QSysInfo>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "common/cpu_features.h"
#include "common/memory_detect.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"
#include "frontend_common/bug_bundle.h"
#include "video_core/renderer_base.h"
#include "yuzu/compatibility_report.h"

namespace CompatibilityReporting {

QString SummarizeEnabledAddons(const QList<Addon>& addons) {
    QStringList labels;
    for (const auto& addon : addons) {
        if (!addon.enabled || addon.name.trimmed().isEmpty()) {
            continue;
        }
        auto label = addon.name.trimmed();
        if (!addon.version.trimmed().isEmpty()) {
            label += QStringLiteral(" (%1)").arg(addon.version.trimmed());
        }
        if (!labels.contains(label)) {
            labels.append(std::move(label));
        }
    }
    if (labels.isEmpty()) {
        return QStringLiteral("None enabled.");
    }
    const QString summary = QStringLiteral("Enabled: ") + labels.join(QStringLiteral("; "));
    constexpr int limit = 500;
    if (summary.size() <= limit) {
        return summary;
    }
    return summary.left(limit - 3) + QStringLiteral("...");
}

Session CurrentSession(Core::System& system) {
    Session session;
    session.build = QStringLiteral("%1 (%2)").arg(QString::fromStdString(Common::BuildDisplayName()),
                                                  QString::fromUtf8(Common::g_scm_rev));
    session.os = QSysInfo::prettyProductName();
#ifdef ARCHITECTURE_x86_64
    session.cpu = QString::fromUtf8(Common::g_cpu_caps.cpu_string);
#endif
    session.ram = QStringLiteral("%1 GiB")
                      .arg(static_cast<double>(Common::GetMemInfo().TotalPhysicalMemory) /
                               (1ULL << 30),
                           0, 'f', 1);
    session.graphics_api =
        QString::fromStdString(Settings::values.renderer_backend.Canonicalize());
    session.settings =
        QString::fromStdString(FrontendCommon::BugBundle::SettingsReport(true)).trimmed();
    if (system.IsPoweredOn()) {
        std::string title;
        if (system.GetGameName(title) == Loader::ResultStatus::Success) {
            session.title = QString::fromStdString(title);
        }
        session.program_id = system.GetApplicationProcessProgramID();
        session.gpu = QString::fromStdString(system.Renderer().GetDeviceVendor());

        const FileSys::PatchManager patch_manager{session.program_id,
                                                  system.GetFileSystemController(),
                                                  system.GetContentProvider()};
        if (const auto metadata = patch_manager.GetControlMetadata(); metadata.first != nullptr) {
            session.game_version = QString::fromStdString(metadata.first->GetVersionString());
        }
        QList<Addon> updates;
        QList<Addon> mods;
        for (const auto& patch : patch_manager.GetPatches()) {
            const Addon addon{QString::fromStdString(patch.name),
                              QString::fromStdString(patch.version), patch.enabled};
            if (patch.type == FileSys::PatchType::Update) {
                updates.append(addon);
            } else if (patch.type == FileSys::PatchType::Mod) {
                mods.append(addon);
            }
        }
        session.updates = SummarizeEnabledAddons(updates);
        session.mods = SummarizeEnabledAddons(mods);
    }
    return session;
}

Dialog::Dialog(const Session& session_, QWidget* parent) : QDialog(parent), session{session_} {
    setWindowTitle(tr("Compatibility Report"));
    setMinimumWidth(620);

    auto* const layout = new QVBoxLayout(this);
    auto* const intro = new QLabel(
        tr("Say how this game runs. Read the report below, then copy it or save it to a file and "
           "share it where you choose. Eglantine sends nothing."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* const form = new QFormLayout;
    outcome = new QComboBox;
    outcome->addItems({tr("Perfect: plays like on the console"), tr("Great: minor issues"),
                       tr("Okay: major issues, still playable"),
                       tr("Bad: starts, but is not playable"), tr("Intro or menu only")});
    form->addRow(tr("How does it run?"), outcome);
    works = new QPlainTextEdit;
    works->setPlaceholderText(tr("What works"));
    works->setFixedHeight(64);
    form->addRow(tr("Works"), works);
    issues = new QPlainTextEdit;
    issues->setPlaceholderText(tr("What does not work, and where it happens"));
    issues->setFixedHeight(64);
    form->addRow(tr("Problems"), issues);
    layout->addLayout(form);

    preview = new QPlainTextEdit;
    preview->setReadOnly(true);
    preview->setMinimumHeight(220);
    layout->addWidget(preview, 1);

    auto* const buttons = new QDialogButtonBox;
    auto* const copy = buttons->addButton(tr("Copy to Clipboard"), QDialogButtonBox::ActionRole);
    auto* const save = buttons->addButton(tr("Save to File..."), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(copy, &QPushButton::clicked, this, &Dialog::Copy);
    connect(save, &QPushButton::clicked, this, &Dialog::Save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(outcome, &QComboBox::currentIndexChanged, this, &Dialog::Refresh);
    connect(works, &QPlainTextEdit::textChanged, this, &Dialog::Refresh);
    connect(issues, &QPlainTextEdit::textChanged, this, &Dialog::Refresh);
    Refresh();
}

Dialog::~Dialog() = default;

QString Dialog::ReportText() const {
    // A fact the emulator does not know is left out, not printed as a dash. The three fields the
    // user fills in always appear, so an empty one reads as left empty.
    const auto line = [](const QString& label, const QString& value, bool always = false) {
        if (value.isEmpty() && !always) {
            return QString{};
        }
        return QStringLiteral("%1: %2\n").arg(label, value.isEmpty() ? QStringLiteral("-") : value);
    };
    QString out;
    out += QStringLiteral("Eglantine compatibility report\n\n");
    out += line(QStringLiteral("Game"), session.title);
    out += line(QStringLiteral("Program ID"),
                session.program_id == 0 ? QString{}
                                        : QStringLiteral("%1").arg(session.program_id, 16, 16,
                                                                   QLatin1Char('0'))
                                              .toUpper());
    out += line(QStringLiteral("Game version"), session.game_version);
    out += line(QStringLiteral("Updates"), session.updates);
    out += line(QStringLiteral("Mods"), session.mods);
    out += line(QStringLiteral("Result"), outcome->currentText(), true);
    out += line(QStringLiteral("Works"), works->toPlainText().trimmed(), true);
    out += line(QStringLiteral("Problems"), issues->toPlainText().trimmed(), true);
    out += QStringLiteral("\n");
    out += line(QStringLiteral("Build"), session.build);
    out += line(QStringLiteral("OS"), session.os);
    out += line(QStringLiteral("CPU"), session.cpu);
    out += line(QStringLiteral("RAM"), session.ram);
    out += line(QStringLiteral("GPU"), session.gpu);
    out += line(QStringLiteral("Graphics API"), session.graphics_api);
    if (!session.settings.isEmpty()) {
        out += QStringLiteral("\nSettings changed from their defaults\n%1\n").arg(session.settings);
    }
    return out;
}

void Dialog::Refresh() {
    preview->setPlainText(ReportText());
}

void Dialog::Copy() {
    QApplication::clipboard()->setText(ReportText());
}

void Dialog::Save() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Compatibility Report"),
                                                      QStringLiteral("compatibility-report.txt"),
                                                      tr("Text file (*.txt)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(ReportText().toUtf8()) < 0) {
        QMessageBox::warning(this, tr("Compatibility Report"),
                             tr("The report could not be saved: %1").arg(file.errorString()));
    }
}

} // namespace CompatibilityReporting
