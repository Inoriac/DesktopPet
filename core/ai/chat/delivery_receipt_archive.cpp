#include "delivery_receipt_archive.h"
#include <QCoreApplication>
#include <QThreadPool>
#include <QTimer>
namespace {
struct PendingReceipt : std::enable_shared_from_this<PendingReceipt> {
  ConversationStore store;
  DeliveryReceipt receipt;
  std::function<void(Result<void, DomainError>)> completion;
  std::function<void(DomainError)> failure;
  bool reported = false;
  PendingReceipt(ConversationStore s, DeliveryReceipt r,
                 std::function<void(Result<void, DomainError>)> c,
                 std::function<void(DomainError)> f)
      : store(std::move(s)), receipt(std::move(r)), completion(std::move(c)),
        failure(std::move(f)) {}
  void attempt() {
    auto self = shared_from_this();
    QThreadPool::globalInstance()->start([self] {
      auto result = self->store.recordDeliveryReceipt(self->receipt);
      if (result) {
        if (self->completion)
          self->completion(std::move(result));
        return;
      }
      if (!self->reported) {
        self->reported = true;
        if (self->failure)
          self->failure(result.error());
      }
      if (result.error().code != "STORE_UNAVAILABLE" &&
          result.error().code != "EVENT_OUTBOX_UNAVAILABLE") {
        if (self->completion)
          self->completion(std::move(result));
        return;
      }
      QMetaObject::invokeMethod(QCoreApplication::instance(), [self] {
        QTimer::singleShot(250, QCoreApplication::instance(),
                           [self] { self->attempt(); });
      });
    });
  }
};
} // namespace
void archiveDeliveryReceipt(
    ConversationStore store, DeliveryReceipt receipt,
    std::function<void(Result<void, DomainError>)> completion,
    std::function<void(DomainError)> failure) {
  auto pending = std::make_shared<PendingReceipt>(
      std::move(store), std::move(receipt), std::move(completion),
      std::move(failure));
  pending->attempt();
}
