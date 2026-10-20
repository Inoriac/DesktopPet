#include "statistic_manager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QUuid>

namespace {
#define USAGE_FIELDS(X) \
    X(callCount) X(successCount) X(failureCount) X(cancelledCount) X(unknownUsageCount) \
    X(promptTokens) X(completionTokens) X(totalTokens) X(reasoningTokens) X(cachedTokens) \
    X(promptCacheHitTokens) X(promptCacheMissTokens)

QJsonObject countersJson(const LlmStatistics& counters) {
    QJsonObject result;
#define WRITE_FIELD(name) result[QStringLiteral(#name)] = counters.name;
    USAGE_FIELDS(WRITE_FIELD)
#undef WRITE_FIELD
    return result;
}

LlmStatistics readCounters(const QJsonObject& value) {
    LlmStatistics counters;
#define READ_FIELD(name) counters.name = value.value(QStringLiteral(#name)).toInteger();
    USAGE_FIELDS(READ_FIELD)
#undef READ_FIELD
    return counters;
}

void addCounters(LlmStatistics& destination, const LlmStatistics& value) {
#define ADD_FIELD(name) destination.name += value.name;
    USAGE_FIELDS(ADD_FIELD)
#undef ADD_FIELD
}
#undef USAGE_FIELDS

LlmStatistics countersFor(const PetStatistics& pet) {
    return {pet.llmCallCount, pet.llmSuccessCount, pet.llmFailureCount,
            pet.llmCancelledCount, pet.llmUnknownUsageCount, pet.llmPromptTokens,
            pet.llmCompletionTokens, pet.llmTotalTokens, pet.llmReasoningTokens,
            pet.llmCachedTokens, pet.llmPromptCacheHitTokens, pet.llmPromptCacheMissTokens};
}

void setCounters(PetStatistics& pet, const LlmStatistics& value) {
    pet.llmCallCount = value.callCount;
    pet.llmSuccessCount = value.successCount;
    pet.llmFailureCount = value.failureCount;
    pet.llmCancelledCount = value.cancelledCount;
    pet.llmUnknownUsageCount = value.unknownUsageCount;
    pet.llmPromptTokens = value.promptTokens;
    pet.llmCompletionTokens = value.completionTokens;
    pet.llmTotalTokens = value.totalTokens;
    pet.llmReasoningTokens = value.reasoningTokens;
    pet.llmCachedTokens = value.cachedTokens;
    pet.llmPromptCacheHitTokens = value.promptCacheHitTokens;
    pet.llmPromptCacheMissTokens = value.promptCacheMissTokens;
}

PetStatistics legacyPet(const QJsonObject& value) {
    PetStatistics pet(value.value("petName").toString());
    pet.sessionCount = value.value("sessionCount").toInt();
    pet.lastActiveTime = QDateTime::fromString(value.value("lastActiveTime").toString(), Qt::ISODate);
    pet.totalRuntimeMs = value.value("totalRuntimeMs").toInteger();
    pet.llmCallCount = value.value("llmCallCount").toInteger();
    pet.llmSuccessCount = value.contains("llmSuccessCount")
        ? value.value("llmSuccessCount").toInteger() : pet.llmCallCount;
    pet.llmFailureCount = value.value("llmFailureCount").toInteger();
    pet.llmCancelledCount = value.value("llmCancelledCount").toInteger();
    pet.llmUnknownUsageCount = value.value("llmUnknownUsageCount").toInteger();
    pet.llmPromptTokens = value.value("llmPromptTokens").toInteger();
    pet.llmCompletionTokens = value.value("llmCompletionTokens").toInteger();
    pet.llmTotalTokens = value.value("llmTotalTokens").toInteger();
    pet.llmReasoningTokens = value.value("llmReasoningTokens").toInteger();
    pet.llmCachedTokens = value.value("llmCachedTokens").toInteger();
    pet.llmPromptCacheHitTokens = value.value("llmPromptCacheHitTokens").toInteger();
    pet.llmPromptCacheMissTokens = value.value("llmPromptCacheMissTokens").toInteger();
    const auto touches = value.value("touchAreaCount").toObject();
    for (auto it = touches.begin(); it != touches.end(); ++it)
        pet.touchAreaCount[it.key()] = it.value().toInt();
    return pet;
}

QJsonObject touchesJson(const PetStatistics& pet) {
    QJsonObject result;
    for (auto it = pet.touchAreaCount.begin(); it != pet.touchAreaCount.end(); ++it)
        result[it.key()] = it.value();
    return result;
}

void trimDays(QMap<QString, LlmStatistics>& daily, const QDateTime& now) {
    const QString oldest = now.toLocalTime().date().addDays(-89).toString(Qt::ISODate);
    while (!daily.isEmpty() && daily.firstKey() < oldest)
        daily.erase(daily.begin());
    while (daily.size() > 90)
        daily.erase(daily.begin());
}
} // namespace

StatisticManager& StatisticManager::getInstance() {
    static StatisticManager instance;
    return instance;
}

StatisticManager::~StatisticManager() {
    saveStatistics();
}

void StatisticManager::initialize(const QString& savePath, int interval) {
    configure(savePath, interval, {}, {});
}

void StatisticManager::initializeForProfile(const QString& appDataRoot, const QString& profileId,
                                             const QString& petName, const QString& legacyPath,
                                             bool allowLegacyImport) {
    const QUuid uuid(profileId);
    const bool validId = !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces).compare(profileId, Qt::CaseInsensitive) == 0;
    const QString path = validId
        ? QDir(appDataRoot).filePath("profiles/" + profileId + "/statistics.json") : QString();
    configure(path, 10, profileId, petName);
    QMutexLocker lock(&dataMutex);
    if (!validId) {
        persistenceBlocked = true;
        storageWarning = QStringLiteral("Invalid statistics profile identity; using memory only.");
        return;
    }
    if (!QFile::exists(filePath) && !persistenceBlocked && allowLegacyImport)
        importLegacy(legacyPath);
}

void StatisticManager::configure(const QString& savePath, int interval,
                                  const QString& profileId, const QString& petName) {
    {
        QMutexLocker lock(&dataMutex);
        if (autoSaveTimer) autoSaveTimer->stop();
        writerLock.reset();
        petStatisticsMap.clear();
        usageHistories.clear();
        runningTimers.clear();
        eventSlots.clear();
        storageWarning.clear();
        identityWarning.clear();
        persistenceBlocked = false;
        filePath = savePath;
        activeProfileId = profileId;
        activePetName = petName;
        autoSaveInterval = interval;
        if (!filePath.isEmpty()) {
            if (!QFileInfo(filePath).dir().mkpath(".")) {
                persistenceBlocked = true;
                storageWarning = QStringLiteral("Cannot create the statistics directory; using memory only.");
            } else if (!activeProfileId.isEmpty()) {
                writerLock = std::make_unique<QLockFile>(filePath + ".lock");
                if (!writerLock->tryLock(0)) {
                    persistenceBlocked = true;
                    storageWarning = QStringLiteral("Statistics are in use by another process; this run is memory only.");
                }
            }
        }
        loadFromFile();
        if (!autoSaveTimer) {
            autoSaveTimer = new QTimer(this);
            connect(autoSaveTimer, &QTimer::timeout, this, &StatisticManager::onAutoSaveTimer);
        }
        if (!runtimeUpdateTimer) {
            runtimeUpdateTimer = new QTimer(this);
            connect(runtimeUpdateTimer, &QTimer::timeout, this, &StatisticManager::onRuntimeUpdateTimer);
        }
        runtimeUpdateTimer->start(1000);
        if (autoSaveEnabled && autoSaveInterval > 0)
            autoSaveTimer->start(autoSaveInterval * 1000);
    }
    registerEventSlot(StatisticEventType::PET_START, [this](const StatisticEvent& event) {
        PetStatistics current;
        {
            QMutexLocker lock(&dataMutex);
            const QString key = statisticsKey(event.petName);
            ensurePetStatistics(key);
            auto& pet = petStatisticsMap[key];
            if (!pet.isRunning) {
                pet.startTime = QDateTime::currentDateTime();
                pet.lastActiveTime = pet.startTime;
                pet.sessionRuntimeMs = 0;
                ++pet.sessionCount;
                pet.isRunning = true;
                runningTimers[key].start();
            }
            current = snapshot(key);
        }
        emit statisticsUpdated(current.petName, current);
    });
    registerEventSlot(StatisticEventType::PET_STOP, [this](const StatisticEvent& event) {
        PetStatistics current;
        {
            QMutexLocker lock(&dataMutex);
            const QString key = statisticsKey(event.petName);
            ensurePetStatistics(key);
            auto& pet = petStatisticsMap[key];
            if (pet.isRunning) {
                pet.sessionRuntimeMs = runningTimers.value(key).elapsed();
                pet.totalRuntimeMs += pet.sessionRuntimeMs;
                runningTimers.remove(key);
                pet.startTime = {};
                pet.isRunning = false;
            }
            pet.lastActiveTime = QDateTime::currentDateTime();
            current = pet;
        }
        emit statisticsUpdated(current.petName, current);
    });
    registerEventSlot(StatisticEventType::BODY_PART_TOUCH, [this](const StatisticEvent& event) {
        PetStatistics current;
        {
            QMutexLocker lock(&dataMutex);
            const QString key = statisticsKey(event.petName);
            ensurePetStatistics(key);
            auto& pet = petStatisticsMap[key];
            pet.lastActiveTime = QDateTime::currentDateTime();
            ++pet.touchAreaCount[event.areaName];
            current = snapshot(key);
        }
        emit statisticsUpdated(current.petName, current);
    });
}

QString StatisticManager::statisticsKey(const QString& name) const {
    return activeProfileId.isEmpty() ? (name.isEmpty() ? QStringLiteral("AI_GLOBAL") : name)
                                     : activePetName;
}

void StatisticManager::emitStatisticEvent(const StatisticEvent& event) {
    QList<std::function<void(const StatisticEvent&)>> callbacks;
    {
        QMutexLocker lock(&dataMutex);
        callbacks = eventSlots.value(event.type);
    }
    for (const auto& callback : callbacks) callback(event);
    emit statisticEventOccurred(event);
}

void StatisticManager::recordPetStart(const QString& name) {
    if (!name.isEmpty()) emitStatisticEvent({StatisticEventType::PET_START, name, {}});
}

void StatisticManager::recordPetStop(const QString& name) {
    if (!name.isEmpty()) emitStatisticEvent({StatisticEventType::PET_STOP, name, {}});
}

void StatisticManager::recordTouchInteraction(const QString& name, const QString& area) {
    if (!name.isEmpty()) emitStatisticEvent({StatisticEventType::BODY_PART_TOUCH, name, area});
}

void StatisticManager::recordLlmCall(const QString& name, bool success, const LlmUsage& usage) {
    ModelAttemptReport report;
    report.attemptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    report.petName = name;
    report.success = success;
    report.usage = usage;
    report.completedAt = QDateTime::currentDateTimeUtc();
    recordLlmAttempt(report);
}

void StatisticManager::recordLlmUsage(const QString& name, const LlmUsage& usage) {
    recordLlmCall(name, true, usage);
}

void StatisticManager::recordLlmAttempt(const ModelAttemptReport& report) {
    PetStatistics current;
    {
        QMutexLocker lock(&dataMutex);
        if (!activeProfileId.isEmpty() && !report.profileId.isEmpty()
            && activeProfileId != report.profileId) {
            identityWarning = QStringLiteral("Ignored model usage from a different profile.");
            qWarning() << "[Statistics]" << identityWarning;
            return;
        }
        const QString key = statisticsKey(report.petName);
        ensurePetStatistics(key);
        auto& pet = petStatisticsMap[key];
        auto& history = usageHistories[key];
        const QDateTime at = report.completedAt.isValid() ? report.completedAt : QDateTime::currentDateTimeUtc();
        const auto& usage = report.usage;
        const bool reported = usage.hasReportedUsage();
        LlmStatistics added;
        added.callCount = 1;
        added.cancelledCount = report.cancelled ? 1 : 0;
        added.successCount = !report.cancelled && report.success ? 1 : 0;
        added.failureCount = !report.cancelled && !report.success ? 1 : 0;
        added.unknownUsageCount = reported ? 0 : 1;
        added.promptTokens = usage.promptTokens;
        added.completionTokens = usage.completionTokens;
        added.totalTokens = !usage.reported && usage.totalTokens == 0
            ? usage.promptTokens + usage.completionTokens : usage.totalTokens;
        added.reasoningTokens = usage.reasoningTokens;
        added.cachedTokens = usage.cachedTokens;
        added.promptCacheHitTokens = usage.promptCacheHitTokens;
        added.promptCacheMissTokens = usage.promptCacheMissTokens;
        auto all = countersFor(pet);
        addCounters(all, added);
        setCounters(pet, all);
        pet.lastActiveTime = at;
        addCounters(history.session, added);
        addCounters(history.daily[at.toLocalTime().date().toString(Qt::ISODate)], added);
        trimDays(history.daily, QDateTime::currentDateTime());
        const QString role = report.role.isEmpty() ? QStringLiteral("unclassified") : report.role;
        const QString modelKey = QString::fromUtf8(QJsonDocument(QJsonArray{role, report.provider, report.model}).toJson(QJsonDocument::Compact));
        auto& model = history.byModel[modelKey];
        model.role = role;
        model.provider = report.provider;
        model.model = report.model;
        addCounters(model.counters, added);
        QJsonObject tokens = countersJson(added);
        for (const auto* field : {"callCount", "successCount", "failureCount", "cancelledCount", "unknownUsageCount"})
            tokens.remove(QLatin1String(field));
        const QJsonObject recent{
            {"attemptId", report.attemptId}, {"completedAt", at.toUTC().toString(Qt::ISODateWithMs)},
            {"role", role}, {"provider", report.provider}, {"model", report.model},
            {"routeId", report.routeId}, {"success", report.success && !report.cancelled},
            {"cancelled", report.cancelled}, {"durationMs", report.durationMs},
            {"result", report.cancelled ? "cancelled" : report.success ? "success" : "failure"},
            {"usageReported", reported}, {"usage", tokens}};
        // Completion callbacks can arrive on different threads or in a different order.
        qsizetype index = 0;
        const QString timestamp = recent.value("completedAt").toString();
        while (index < history.recentCalls.size()
               && history.recentCalls.at(index).toObject().value("completedAt").toString() > timestamp)
            ++index;
        history.recentCalls.insert(index, recent);
        while (history.recentCalls.size() > 200) history.recentCalls.removeLast();
        current = snapshot(key);
    }
    emit statisticsUpdated(current.petName, current);
}

PetStatistics StatisticManager::snapshot(const QString& name) const {
    auto pet = petStatisticsMap.value(name, PetStatistics(name));
    const auto timer = runningTimers.constFind(name);
    if (pet.isRunning && timer != runningTimers.constEnd()) {
        pet.sessionRuntimeMs = timer->elapsed();
        pet.totalRuntimeMs += pet.sessionRuntimeMs;
    }
    return pet;
}

std::optional<PetStatistics> StatisticManager::getPetStatistics(const QString& name) {
    QMutexLocker lock(&dataMutex);
    if (!petStatisticsMap.contains(name)) return std::nullopt;
    return snapshot(name);
}

QHash<QString, PetStatistics> StatisticManager::getAllPetStatistics() {
    QMutexLocker lock(&dataMutex);
    auto result = petStatisticsMap;
    for (auto it = result.begin(); it != result.end(); ++it) it.value() = snapshot(it.key());
    return result;
}

QJsonObject StatisticManager::overview(const QDateTime& now) const {
    QMutexLocker lock(&dataMutex);
    const QString key = !activeProfileId.isEmpty() ? activePetName
        : petStatisticsMap.size() == 1 ? petStatisticsMap.cbegin().key() : QStringLiteral("AI_GLOBAL");
    return overviewLocked(key, now);
}

QString StatisticManager::persistenceWarning() const {
    QMutexLocker lock(&dataMutex);
    return storageWarning.isEmpty() ? identityWarning : storageWarning;
}

QJsonObject StatisticManager::overviewLocked(const QString& key, const QDateTime& now) const {
    const auto pet = snapshot(key);
    const auto history = usageHistories.value(key);
    QJsonArray daily;
    const QString oldest = now.toLocalTime().date().addDays(-89).toString(Qt::ISODate);
    for (auto it = history.daily.begin(); it != history.daily.end(); ++it) {
        if (it.key() < oldest) continue;
        auto day = countersJson(it.value());
        day["date"] = it.key();
        daily.append(day);
    }
    QJsonArray models;
    for (const auto& model : history.byModel) {
        auto value = countersJson(model.counters);
        value["role"] = model.role;
        value["provider"] = model.provider;
        value["model"] = model.model;
        models.append(value);
    }
    qint64 touchCount = 0;
    for (int value : pet.touchAreaCount) touchCount += value;
    return {{"allTime", countersJson(countersFor(pet))},
            {"today", countersJson(history.daily.value(now.toLocalTime().date().toString(Qt::ISODate)))},
            {"session", countersJson(history.session)}, {"daily", daily}, {"byModel", models},
            {"recentCalls", history.recentCalls}, {"legacyImported", history.legacyImported},
            {"persistenceWarning", storageWarning.isEmpty() ? identityWarning : storageWarning},
            {"uptime", QJsonObject{{"totalMs", pet.totalRuntimeMs}, {"sessionMs", pet.sessionRuntimeMs},
                {"sessionCount", pet.sessionCount}, {"touchCount", touchCount},
                {"touchAreaCount", touchesJson(pet)}}}};
}

void StatisticManager::saveStatistics() {
    QMutexLocker lock(&dataMutex);
    saveToFile();
}

void StatisticManager::loadStatistics() {
    QMutexLocker lock(&dataMutex);
    loadFromFile();
}

void StatisticManager::clearStatistics(const QString& name) {
    QMutexLocker lock(&dataMutex);
    if (name.isEmpty()) {
        petStatisticsMap.clear();
        usageHistories.clear();
        runningTimers.clear();
    } else {
        petStatisticsMap.remove(name);
        usageHistories.remove(name);
        runningTimers.remove(name);
    }
    saveToFile();
}

void StatisticManager::setAutoSaveEnabled(bool enabled) {
    QMutexLocker lock(&dataMutex);
    autoSaveEnabled = enabled;
    if (!autoSaveTimer) return;
    if (enabled && autoSaveInterval > 0) autoSaveTimer->start(autoSaveInterval * 1000);
    else autoSaveTimer->stop();
}

void StatisticManager::setAutoSaveInterval(int seconds) {
    QMutexLocker lock(&dataMutex);
    autoSaveInterval = seconds;
    if (!autoSaveTimer) return;
    if (autoSaveEnabled && seconds > 0) autoSaveTimer->start(seconds * 1000);
    else autoSaveTimer->stop();
}

void StatisticManager::onAutoSaveTimer() { saveStatistics(); }
void StatisticManager::onRuntimeUpdateTimer() {
    QMutexLocker lock(&dataMutex);
    for (auto it = runningTimers.begin(); it != runningTimers.end(); ++it)
        petStatisticsMap[it.key()].sessionRuntimeMs = it.value().elapsed();
}

void StatisticManager::ensurePetStatistics(const QString& name) {
    if (!petStatisticsMap.contains(name)) petStatisticsMap.insert(name, PetStatistics(name));
}

void StatisticManager::saveToFile() {
    if (filePath.isEmpty() || persistenceBlocked) return;
    QSaveFile file(filePath);
    const QByteArray payload = QJsonDocument(statisticsToJson()).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() || !file.commit()) {
        storageWarning = QStringLiteral("Statistics could not be saved: ") + file.errorString();
        qWarning() << "[Statistics]" << storageWarning;
    } else storageWarning.clear();
}

void StatisticManager::loadFromFile() {
    if (filePath.isEmpty() || !QFile::exists(filePath)) return;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        persistenceBlocked = true;
        storageWarning = QStringLiteral("Statistics could not be read; the original file is preserved.");
        return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto root = document.object();
    bool valid = error.error == QJsonParseError::NoError && document.isObject();
    if (!activeProfileId.isEmpty()) {
        const auto statistics = root.value("statistics").toObject();
        const auto all = statistics.value("allTime").toObject();
        const auto uptime = statistics.value("uptime").toObject();
        valid = valid && root.value("version").toInt() == 2
            && root.value("profileId").toString() == activeProfileId
            && root.value("statistics").isObject() && all.value("callCount").isDouble()
            && all.value("totalTokens").isDouble() && uptime.value("totalMs").isDouble()
            && statistics.value("daily").isArray() && statistics.value("byModel").isArray()
            && statistics.value("recentCalls").isArray();
    } else valid = valid && root.value("pets").isArray();
    if (!valid) {
        persistenceBlocked = true;
        storageWarning = QStringLiteral("Statistics format or profile identity is invalid; the original file is preserved.");
        return;
    }
    jsonToStatistics(root);
}

QJsonObject StatisticManager::statisticsToJson() {
    const auto now = QDateTime::currentDateTimeUtc();
    if (!activeProfileId.isEmpty())
        return {{"version", 2}, {"profileId", activeProfileId}, {"petName", activePetName},
                {"lastSaved", now.toString(Qt::ISODateWithMs)},
                {"statistics", overviewLocked(activePetName, now)}};
    QJsonArray pets;
    for (const auto& key : petStatisticsMap.keys()) {
        const auto pet = snapshot(key);
        pets.append(QJsonObject{{"petName", pet.petName}, {"sessionCount", pet.sessionCount},
            {"lastActiveTime", pet.lastActiveTime.toString(Qt::ISODate)},
            {"totalRuntimeMs", pet.totalRuntimeMs}, {"sessionRuntimeMs", pet.sessionRuntimeMs},
            {"llmCallCount", pet.llmCallCount}, {"llmSuccessCount", pet.llmSuccessCount},
            {"llmFailureCount", pet.llmFailureCount}, {"llmCancelledCount", pet.llmCancelledCount},
            {"llmUnknownUsageCount", pet.llmUnknownUsageCount}, {"llmPromptTokens", pet.llmPromptTokens},
            {"llmCompletionTokens", pet.llmCompletionTokens}, {"llmTotalTokens", pet.llmTotalTokens},
            {"llmReasoningTokens", pet.llmReasoningTokens}, {"llmCachedTokens", pet.llmCachedTokens},
            {"llmPromptCacheHitTokens", pet.llmPromptCacheHitTokens},
            {"llmPromptCacheMissTokens", pet.llmPromptCacheMissTokens},
            {"touchAreaCount", touchesJson(pet)}});
    }
    return {{"pets", pets}, {"version", "1.0"}, {"lastSaved", now.toString(Qt::ISODateWithMs)}};
}

void StatisticManager::jsonToStatistics(const QJsonObject& root) {
    petStatisticsMap.clear();
    usageHistories.clear();
    runningTimers.clear();
    if (activeProfileId.isEmpty()) {
        for (const auto& value : root.value("pets").toArray()) {
            auto pet = legacyPet(value.toObject());
            if (!pet.petName.isEmpty()) petStatisticsMap.insert(pet.petName, pet);
        }
        return;
    }
    const auto statistics = root.value("statistics").toObject();
    PetStatistics pet(activePetName);
    setCounters(pet, readCounters(statistics.value("allTime").toObject()));
    const auto uptime = statistics.value("uptime").toObject();
    pet.totalRuntimeMs = uptime.value("totalMs").toInteger();
    pet.sessionCount = uptime.value("sessionCount").toInt();
    const auto touches = uptime.value("touchAreaCount").toObject();
    for (auto it = touches.begin(); it != touches.end(); ++it) pet.touchAreaCount[it.key()] = it.value().toInt();
    petStatisticsMap.insert(activePetName, pet);
    auto& history = usageHistories[activePetName];
    history.legacyImported = statistics.value("legacyImported").toBool();
    for (const auto& value : statistics.value("daily").toArray()) {
        const auto day = value.toObject();
        const auto date = day.value("date").toString();
        if (QDate::fromString(date, Qt::ISODate).isValid()) history.daily[date] = readCounters(day);
    }
    trimDays(history.daily, QDateTime::currentDateTime());
    for (const auto& value : statistics.value("byModel").toArray()) {
        const auto row = value.toObject();
        ModelUsage model{row.value("role").toString(), row.value("provider").toString(), row.value("model").toString(), readCounters(row)};
        const QString key = QString::fromUtf8(QJsonDocument(QJsonArray{model.role, model.provider, model.model}).toJson(QJsonDocument::Compact));
        history.byModel[key] = model;
    }
    history.recentCalls = statistics.value("recentCalls").toArray();
    while (history.recentCalls.size() > 200) history.recentCalls.removeLast();
}

void StatisticManager::importLegacy(const QString& path) {
    if (path.isEmpty() || !QFile::exists(path)) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        storageWarning = QStringLiteral("Legacy statistics could not be read; no import was performed.");
        return;
    }
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    if (!root.value("pets").isArray()) {
        storageWarning = QStringLiteral("Legacy statistics are invalid; no import was performed.");
        return;
    }
    QList<PetStatistics> matching;
    for (const auto& value : root.value("pets").toArray()) {
        const auto pet = legacyPet(value.toObject());
        if (pet.petName == activePetName) matching.append(pet);
    }
    if (matching.size() == 1) {
        petStatisticsMap[activePetName] = matching.first();
        usageHistories[activePetName].legacyImported = true;
    }
}
