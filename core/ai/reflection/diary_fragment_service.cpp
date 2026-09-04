#include "diary_fragment_service.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUuid>

#include "ai/context/context_assembler.h"
#include "ai/event/event_ledger.h"
#include "ai/model/model_router.h"
#include "ai/runtime/agent_runtime_services.h"
#include "private_key_provider.h"
#include "private_psyche_crypto.h"
#include "sqlite_private_psyche_repository.h"

namespace {

QJsonObject fragmentSchema() {
    return {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("body")}},
        {QStringLiteral("properties"), QJsonObject{
             {QStringLiteral("body"), QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("emotion"), QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("object")}}}}}
    };
}

Result<QPair<QString, QJsonObject>, DomainError> parseFragment(
    const QString& content, int maxChars) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(content.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<QPair<QString, QJsonObject>, DomainError>::failure(
            domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                        QStringLiteral("fragment output is not a JSON object")));
    }
    const QJsonObject object = document.object();
    const QString body = object.value(QStringLiteral("body"))
                             .toString().trimmed().left(maxChars);
    const QJsonObject emotion = object.value(QStringLiteral("emotion")).toObject();
    if (body.isEmpty()) {
        return Result<QPair<QString, QJsonObject>, DomainError>::failure(
            domainError(QStringLiteral("MODEL_OUTPUT_INVALID"),
                        QStringLiteral("fragment body is empty")));
    }
    return Result<QPair<QString, QJsonObject>, DomainError>::success({body, emotion});
}

QJsonObject fragmentEnvelope(const DiaryFragment& fragment) {
    return {
        {QStringLiteral("body"), fragment.body},
        {QStringLiteral("emotionSnapshot"), fragment.emotionSnapshot},
        {QStringLiteral("localDate"), fragment.localDate.toString(Qt::ISODate)},
        {QStringLiteral("segmentIndex"), fragment.segmentIndex},
        {QStringLiteral("sourceFromSequence"),
         static_cast<double>(fragment.sourceFromSequence)},
        {QStringLiteral("sourceToSequence"),
         static_cast<double>(fragment.sourceToSequence)},
        {QStringLiteral("createdAt"), fragment.createdAt.toString(Qt::ISODateWithMs)}
    };
}

Result<DiaryFragment, DomainError> parseFragmentEnvelope(
    const QByteArray& plaintext,
    const EncryptedDiaryFragment& encrypted) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(plaintext, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<DiaryFragment, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                        QStringLiteral("decrypted fragment envelope is invalid")));
    }
    const QJsonObject object = document.object();
    DiaryFragment fragment;
    fragment.fragmentId = encrypted.fragmentId;
    fragment.profileId = encrypted.profileId;
    fragment.localDate = QDate::fromString(
        object.value(QStringLiteral("localDate")).toString(), Qt::ISODate);
    fragment.segmentIndex = object.value(QStringLiteral("segmentIndex")).toInt();
    fragment.body = object.value(QStringLiteral("body")).toString();
    fragment.emotionSnapshot = object.value(QStringLiteral("emotionSnapshot")).toObject();
    fragment.sourceFromSequence = static_cast<qint64>(
        object.value(QStringLiteral("sourceFromSequence")).toDouble(0));
    fragment.sourceToSequence = static_cast<qint64>(
        object.value(QStringLiteral("sourceToSequence")).toDouble(0));
    fragment.status = encrypted.status;
    fragment.createdAt = QDateTime::fromString(
        object.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
    if (!fragment.localDate.isValid() || fragment.body.trimmed().isEmpty()) {
        return Result<DiaryFragment, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                        QStringLiteral("decrypted fragment fields are invalid")));
    }
    return Result<DiaryFragment, DomainError>::success(fragment);
}

QList<ChatMessage> fragmentProjection(
    const QDate& localDate,
    const QList<EventRecord>& events) {
    QJsonArray eventArray;
    for (const EventRecord& event : events) {
        QJsonObject obj;
        obj.insert(QStringLiteral("type"), event.type);
        // 从 payload 提取可读摘要：优先 text 字段，其次紧凑 JSON（截断）
        const QString text = event.payload.value(QStringLiteral("text")).toString();
        const QString summary = !text.isEmpty()
            ? text.left(120)
            : QString::fromUtf8(QJsonDocument(event.payload)
                                    .toJson(QJsonDocument::Compact)).left(120);
        obj.insert(QStringLiteral("summary"), summary);
        obj.insert(QStringLiteral("timestamp"),
                   event.createdAt.toString(Qt::ISODateWithMs));
        eventArray.append(obj);
    }
    QJsonObject input;
    input.insert(QStringLiteral("localDate"), localDate.toString(Qt::ISODate));
    input.insert(QStringLiteral("events"), eventArray);

    ChatMessage system;
    system.role = QStringLiteral("system");
    system.content = QStringLiteral(
        "将最近发生的事件写成一小段便签式的日记片段（200-400字）。"
        "只记录输入事件中的事实，不编造。若事件中包含情绪数据，提取到 emotion 对象，"
        "否则 emotion 留空。只返回 body 字符串和 emotion 对象。");
    ChatMessage user;
    user.role = QStringLiteral("user");
    user.content = QString::fromUtf8(
        QJsonDocument(input).toJson(QJsonDocument::Compact));
    return {system, user};
}

} // namespace

DiaryFragmentService::DiaryFragmentService(
    QString profileId,
    AgentRuntimeServices* services,
    ModelRouter* modelRouter,
    ContextAssembler* contextAssembler,
    PrivateKeyProvider* keyProvider,
    PrivatePsycheCrypto* crypto,
    SqlitePrivatePsycheRepository* repository,
    EventLedger* eventLedger,
    Policy policy)
    : m_profileId(std::move(profileId)),
      m_services(services),
      m_modelRouter(modelRouter),
      m_contextAssembler(contextAssembler),
      m_keyProvider(keyProvider),
      m_crypto(crypto),
      m_repository(repository),
      m_eventLedger(eventLedger),
      m_policy(policy) {}

DiaryFragmentService::~DiaryFragmentService() {
    m_alive->store(false, std::memory_order_release);
}

void DiaryFragmentService::setIdleProbe(std::function<int()> userIdleSeconds) {
    m_userIdleSeconds = std::move(userIdleSeconds);
}

void DiaryFragmentService::setBusyProbe(std::function<bool()> isBrainBusy) {
    m_isBrainBusy = std::move(isBrainBusy);
}

void DiaryFragmentService::start() {
    if (m_started) return;
    m_started = true;
    // 周期性便签收集：满足“短空闲”（≥ 5 分钟，低于 Sleep 的 30 分钟）
    // 且大脑不忙时，把自上次便签以来的新事件写成一段便签。
    m_timer.setInterval(qMax(1, m_policy.idleWindowMinutes) * 60 * 1000);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this]() {
        constexpr int kMinIdleSeconds = 300;
        const int idle = m_userIdleSeconds ? m_userIdleSeconds() : -1;
        const bool busy = m_isBrainBusy && m_isBrainBusy();
        if (idle < kMinIdleSeconds || busy) return;
        const auto result = collectFragmentIfIdle();
        if (!result.isOk()) {
            qWarning("[DiaryFragment] collect skipped: %s",
                     qUtf8Printable(result.error().message));
        }
    });
    m_timer.start();

    // 启动后延迟检测孤儿便签（有 Draft 无日记的历史日期），避免与启动流程争资源。
    const std::shared_ptr<std::atomic_bool> alive = m_alive;
    QTimer::singleShot(60 * 1000, this, [this, alive]() {
        if (!alive->load(std::memory_order_acquire)) return;
        const auto orphans = detectOrphanDrafts();
        if (!orphans.isOk()) return;
        const QDate today = QDateTime::currentDateTime().date();
        for (const auto& orphan : orphans.value()) {
            if (orphan.first.isValid() && orphan.first < today) {
                emit recoveryNeeded(orphan.first);  // 最老优先（已按日期升序）
                break;  // 一次只补一篇，下次启动继续
            }
        }
    });
}

void DiaryFragmentService::stop() {
    m_timer.stop();
    m_started = false;
}

Result<void, DomainError> DiaryFragmentService::collectFragmentIfIdle() {
    if (!m_repository || !m_eventLedger || !m_modelRouter || !m_services) {
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("fragment dependencies are unavailable")));
    }
    const auto authorization = m_services->authorizationFor(
        QStringLiteral("reflection"));
    if (!authorization.isOk()) {
        return Result<void, DomainError>::failure(authorization.error());
    }
    const QDate today = QDateTime::currentDateTime().date();
    const auto existing = m_repository->draftFragments(m_profileId, today);
    if (!existing.isOk()) {
        return Result<void, DomainError>::failure(existing.error());
    }
    qint64 lastSequence = 0;
    int nextSegment = 0;
    if (!existing.value().isEmpty()) {
        const auto& last = existing.value().last();
        lastSequence = last.sourceToSequence;
        nextSegment = last.segmentIndex + 1;
    }
    EventFilter filter{{}, QString{}, authorization.value()};
    const auto events = m_eventLedger->readAfter(
        lastSequence, filter, m_policy.minEventsSinceLastFragment * 3);
    if (!events.isOk()) {
        return Result<void, DomainError>::failure(events.error());
    }
    if (events.value().size() < m_policy.minEventsSinceLastFragment) {
        return Result<void, DomainError>::success();
    }
    const qint64 fromSequence = lastSequence + 1;
    const qint64 toSequence = events.value().last().sequence;
    const auto fragmentId = composeFragment(
        today, nextSegment, fromSequence, toSequence, events.value());
    if (!fragmentId.isOk()) {
        return Result<void, DomainError>::failure(fragmentId.error());
    }
    emit fragmentCollected(fragmentId.value(), today);
    return Result<void, DomainError>::success();
}

Result<QList<DiaryFragment>, DomainError> DiaryFragmentService::draftsForDate(
    const QDate& localDate) {
    if (!m_repository || !m_keyProvider || !m_crypto) {
        return Result<QList<DiaryFragment>, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("fragment read dependencies are unavailable")));
    }
    const auto encrypted = m_repository->draftFragments(m_profileId, localDate);
    if (!encrypted.isOk()) {
        return Result<QList<DiaryFragment>, DomainError>::failure(encrypted.error());
    }
    QList<DiaryFragment> fragments;
    for (const auto& enc : encrypted.value()) {
        const auto fragment = decryptFragment(enc);
        if (fragment.isOk()) {
            fragments.append(fragment.value());
        }
    }
    return Result<QList<DiaryFragment>, DomainError>::success(fragments);
}

Result<QList<QPair<QDate, int>>, DomainError>
DiaryFragmentService::detectOrphanDrafts() const {
    if (!m_repository) {
        return Result<QList<QPair<QDate, int>>, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("orphan detection is unavailable")));
    }
    return m_repository->orphanDraftDates(m_profileId);
}

Result<QString, DomainError> DiaryFragmentService::composeFragment(
    const QDate& localDate,
    int segmentIndex,
    qint64 fromSequence,
    qint64 toSequence,
    const QList<EventRecord>& events) {
    if (!m_keyProvider || !m_crypto || !m_repository || !m_modelRouter) {
        return Result<QString, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("fragment compose dependencies are unavailable")));
    }
    const auto key = m_keyProvider->loadOrCreate(m_profileId);
    if (!key.isOk()) {
        return Result<QString, DomainError>::failure(key.error());
    }
    ModelRequest modelRequest;
    modelRequest.role = ModelRole::Diary;
    modelRequest.messages = fragmentProjection(localDate, events);
    modelRequest.responseSchema = fragmentSchema();
    modelRequest.profileId = m_profileId;
    modelRequest.petName = QStringLiteral("diary_fragment");
    
    // Fragment collection runs synchronously via blocking wait on async call
    QEventLoop loop;
    Result<ModelCompletion, DomainError> completionResult =
        Result<ModelCompletion, DomainError>::failure(
            domainError(QStringLiteral("MODEL_TIMEOUT"),
                        QStringLiteral("fragment LLM call timed out")));
    m_modelRouter->completeAsync(
        modelRequest,
        [&loop, &completionResult](Result<ModelCompletion, DomainError> result) {
            completionResult = std::move(result);
            loop.quit();
        });
    loop.exec();
    
    if (!completionResult.isOk()) {
        return Result<QString, DomainError>::failure(completionResult.error());
    }
    const auto parsed = parseFragment(
        completionResult.value().response.content, m_policy.maxFragmentBodyChars);
    if (!parsed.isOk()) {
        return Result<QString, DomainError>::failure(parsed.error());
    }
    DiaryFragment fragment;
    fragment.fragmentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    fragment.profileId = m_profileId;
    fragment.localDate = localDate;
    fragment.segmentIndex = segmentIndex;
    fragment.body = parsed.value().first;
    fragment.emotionSnapshot = parsed.value().second;
    fragment.sourceFromSequence = fromSequence;
    fragment.sourceToSequence = toSequence;
    fragment.status = DiaryFragmentStatus::Draft;
    fragment.createdAt = QDateTime::currentDateTimeUtc();
    const PrivateRecordAad aad{
        1, m_profileId, QStringLiteral("diary_fragment"), fragment.fragmentId,
        key.value().keyVersion};
    const auto encrypted = m_crypto->encrypt(
        QJsonDocument(fragmentEnvelope(fragment)).toJson(QJsonDocument::Compact),
        aad, key.value());
    if (!encrypted.isOk()) {
        return Result<QString, DomainError>::failure(encrypted.error());
    }
    const auto saved = m_repository->saveFragment(fragment, encrypted.value());
    if (!saved.isOk()) {
        return Result<QString, DomainError>::failure(saved.error());
    }
    return Result<QString, DomainError>::success(fragment.fragmentId);
}

Result<DiaryFragment, DomainError> DiaryFragmentService::decryptFragment(
    const EncryptedDiaryFragment& encrypted) const {
    if (!m_keyProvider || !m_crypto) {
        return Result<DiaryFragment, DomainError>::failure(
            domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                        QStringLiteral("fragment decrypt dependencies are unavailable")));
    }
    const auto key = m_keyProvider->loadOrCreate(m_profileId);
    if (!key.isOk()) {
        return Result<DiaryFragment, DomainError>::failure(key.error());
    }
    const PrivateRecordAad aad{
        encrypted.encrypted.schemaVersion, encrypted.profileId,
        QStringLiteral("diary_fragment"), encrypted.fragmentId,
        encrypted.encrypted.keyVersion};
    const auto plaintext = m_crypto->decrypt(encrypted.encrypted, aad, key.value());
    if (!plaintext.isOk()) {
        return Result<DiaryFragment, DomainError>::failure(plaintext.error());
    }
    return parseFragmentEnvelope(plaintext.value(), encrypted);
}
