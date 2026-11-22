// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <thread>

#include <QBuffer>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <fmt/format.h>

#include "common/cpu_features.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/memory_detect.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/loader/loader.h"
#include "video_core/renderer_base.h"
#include "yuzu/bug_report_dialog.h"

namespace {

std::string Gib(u64 bytes) {
    return fmt::format("{:.1f} GiB", static_cast<double>(bytes) / (1ULL << 30));
}

} // namespace

BugReportDialog::BugReportDialog(Core::System& system, QWidget* parent)
    : QDialog(parent), shot{std::make_shared<Shot>()} {
    setWindowTitle(tr("Report a Bug"));
    setMinimumWidth(560);

    CollectStaticParts(system);

    // The screen as it is now. The renderer delivers it on its own thread: the callback only
    // fills `shot`, and the timer below picks it up on this one.
    if (system.IsPoweredOn()) {
        auto& renderer = system.Renderer();
        if (!renderer.IsScreenshotPending()) {
            constexpr u32 Width = 1280;
            constexpr u32 Height = 720;
            shot->image = QImage(QSize(Width, Height), QImage::Format_RGB32);
            const auto layout = Layout::DefaultFrameLayout(Width, Height);
            renderer.RequestScreenshot(
                shot->image.bits(),
                [shot = shot](bool invert_y) {
                    shot->invert = invert_y;
                    shot->ready.store(true, std::memory_order_release);
                },
                layout);
            screenshot_requested = true;
        }
    }

    auto* const layout = new QVBoxLayout(this);
    auto* const intro = new QLabel(
        tr("Describe what went wrong and what you expected. Eglantine saves a report on this "
           "computer. A summary of it is shown before it is written, and Eglantine sends "
           "nothing: you decide whether to share the file. Open the ZIP and inspect it before "
           "you share it."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    description = new QPlainTextEdit;
    description->setPlaceholderText(
        tr("For example: the sky turns white after the first cutscene in world 2."));
    description->setMinimumHeight(90);
    layout->addWidget(description, 1);

    include_screenshot = new QCheckBox(tr("Include a screenshot of the game"));
    include_screenshot->setEnabled(false);
    layout->addWidget(include_screenshot);
    include_log = new QCheckBox(tr("Include this session's log (your user name is replaced)"));
    layout->addWidget(include_log);

    auto* const privacy = new QLabel(
        tr("Settings are always included, without passwords or tokens. No game files, saves, "
           "keys or firmware are ever included."));
    privacy->setWordWrap(true);
    layout->addWidget(privacy);

    review_text = new QPlainTextEdit;
    review_text->setReadOnly(true);
    review_text->setMinimumHeight(200);
    review_text->hide();
    layout->addWidget(review_text, 2);

    status = new QLabel;
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status->hide();
    layout->addWidget(status);

    buttons = new QDialogButtonBox;
    review_button = buttons->addButton(tr("Show Summary"), QDialogButtonBox::ActionRole);
    save = buttons->addButton(tr("Save Report"), QDialogButtonBox::AcceptRole);
    save->setEnabled(false);
    open_folder = buttons->addButton(tr("Open Folder"), QDialogButtonBox::ActionRole);
    open_folder->hide();
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(review_button, &QPushButton::clicked, this, &BugReportDialog::Review);
    connect(save, &QPushButton::clicked, this, &BugReportDialog::Save);
    connect(buttons, &QDialogButtonBox::rejected, this, &BugReportDialog::reject);
    connect(open_folder, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(
            Common::FS::PathToUTF8String(FrontendCommon::BugBundle::DefaultDirectory()))));
    });
    connect(description, &QPlainTextEdit::textChanged, this, &BugReportDialog::InvalidateReview);
    connect(include_screenshot, &QCheckBox::toggled, this, &BugReportDialog::InvalidateReview);
    connect(include_log, &QCheckBox::toggled, this, &BugReportDialog::InvalidateReview);
    connect(&writer, &QFutureWatcher<Written>::finished, this,
            [this] { ShowResult(writer.result()); });

    if (screenshot_requested) {
        poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, &BugReportDialog::PollScreenshot);
        poll->start(100);
    }
}

BugReportDialog::~BugReportDialog() {
    writer.waitForFinished();
}

void BugReportDialog::CollectStaticParts(Core::System& system) {
    report.trigger = "ui";
    if (system.IsPoweredOn()) {
        std::string title;
        if (system.GetGameName(title) == Loader::ResultStatus::Success) {
            report.game_title = std::move(title);
        }
        report.game_program_id = fmt::format("{:016X}", system.GetApplicationProcessProgramID());
    }

    auto& info = report.system;
    info.emplace_back("os", QSysInfo::prettyProductName().toStdString());
    info.emplace_back("architecture", QSysInfo::currentCpuArchitecture().toStdString());
#ifdef ARCHITECTURE_x86_64
    info.emplace_back("cpu", std::string{Common::g_cpu_caps.cpu_string});
#endif
    info.emplace_back("threads", std::to_string(std::thread::hardware_concurrency()));
    info.emplace_back("ram", Gib(Common::GetMemInfo().TotalPhysicalMemory));
    info.emplace_back("renderer_backend", Settings::values.renderer_backend.Canonicalize());
    if (system.IsPoweredOn()) {
        info.emplace_back("gpu", system.Renderer().GetDeviceVendor());
    }
    report.settings = FrontendCommon::BugBundle::SettingsReport();
    report.log_name = LOG_FILE;
}

void BugReportDialog::PollScreenshot() {
    if (!shot->ready.load(std::memory_order_acquire)) {
        return;
    }
    poll->stop();
    QImage image = shot->image;
    if (shot->invert) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        image = image.flipped(Qt::Vertical);
#else
        image = image.mirrored(false, true);
#endif
    }
    QBuffer buffer(&screenshot_png);
    buffer.open(QIODevice::WriteOnly);
    if (image.save(&buffer, "PNG")) {
        // Offered, never selected for the user: a screenshot can show anything on screen.
        include_screenshot->setEnabled(true);
    } else {
        screenshot_png.clear();
    }
}

void BugReportDialog::InvalidateReview() {
    if (writing) {
        return;
    }
    reviewed = false;
    save->setEnabled(false);
    if (review_text->isVisible()) {
        review_text->setPlainText(
            tr("The report changed. Press Show Summary to read it again before saving."));
    }
}

void BugReportDialog::Review() {
    report.description = description->toPlainText().toStdString();
    report.screenshot_png.clear();
    if (include_screenshot->isChecked() && !screenshot_png.isEmpty()) {
        report.screenshot_png.assign(screenshot_png.begin(), screenshot_png.end());
    }
    report.log.clear();
    if (include_log->isChecked()) {
        report.log = FrontendCommon::BugBundle::ReadLog(
            Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / LOG_FILE);
    }
    review_text->setPlainText(QString::fromStdString(FrontendCommon::BugBundle::Preview(report)));
    review_text->show();
    reviewed = true;
    save->setEnabled(true);
}

void BugReportDialog::Save() {
    if (!reviewed) {
        return;
    }
    writing = true;
    save->setEnabled(false);
    review_button->setEnabled(false);
    description->setReadOnly(true);
    include_screenshot->setEnabled(false);
    include_log->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setEnabled(false);
    status->setText(tr("Writing the report..."));
    status->show();
    writer.setFuture(QtConcurrent::run([report = report] {
        Written written;
        try {
            written.result = FrontendCommon::BugBundle::WriteBundle(
                report, FrontendCommon::BugBundle::DefaultDirectory());
        } catch (const std::exception& failure) {
            written.error = QString::fromStdString(failure.what());
        }
        return written;
    }));
}

void BugReportDialog::ShowResult(const Written& written) {
    writing = false;
    auto* const close = buttons->button(QDialogButtonBox::Cancel);
    close->setEnabled(true);
    close->setText(tr("Close"));
    save->hide();
    review_button->hide();
    if (!written.error.isEmpty()) {
        status->setText(tr("The report could not be written: %1").arg(written.error));
        return;
    }
    QString text = tr("Saved %1 (%2 MiB). It stays on this computer; share it only if you want to.")
                       .arg(QString::fromStdString(
                                Common::FS::PathToUTF8String(written.result.path)),
                            QString::number(static_cast<double>(written.result.bytes) / (1 << 20),
                                            'f', 1));
    if (!written.result.dropped.empty()) {
        text += QStringLiteral("\n") + tr("Left out to fit the size limit: %1")
                                           .arg(QString::number(written.result.dropped.size()));
    }
    status->setText(text);
    open_folder->show();
}

void BugReportDialog::reject() {
    if (!writing) {
        QDialog::reject();
    }
}
