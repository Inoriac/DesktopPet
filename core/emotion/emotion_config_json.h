#ifndef DESKTOP_PET_EMOTION_CONFIG_JSON_H
#define DESKTOP_PET_EMOTION_CONFIG_JSON_H

#include "emotion_types.h"

#include <QJsonObject>

EmotionConfig parseEmotionConfig(const QJsonObject& object);

#endif // DESKTOP_PET_EMOTION_CONFIG_JSON_H
