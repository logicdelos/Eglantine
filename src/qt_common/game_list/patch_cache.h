// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <unordered_map>

#include <QString>

/// The add-on and update lines of each title, kept for one library scan. It lives in memory only:
/// a change of add-ons, updates or their enabled state shows on the next scan, without a stale
/// file on disk to remove.
class GameListPatchCache {
public:
    QString Get(quint64 title_id, bool enabled, const std::function<QString()>& generator) {
        if (!enabled || title_id == 0) {
            return generator();
        }
        if (const auto it = entries.find(title_id); it != entries.end()) {
            return it->second;
        }
        auto text = generator();
        if (!text.isEmpty()) {
            entries.emplace(title_id, text);
        }
        return text;
    }

    void Clear() {
        entries.clear();
    }

private:
    std::unordered_map<quint64, QString> entries;
};
