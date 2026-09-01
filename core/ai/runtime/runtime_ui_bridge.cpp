#include "runtime_ui_bridge.h"

#include <utility>

CallbackRuntimeUiBridge::CallbackRuntimeUiBridge(
    RuntimeUiCallbacks callbacks,
    AnimationPlayer* animationPlayer,
    AnimationManager* animationManager)
    : m_callbacks(std::move(callbacks))
    , m_animationPlayer(animationPlayer)
    , m_animationManager(animationManager) {}

void CallbackRuntimeUiBridge::showChatBubble(const QString& text, int durationMs) {
    if (m_callbacks.showChatBubble) m_callbacks.showChatBubble(text, durationMs);
}

void CallbackRuntimeUiBridge::notifyUser(const QString& title,
                                         const QString& message,
                                         int durationMs) {
    if (m_callbacks.notifyUser) m_callbacks.notifyUser(title, message, durationMs);
}

void CallbackRuntimeUiBridge::setAnimationSystem(
    AnimationPlayer* animationPlayer,
    AnimationManager* animationManager) {
    m_animationPlayer = animationPlayer;
    m_animationManager = animationManager;
}
