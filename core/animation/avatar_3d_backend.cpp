#include "avatar_3d_backend.h"
Avatar3DBackend::Avatar3DBackend(AnimationPlayer* player, QJsonObject resources,
                                 std::function<void(bool)> drawing,
                                 std::function<bool(QPoint, QPoint*)> movement, QObject* parent)
    : AvatarBackend(parent), m_player(player), m_resources(std::move(resources)),
      m_drawing(std::move(drawing)), m_movement(std::move(movement)) {
    if (player) {
        player->setManaged(true);
        connect(player, &AnimationPlayer::cycleFinished, this, &AvatarBackend::cycleFinished);
        connect(player, &AnimationPlayer::playbackFailed, this, &AvatarBackend::playbackFailed);
        connect(player, &QObject::destroyed, this, [this] {
            m_player = nullptr;
            emit playbackFailed(m_generation, "animation_player_destroyed");
        });
    }
}
bool Avatar3DBackend::start(const QString& state, const QString& action, bool loop, quint64 generation,
                            QString* error) {
    if (!m_player) {
        *error = "animation_player_unavailable";
        return false;
    }
    if (!m_player->startManaged(state, action, loop, generation, error))
        return false;
    m_generation = generation;
    return true;
}
void Avatar3DBackend::stop(const QString& initialState) {
    if (m_player)
        m_player->resetManaged(initialState);
}
void Avatar3DBackend::setDrawing(bool enabled) {
    if (m_drawing)
        m_drawing(enabled);
}
bool Avatar3DBackend::moveTo(const QPoint& target, QPoint* actual) {
    return m_movement && m_movement(target, actual);
}
