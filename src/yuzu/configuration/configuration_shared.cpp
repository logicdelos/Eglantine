// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <type_traits>
#include <vector>
#include <QCheckBox>
#include <QFile>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include "yuzu/configuration/configuration_shared.h"

namespace ConfigurationShared {

namespace {
QString IconPath(const QString& name) {
    return QStringLiteral(":/eglantine_dark/icons/%1.png").arg(name);
}
} // namespace

QIcon PageIcon(const QString& name) {
    QIcon icon;
    const QString normal = IconPath(name);
    if (name.isEmpty() || !QFile::exists(normal)) {
        return icon;
    }
    QString active = IconPath(name + QStringLiteral("_active"));
    if (!QFile::exists(active)) {
        active = normal;
    }
    icon.addFile(normal, {}, QIcon::Normal, QIcon::Off);
    icon.addFile(active, {}, QIcon::Normal, QIcon::On);
    icon.addFile(active, {}, QIcon::Selected, QIcon::Off);
    icon.addFile(active, {}, QIcon::Selected, QIcon::On);
    return icon;
}

QIcon RestoreIcon(const QWidget& widget) {
    const QString reset = IconPath(QStringLiteral("reset"));
    if (QFile::exists(reset)) {
        return QIcon(reset);
    }
    return widget.style()->standardIcon(QStyle::SP_LineEditClearButton);
}

QScrollArea* ScrollTab(QWidget* tab) {
    auto* const area = new QScrollArea;
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    area->setWidget(tab);
    area->setAccessibleName(tab->accessibleName());
    return area;
}

void UseToggles(QWidget* root) {
    for (auto* const checkbox : root->findChildren<QCheckBox*>()) {
        checkbox->setProperty("toggle", true);
    }
}

void FitToScreen(QWidget* dialog, const QSize& preferred) {
    const QScreen* screen = dialog->screen();
    if (dialog->parentWidget() != nullptr) {
        screen = dialog->parentWidget()->screen();
    }
    if (screen == nullptr) {
        dialog->resize(preferred);
        return;
    }
    const QSize available = screen->availableGeometry().size() * 9 / 10;
    dialog->resize(std::min(preferred.width(), available.width()),
                   std::min(preferred.height(), available.height()));
}

Tab::Tab(std::shared_ptr<std::vector<Tab*>> group, QWidget* parent) : QWidget(parent) {
    if (group != nullptr) {
        group->push_back(this);
    }
}

Tab::~Tab() = default;

} // namespace ConfigurationShared
