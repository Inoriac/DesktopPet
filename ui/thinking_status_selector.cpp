#include "thinking_status_selector.h"

#include <QStringList>

namespace {

const QStringList& presetsFor(ChatActivityStage stage) {
    static const QStringList waiting = {
        QStringLiteral("摸鱼中"),
        QStringLiteral("脑袋转转"),
        QStringLiteral("灵感加载中"),
        QStringLiteral("认真想想")};
    static const QStringList streaming = {
        QStringLiteral("组织语言中"),
        QStringLiteral("慢慢说给你听")};
    static const QStringList preparing = {
        QStringLiteral("准备动手"),
        QStringLiteral("翻翻工具箱"),
        QStringLiteral("安排一下步骤")};
    static const QStringList running = {
        QStringLiteral("忙活一下"),
        QStringLiteral("动手处理中"),
        QStringLiteral("工具箱翻找中")};
    static const QStringList finalizing = {
        QStringLiteral("收尾中"),
        QStringLiteral("整理结果中"),
        QStringLiteral("马上就好")};

    switch (stage) {
    case ChatActivityStage::WaitingForModel:
        return waiting;
    case ChatActivityStage::StreamingText:
        return streaming;
    case ChatActivityStage::PreparingTool:
        return preparing;
    case ChatActivityStage::RunningTool:
        return running;
    case ChatActivityStage::Finalizing:
        return finalizing;
    }
    return waiting;
}

} // namespace

QString ThinkingStatusSelector::next(ChatActivityStage stage,
                                     const QString& requestId) {
    if (requestId != m_requestId) reset(requestId);
    const QStringList& presets = presetsFor(stage);
    if (presets.isEmpty()) return {};
    const int key = static_cast<int>(stage);
    const int index = m_nextIndexes.value(key, 0) % presets.size();
    m_nextIndexes.insert(key, (index + 1) % presets.size());
    return presets.at(index);
}

void ThinkingStatusSelector::reset(const QString& requestId) {
    m_requestId = requestId;
    m_nextIndexes.clear();
}
