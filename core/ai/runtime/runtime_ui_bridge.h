#ifndef DESKTOP_PET_RUNTIME_UI_BRIDGE_H
#define DESKTOP_PET_RUNTIME_UI_BRIDGE_H

#include <QString>

#include <functional>

class AnimationManager;
class AnimationPlayer;

class RuntimeUiBridge {
public:
    virtual ~RuntimeUiBridge() = default;
    virtual void showChatBubble(const QString& text, int durationMs) = 0;
    virtual void notifyUser(const QString& title, const QString& message,
                            int durationMs) = 0;
    virtual AnimationPlayer* animationPlayer() const = 0;
    virtual AnimationManager* animationManager() const = 0;
};

struct RuntimeUiCallbacks {
    std::function<void(const QString&, int)> showChatBubble;
    std::function<void(const QString&, const QString&, int)> notifyUser;
};

class CallbackRuntimeUiBridge final : public RuntimeUiBridge {
public:
    CallbackRuntimeUiBridge(RuntimeUiCallbacks callbacks,
                            AnimationPlayer* animationPlayer,
                            AnimationManager* animationManager);

    void showChatBubble(const QString& text, int durationMs) override;
    void notifyUser(const QString& title, const QString& message,
                    int durationMs) override;
    void setAnimationSystem(AnimationPlayer* animationPlayer,
                            AnimationManager* animationManager);
    AnimationPlayer* animationPlayer() const override { return m_animationPlayer; }
    AnimationManager* animationManager() const override { return m_animationManager; }

private:
    RuntimeUiCallbacks m_callbacks;
    AnimationPlayer* m_animationPlayer = nullptr;
    AnimationManager* m_animationManager = nullptr;
};

#endif // DESKTOP_PET_RUNTIME_UI_BRIDGE_H
