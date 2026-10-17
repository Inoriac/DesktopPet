#include "voice/voice_synthesis_service.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

namespace {
// A protocol worker only: no model downloads, synthesis or audio device access.
const char worker[] = R"PY(
import json, os, pathlib, sys
trace = pathlib.Path(__file__).with_suffix('.jsonl')
for line in sys.stdin:
    event = json.loads(line)
    with trace.open('a', encoding='utf-8') as output:
        output.write(json.dumps(event) + '\n')
    kind = event['type']
    if kind == 'shutdown':
        break
    if kind == 'speak' and event['text'] == '__exit__':
        sys.stdout.write('{"unfinished":')
        sys.stdout.flush()
        os._exit(7)
    if kind == 'speak' and event['text'] == '__hold__':
        continue
    reply = 'configured' if kind == 'configure' else 'speech_finished'
    print(json.dumps({'type': reply, 'requestId': event['requestId']}), flush=True)
)PY";

struct Fixture {
    QTemporaryDir directory;
    VoiceConfig config;
    VoiceSynthesisService service;
    bool init() {
        config.enabled = true;
        config.preloadOnStart = false;
        config.pythonExecutable = QString::fromUtf8(VOICE_TEST_PYTHON);
        config.workerScript = directory.filePath("worker.py");
        QFile script(config.workerScript);
        if (!script.open(QIODevice::WriteOnly)) return false;
        script.write(worker);
        script.close();
        service.setConfig(config);
        return true;
    }
    QList<QJsonObject> events(const QString &kind) const {
        QList<QJsonObject> result;
        QFile trace(directory.filePath("worker.jsonl"));
        if (!trace.open(QIODevice::ReadOnly)) return result;
        while (!trace.atEnd()) {
            const auto event = QJsonDocument::fromJson(trace.readLine()).object();
            if (event["type"].toString() == kind) result.append(event);
        }
        return result;
    }
};
}

class VoiceSynthesisTests : public QObject {
    Q_OBJECT
private slots:
    void stop_discards_pending_speech_and_allows_restart() {
        Fixture fixture;
        QVERIFY(fixture.init());
        fixture.service.speak("__hold__", "assistant");
        QTRY_COMPARE(fixture.events("speak").size(), 1);
        fixture.service.speak("discard me", "assistant");
        fixture.service.stop();
        QTRY_COMPARE(fixture.service.findChildren<QProcess *>().size(), 0);
        fixture.service.speak("new speech", "assistant");
        QTRY_COMPARE(fixture.events("speak").size(), 2);
        QCOMPARE(fixture.events("speak").last()["text"].toString(), "new speech");
        QCOMPARE(fixture.events("configure").size(), 2);
    }

    void unexpected_exit_clears_process_and_partial_output() {
        Fixture fixture;
        QVERIFY(fixture.init());
        fixture.service.speak("__exit__", "assistant");
        QTRY_COMPARE(fixture.events("speak").size(), 1);
        QTRY_COMPARE(fixture.service.findChildren<QProcess *>().size(), 0);
        fixture.service.speak("recovered", "assistant");
        QTRY_COMPARE(fixture.events("speak").size(), 2);
        QCOMPARE(fixture.events("speak").last()["text"].toString(), "recovered");
    }

    void worker_configuration_changes_take_effect() {
        Fixture fixture;
        QVERIFY(fixture.init());
        fixture.config.preloadOnStart = true;
        fixture.service.setConfig(fixture.config);
        QTRY_COMPARE(fixture.events("configure").size(), 1);
        fixture.config.customSpeaker.referenceAudioText = "新的参考文本";
        fixture.service.setConfig(fixture.config);
        QTRY_COMPARE(fixture.events("configure").size(), 2);
        QCOMPARE(fixture.events("configure").last()["config"].toObject()["customSpeaker"].toObject()["referenceAudioText"].toString(), "新的参考文本");
        fixture.config.saveAudio = !fixture.config.saveAudio;
        fixture.config.outputDir = fixture.directory.filePath("audio");
        fixture.service.setConfig(fixture.config);
        QTRY_COMPARE(fixture.events("configure").size(), 3);
        QCOMPARE(fixture.events("configure").last()["config"].toObject()["outputDir"].toString(), fixture.config.outputDir);
        // Source switches are handled by the host and do not restart the worker.
        const auto processes = fixture.service.findChildren<QProcess *>();
        fixture.config.sources.assistant = false;
        fixture.service.setConfig(fixture.config);
        QCOMPARE(fixture.service.findChildren<QProcess *>(), processes);
    }

    void failed_start_can_recover_after_configuration_fix() {
        Fixture fixture;
        QVERIFY(fixture.init());
        fixture.config.pythonExecutable = fixture.directory.filePath("missing-python");
        fixture.config.preloadOnStart = true;
        fixture.service.setConfig(fixture.config);
        QTRY_COMPARE(fixture.service.findChildren<QProcess *>().size(), 0);
        fixture.config.pythonExecutable = QString::fromUtf8(VOICE_TEST_PYTHON);
        fixture.service.setConfig(fixture.config);
        fixture.service.speak("recovered", "assistant");
        QTRY_COMPARE(fixture.events("speak").size(), 1);
    }
};
QTEST_GUILESS_MAIN(VoiceSynthesisTests)
#include "test_voice_synthesis.moc"
