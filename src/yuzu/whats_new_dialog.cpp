// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCheckBox>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "qt_common/config/uisettings.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/whats_new_dialog.h"

namespace {

QWidget* Item(const QString& title, const QString& text) {
    auto* const item = new QWidget;
    item->setObjectName(QStringLiteral("WhatsNewItem"));
    item->setAttribute(Qt::WA_StyledBackground);
    auto* const layout = new QHBoxLayout(item);
    layout->setContentsMargins(0, 12, 0, 12);
    layout->setSpacing(14);

    auto* const column = new QVBoxLayout;
    column->setSpacing(4);
    auto* const heading = new QLabel(title);
    heading->setObjectName(QStringLiteral("WhatsNewHeading"));
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);
    auto* const body = new QLabel(text);
    body->setObjectName(QStringLiteral("WhatsNewText"));
    body->setWordWrap(true);
    column->addWidget(heading);
    column->addWidget(body);
    layout->addLayout(column, 1);
    return item;
}

} // namespace

WhatsNewDialog::WhatsNewDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("WhatsNewDialog"));
    setWindowTitle(tr("What's New in Eglantine"));

    auto* const root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* const content = new QWidget;
    auto* const body = new QVBoxLayout(content);
    body->setContentsMargins(24, 20, 24, 8);
    body->setSpacing(0);

    auto* const header = new QHBoxLayout;
    header->setSpacing(14);
    header->setContentsMargins(0, 0, 0, 10);
    auto* const logo = new QLabel;
    logo->setPixmap(QGuiApplication::windowIcon().pixmap(44));
    header->addWidget(logo, 0, Qt::AlignTop);
    auto* const titles = new QVBoxLayout;
    titles->setSpacing(3);
    auto* const title = new QLabel(tr("What's new in Eglantine"));
    title->setObjectName(QStringLiteral("WhatsNewTitle"));
    QFont title_font = title->font();
    title_font.setBold(true);
    title_font.setPointSizeF(title_font.pointSizeF() * 1.35);
    title->setFont(title_font);
    auto* const subtitle =
        new QLabel(tr("Here is what changed. Menus and shortcuts are where they were."));
    subtitle->setObjectName(QStringLiteral("WhatsNewText"));
    subtitle->setWordWrap(true);
    titles->addWidget(title);
    titles->addWidget(subtitle);
    header->addLayout(titles, 1);
    body->addLayout(header);

    body->addWidget(Item(tr("Enhancements are optional"),
                         tr("Games run the same without any enhancement. Upscaling, frame "
                            "generation and the other extras are off until you turn them on in "
                            "Configure or in a game's Properties.")));
    body->addWidget(Item(tr("Recommended settings, per game"),
                         tr("Games with known-good settings list them in their own Properties, "
                            "under the Tweaks tab. Tick the ones you want and choose Apply "
                            "Selected. Nothing is applied automatically.")));
    body->addWidget(Item(tr("Report a bug, with a summary first"),
                         tr("Help > Report a Bug (Ctrl+F12) saves a bundle on your computer with "
                            "the details you choose. The review shows a summary of what it holds, "
                            "so open the ZIP and inspect it before you share it. Eglantine sends "
                            "nothing by itself.")));
    body->addWidget(Item(tr("Safer profile import"),
                         tr("Importing from another emulator copies your data and keeps the "
                            "originals. If an import is interrupted, Eglantine offers to recover "
                            "it the next time and keeps your current profile meanwhile.")));
    body->addWidget(Item(tr("Same places as before"),
                         tr("Configure is in Emulation > Configure (Ctrl+,). Right-click a game "
                            "and choose Configure Game for its own settings. Help > What's New "
                            "shows this again.")));
    auto* const scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    auto* const footer = new QWidget;
    footer->setObjectName(QStringLiteral("WhatsNewFooter"));
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* const buttons = new QHBoxLayout(footer);
    buttons->setContentsMargins(20, 12, 20, 12);
    buttons->setSpacing(8);
    dont_show_again = new QCheckBox(tr("Don't show again"));
    dont_show_again->setChecked(!UISettings::values.show_whats_new.GetValue());
    buttons->addWidget(dont_show_again);
    buttons->addStretch(1);
    auto* const configure = new QPushButton(tr("Open Configure"));
    configure->setAutoDefault(false);
    connect(configure, &QPushButton::clicked, this, [this] {
        open_configure = true;
        accept();
    });
    buttons->addWidget(configure);
    auto* const got_it = new QPushButton(tr("Got it"));
    got_it->setDefault(true);
    connect(got_it, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(got_it);
    root->addWidget(footer);

    ensurePolished();
    ConfigurationShared::FitToScreen(
        this, QSize(560, body->heightForWidth(560) + footer->sizeHint().height()));
}

WhatsNewDialog::~WhatsNewDialog() = default;

void WhatsNewDialog::done(int result) {
    UISettings::values.show_whats_new = !dont_show_again->isChecked();
    QDialog::done(result);
}