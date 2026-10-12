#pragma once
#include <QPoint>
#include <QString>
#include <Qt>
// Only the composition root chooses the concrete render widget. Interaction
// consumers use this interface, without knowing cameras, skeletons or rays.
class AvatarView {
  public:
    virtual ~AvatarView() = default;
    virtual QString hitRegion(const QPoint& localPosition) const = 0;
    virtual bool handleCameraKey(int key, Qt::KeyboardModifiers modifiers) = 0;
};
