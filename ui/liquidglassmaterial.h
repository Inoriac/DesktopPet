#ifndef DESKTOP_PET_LIQUIDGLASSMATERIAL_H
#define DESKTOP_PET_LIQUIDGLASSMATERIAL_H

#include <QColor>
#include <QImage>
#include <QWidget>

struct LiquidGlassMaterialSample {
    QImage background;
    QColor materialColor;
    QColor textColor;
    bool valid = false;
};

class LiquidGlassMaterialAnalyzer {
public:
    static LiquidGlassMaterialSample analyze(QWidget* widget, int opacityPercent, const QColor& currentTextColor);
    static QColor blendColors(const QColor& a, const QColor& b, qreal t);
    static qreal colorDistance(const QColor& a, const QColor& b);
};

#endif // DESKTOP_PET_LIQUIDGLASSMATERIAL_H
