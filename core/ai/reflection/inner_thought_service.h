#ifndef DESKTOP_PET_INNER_THOUGHT_SERVICE_H
#define DESKTOP_PET_INNER_THOUGHT_SERVICE_H

#include "cancellation_token.h"
#include "reflection_types.h"

#include <atomic>
#include <memory>

class EventLedger;
class ModelRouter;
class PrivateKeyProvider;
class PrivatePsycheCrypto;
class SqlitePrivatePsycheRepository;

class InnerThoughtService {
public:
    InnerThoughtService(QString profileId,
                        ModelRouter* modelRouter,
                        PrivateKeyProvider* keyProvider,
                        PrivatePsycheCrypto* crypto,
                        SqlitePrivatePsycheRepository* repository,
                        EventLedger* eventLedger = nullptr);
    ~InnerThoughtService();

    void createAsync(const InnerThoughtRequest& request,
                     const CancellationToken& token,
                     InnerThoughtHandler handler);
    Result<InnerThoughtSummary, DomainError> readSummary(
        const QString& entryId) const;

private:
    QString m_profileId;
    ModelRouter* m_modelRouter = nullptr;
    PrivateKeyProvider* m_keyProvider = nullptr;
    PrivatePsycheCrypto* m_crypto = nullptr;
    SqlitePrivatePsycheRepository* m_repository = nullptr;
    EventLedger* m_eventLedger = nullptr;
    std::shared_ptr<std::atomic_bool> m_alive =
        std::make_shared<std::atomic_bool>(true);
};

#endif // DESKTOP_PET_INNER_THOUGHT_SERVICE_H
