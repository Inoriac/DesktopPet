#ifndef DESKTOP_PET_SQLITE_EMOTION_STATE_REPOSITORY_H
#define DESKTOP_PET_SQLITE_EMOTION_STATE_REPOSITORY_H

#include "emotion_state_repository.h"

#include <QString>

class SQLiteEmotionStateRepository final : public IEmotionStateRepository {
public:
    explicit SQLiteEmotionStateRepository(QString databasePath);
    ~SQLiteEmotionStateRepository() override;

    std::optional<PersistedEmotionState> load() override;
    bool save(const PersistedEmotionState& state) override;

    QString databasePath() const { return m_databasePath; }
    QString lastError() const { return m_lastError; }
    bool isOpen() const;
    void close();

private:
    bool ensureOpen();
    bool initializeSchema();
    void setError(const QString& error);

    QString m_databasePath;
    QString m_connectionName;
    QString m_lastError;
};

#endif // DESKTOP_PET_SQLITE_EMOTION_STATE_REPOSITORY_H
