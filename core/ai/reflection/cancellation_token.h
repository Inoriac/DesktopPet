#ifndef DESKTOP_PET_CANCELLATION_TOKEN_H
#define DESKTOP_PET_CANCELLATION_TOKEN_H

#include <atomic>
#include <memory>

#include <QtGlobal>

class CancellationToken {
public:
    CancellationToken() = default;
    bool isCancelled() const;
    quint64 generation() const { return m_generation; }

private:
    friend class CancellationSource;
    CancellationToken(std::shared_ptr<std::atomic<quint64>> state,
                      quint64 generation);

    std::shared_ptr<std::atomic<quint64>> m_state;
    quint64 m_generation = 0;
};

class CancellationSource {
public:
    CancellationSource();

    CancellationToken token() const;
    void cancel();
    void reset();
    quint64 generation() const;

private:
    std::shared_ptr<std::atomic<quint64>> m_state;
};

#endif // DESKTOP_PET_CANCELLATION_TOKEN_H
