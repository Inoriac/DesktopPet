#ifndef DESKTOP_PET_RUNTIME_UI_BRIDGE_H
#define DESKTOP_PET_RUNTIME_UI_BRIDGE_H

#include <QString>

#include <functional>

class AvatarPresentation;

class RuntimeUiBridge {
public:
    virtual ~RuntimeUiBridge() = default;
    virtual void showChatBubble(const QString& text, int durationMs) = 0;
    virtual void notifyUser(const QString& title, const QString& message,
                            int durationMs) = 0;
    virtual AvatarPresentation* presentation() const = 0;
};

struct RuntimeUiCallbacks {
    std::function<void(const QString&, int)> showChatBubble;
    std::function<void(const QString&, const QString&, int)> notifyUser;
};

class CallbackRuntimeUiBridge final : public RuntimeUiBridge {
public:
    CallbackRuntimeUiBridge(RuntimeUiCallbacks callbacks,
                            AvatarPresentation* presentation = nullptr);

    void showChatBubble(const QString& text, int durationMs) override;
    void notifyUser(const QString& title, const QString& message,
                    int durationMs) override;
    AvatarPresentation* presentation() const override { return m_presentation; }

private:
    RuntimeUiCallbacks m_callbacks;
    AvatarPresentation* m_presentation = nullptr;
};

#endif // DESKTOP_PET_RUNTIME_UI_BRIDGE_H
