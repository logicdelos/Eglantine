// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <QByteArray>
#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <QString>

#include "frontend_common/bug_bundle.h"

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace Core {
class System;
}

/// Help > Report a Bug and its hotkey. Notes the screen when the user asked, takes a description,
/// shows everything the report will hold and, only after that, writes it as a zip on this
/// computer. Nothing is sent anywhere.
class BugReportDialog : public QDialog {
    Q_OBJECT

public:
    BugReportDialog(Core::System& system, QWidget* parent);
    ~BugReportDialog() override;

private:
    struct Written {
        FrontendCommon::BugBundle::Result result;
        QString error;
    };
    /// Filled by the renderer thread; the dialog only reads it once `ready` is set.
    struct Shot {
        QImage image;
        bool invert{false};
        std::atomic<bool> ready{false};
    };

    void CollectStaticParts(Core::System& system);
    void PollScreenshot();
    void InvalidateReview();
    void Review();
    void Save();
    void ShowResult(const Written& written);
    void reject() override;

    FrontendCommon::BugBundle::Report report;
    std::shared_ptr<Shot> shot;
    QByteArray screenshot_png;
    bool screenshot_requested = false;
    bool reviewed = false;
    bool writing = false;
    QFutureWatcher<Written> writer;
    QTimer* poll{};

    QLabel* status{};
    QPlainTextEdit* description{};
    QCheckBox* include_screenshot{};
    QCheckBox* include_log{};
    QPlainTextEdit* review_text{};
    QDialogButtonBox* buttons{};
    QPushButton* review_button{};
    QPushButton* save{};
    QPushButton* open_folder{};
};
