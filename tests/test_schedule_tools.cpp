#include <QTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

#include "ai/memory/memory_store.h"
#include "ai/tools/schedule_tools.h"
#include "ai/tools/runtime/tool_runtime.h"

namespace {
QJsonObject reminder() {
    return {{"type", "once_at"}, {"title", "喝水"}, {"delay_minutes", 10},
            {"respect_quiet_hours", 0}};
}

MemoryEntry shadow(const MemoryStore& memory, const QString& taskId) {
    for (const auto& entry : memory.all()) {
        if (entry.type == MemoryType::TaskShadow
            && entry.payload.value("linked_task_id").toString() == taskId) return entry;
    }
    return {};
}

int indexJobCount(const MemoryStore& memory) {
    QSqlQuery query(QSqlDatabase::database(memory.databaseConnectionName(), false));
    if (!query.exec("SELECT COUNT(*) FROM memory_index_jobs") || !query.next()) return -1;
    return query.value(0).toInt();
}

struct Fixture {
    QTemporaryDir directory;
    MemoryStore memory;
    AgentScheduler scheduler;
    bool ready = false;
    Fixture() {
        memory.setStoragePath({});
        memory.setDatabasePath(directory.filePath("memory.db"));
        scheduler.setStoragePath(directory.filePath("tasks.json"));
        ready = directory.isValid() && memory.loadDatabaseOnly() && scheduler.load();
        if (ready) connectSchedulerMemory(scheduler, memory);
    }
    QString create() {
        const auto result = ScheduleCreateTool(&scheduler, &memory).execute(reminder());
        return result.success ? result.data.value("task").toObject().value("id").toString() : QString();
    }
};
}

class TestScheduleTools : public QObject {
    Q_OBJECT
private slots:
    void projectionPreservesStagedEntriesAndMetadata();
    void projectionHonorsStagedAndDurableDeletion();
    void replayDoesNotWriteAgain();
    void memoryFailureRetainsOutboxUntilRecovery();
    void failedProjectionWriteRetriesWithoutPartialChanges();
    void listReportsUnavailableStorage();
    void disabledTasksAreArchivedInMemory();
    void durationSchemasMatchSchedulerLimits();
    void listPagesSurviveRuntimeSanitizerWithOneHundredCompletedTasks();
    void snoozeRejectsInvalidMinutes_data();
    void snoozeRejectsInvalidMinutes();
    void snoozeDefaultsOnlyWhenMinutesAreMissing();
};

void TestScheduleTools::projectionPreservesStagedEntriesAndMetadata() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const QString mirrorPath = fixture.directory.filePath("legacy-memory.json");
    fixture.memory.setStoragePath(mirrorPath);
    QFile mirror(mirrorPath);
    QVERIFY(mirror.open(QIODevice::WriteOnly));
    mirror.write("legacy mirror must not be rewritten");
    mirror.close();
    const QString taskId = fixture.create();
    QVERIFY(!taskId.isEmpty());
    MemoryEntry existing;
    existing.type = MemoryType::Semantic;
    existing.summary = "original";
    existing = fixture.memory.addEntry(existing);
    QVERIFY(!existing.id.isEmpty());
    MemoryMutationBatch staged;
    MemoryEntry changed = existing;
    changed.summary = "staged update";
    QVERIFY(fixture.memory.stageEntryUpdate(changed, &staged));
    MemoryEntry addition;
    addition.summary = "staged addition";
    addition = fixture.memory.stageEntry(addition, &staged);
    auto target = shadow(fixture.memory, taskId);
    target.privacyLevel = PrivacyLevel::Sensitive;
    target.payload["local_annotation"] = "keep staged metadata";
    QVERIFY(fixture.memory.stageEntryUpdate(target, &staged));

    const auto result = ScheduleSnoozeTool(&fixture.scheduler, &fixture.memory)
        .execute({{"id", taskId}, {"minutes", 5}});
    QVERIFY(result.success);
    QVERIFY(result.data.value("memory_updated").toBool());
    QVERIFY(fixture.memory.findById(addition.id));
    QCOMPARE(fixture.memory.findById(addition.id)->summary, QString("staged addition"));
    QCOMPARE(fixture.memory.findById(existing.id)->summary, QString("staged update"));
    QCOMPARE(fixture.memory.readForRecall(existing.id)->summary, QString("original"));
    QVERIFY(!fixture.memory.readForRecall(addition.id).has_value());
    QCOMPARE(shadow(fixture.memory, taskId).privacyLevel, PrivacyLevel::Sensitive);
    QCOMPARE(shadow(fixture.memory, taskId).payload.value("local_annotation").toString(),
             QString("keep staged metadata"));
    QVERIFY(mirror.open(QIODevice::ReadOnly));
    QCOMPARE(mirror.readAll(), QByteArray("legacy mirror must not be rewritten"));
}

void TestScheduleTools::projectionHonorsStagedAndDurableDeletion() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const QString first = fixture.create();
    QVERIFY(!first.isEmpty());
    auto forgotten = shadow(fixture.memory, first);
    forgotten.status = MemoryStatus::Deleted;
    MemoryMutationBatch staged;
    QVERIFY(fixture.memory.stageEntryUpdate(forgotten, &staged));
    QVERIFY(ScheduleSnoozeTool(&fixture.scheduler, &fixture.memory)
        .execute({{"id", first}, {"minutes", 5}}).success);
    QCOMPARE(shadow(fixture.memory, first).status, MemoryStatus::Deleted);

    const QString second = fixture.create();
    QVERIFY(!second.isEmpty());
    MemoryStore other;
    other.setStoragePath({});
    other.setDatabasePath(fixture.memory.databasePath());
    QVERIFY(other.loadDatabaseOnly());
    const auto durable = shadow(other, second);
    QVERIFY(!durable.id.isEmpty());
    QVERIFY(other.updateStatusById(durable.id, MemoryStatus::Deleted));
    QCOMPARE(shadow(fixture.memory, second).status, MemoryStatus::Active);
    const int count = fixture.memory.all().size();
    QVERIFY(ScheduleSnoozeTool(&fixture.scheduler, &fixture.memory)
        .execute({{"id", second}, {"minutes", 5}}).success);
    QCOMPARE(fixture.memory.all().size(), count);
    QCOMPARE(shadow(fixture.memory, second).status, MemoryStatus::Deleted);
    QCOMPARE(fixture.memory.readForRecall(durable.id)->status, MemoryStatus::Deleted);
    QCOMPARE(shadow(fixture.memory, first).status, MemoryStatus::Deleted);
}

void TestScheduleTools::replayDoesNotWriteAgain() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const QString taskId = fixture.create();
    QVERIFY(!taskId.isEmpty());
    const auto before = shadow(fixture.memory, taskId);
    const int jobs = indexJobCount(fixture.memory);
    QVERIFY(jobs >= 0);
    // Exercise the actual upsert with an identical projection, not an empty outbox.
    QVERIFY(fixture.memory.synchronizeTaskShadow(before));
    QVERIFY(fixture.memory.synchronizeTaskShadow(before));
    QCOMPARE(indexJobCount(fixture.memory), jobs);
    QCOMPARE(shadow(fixture.memory, taskId).toJson(), before.toJson());
    QCOMPARE(fixture.memory.all().size(), 1);
}

void TestScheduleTools::memoryFailureRetainsOutboxUntilRecovery() {
    QTemporaryDir directory;
    QFile blocker(directory.filePath("blocker"));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.write("not a directory");
    blocker.close();
    MemoryStore memory;
    memory.setStoragePath({});
    memory.setDatabasePath(directory.filePath("blocker/memory.db"));
    AgentScheduler scheduler;
    scheduler.setStoragePath(directory.filePath("tasks.json"));
    QVERIFY(scheduler.load());
    connectSchedulerMemory(scheduler, memory);
    const auto result = ScheduleCreateTool(&scheduler, &memory).execute(reminder());
    QVERIFY(result.success); // The authoritative schedule is durable.
    QVERIFY(!result.data.value("memory_recorded").toBool());
    QVERIFY(!scheduler.synchronizeState());
    QFile persisted(scheduler.storagePath());
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QVERIFY(!QJsonDocument::fromJson(persisted.readAll()).object().value("pending_states").toArray().isEmpty());
    persisted.close();
    QVERIFY(QFile::remove(blocker.fileName()));
    QVERIFY(QDir().mkpath(blocker.fileName()));
    QVERIFY(scheduler.synchronizeState());
    const QString id = result.data.value("task").toObject().value("id").toString();
    QVERIFY(!shadow(memory, id).id.isEmpty());
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(persisted.readAll()).object().value("pending_states").toArray().isEmpty());
}

void TestScheduleTools::listReportsUnavailableStorage() {
    QTemporaryDir directory;
    QFile file(directory.filePath("tasks.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("broken");
    file.close();
    AgentScheduler scheduler;
    scheduler.setStoragePath(file.fileName());
    QVERIFY(!scheduler.load());
    const auto result = ScheduleListTool(&scheduler).execute({});
    QVERIFY(!result.success);
    QVERIFY(!result.errorMessage.isEmpty());
    QVERIFY(!result.data.contains("count"));
}

void TestScheduleTools::failedProjectionWriteRetriesWithoutPartialChanges() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const QString id = fixture.create();
    QVERIFY(!id.isEmpty());
    const auto before = shadow(fixture.memory, id);
    const int jobs = indexJobCount(fixture.memory);
    QSqlQuery query(QSqlDatabase::database(fixture.memory.databaseConnectionName(), false));
    // Repository updates use INSERT OR REPLACE, so reject the insert atomically.
    QVERIFY(query.exec("CREATE TRIGGER reject_task_shadow BEFORE INSERT ON memory_items "
        "WHEN NEW.type='task_shadow' BEGIN SELECT RAISE(FAIL,'injected task shadow failure'); END"));
    const auto result = ScheduleSnoozeTool(&fixture.scheduler, &fixture.memory)
        .execute({{"id", id}, {"minutes", 5}});
    QVERIFY(result.success);
    QVERIFY(!result.data.value("memory_updated").toBool());
    QCOMPARE(shadow(fixture.memory, id).toJson(), before.toJson());
    QCOMPARE(indexJobCount(fixture.memory), jobs);
    QVERIFY(!fixture.scheduler.synchronizeState());
    QVERIFY(query.exec("DROP TRIGGER reject_task_shadow"));
    QVERIFY(fixture.scheduler.synchronizeState());
    QCOMPARE(shadow(fixture.memory, id).payload.value("next_trigger_at").toString(),
        fixture.scheduler.tasks().first().nextTriggerAt.toUTC().toString(Qt::ISODate));
    QCOMPARE(indexJobCount(fixture.memory), jobs + 1);
}

void TestScheduleTools::disabledTasksAreArchivedInMemory() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const QString id = fixture.create();
    auto task = fixture.scheduler.tasks().first();
    task.enabled = false;
    QFile file(fixture.scheduler.storagePath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(QJsonObject{{"version", 2}, {"tasks", QJsonArray{task.toJson()}},
        {"pending_states", QJsonArray{}}}).toJson());
    file.close();
    QVERIFY(fixture.scheduler.load());
    connectSchedulerMemory(fixture.scheduler, fixture.memory);
    const auto entry = shadow(fixture.memory, id);
    QCOMPARE(entry.status, MemoryStatus::Archived);
    QCOMPARE(entry.payload.value("task_status").toString(), QString("disabled"));
    QVERIFY(entry.summary.contains(QStringLiteral("已停用")));
    const auto listed = ScheduleListTool(&fixture.scheduler).execute({});
    QVERIFY(listed.success);
    QCOMPARE(listed.data.value("tasks").toArray().first().toObject().value("status").toString(), QString("disabled"));
    QCOMPARE(ScheduleListTool(&fixture.scheduler).execute({{"include_disabled", 0}}).data.value("count").toInt(), 0);
    QVERIFY(listed.data.contains("recent_completed"));
}

void TestScheduleTools::snoozeRejectsInvalidMinutes_data() {
    QTest::addColumn<QJsonValue>("minutes");
    QTest::newRow("zero") << QJsonValue(0);
    QTest::newRow("negative") << QJsonValue(-1);
    QTest::newRow("fraction") << QJsonValue(1.5);
    QTest::newRow("string") << QJsonValue("5");
    QTest::newRow("null") << QJsonValue(QJsonValue::Null);
    QTest::newRow("unsafe-json-integer") << QJsonValue(9007199254740992.0);
    QTest::newRow("milliseconds-overflow") << QJsonValue(200000000000000.0);
    QTest::newRow("milliseconds-json-limit") << QJsonValue(150119987580.0);
}

void TestScheduleTools::snoozeRejectsInvalidMinutes() {
    QFETCH(QJsonValue, minutes);
    Fixture fixture;
    QVERIFY(fixture.ready);
    const QString id = fixture.create();
    QVERIFY(!id.isEmpty());
    const auto before = fixture.scheduler.tasks().first().toJson();
    ScheduleSnoozeTool tool(&fixture.scheduler, &fixture.memory);
    const QJsonObject args{{"id", id}, {"minutes", minutes}};
    QVERIFY(!tool.validate(args));
    QVERIFY(!tool.execute(args).success);
    QCOMPARE(fixture.scheduler.tasks().first().toJson(), before);
}

void TestScheduleTools::snoozeDefaultsOnlyWhenMinutesAreMissing() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    const QString id = fixture.create();
    const auto before = QDateTime::currentDateTimeUtc();
    ScheduleSnoozeTool tool(&fixture.scheduler, &fixture.memory);
    QVERIFY(tool.validate({{"id", id}}));
    const auto result = tool.execute({{"id", id}});
    QVERIFY(result.success);
    QCOMPARE(result.data.value("minutes").toInt(), 10);
    const auto delay = before.secsTo(fixture.scheduler.tasks().first().nextTriggerAt);
    QVERIFY(delay >= 599 && delay <= 601);
}

void TestScheduleTools::durationSchemasMatchSchedulerLimits() {
    Fixture fixture;
    QVERIFY(fixture.ready);
    constexpr qint64 maxMinutes = 9007199254740991LL / 60000;
    ScheduleCreateTool create(&fixture.scheduler, &fixture.memory);
    const auto schema = create.parameterSchema();
    const auto properties = schema.value("properties").toObject();
    for (const QString name : {QStringLiteral("delay_minutes"), QStringLiteral("interval_minutes"),
                               QStringLiteral("interval_ms")}) {
        const auto duration = properties.value(name).toObject();
        QVERIFY(!duration.contains("default"));
        QCOMPARE(duration.value("minimum").toDouble(), name == "interval_ms" ? 60000.0 : 1.0);
        QCOMPARE(duration.value("maximum").toDouble(), name == "interval_ms"
            ? 9007199254740991.0 : static_cast<double>(maxMinutes));
    }
    QCOMPARE(schema.value("oneOf").toArray().size(), 3);
    ScheduleSnoozeTool snooze(&fixture.scheduler, &fixture.memory);
    QCOMPARE(snooze.parameterSchema().value("properties").toObject().value("minutes").toObject()
        .value("maximum").toDouble(), static_cast<double>(maxMinutes));
    QVERIFY(snooze.validate({{"id", "test"}, {"minutes", maxMinutes}}));
    QVERIFY(!snooze.validate({{"id", "test"}, {"minutes", maxMinutes + 1}}));
    const auto created = create.execute(reminder());
    QVERIFY(created.success);
    const auto task = created.data.value("task").toObject();
    QCOMPARE(task.value("trigger").toObject().value("type").toString(), QString("once_at"));
    QVERIFY(task.value("policy").isObject());
}

void TestScheduleTools::listPagesSurviveRuntimeSanitizerWithOneHundredCompletedTasks() {
    QTemporaryDir directory;
    const auto now = QDateTime::currentDateTimeUtc();
    const QString longDisplay(1000, QLatin1Char('\\'));
    QJsonArray current;
    QJsonArray history;
    ScheduledTask task;
    task.title = task.message = task.description = longDisplay;
    task.triggerType = "once_at";
    task.onceAt = now.addSecs(3600);
    task.createdAt = task.updatedAt = now;
    task.nextTriggerAt = task.onceAt;
    task.respectQuietHours = false;
    for (int i = 0; i < 8; ++i) {
        task.id = QString("current-%1").arg(i);
        current.append(task.toJson());
    }
    task.enabled = false;
    task.nextTriggerAt = {};
    task.onceAt = now.addSecs(-200);
    for (int i = 0; i < 100; ++i) {
        task.id = QString("completed-%1").arg(i);
        task.lastTriggeredAt = now.addSecs(i - 100);
        history.append(task.toJson());
    }
    QFile file(directory.filePath("tasks.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{{"version", 2}, {"tasks", current},
        {"completed_tasks", history}, {"pending_states", QJsonArray{}}}).toJson());
    file.close();
    AgentScheduler scheduler;
    scheduler.setStoragePath(file.fileName());
    QVERIFY(scheduler.load());
    ToolRegistry registry;
    registry.registerTool(std::make_unique<ScheduleListTool>(&scheduler));
    ToolRuntime runtime;
    runtime.setToolRegistry(&registry);
    const auto queryPage = [&](const QJsonObject& arguments) {
        ToolExecutionRequest request;
        request.toolName = "schedule_list";
        request.arguments = arguments;
        request.policyContext.initiatedByLlm = true;
        request.policyContext.triggerTag = "user_request";
        const auto outcome = runtime.execute(request);
        return qMakePair(outcome, runtime.sanitizer()->toPayload(outcome.result));
    };
    const auto first = queryPage({});
    QVERIFY(first.first.executed);
    QVERIFY(first.first.result.success);
    QVERIFY(first.second.size() < runtime.sanitizer()->maxPayloadLength());
    const auto firstData = QJsonDocument::fromJson(first.second.toUtf8()).object().value("data").toObject();
    QVERIFY(!firstData.value("truncated").toBool());
    QCOMPARE(firstData.value("total_count").toInt(), 8);
    QCOMPARE(firstData.value("recent_completed_total").toInt(), 100);
    QVERIFY(!firstData.value("tasks").toArray().isEmpty());
    QVERIFY(!firstData.value("recent_completed").toArray().isEmpty());
    QCOMPARE(firstData.value("recent_completed").toArray().first().toObject().value("id").toString(),
             QString("completed-99"));
    QVERIFY(firstData.value("next_offset").toInt() > 0);
    QVERIFY(firstData.value("completed_next_offset").toInt() > 0);

    // The caller can independently walk either stream, even when requesting a
    // large page that the character budget must reduce to a few complete rows.
    for (bool completedStream : {false, true}) {
        QSet<QString> seen;
        int offset = 0;
        for (int page = 0; page < 100; ++page) {
            const auto response = queryPage(completedStream
                ? QJsonObject{{"limit", 0}, {"completed_limit", 100}, {"completed_offset", offset}}
                : QJsonObject{{"limit", 100}, {"offset", offset}, {"completed_limit", 0}});
            QVERIFY(response.first.result.success);
            QVERIFY(response.second.size() < runtime.sanitizer()->maxPayloadLength());
            const auto data = QJsonDocument::fromJson(response.second.toUtf8()).object().value("data").toObject();
            QVERIFY(!data.value("truncated").toBool());
            const auto rows = data.value(completedStream ? "recent_completed" : "tasks").toArray();
            QVERIFY(!rows.isEmpty());
            for (const auto& row : rows) {
                const auto id = row.toObject().value("id").toString();
                QVERIFY(!id.isEmpty());
                QVERIFY(!seen.contains(id));
                seen.insert(id);
            }
            if (!data.value(completedStream ? "completed_has_more" : "has_more").toBool()) break;
            const int next = data.value(completedStream ? "completed_next_offset" : "next_offset").toInt();
            QVERIFY(next > offset);
            offset = next;
        }
        QCOMPARE(seen.size(), completedStream ? 100 : 8);
        QVERIFY(seen.contains(completedStream ? "completed-0" : "current-0"));
    }
    // Only list presentation is bounded; neither scheduler state nor the
    // identifiers used to snooze the oldest record have been changed.
    QCOMPARE(scheduler.completedTasks().first().title, longDisplay);
    QVERIFY(ScheduleSnoozeTool(&scheduler).execute({{"id", "completed-0"}, {"minutes", 5}}).success);
}

QTEST_GUILESS_MAIN(TestScheduleTools)
#include "test_schedule_tools.moc"
