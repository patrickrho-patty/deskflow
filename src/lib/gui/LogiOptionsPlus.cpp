/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "LogiOptionsPlus.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryFile>
#include <qscopeguard.h>

#include <unistd.h>

#include <array>

namespace deskflow::gui {

namespace {

struct ButtonSpec
{
  const char *slotSuffix;
  const char *regularAction;
  int hidUsage;
  const char *actionName;
};

struct TargetAssignment
{
  QString profileKey;
  int index = -1;
  QString slotId;
  const ButtonSpec *button = nullptr;
};

struct AssignmentId
{
  QString profileKey;
  QString slotId;

  bool operator==(const AssignmentId &other) const
  {
    return profileKey == other.profileKey && slotId == other.slotId;
  }
};

size_t qHash(const AssignmentId &id, size_t seed = 0)
{
  return qHash(id.profileKey, seed) ^ qHash(id.slotId, seed + 0x9e3779b9);
}

constexpr std::array<ButtonSpec, 2> kSideButtons = {{
    {"_c83", "OSX_GESTURE_BACK", 4, "MB4"},
    {"_c86", "OSX_GESTURE_FORWARD", 5, "MB5"},
}};

QString sqlitePath()
{
  return QStringLiteral("/usr/bin/sqlite3");
}

QString launchctlPath()
{
  return QStringLiteral("/bin/launchctl");
}

QString killallPath()
{
  return QStringLiteral("/usr/bin/killall");
}

void setError(QString *errorMessage, const QString &message)
{
  if (errorMessage != nullptr) {
    *errorMessage = message;
  }
}

QString processError(const QString &program, const QStringList &arguments, QProcess &process)
{
  auto error = QStringLiteral("%1 %2 failed").arg(program, arguments.join(QStringLiteral(" ")));
  const auto stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
  if (!stderrText.isEmpty()) {
    error.append(QStringLiteral(": %1").arg(stderrText));
  }
  return error;
}

bool runProcess(
    const QString &program, const QStringList &arguments, QByteArray *stdoutData = nullptr, QString *errorMessage = nullptr
)
{
  QProcess process;
  process.start(program, arguments);

  if (!process.waitForStarted()) {
    setError(errorMessage, QStringLiteral("Could not start %1: %2").arg(program, process.errorString()));
    return false;
  }

  if (!process.waitForFinished(15000)) {
    process.kill();
    process.waitForFinished();
    setError(errorMessage, QStringLiteral("%1 timed out").arg(program));
    return false;
  }

  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    setError(errorMessage, processError(program, arguments, process));
    return false;
  }

  if (stdoutData != nullptr) {
    *stdoutData = process.readAllStandardOutput();
  }
  return true;
}

void runProcessQuiet(const QString &program, const QStringList &arguments)
{
  QString ignoredError;
  runProcess(program, arguments, nullptr, &ignoredError);
}

QString sqlString(const QString &value)
{
  auto escaped = value;
  escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
  return QStringLiteral("'%1'").arg(escaped);
}

bool parseSettingsJson(const QByteArray &json, QJsonObject &root, QString *errorMessage)
{
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(json, &parseError);
  if (parseError.error != QJsonParseError::NoError) {
    setError(errorMessage, QStringLiteral("Could not parse Logi Options settings: %1").arg(parseError.errorString()));
    return false;
  }

  if (!document.isObject()) {
    setError(errorMessage, QStringLiteral("Logi Options settings are not a JSON object"));
    return false;
  }

  root = document.object();
  return true;
}

const ButtonSpec *buttonSpecForSlot(const QString &slotId)
{
  if (!slotId.contains(QStringLiteral("mx-master-3s"), Qt::CaseInsensitive)) {
    return nullptr;
  }

  for (const auto &button : kSideButtons) {
    if (slotId.endsWith(QString::fromLatin1(button.slotSuffix))) {
      return &button;
    }
  }

  return nullptr;
}

QVector<TargetAssignment> findTargetAssignments(const QJsonObject &root)
{
  QVector<TargetAssignment> targets;

  for (auto profileIt = root.constBegin(); profileIt != root.constEnd(); ++profileIt) {
    if (!profileIt.key().startsWith(QStringLiteral("profile-")) || !profileIt.value().isObject()) {
      continue;
    }

    const auto profile = profileIt.value().toObject();
    const auto assignments = profile.value(QStringLiteral("assignments")).toArray();
    for (int i = 0; i < assignments.size(); ++i) {
      const auto assignment = assignments.at(i).toObject();
      const auto slotId = assignment.value(QStringLiteral("slotId")).toString();

      const auto *button = buttonSpecForSlot(slotId);
      if (button == nullptr) {
        continue;
      }

      targets.append({profileIt.key(), i, slotId, button});
    }
  }

  return targets;
}

QJsonObject regularMacro(const ButtonSpec &button)
{
  QJsonObject mouse;
  mouse.insert(QStringLiteral("action"), QString::fromLatin1(button.regularAction));

  QJsonObject macro;
  macro.insert(QStringLiteral("mouse"), mouse);
  macro.insert(QStringLiteral("type"), QStringLiteral("MOUSE"));
  return macro;
}

QJsonObject patchedMacro(const ButtonSpec &button)
{
  QJsonObject mouse;
  mouse.insert(QStringLiteral("action"), QStringLiteral("BUTTON"));
  mouse.insert(QStringLiteral("hidUsage"), button.hidUsage);

  QJsonObject macro;
  macro.insert(QStringLiteral("actionName"), QString::fromLatin1(button.actionName));
  macro.insert(QStringLiteral("mouse"), mouse);
  macro.insert(QStringLiteral("type"), QStringLiteral("MOUSE"));
  return macro;
}

QJsonObject assignmentMacro(const QJsonObject &assignment)
{
  const auto card = assignment.value(QStringLiteral("card")).toObject();
  return card.value(QStringLiteral("macro")).toObject();
}

QJsonObject assignmentMacro(const QJsonObject &root, const TargetAssignment &target)
{
  const auto profile = root.value(target.profileKey).toObject();
  const auto assignments = profile.value(QStringLiteral("assignments")).toArray();
  if (target.index < 0 || target.index >= assignments.size()) {
    return {};
  }
  return assignmentMacro(assignments.at(target.index).toObject());
}

bool isPatchedMacro(const QJsonObject &macro, const ButtonSpec &button)
{
  const auto mouse = macro.value(QStringLiteral("mouse")).toObject();
  return macro.value(QStringLiteral("type")).toString() == QStringLiteral("MOUSE") &&
         mouse.value(QStringLiteral("action")).toString() == QStringLiteral("BUTTON") &&
         mouse.value(QStringLiteral("hidUsage")).toInt() == button.hidUsage;
}

bool setAssignmentMacro(QJsonObject &root, const TargetAssignment &target, const QJsonObject &macro)
{
  auto profile = root.value(target.profileKey).toObject();
  auto assignments = profile.value(QStringLiteral("assignments")).toArray();
  if (target.index < 0 || target.index >= assignments.size()) {
    return false;
  }

  auto assignment = assignments.at(target.index).toObject();
  auto card = assignment.value(QStringLiteral("card")).toObject();
  card.insert(QStringLiteral("macro"), macro);
  assignment.insert(QStringLiteral("card"), card);
  assignments.replace(target.index, assignment);
  profile.insert(QStringLiteral("assignments"), assignments);
  root.insert(target.profileKey, profile);
  return true;
}

AssignmentId assignmentId(const TargetAssignment &target)
{
  return {target.profileKey, target.slotId};
}

QString originalMacrosPath(const QString &databasePath)
{
  return QFileInfo(databasePath).dir().filePath(QStringLiteral("settings.db.deskflow-original-side-buttons.json"));
}

QHash<AssignmentId, QJsonObject> readOriginalMacros(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }

  const auto document = QJsonDocument::fromJson(file.readAll());
  const auto assignments = document.object().value(QStringLiteral("assignments")).toArray();

  QHash<AssignmentId, QJsonObject> macros;
  for (const auto &value : assignments) {
    const auto assignment = value.toObject();
    const auto profileKey = assignment.value(QStringLiteral("profileKey")).toString();
    const auto slotId = assignment.value(QStringLiteral("slotId")).toString();
    const auto macro = assignment.value(QStringLiteral("macro")).toObject();
    if (!profileKey.isEmpty() && !slotId.isEmpty() && !macro.isEmpty()) {
      macros.insert({profileKey, slotId}, macro);
    }
  }

  return macros;
}

bool saveOriginalMacros(
    const QString &path, const QString &databasePath, const QJsonObject &root, const QVector<TargetAssignment> &targets,
    QHash<AssignmentId, QJsonObject> &macros, QString *errorMessage
)
{
  bool changed = false;

  QJsonArray assignments;
  for (auto it = macros.constBegin(); it != macros.constEnd(); ++it) {
    QJsonObject assignment;
    assignment.insert(QStringLiteral("profileKey"), it.key().profileKey);
    assignment.insert(QStringLiteral("slotId"), it.key().slotId);
    assignment.insert(QStringLiteral("macro"), it.value());
    assignments.append(assignment);
  }

  for (const auto &target : targets) {
    if (target.button == nullptr) {
      continue;
    }

    const auto macro = assignmentMacro(root, target);
    const auto id = assignmentId(target);
    if (macros.contains(id) || isPatchedMacro(macro, *target.button)) {
      continue;
    }

    macros.insert(id, macro);
    QJsonObject assignment;
    assignment.insert(QStringLiteral("profileKey"), target.profileKey);
    assignment.insert(QStringLiteral("slotId"), target.slotId);
    assignment.insert(QStringLiteral("macro"), macro);
    assignments.append(assignment);
    changed = true;
  }

  if (!changed && QFileInfo::exists(path)) {
    return true;
  }

  QJsonObject documentRoot;
  documentRoot.insert(QStringLiteral("version"), 1);
  documentRoot.insert(QStringLiteral("settingsDatabase"), databasePath);
  documentRoot.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  documentRoot.insert(QStringLiteral("assignments"), assignments);

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    setError(errorMessage, QStringLiteral("Could not write original Logi Options mapping: %1").arg(file.errorString()));
    return false;
  }

  file.write(QJsonDocument(documentRoot).toJson(QJsonDocument::Indented));
  if (!file.commit()) {
    setError(errorMessage, QStringLiteral("Could not save original Logi Options mapping: %1").arg(file.errorString()));
    return false;
  }

  return true;
}

bool patchSettingsJson(
    QJsonObject &root, const QVector<TargetAssignment> &targets, bool enabled,
    const QHash<AssignmentId, QJsonObject> &originals
)
{
  bool changed = false;

  for (const auto &target : targets) {
    if (target.button == nullptr) {
      continue;
    }

    const auto macro =
        enabled ? patchedMacro(*target.button) : originals.value(assignmentId(target), regularMacro(*target.button));
    if (assignmentMacro(root, target) == macro) {
      continue;
    }

    changed = setAssignmentMacro(root, target, macro) || changed;
  }

  return changed;
}

} // namespace

QString LogiOptionsPlus::settingsDatabasePath() const
{
  return QDir::home().filePath(QStringLiteral("Library/Application Support/LogiOptionsPlus/settings.db"));
}

LogiOptionsPlus::SideButtonStatus LogiOptionsPlus::sideButtonStatus(QString *errorMessage) const
{
  QJsonObject root;
  if (!loadSettings(root, errorMessage)) {
    return {};
  }

  SideButtonStatus status;

  const auto targets = findTargetAssignments(root);
  if (targets.isEmpty()) {
    return status;
  }

  status.supported = true;
  status.enabled = true;
  for (const auto &target : targets) {
    if (target.button == nullptr || !isPatchedMacro(assignmentMacro(root, target), *target.button)) {
      status.enabled = false;
      break;
    }
  }

  return status;
}

bool LogiOptionsPlus::hasSupportedMouse(QString *errorMessage) const
{
  return sideButtonStatus(errorMessage).supported;
}

bool LogiOptionsPlus::sideButtonPatchEnabled(QString *errorMessage) const
{
  const auto status = sideButtonStatus(errorMessage);
  if (!status.supported && errorMessage != nullptr && errorMessage->isEmpty()) {
    setError(errorMessage, QStringLiteral("No supported Logi Options side-button assignments were found"));
  }
  return status.enabled;
}

bool LogiOptionsPlus::setSideButtonPatchEnabled(bool enabled, QString *errorMessage) const
{
  const auto databasePath = settingsDatabasePath();
  QJsonObject root;
  if (!loadSettings(root, errorMessage)) {
    return false;
  }

  const auto targets = findTargetAssignments(root);
  if (targets.isEmpty()) {
    setError(errorMessage, QStringLiteral("No MX Master 3S back/forward assignments were found in Logi Options"));
    return false;
  }

  const auto originalPath = originalMacrosPath(databasePath);
  auto originals = readOriginalMacros(originalPath);
  if (enabled && !saveOriginalMacros(originalPath, databasePath, root, targets, originals, errorMessage)) {
    return false;
  }

  if (!patchSettingsJson(root, targets, enabled, originals)) {
    return true;
  }

  stopServices();
  auto restartServicesOnExit = qScopeGuard([this] { restartServices(); });

  if (!backupDatabase(errorMessage)) {
    return false;
  }

  if (!writeSettingsJson(QJsonDocument(root).toJson(QJsonDocument::Compact), errorMessage)) {
    return false;
  }

  if (!enabled) {
    QFile::remove(originalPath);
  }

  return true;
}

bool LogiOptionsPlus::loadSettings(QJsonObject &root, QString *errorMessage) const
{
  const auto json = readSettingsJson(errorMessage);
  if (json.isEmpty()) {
    return false;
  }

  return parseSettingsJson(json, root, errorMessage);
}

QByteArray LogiOptionsPlus::readSettingsJson(QString *errorMessage) const
{
  const auto databasePath = settingsDatabasePath();
  if (!QFileInfo::exists(databasePath)) {
    setError(errorMessage, QStringLiteral("Logi Options settings database was not found: %1").arg(databasePath));
    return {};
  }

  QByteArray stdoutData;
  const QStringList arguments = {
      QStringLiteral("-batch"),
      QStringLiteral("-noheader"),
      databasePath,
      QStringLiteral("select cast(file as text) from data where _id = 1;"),
  };

  if (!runProcess(sqlitePath(), arguments, &stdoutData, errorMessage)) {
    return {};
  }

  return stdoutData.trimmed();
}

bool LogiOptionsPlus::writeSettingsJson(const QByteArray &json, QString *errorMessage) const
{
  QTemporaryFile file(QDir::temp().filePath(QStringLiteral("deskflow-logi-options-XXXXXX.json")));
  if (!file.open()) {
    setError(errorMessage, QStringLiteral("Could not create a temporary settings file: %1").arg(file.errorString()));
    return false;
  }

  file.write(json);
  if (!file.flush()) {
    setError(errorMessage, QStringLiteral("Could not write temporary settings file: %1").arg(file.errorString()));
    return false;
  }

  const auto sql = QStringLiteral(
                       "BEGIN IMMEDIATE;"
                       "UPDATE data SET file = readfile(%1) WHERE _id = 1;"
                       "COMMIT;"
                       "PRAGMA wal_checkpoint(FULL);"
  )
                       .arg(sqlString(file.fileName()));

  const QStringList arguments = {QStringLiteral("-batch"), settingsDatabasePath(), sql};
  return runProcess(sqlitePath(), arguments, nullptr, errorMessage);
}

bool LogiOptionsPlus::backupDatabase(QString *errorMessage) const
{
  const auto timestamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz"));
  const auto backupPath = QStringLiteral("%1.deskflow-backup-%2").arg(settingsDatabasePath(), timestamp);
  const QStringList arguments = {settingsDatabasePath(), QStringLiteral(".backup %1").arg(sqlString(backupPath))};
  return runProcess(sqlitePath(), arguments, nullptr, errorMessage);
}

void LogiOptionsPlus::stopServices() const
{
  const auto domain = QStringLiteral("gui/%1/com.logi.cp-dev-mgr").arg(getuid());
  runProcessQuiet(launchctlPath(), {QStringLiteral("stop"), domain});
  runProcessQuiet(killallPath(), {QStringLiteral("logioptionsplus_agent")});
  runProcessQuiet(killallPath(), {QStringLiteral("logioptionsplus_updater")});
}

void LogiOptionsPlus::restartServices() const
{
  const auto domain = QStringLiteral("gui/%1/com.logi.cp-dev-mgr").arg(getuid());
  runProcessQuiet(launchctlPath(), {QStringLiteral("kickstart"), QStringLiteral("-k"), domain});
}

} // namespace deskflow::gui
