// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>
#include <QIcon>
#include <QString>
#include <QWidget>
#include <qobjectdefs.h>

class QObject;
class QScrollArea;
class QSize;

namespace ConfigurationShared {

/// A category or tab icon from the Eglantine icon set, accent coloured when selected.
/// Null when the running build has no such icon.
QIcon PageIcon(const QString& name);

/// The icon of a per-game "use the global value" button.
QIcon RestoreIcon(const QWidget& widget);

/// Wraps a settings tab in a frameless scroll area, so a long tab scrolls inside its dialog
/// instead of growing the dialog past the screen.
QScrollArea* ScrollTab(QWidget* tab);

/// Marks the check boxes under root as switches (property "toggle"), for themes that draw them so.
void UseToggles(QWidget* root);

/// Resizes a dialog to its preferred size, shrunk to fit the screen it opens on.
void FitToScreen(QWidget* dialog, const QSize& preferred);

class Tab : public QWidget {
    Q_OBJECT

public:
    explicit Tab(std::shared_ptr<std::vector<Tab*>> group, QWidget* parent = nullptr);
    ~Tab();

    virtual void ApplyConfiguration() = 0;
    virtual void SetConfiguration() = 0;
};

} // namespace ConfigurationShared
