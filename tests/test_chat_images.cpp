#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QLockFile>
#include <QTimer>
#include "ai/chat/chat_image_store.h"
#include "ai/chat/conversation_store.h"
#include "ui/chat_conversation_model.h"

namespace {
const QString profile = "11111111-1111-4111-8111-111111111111";
const QByteArray png = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAACXBIWXMAAA9hAAAPYQGoP6dpAAAAFklEQVQImWMUa//BwMDAxMDAwMDAAAASRAGZJqsQuAAAAABJRU5ErkJggg==");
QString source(QTemporaryDir& dir) {
    const auto path=dir.filePath("source.png"); QFile f(path); if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(png); return path;
}
ProfileChatStoreOptions options(QTemporaryDir& dir, bool sqlite=false) {
    ProfileChatStoreOptions o; o.appDataRoot=dir.path(); o.profileId=profile; o.registeredProfileIds={profile};
    if (sqlite) o.runtimeDatabasePath=dir.filePath("runtime.db");
    return o;
}
}
class ChatImageTests : public QObject {
    Q_OBJECT
private slots:
    void copiesAndScopesImages() {
        QTemporaryDir dir; ChatImageStore store(dir.filePath("images")); const auto path=source(dir);
        const auto imported=store.importImages({QJsonObject{{"path",path},{"name","../合成.png"}}});
        QVERIFY(imported); QCOMPARE(imported.value().size(),1);
        const auto image=imported.value().first().toObject(); QVERIFY(image["path"].toString()!=path);
        QVERIFY(!image["path"].toString().contains("合成")); QCOMPARE(image["mimeType"].toString(),"image/png");
        QVERIFY(QFile::remove(path)); const auto blocks=store.contentBlocks(imported.value()); QVERIFY(blocks);
        QCOMPARE(QByteArray::fromBase64(blocks.value().first().toObject()["data"].toString().toLatin1()),png);
        ChatImageStore other(dir.filePath("other")); QVERIFY(!other.contentBlocks(imported.value()));
    }
    void rejectsAndCleansIncompleteImport() {
        QTemporaryDir dir; ChatImageStore store(dir.filePath("images")); const auto path=source(dir);
        const QJsonObject valid{{"path",path},{"name","image.png"}};
        QVERIFY(!store.importImages({valid,QJsonObject{{"path",dir.filePath("missing.png")}}}));
        QCOMPARE(QDir(dir.filePath("images")).entryList(QDir::Files).size(),0);
        QVERIFY(!store.importImages({valid,valid,valid,valid,valid}));
        QFile huge(path); QVERIFY(huge.open(QIODevice::Append)); QVERIFY(huge.resize(2*1024*1024+1)); huge.close();
        QVERIFY(!store.importImages({valid}));
    }
    void imageOnlyHistoryAndRetry_data() { QTest::addColumn<bool>("sqlite"); QTest::newRow("jsonl")<<false; QTest::newRow("sqlite")<<true; }
    void imageOnlyHistoryAndRetry() {
        QFETCH(bool,sqlite); QTemporaryDir dir; const auto o=options(dir,sqlite); ChatConversationModel model; QString error;
        QVERIFY2(model.initialize(o,&error),qPrintable(error)); std::optional<ChatHistoryResult> sent;
        model.appendUserMessageWithImages("",{QJsonObject{{"path",source(dir)},{"name","test.png"}}},[&](auto r){sent=std::move(r);});
        QVERIFY(!sent); QTRY_VERIFY(sent.has_value()); QVERIFY(*sent);
        QCOMPARE(model.messages().size(),1); const auto entry=model.messages().first(); QVERIFY(entry.content.isEmpty()); QCOMPARE(entry.images.size(),1);
        QCOMPARE(model.recentHistoryPage()["messages"].toArray().size(),1);
        model.beginAssistantMessage("reply",entry.id); model.finishAssistantMessage("reply",ChatMessageStatus::Failed,"vision unavailable");
        QCOMPARE(model.sourceImagesForReply("reply"),entry.images);
        const auto retry=model.appendUserMessage("",QDateTime::currentDateTime(),model.sourceImagesForReply("reply")); QVERIFY(!retry.isEmpty());
        ChatConversationModel reopened; QVERIFY(reopened.initialize(o,&error)); QCOMPARE(reopened.messages().first().images,entry.images);
        QCOMPARE(reopened.sourceImagesForReply("reply"), entry.images);
        std::optional<ChatHistoryResult> page;
        reopened.requestHistory("get_chat_history", {{"limit", 2}, {"beforeMessageId", retry}},
                                [&](auto r) { page = std::move(r); });
        QTRY_VERIFY(page.has_value());
        QVERIFY(*page);
        const auto messages = page->value()["messages"].toArray();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.first().toObject()["images"].toArray(), entry.images);
        QCOMPARE(messages.last().toObject()["id"].toString(), QString("reply"));
        QCOMPARE(messages.last().toObject()["status"].toString(), QString("failed"));
        QCOMPARE(messages.last().toObject()["replyToId"].toString(), entry.id);
        if (sqlite) {
            ConversationStore store(profile,o.runtimeDatabasePath); IncomingMessage m; m.channel="desktop";m.connectionKey="local_owner";m.remoteConversationId="local_owner";
            m.localMessageId=entry.id;m.content="";m.occurredAt=entry.timestamp;m.originKind=OriginKind::LocalOwner;m.kind=ConversationKind::LocalOwner;m.images=entry.images;
            const auto accepted=store.acceptMessage(m); QVERIFY(accepted); QVERIFY(accepted.value().duplicate);
            m.images={}; QVERIFY(!store.acceptMessage(m));
            auto changed=entry; changed.images.append(entry.images.first()); QVERIFY(!store.appendLocalHistory(changed));
            HostConversationBinding binding;binding.channel="test";binding.connectionKey="test";binding.remoteConversationId="room";binding.kind=ConversationKind::Direct; QVERIFY(store.registerBinding(binding));
            m.channel="test";m.connectionKey="test";m.remoteConversationId="room";m.kind=ConversationKind::Direct;m.originKind=OriginKind::External;m.localMessageId="";m.remoteMessageId="external";m.content="text";m.images=entry.images; QVERIFY(!store.acceptMessage(m));
        }
    }
    void imageImportDoesNotBlockOwnerAndDropsOldProfileCallbacks() {
        QTemporaryDir dir; auto o=options(dir); ChatConversationModel model; QString error; QVERIFY(model.initialize(o,&error));
        const auto history=dir.filePath("profiles/"+profile+"/chat_history.jsonl");
        QLockFile lock(history+".lock"); QVERIFY(lock.tryLock());
        bool called=false, heartbeat=false;
        model.appendUserMessageWithImages("",{QJsonObject{{"path",source(dir)}}},[&](auto){called=true;});
        auto other=o; other.profileId="22222222-2222-4222-8222-222222222222";other.registeredProfileIds={other.profileId};
        QVERIFY(model.initialize(other,&error));
        QTimer::singleShot(20,[&]{heartbeat=true;lock.unlock();});
        QTRY_VERIFY(heartbeat);QTRY_VERIFY(QFileInfo(history).size()>0);
        QCoreApplication::processEvents();QVERIFY(!called);QVERIFY(model.messages().isEmpty());
    }
    void persistenceFailureLeavesNoImageOrMessage() {
        QTemporaryDir dir; auto o=options(dir); ChatConversationModel model; QString error; QVERIFY(model.initialize(o,&error));
        const auto history=dir.filePath("profiles/"+profile+"/chat_history.jsonl"); QVERIFY(QFile::remove(history)); QVERIFY(QDir().mkdir(history));
        std::optional<ChatHistoryResult> sent; model.appendUserMessageWithImages("text",{QJsonObject{{"path",source(dir)}}},[&](auto r){sent=std::move(r);});
        QTRY_VERIFY(sent.has_value()); QVERIFY(!*sent); QVERIFY(model.messages().isEmpty());
        QCOMPARE(QDir(dir.filePath("profiles/"+profile+"/chat/images")).entryList(QDir::Files).size(),0);
    }
};
QTEST_GUILESS_MAIN(ChatImageTests)
#include "test_chat_images.moc"
