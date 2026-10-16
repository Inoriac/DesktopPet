#pragma once
#include "conversation_store.h"
// Owns an immutable receipt until the original database commits it. Retries
// persistence only; never invokes a sender. Completion may follow facade
// teardown.
void archiveDeliveryReceipt(
    ConversationStore store, DeliveryReceipt receipt,
    std::function<void(Result<void, DomainError>)> completion,
    std::function<void(DomainError)> failure = {});
