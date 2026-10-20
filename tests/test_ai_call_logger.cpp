#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "ai/ai_call_logger.h"

namespace {
QList<QJsonObject> readRecords(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QList<QJsonObject> result;
    while (!file.atEnd()) result.append(QJsonDocument::fromJson(file.readLine()).object());
    return result;
}
QJsonObject record(int sequence) {
    return {{"type", "request"}, {"request_id", QString::number(sequence)},
            {"content", QString(300, 'x')}};
}
}

class TestAiCallLogger : public QObject {
    Q_OBJECT
private slots:
    void requestAndResponseKeepPairIdsInSeparateProfileLogs();
    void rotatesFilesWithinSizeAndBackupLimit();
    void oversizedRecordUsesTruthfulMetadata();
    void writeAndRotationFailuresAreReported();
};

void TestAiCallLogger::requestAndResponseKeepPairIdsInSeparateProfileLogs() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto firstPath = directory.filePath("profiles/first/logs/ai_calls.jsonl");
    const auto secondPath = directory.filePath("profiles/second/logs/ai_calls.jsonl");
    AiCallLogger logger(firstPath);
    ChatMessage message;
    message.role = "user";
    message.content = "synthetic question";
    QVERIFY(logger.appendRecord(AiCallLogger::requestRecord("request-1", "first", "user", "manual", 0, {message}, {})));
    LlmResponse response;
    response.id = "response-1";
    response.content = "synthetic answer";
    response.usage.totalTokens = 13;
    QVERIFY(logger.appendRecord(AiCallLogger::responseRecord("request-1", "first", true, response, {})));
    logger.setLogFilePath(secondPath);
    QVERIFY(logger.appendRecord(AiCallLogger::requestRecord("request-2", "second", "user", "manual", 0, {message}, {})));
    const auto first = readRecords(firstPath);
    const auto second = readRecords(secondPath);
    QCOMPARE(first.size(), 2);
    QCOMPARE(first[0]["request_id"].toString(), QString("request-1"));
    QCOMPARE(first[1]["request_id"], first[0]["request_id"]);
    QCOMPARE(first[0]["messages"].toArray()[0].toObject()["content"].toString(), message.content);
    QCOMPARE(first[1]["content"].toString(), response.content);
    QCOMPARE(first[1]["usage"].toObject()["total_tokens"].toInt(), 13);
    QVERIFY(first[1]["usage_reported"].toBool());
    LlmResponse zeroUsage;
    QVERIFY(!AiCallLogger::responseRecord("unknown", "first", false, zeroUsage, {})["usage_reported"].toBool());
    zeroUsage.usage.reported = true;
    QVERIFY(AiCallLogger::responseRecord("zero", "first", true, zeroUsage, {})["usage_reported"].toBool());
    QCOMPARE(second.size(), 1);
    QCOMPARE(second[0]["request_id"].toString(), QString("request-2"));
}

void TestAiCallLogger::rotatesFilesWithinSizeAndBackupLimit() {
    QTemporaryDir directory;
    const auto path = directory.filePath("ai_calls.jsonl");
    AiCallLogger logger(path, 512, 3);
    for (int i = 0; i < 7; ++i) QVERIFY(logger.appendRecord(record(i)));
    for (int backup = 0; backup <= 3; ++backup) {
        const auto filePath = backup == 0 ? path : path + '.' + QString::number(backup);
        QVERIFY(QFileInfo(filePath).size() <= 512);
        const auto lines = readRecords(filePath);
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines[0]["request_id"].toString(), QString::number(6 - backup));
    }
    QVERIFY(!QFileInfo::exists(path + ".4"));
}

void TestAiCallLogger::oversizedRecordUsesTruthfulMetadata() {
    QTemporaryDir directory;
    const auto path = directory.filePath("ai_calls.jsonl");
    AiCallLogger logger(path, 512, 3);
    const QJsonObject large{{"type", "response"}, {"request_id", "request-large"},
        {"timestamp", "2026-10-20T08:00:00Z"}, {"pet_name", "synthetic"},
        {"content", QString(10000, 'x')}, {"reasoning_content", "private synthetic detail"}};
    QVERIFY(logger.appendRecord(large));
    QVERIFY(QFileInfo(path).size() <= 512);
    const auto lines = readRecords(path);
    QCOMPARE(lines.size(), 1);
    const auto omitted = lines[0];
    QVERIFY(omitted["omitted"].toBool());
    QCOMPARE(omitted["omitted_reason"].toString(), QString("record_exceeds_size_limit"));
    QCOMPARE(omitted["original_bytes"].toInteger(), QJsonDocument(large).toJson(QJsonDocument::Compact).size() + 1);
    QCOMPARE(omitted["request_id"], large["request_id"]);
    QCOMPARE(omitted["type"], large["type"]);
    QVERIFY(!omitted.contains("content"));
    QVERIFY(!omitted.contains("reasoning_content"));
    QVERIFY(logger.appendRecord(record(2)));
    QCOMPARE(readRecords(path + ".1")[0], omitted);
}

void TestAiCallLogger::writeAndRotationFailuresAreReported() {
    QTemporaryDir directory;
    QVERIFY(!AiCallLogger("").appendRecord(record(0)));
    QVERIFY(!AiCallLogger(directory.path()).appendRecord(record(0)));
    QFile blockingParent(directory.filePath("not-a-directory"));
    QVERIFY(blockingParent.open(QIODevice::WriteOnly));
    blockingParent.close();
    QVERIFY(!AiCallLogger(blockingParent.fileName() + "/ai_calls.jsonl").appendRecord(record(0)));

    const auto path = directory.filePath("ai_calls.jsonl");
    AiCallLogger logger(path, 512, 1);
    QVERIFY(logger.appendRecord(record(1)));
    QVERIFY(QDir().mkpath(path + ".1"));
    QVERIFY(!logger.appendRecord(record(2)));
    QCOMPARE(readRecords(path).size(), 1);
    QCOMPARE(readRecords(path)[0]["request_id"].toString(), QString("1"));
}

QTEST_GUILESS_MAIN(TestAiCallLogger)
#include "test_ai_call_logger.moc"
