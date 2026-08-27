#ifndef DESKTOP_PET_LLM_STREAM_TYPES_H
#define DESKTOP_PET_LLM_STREAM_TYPES_H

#include <QMetaType>
#include <QString>
#include <functional>

enum class ChatActivityStage {
    WaitingForModel,
    StreamingText,
    PreparingTool,
    RunningTool,
    Finalizing
};

enum class ChatMessageStatus {
    Pending,
    Streaming,
    Complete,
    Interrupted,
    Stopped,
    Failed
};

enum class LlmStreamEventType {
    Started,
    StageChanged,
    TextDelta
};

struct LlmStreamEvent {
    LlmStreamEventType type = LlmStreamEventType::Started;
    QString requestId;
    ChatActivityStage stage = ChatActivityStage::WaitingForModel;
    QString textDelta;
};

class LlmRequestHandle {
public:
    virtual ~LlmRequestHandle() = default;
    virtual void cancel() = 0;
    virtual bool isCancelled() const = 0;
};

using LlmStreamObserver = std::function<void(const LlmStreamEvent&)>;

Q_DECLARE_METATYPE(ChatMessageStatus)

#endif // DESKTOP_PET_LLM_STREAM_TYPES_H
