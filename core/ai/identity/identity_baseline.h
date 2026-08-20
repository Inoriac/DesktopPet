#ifndef DESKTOP_PET_IDENTITY_BASELINE_H
#define DESKTOP_PET_IDENTITY_BASELINE_H

#include <QJsonObject>
#include <QMap>
#include <QString>

struct IdentityBaseline {
    int schemaVersion = 1;
    QMap<QString, double> traits;
    QString speakingStyle;
    double anchorStrength = 0.95;

    static IdentityBaseline defaults();
    static IdentityBaseline fromJson(const QJsonObject& object);
    QJsonObject toJson() const;
};

#endif // DESKTOP_PET_IDENTITY_BASELINE_H
