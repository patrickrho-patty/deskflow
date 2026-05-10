/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QString>

class QJsonObject;

namespace deskflow::gui {

class LogiOptionsPlus
{
public:
  struct SideButtonStatus
  {
    bool supported = false;
    bool enabled = false;
  };

  [[nodiscard]] QString settingsDatabasePath() const;
  [[nodiscard]] SideButtonStatus sideButtonStatus(QString *errorMessage = nullptr) const;
  [[nodiscard]] bool hasSupportedMouse(QString *errorMessage = nullptr) const;
  [[nodiscard]] bool sideButtonPatchEnabled(QString *errorMessage = nullptr) const;

  bool setSideButtonPatchEnabled(bool enabled, QString *errorMessage = nullptr) const;

private:
  bool loadSettings(QJsonObject &root, QString *errorMessage) const;
  [[nodiscard]] QByteArray readSettingsJson(QString *errorMessage) const;
  bool writeSettingsJson(const QByteArray &json, QString *errorMessage) const;
  bool backupDatabase(QString *errorMessage) const;
  void stopServices() const;
  void restartServices() const;
};

} // namespace deskflow::gui
