#include "task_tool_executor.h"
#include "ai/tools/web_tools.h"
#include "ai/tools/file_tools.h"
#include "ai/tools/life_tools.h"
#include "ai/tools/environment_tools.h"
#include "ai/tools/music_tools.h"

namespace {
std::unique_ptr<AITool> makeTool(const QString& name, const QStringList& roots) {
    if (name == "web_search") return std::make_unique<WebSearchTool>();
    if (name == "web_fetch") return std::make_unique<WebFetchTool>();
    if (name == "read_text_file") return std::make_unique<ReadTextFileTool>(roots);
    if (name == "list_directory") return std::make_unique<ListDirectoryTool>(roots);
    if (name == "weather_query") return std::make_unique<WeatherQueryTool>();
    if (name == "holiday_query") return std::make_unique<HolidayQueryTool>();
    if (name == "daily_briefing") return std::make_unique<DailyBriefingTool>();
    if (name == "get_current_time") return std::make_unique<GetCurrentTimeTool>();
    if (name == "get_user_idle_state") return std::make_unique<GetUserIdleStateTool>();
    if (name == "get_battery_status") return std::make_unique<GetBatteryStatusTool>();
    if (name == "get_network_status") return std::make_unique<GetNetworkStatusTool>();
    if (name == "lx_music_status") return std::make_unique<LxMusicStatusTool>();
    if (name == "lx_music_lyric") return std::make_unique<LxMusicLyricTool>();
    if (name == "lx_music_list_playlists") return std::make_unique<LxMusicListPlaylistsTool>();
    if (name == "lx_music_playlist_songs") return std::make_unique<LxMusicPlaylistSongsTool>();
    if (name == "lx_music_play") return std::make_unique<LxMusicPlayTool>();
    if (name == "lx_music_pause") return std::make_unique<LxMusicPauseTool>();
    if (name == "lx_music_skip_next") return std::make_unique<LxMusicSkipNextTool>();
    if (name == "lx_music_skip_prev") return std::make_unique<LxMusicSkipPrevTool>();
    if (name == "lx_music_set_volume") return std::make_unique<LxMusicVolumeTool>();
    return {};
}
}

TaskToolExecutor::TaskToolExecutor(QObject* parent) : QObject(parent), m_worker(new QObject) {
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("ComplexTaskTools"));
    m_thread.start();
}

TaskToolExecutor::~TaskToolExecutor() {
    for (const auto& cancelled : m_cancelled) cancelled->store(true);
    m_thread.quit();
    // Existing tools have bounded I/O waits. Finish ownership on shutdown;
    // never terminate a thread while a network object is being destroyed.
    m_thread.wait();
}

bool TaskToolExecutor::supports(const QString& name) { return bool(makeTool(name, {})); }

void TaskToolExecutor::execute(ToolExecutionRequest request,
                             std::function<void(ToolExecutionOutcome)> completion) {
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_cancelled.insert(request.requestId, cancelled);
    QMetaObject::invokeMethod(m_worker, [this, cancelled, request, completion = std::move(completion)]() mutable {
        if (cancelled->load()) return;
        ToolRegistry registry;
        auto tool = makeTool(request.toolName, request.policyContext.allowedRootPaths);
        ToolExecutionOutcome outcome;
        if (tool) {
            registry.registerTool(std::move(tool));
            ToolRuntime runtime;
            runtime.setToolRegistry(&registry);
            if (request.toolName == "web_fetch")
                request.arguments.insert("timeout_ms", qBound(1, request.timeoutMs, 15000));
            outcome = runtime.execute(request);
        } else {
            outcome.requestId = request.requestId;
            outcome.toolName = request.toolName;
            outcome.result = ToolResult::fail(QStringLiteral("工具未配置异步执行器"));
        }
        QMetaObject::invokeMethod(this, [this, cancelled, outcome, completion = std::move(completion)]() mutable {
            m_cancelled.remove(outcome.requestId);
            if (!cancelled->load()) completion(outcome);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void TaskToolExecutor::cancel(const QString& id) {
    const auto token = m_cancelled.take(id);
    if (token) token->store(true);
}
