#pragma once
#include "selection_types.h"
#include "ai/model/model_router.h"
#include <QObject>
#include <QThreadPool>

class ActivitySelector final : public QObject {
  public:
    using Completion =
        std::function<void(Result<SelectionProposal, DomainError>)>;
    using Prepared = std::function<void(Result<SelectionRequest, DomainError>)>;
    explicit ActivitySelector(ModelRouter *router, QObject *parent = nullptr);
    ~ActivitySelector() override;
    void prepareAsync(SelectionRequest request, Prepared completion);
    void selectActivity(const SelectionRequest &,
                        std::shared_ptr<ModelAttemptHooks>, Completion);
    static Result<SelectionRequest, DomainError>
    prepare(SelectionRequest request);
    static Result<SelectionProposal, DomainError>
    parse(const QJsonObject &, const SelectionRequest &);
    static QJsonObject schema(const SelectionRequest &);
    static QJsonObject materials(const SelectionRequest &);

  private:
    ModelRouter *m_router;
    QThreadPool m_worker;
};
