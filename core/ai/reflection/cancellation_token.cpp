#include "cancellation_token.h"

#include <utility>

CancellationToken::CancellationToken(
    std::shared_ptr<std::atomic<quint64>> state,
    quint64 generation)
    : m_state(std::move(state)), m_generation(generation) {}

bool CancellationToken::isCancelled() const {
    return !m_state || m_state->load(std::memory_order_acquire) != m_generation;
}

CancellationSource::CancellationSource()
    : m_state(std::make_shared<std::atomic<quint64>>(1)) {}

CancellationToken CancellationSource::token() const {
    return CancellationToken(m_state, generation());
}

void CancellationSource::cancel() {
    m_state->fetch_add(1, std::memory_order_acq_rel);
}

void CancellationSource::reset() {
    cancel();
}

quint64 CancellationSource::generation() const {
    return m_state->load(std::memory_order_acquire);
}
