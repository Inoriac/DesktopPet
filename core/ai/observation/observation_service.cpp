#include "observation_service.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <algorithm>
#include <deque>

Result<SourceLaunch, DomainError>
prepareSourceLaunch(const ObservationBinding &b,
                    const LoadedObservationSource &source,
                    const QJsonObject &settings) {
  using R = Result<SourceLaunch, DomainError>;
  auto fail = [](const QString &code, const QString &message) {
    return R::failure(domainError(code, message));
  };
  SourceLaunch launch;
  launch.binding = b;
  launch.source = source;
  launch.generation = source.catalogRevision;
  if (source.pluginId != b.pluginId ||
      source.id != b.source["sourceId"].toString())
    return fail("UNKNOWN_REFERENCE",
                "Source identity does not match host binding");
  for (const auto &value : source.definition["configuration"].toArray()) {
    const auto d = value.toObject();
    const auto v = settings[d["key"].toString()];
    if (d["required"].toBool() && (!v.isString() || v.toString().isEmpty()))
      return fail("DEPENDENCY_UNAVAILABLE",
                  "Required source configuration is absent");
    if (v.isUndefined())
      continue;
    if (!v.isString())
      return fail("INVALID_INPUT", "Source configuration must contain strings");
    if ((d["kind"] == "path" || d["kind"] == "executable") &&
        !QDir::isAbsolutePath(v.toString()))
      return fail("INVALID_INPUT",
                  "Source path configuration must be absolute");
    if (d["kind"] == "secret")
      launch.secrets.append(v.toString());
  }
  const auto invocation = source.definition["invocation"].toObject();
  launch.program =
      settings[invocation["program"].toObject()["setting"].toString()]
          .toString();
  if (launch.program != b.source["executor"].toString())
    return fail("PERMISSION_DENIED",
                "Source executor does not match host grant");
  if (!QDir::isAbsolutePath(launch.program) ||
      !QFileInfo(launch.program).isExecutable())
    return fail("DEPENDENCY_UNAVAILABLE", "Source executable is unavailable");
  for (const auto &value : invocation["arguments"].toArray()) {
    auto v = value.isString()
                 ? value
                 : settings[value.toObject()["setting"].toString()];
    if (!v.isString())
      return fail("DEPENDENCY_UNAVAILABLE",
                  "Source argument configuration is absent");
    launch.arguments.append(v.toString());
  }
  return R::success(launch);
}
struct ObservationService::Worker : QObject {
  struct Session {
    SourceLaunch launch;
    QProcess *process = nullptr;
    QTimer *retry = nullptr;
    QByteArray buffer;
    QString committedCursor;
    qint64 lastNotice = 0;
    bool enabled = false;
    int retries = 0;
  };
  struct Queued {
    std::shared_ptr<Session> session;
    QJsonObject payload;
  };
  QString profile, path;
  QPointer<ObservationService> owner;
  QHash<QString, std::shared_ptr<Session>> sessions;
  std::deque<Queued> queue;
  bool draining = false, closing = false;
  Worker(QString p, QString db, ObservationService *o)
      : profile(std::move(p)), path(std::move(db)), owner(o) {
    auto *recovery = new QTimer(this);
    recovery->setInterval(1000);
    connect(recovery, &QTimer::timeout, this,
            [this] { ConversationStore(profile, path).recoverOutbox(); });
    QMetaObject::invokeMethod(this, [recovery] { recovery->start(); });
  }
  void state(const std::shared_ptr<Session> &s, const QString &status,
             const QString &error = {}) {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if ((status == "status" || status == "diagnostic") &&
        now - s->lastNotice < 1000)
      return;
    s->lastNotice = now;
    SourceState value{s->launch.binding.id, status, error, s->committedCursor,
                      s->launch.generation};
    QMetaObject::invokeMethod(QCoreApplication::instance(), [o = owner, value] {
      if (o)
        emit o->sourceStateChanged(value);
    });
  }
  void accepted(const AcceptedMessage &message) {
    QMetaObject::invokeMethod(QCoreApplication::instance(),
                              [o = owner, message] {
                                if (o)
                                  emit o->messageAccepted(message);
                              });
  }
  void stop(const std::shared_ptr<Session> &s) {
    s->enabled = false;
    if (s->retry) {
      s->retry->stop();
      s->retry->deleteLater();
      s->retry = nullptr;
    }
    if (s->process) {
      auto *p = s->process;
      s->process = nullptr;
      p->disconnect(this);
      if (p->state() != QProcess::NotRunning) {
        p->write("{\"type\":\"stop\"}\n");
        connect(p, &QProcess::finished, p, &QObject::deleteLater);
        p->kill();
      } else
        p->deleteLater();
    }
    s->buffer.clear();
  }
  void allStop() {
    for (const auto &s : sessions)
      stop(s);
    sessions.clear();
  }
  bool write(const std::shared_ptr<Session> &s, const QJsonObject &o) {
    auto *p = s->process;
    if (!p || p->state() != QProcess::Running)
      return false;
    const auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    if (p->bytesToWrite() + bytes.size() > 65536) {
      state(s, "replay_required", "SOURCE_STDIN_LIMIT");
      stop(s);
      return false;
    }
    return p->write(bytes) == bytes.size();
  }
  void reconnect(const std::shared_ptr<Session> &s) {
    if (!s->enabled)
      return;
    auto valid = s->launch.validate ? s->launch.validate()
                                    : Result<void, DomainError>::success();
    if (!valid) {
      state(s, "unavailable", valid.error().code);
      stop(s);
      return;
    }
    state(s, "reconnecting");
    if (!s->retry) {
      s->retry = new QTimer(this);
      s->retry->setSingleShot(true);
      connect(s->retry, &QTimer::timeout, this, [this, s] { start(s); });
    }
    s->retry->start(qMin(30000, 100 * (1 << qMin(8, s->retries++))));
  }
  void start(const std::shared_ptr<Session> &s) {
    if (!s->enabled)
      return;
    auto valid = s->launch.validate ? s->launch.validate()
                                    : Result<void, DomainError>::success();
    if (!valid) {
      state(s, "unavailable", valid.error().code);
      stop(s);
      return;
    }
    auto cursor = ObservationStore(profile, path).cursor(s->launch.binding);
    if (!cursor) {
      state(s, "unavailable", cursor.error().code);
      reconnect(s);
      return;
    }
    s->committedCursor = cursor.value();
    auto *p = new QProcess(this);
    s->process = p;
    s->buffer.clear();
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONDONTWRITEBYTECODE", "1");
    p->setProcessEnvironment(env);
    connect(p, &QProcess::started, this, [this, s, cursor] {
      if (!s->enabled)
        return;
      write(s, {{"type", "start"}, {"cursor", cursor.value()}});
      state(s, "running");
    });
    connect(p, &QProcess::readyReadStandardError, this, [this, s, p] {
      p->setReadChannel(QProcess::StandardError);
      const bool overflow = p->bytesAvailable() > 65536;
      // Diagnostics expose a fixed code only; discard bounded chunks, never
      // copy arbitrary process output (which may include credentials).
      qint64 remaining = 65536;
      while (remaining > 0 && p->bytesAvailable() > 0)
        remaining -= p->read(qMin<qint64>(8192, remaining)).size();
      p->setReadChannel(QProcess::StandardOutput);
      state(s, overflow ? "replay_required" : "diagnostic",
            overflow ? "SOURCE_STDERR_LIMIT" : "SOURCE_STDERR");
      if (overflow)
        stop(s);
    });
    connect(p, &QProcess::readyReadStandardOutput, this, [this, s, p] {
      p->setReadChannel(QProcess::StandardOutput);
      int lines = 0;
      while (s->process == p && p->bytesAvailable() > 0) {
        // Bound temporary copies as well as the retained partial line.
        s->buffer += p->read(qMin<qint64>(8192, 65537 - s->buffer.size()));
        while (s->process == p) {
          const auto newline = s->buffer.indexOf('\n');
          if (newline < 0) {
            if (s->buffer.size() > 65536) {
              state(s, "replay_required", "SOURCE_LINE_LIMIT");
              stop(s);
            }
            break;
          }
          if (newline > 65536) {
            state(s, "replay_required", "SOURCE_LINE_LIMIT");
            stop(s);
            break;
          }
          auto line = s->buffer.left(newline);
          s->buffer.remove(0, newline + 1);
          if (line.endsWith('\r'))
            line.chop(1);
          QJsonParseError error;
          const auto doc = QJsonDocument::fromJson(line, &error);
          if (error.error != QJsonParseError::NoError || !doc.isObject() ||
              QString::fromUtf8(line).toUtf8() != line) {
            state(s, "replay_required", "SOURCE_INVALID_JSON");
            stop(s);
            break;
          }
          if (++lines > 4096) {
            state(s, "replay_required", "SOURCE_OUTPUT_LIMIT");
            stop(s);
            break;
          }
          const auto o = doc.object();
          if (o["type"] == "status") {
            state(s, "status");
            continue;
          }
          if (o["type"] != "observation") {
            state(s, "replay_required", "SOURCE_INVALID_TYPE");
            stop(s);
            break;
          }
          if (queue.size() >= 256) {
            state(s, "replay_required", "SOURCE_QUEUE_LIMIT");
            stop(s);
            break;
          }
          queue.push_back({s, o});
        }
      }
      if (!queue.empty() && !draining) {
        draining = true;
        QTimer::singleShot(0, this, [this] { drain(); });
      }
    });
    connect(p, &QProcess::finished, this, [this, s, p] {
      if (s->process != p)
        return;
      s->process = nullptr;
      p->deleteLater();
      s->buffer.clear();
      reconnect(s);
    });
    connect(p, &QProcess::errorOccurred, this,
            [this, s, p](QProcess::ProcessError error) {
              if (error == QProcess::FailedToStart && s->process == p) {
                s->process = nullptr;
                p->deleteLater();
                reconnect(s);
              }
            });
    p->start(s->launch.program, s->launch.arguments);
  }
  void drain() {
    if (queue.empty()) {
      draining = false;
      if (closing) {
        bool live = false;
        for (auto *p : findChildren<QProcess *>())
          live = live || p->state() != QProcess::NotRunning;
        if (live)
          QTimer::singleShot(25, this, [this] { drain(); });
        else
          thread()->quit();
      }
      return;
    }
    const auto next = queue.front();
    const auto s = next.session;
    auto result =
        ObservationStore(profile, path)
            .ingest(s->launch.binding,
                    {s->launch.binding.id, next.payload, s->launch.generation});
    if (result) {
      queue.pop_front();
      s->committedCursor = result.value().cursor;
      if (s->enabled && sessions.value(s->launch.binding.id) == s)
        write(s, {{"type", "ack"},
                  {"eventId", result.value().eventId},
                  {"cursor", result.value().cursor}});
      accepted(result.value().message);
      QTimer::singleShot(0, this, [this] { drain(); });
    } else {
      state(s, "replay_required", result.error().code);
      stop(s);
      if (result.error().code == "STORE_UNAVAILABLE") {
        // Preserve this bound fact and block later journal watermarks.
        QTimer::singleShot(250, this, [this] { drain(); });
      } else {
        // An invalid journal entry cannot authorize advancing past it.
        queue.erase(
            std::remove_if(queue.begin(), queue.end(),
                           [&](const Queued &v) { return v.session == s; }),
            queue.end());
        QTimer::singleShot(0, this, [this] { drain(); });
      }
    }
  }
  void shutdown() {
    closing = true;
    owner.clear();
    allStop();
    if (!draining) {
      draining = true;
      drain();
    }
  }
};
ObservationService::ObservationService(QString profile, QString path,
                                       QObject *parent)
    : QObject(parent) {
  m_thread = new QThread;
  m_worker = new Worker(std::move(profile), std::move(path), this);
  m_worker->moveToThread(m_thread);
  connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
  connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
  m_thread->start();
}
ObservationService::~ObservationService() {
  std::lock_guard lock(m_mutex);
  auto *worker = m_worker;
  m_worker = nullptr;
  // The worker owns already received facts until durable commit. Retirement
  // cannot discard them or block the UI behind a SQLite lock.
  QMetaObject::invokeMethod(worker, [worker] { worker->shutdown(); });
}
void ObservationService::post(std::function<void(Worker *)> action) {
  std::lock_guard lock(m_mutex);
  if (m_worker)
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, action = std::move(action)] { action(worker); });
}
void ObservationService::ingestObservation(ObservationBinding binding,
                                           ObservationEnvelope observation,
                                           ObservationCompletion completion) {
  post([binding = std::move(binding), observation = std::move(observation),
        completion = std::move(completion)](Worker *w) {
    auto result =
        ObservationStore(w->profile, w->path).ingest(binding, observation);
    if (result)
      w->accepted(result.value().message);
    if (completion)
      completion(std::move(result));
  });
}
void ObservationService::setObservationSource(SourceLaunch launch, bool enabled,
                                              SourceCompletion completion) {
  post([launch = std::move(launch), enabled,
        completion = std::move(completion)](Worker *w) {
    auto old = w->sessions.value(launch.binding.id);
    if (old)
      w->stop(old);
    auto s = std::make_shared<Worker::Session>();
    s->launch = launch;
    s->enabled = enabled;
    w->sessions[launch.binding.id] = s;
    if (enabled)
      w->start(s);
    else
      w->state(s, "disabled");
    if (completion)
      completion(Result<SourceState, DomainError>::success(
          {launch.binding.id,
           enabled ? "starting" : "disabled",
           {},
           {},
           launch.generation}));
  });
}
void ObservationService::stopSources() {
  post([](Worker *w) { w->allStop(); });
}
