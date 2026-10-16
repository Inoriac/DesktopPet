#include "activity_runtime.h"
#include "ai/task/task_orchestrator.h"
#include "ai/task/task_store.h"
#include <QTimer>
struct ActivityRuntime::Worker : QObject {
    std::unique_ptr<TaskStore> tasks;
    std::unique_ptr<TaskOrchestrator> runner;
    QString profile, path;
    Worker(QString p, QString db)
        : profile(std::move(p)), path(std::move(db)) {}
};
ActivityRuntime::ActivityRuntime(QString profileId, QString path,
                                 QObject *parent)
    : QObject(parent), m_profileId(std::move(profileId)),
      m_databasePath(std::move(path)) {
    m_worker = new Worker(m_profileId, m_databasePath);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
    QPointer<ActivityRuntime> self(this);
    auto *worker = m_worker;
    QMetaObject::invokeMethod(worker, [self, worker] {
        worker->tasks = std::make_unique<TaskStore>();
        if (!worker->tasks->open(worker->path, worker->profile)) {
            if (self)
                emit self->userMessage(worker->tasks->lastError());
            return;
        }
        auto recovered = worker->tasks->activityStore()->recoverAfterRestart();
        if (!recovered) {
            if (self)
                emit self->userMessage(recovered.error().message);
            return;
        }
        worker->runner =
            std::make_unique<TaskOrchestrator>(worker->tasks.get());
        if (!self)
            return;
        QObject::connect(worker->runner.get(), &TaskOrchestrator::taskChanged,
                         self, [self](const QJsonObject &task) {
                             if (self) {
                                 self->m_snapshot = task;
                                 emit self->taskChanged(task);
                             }
                         });
        QObject::connect(worker->runner.get(), &TaskOrchestrator::activeChanged,
                         self, [self](bool active) {
                             if (self) {
                                 self->m_active = active;
                                 emit self->activeChanged(active);
                             }
                         });
        QObject::connect(worker->runner.get(), &TaskOrchestrator::userMessage,
                         self, &ActivityRuntime::userMessage);
        QObject::connect(worker->runner.get(), &TaskOrchestrator::taskFinished,
                         self, &ActivityRuntime::taskFinished);
        QObject::connect(worker->runner.get(),
                         &TaskOrchestrator::experienceReady, self,
                         &ActivityRuntime::experienceReady);
        const auto pending = worker->tasks->unfinishedTasks();
        if (pending.size() == 1)
            worker->runner->recover(pending.first().id);
        else if (pending.size() > 1)
            emit self->userMessage(
                "Multiple paused activities are available; select "
                "an activity id to resume.");
        QMetaObject::invokeMethod(self, [self] {
            if (self)
                emit self->ready();
        });
    });
}
ActivityRuntime::~ActivityRuntime() {
    auto *worker = m_worker;
    QMetaObject::invokeMethod(worker, [worker] {
        if (worker->runner)
            worker->runner->shutdown();
        worker->runner.reset();
        worker->tasks.reset();
        QThread::currentThread()->quit();
    });
    m_thread.wait();
}
bool ActivityRuntime::hasUnfinishedTask() const {
    return !m_snapshot["id"].toString().isEmpty() &&
           !TaskRecord::fromJson(m_snapshot).terminal();
}
void ActivityRuntime::setDependencies(TaskDependencies d) {
    auto *w = m_worker;
    QMetaObject::invokeMethod(w, [w, d = std::move(d)]() mutable {
        if (w->runner)
            w->runner->setDependencies(std::move(d));
    });
}
void ActivityRuntime::setCapabilityValidator(
    ActivityStore::CapabilityValidator validator) {
    auto *worker = m_worker;
    QMetaObject::invokeMethod(
        worker, [worker, validator = std::move(validator)]() mutable {
            if (worker->tasks && worker->tasks->activityStore())
                worker->tasks->activityStore()->setCapabilityValidator(
                    std::move(validator));
        });
}
void ActivityRuntime::startTaskAsync(TaskStartRequest r) {
    auto *w = m_worker;
    QMetaObject::invokeMethod(w, [w, r = std::move(r)] {
        if (w->runner)
            w->runner->start(r);
    });
}
void ActivityRuntime::resumeTaskAsync(QString id, QString feedback,
                                      TaskBudgetExtension extension) {
    auto *w = m_worker;
    QMetaObject::invokeMethod(w, [w, id = std::move(id),
                                  feedback = std::move(feedback), extension] {
        if (w->runner) {
            if (w->runner->currentTask().id != id && !w->runner->recover(id))
                return;
            w->runner->resume(feedback, extension);
        }
    });
}
void ActivityRuntime::pauseTaskAsync() {
    auto *w = m_worker;
    QMetaObject::invokeMethod(w, [w] {
        if (w->runner)
            w->runner->pause();
    });
}
void ActivityRuntime::stopTaskAsync() {
    auto *w = m_worker;
    QMetaObject::invokeMethod(w, [w] {
        if (w->runner)
            w->runner->stop();
    });
}
void ActivityRuntime::shutdownAsync() {
    auto *w = m_worker;
    QMetaObject::invokeMethod(w, [w] {
        if (w->runner)
            w->runner->shutdown();
    });
}
void ActivityRuntime::resolveUnknownAsync(QString id, QString call,
                                          bool succeeded) {
    auto *w = m_worker;
    QMetaObject::invokeMethod(w, [w, id, call, succeeded] {
        if (w->runner) {
            if (w->runner->currentTask().id != id && !w->runner->recover(id))
                return;
            w->runner->resolveUnknownCall(
                call, succeeded,
                {{"confirmed_by_user", true}, {"succeeded", succeeded}});
        }
    });
}
void ActivityRuntime::startActivityAsync(ActivityIntent i,
                                         Completion completion) {
    auto *w = m_worker;
    QPointer<ActivityRuntime> self(this);
    QMetaObject::invokeMethod(w, [w, self, i = std::move(i),
                                  completion =
                                      std::move(completion)]() mutable {
        auto r =
            w->tasks && w->tasks->activityStore()
                ? w->tasks->activityStore()->startActivity(i)
                : Result<ActivitySnapshot, DomainError>::failure(domainError(
                      "STORE_UNAVAILABLE", "Activity store unavailable"));
        if (r && r.value().state == ActivityState::Running && w->runner)
            w->runner->activate(r.value().activityId);
        if (self)
            QMetaObject::invokeMethod(
                self, [self, r = std::move(r),
                       completion = std::move(completion)]() mutable {
                    if (self && completion)
                        completion(std::move(r));
                });
    });
}
void ActivityRuntime::transitionActivityAsync(ActivityTransition t,
                                              Completion completion) {
    auto *w = m_worker;
    QPointer<ActivityRuntime> self(this);
    QMetaObject::invokeMethod(w, [w, self, t = std::move(t),
                                  completion =
                                      std::move(completion)]() mutable {
        const bool current =
            w->runner && w->runner->currentTask().id == t.activityId;
        if (current && w->runner->isActive())
            t.activeMs = w->runner->currentActiveMs();
        auto r =
            w->tasks && w->tasks->activityStore()
                ? w->tasks->activityStore()->transitionActivity(t)
                : Result<ActivitySnapshot, DomainError>::failure(domainError(
                      "STORE_UNAVAILABLE", "Activity store unavailable"));
        if (r && current && r.value().state != ActivityState::Running)
            w->runner->releaseCurrent();
        else if (r && current)
            w->runner->reloadCurrent();
        if (r && (current || !w->runner || !w->runner->isActive())) {
            auto task = w->tasks->find(t.activityId);
            if (task && self)
                QMetaObject::invokeMethod(
                    self, [self, snapshot = task->toJson()] {
                        if (self) {
                            self->m_snapshot = snapshot;
                            emit self->taskChanged(snapshot);
                        }
                    });
        }
        if (self)
            QMetaObject::invokeMethod(
                self, [self, r = std::move(r),
                       completion = std::move(completion)]() mutable {
                    if (self && completion)
                        completion(std::move(r));
                });
    });
}
void ActivityRuntime::resumeActivityAsync(ResumeActivityRequest request,
                                          Completion completion) {
    auto *w = m_worker;
    QPointer<ActivityRuntime> self(this);
    QMetaObject::invokeMethod(w, [w, self, request = std::move(request),
                                  completion =
                                      std::move(completion)]() mutable {
        if (w->runner && w->runner->isActive() &&
            w->runner->currentTask().id == request.expectedForegroundId)
            request.foregroundActiveMs = w->runner->currentActiveMs();
        auto r =
            w->tasks && w->tasks->activityStore()
                ? w->tasks->activityStore()->resumeActivity(request)
                : Result<ActivitySnapshot, DomainError>::failure(domainError(
                      "STORE_UNAVAILABLE", "Activity store unavailable"));
        if (r && w->runner) {
            w->runner->releaseCurrent();
            w->runner->activate(r.value().activityId);
        }
        if (self)
            QMetaObject::invokeMethod(
                self, [self, r = std::move(r),
                       completion = std::move(completion)]() mutable {
                    if (self && completion)
                        completion(std::move(r));
                });
    });
}
void ActivityRuntime::listActivitiesAsync(
    std::function<void(Result<QList<ActivitySnapshot>, DomainError>)>
        completion) {
    auto *w = m_worker;
    QPointer<ActivityRuntime> self(this);
    QMetaObject::invokeMethod(
        w, [w, self, completion = std::move(completion)]() mutable {
            auto r =
                w->tasks && w->tasks->activityStore()
                    ? w->tasks->activityStore()->list()
                    : Result<QList<ActivitySnapshot>, DomainError>::failure(
                          domainError("STORE_UNAVAILABLE",
                                      "Activity store unavailable"));
            if (self)
                QMetaObject::invokeMethod(
                    self, [self, r = std::move(r),
                           completion = std::move(completion)]() mutable {
                        if (self && completion)
                            completion(std::move(r));
                    });
        });
}
void ActivityRuntime::pendingExperiencesAsync(
    std::function<void(Result<QJsonArray, DomainError>)> completion) {
    auto *w = m_worker;
    QPointer<ActivityRuntime> self(this);
    QMetaObject::invokeMethod(
        w, [w, self, completion = std::move(completion)]() mutable {
            auto r =
                w->tasks && w->tasks->activityStore()
                    ? w->tasks->activityStore()->pendingExperiences()
                    : Result<QJsonArray, DomainError>::failure(domainError(
                          "STORE_UNAVAILABLE", "Activity store unavailable"));
            if (self)
                QMetaObject::invokeMethod(
                    self, [self, r = std::move(r),
                           completion = std::move(completion)]() mutable {
                        if (self && completion)
                            completion(std::move(r));
                    });
        });
}
void ActivityRuntime::acknowledgeExperienceAsync(
    QString id, std::function<void(Result<void, DomainError>)> completion) {
    auto *w = m_worker;
    QPointer<ActivityRuntime> self(this);
    QMetaObject::invokeMethod(w, [w, self, id = std::move(id),
                                  completion =
                                      std::move(completion)]() mutable {
        auto r = w->tasks && w->tasks->activityStore()
                     ? w->tasks->activityStore()->acknowledgeExperience(id)
                     : Result<void, DomainError>::failure(domainError(
                           "STORE_UNAVAILABLE", "Activity store unavailable"));
        if (self)
            QMetaObject::invokeMethod(
                self, [self, r = std::move(r),
                       completion = std::move(completion)]() mutable {
                    if (self && completion)
                        completion(std::move(r));
                });
    });
}

void ActivityRuntime::prepareActionAsync(
    ActionPreparation request,
    std::function<void(Result<ActionSnapshot, DomainError>)> completion) {
    auto *worker = m_worker;
    QPointer<ActivityRuntime> self(this);
    QMetaObject::invokeMethod(
        worker, [worker, self, request = std::move(request),
                 completion = std::move(completion)]() mutable {
            auto result =
                worker->tasks && worker->tasks->activityStore()
                    ? worker->tasks->activityStore()->prepareAction(request)
                    : Result<ActionSnapshot, DomainError>::failure(domainError(
                          "STORE_UNAVAILABLE", "Activity store unavailable"));
            if (self)
                QMetaObject::invokeMethod(
                    self, [self, result = std::move(result),
                           completion = std::move(completion)]() mutable {
                        if (self && completion)
                            completion(std::move(result));
                    });
        });
}
void ActivityRuntime::markActionDispatchedAsync(
    ActionDispatch request,
    std::function<void(Result<ActionSnapshot, DomainError>)> completion) {
    auto *worker = m_worker;
    QPointer<ActivityRuntime> self(this);
    QMetaObject::invokeMethod(worker, [worker, self,
                                       request = std::move(request),
                                       completion =
                                           std::move(completion)]() mutable {
        auto result =
            worker->tasks && worker->tasks->activityStore()
                ? worker->tasks->activityStore()->markActionDispatched(request)
                : Result<ActionSnapshot, DomainError>::failure(domainError(
                      "STORE_UNAVAILABLE", "Activity store unavailable"));
        if (self)
            QMetaObject::invokeMethod(
                self, [self, result = std::move(result),
                       completion = std::move(completion)]() mutable {
                    if (self && completion)
                        completion(std::move(result));
                });
    });
}
void ActivityRuntime::observeActionAsync(
    ActionObservation observation,
    std::function<void(Result<void, DomainError>)> completion) {
    auto *worker = m_worker;
    QPointer<ActivityRuntime> self(this);
    QMetaObject::invokeMethod(
        worker, [worker, self, observation = std::move(observation),
                 completion = std::move(completion)]() mutable {
            auto result =
                worker->tasks && worker->tasks->activityStore()
                    ? worker->tasks->activityStore()->observeAction(observation)
                    : Result<void, DomainError>::failure(domainError(
                          "STORE_UNAVAILABLE", "Activity store unavailable"));
            if (self)
                QMetaObject::invokeMethod(
                    self, [self, result = std::move(result),
                           completion = std::move(completion)]() mutable {
                        if (self && completion)
                            completion(std::move(result));
                    });
        });
}
