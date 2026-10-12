#pragma once
#include "animation_player.h"
#include "presentation/avatar_presentation.h"

// Skeletal playback adapter. View/window effects are injected here so its
// playback lifecycle can be verified without an OpenGL context.
class Avatar3DBackend final : public AvatarBackend {
    Q_OBJECT
  public:
    Avatar3DBackend(AnimationPlayer* player, QJsonObject resources, std::function<void(bool)> drawing,
                    std::function<bool(QPoint, QPoint*)> movement, QObject* parent = nullptr);
    QJsonObject resources() const override {
        return m_resources;
    }
    bool start(const QString& state, const QString& action, bool loop, quint64 generation,
               QString* error) override;
    void stop(const QString& initialState) override;
    void setDrawing(bool enabled) override;
    bool moveTo(const QPoint& target, QPoint* actual) override;

  private:
    QPointer<AnimationPlayer> m_player;
    QJsonObject m_resources;
    std::function<void(bool)> m_drawing;
    std::function<bool(QPoint, QPoint*)> m_movement;
    quint64 m_generation = 0;
};
