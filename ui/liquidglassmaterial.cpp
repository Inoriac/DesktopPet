#include "liquidglassmaterial.h"

#include <QGuiApplication>
#include <QPixmap>
#include <QScreen>
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int kBlurRadius = 12;
constexpr int kSampleMaxSize = 48;
constexpr qreal kWcagAaContrast = 4.5;

#ifdef Q_OS_WIN
class ScopedCaptureExclusion {
public:
    explicit ScopedCaptureExclusion(QWidget* widget)
        : m_hwnd(widget ? reinterpret_cast<HWND>(widget->winId()) : nullptr) {
#ifndef WDA_EXCLUDEFROMCAPTURE
        constexpr DWORD WDA_EXCLUDEFROMCAPTURE = 0x00000011;
#endif
        m_restore = m_hwnd && SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE);
    }

    ~ScopedCaptureExclusion() {
        if (m_restore) {
            SetWindowDisplayAffinity(m_hwnd, 0);
        }
    }

private:
    HWND m_hwnd = nullptr;
    bool m_restore = false;
};
#endif

qreal srgbToLinear(qreal channel) {
    channel /= 255.0;
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

qreal relativeLuminance(const QColor& color) {
    return 0.2126 * srgbToLinear(color.red())
         + 0.7152 * srgbToLinear(color.green())
         + 0.0722 * srgbToLinear(color.blue());
}

qreal contrastRatio(const QColor& a, const QColor& b) {
    const qreal lighter = std::max(relativeLuminance(a), relativeLuminance(b));
    const qreal darker = std::min(relativeLuminance(a), relativeLuminance(b));
    return (lighter + 0.05) / (darker + 0.05);
}

QImage boxBlur(const QImage& src, int radius) {
    if (src.isNull() || radius <= 0) {
        return src;
    }

    const QImage input = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage temp(input.size(), input.format());
    QImage out(input.size(), input.format());
    const int w = input.width();
    const int h = input.height();
    const int count = radius * 2 + 1;

    for (int y = 0; y < h; ++y) {
        const QRgb* inLine = reinterpret_cast<const QRgb*>(input.constScanLine(y));
        QRgb* tempLine = reinterpret_cast<QRgb*>(temp.scanLine(y));
        int a = 0, r = 0, g = 0, b = 0;
        for (int x = -radius; x <= radius; ++x) {
            const QRgb px = inLine[std::clamp(x, 0, w - 1)];
            a += qAlpha(px); r += qRed(px); g += qGreen(px); b += qBlue(px);
        }
        for (int x = 0; x < w; ++x) {
            tempLine[x] = qRgba(r / count, g / count, b / count, a / count);
            const QRgb remove = inLine[std::clamp(x - radius, 0, w - 1)];
            const QRgb add = inLine[std::clamp(x + radius + 1, 0, w - 1)];
            a += qAlpha(add) - qAlpha(remove);
            r += qRed(add) - qRed(remove);
            g += qGreen(add) - qGreen(remove);
            b += qBlue(add) - qBlue(remove);
        }
    }

    for (int x = 0; x < w; ++x) {
        int a = 0, r = 0, g = 0, b = 0;
        for (int y = -radius; y <= radius; ++y) {
            const QRgb px = reinterpret_cast<const QRgb*>(temp.constScanLine(std::clamp(y, 0, h - 1)))[x];
            a += qAlpha(px); r += qRed(px); g += qGreen(px); b += qBlue(px);
        }
        for (int y = 0; y < h; ++y) {
            reinterpret_cast<QRgb*>(out.scanLine(y))[x] = qRgba(r / count, g / count, b / count, a / count);
            const QRgb remove = reinterpret_cast<const QRgb*>(temp.constScanLine(std::clamp(y - radius, 0, h - 1)))[x];
            const QRgb add = reinterpret_cast<const QRgb*>(temp.constScanLine(std::clamp(y + radius + 1, 0, h - 1)))[x];
            a += qAlpha(add) - qAlpha(remove);
            r += qRed(add) - qRed(remove);
            g += qGreen(add) - qGreen(remove);
            b += qBlue(add) - qBlue(remove);
        }
    }

    return out;
}

QImage captureBackground(QWidget* widget) {
    const QRect globalRect(widget->mapToGlobal(QPoint(0, 0)), widget->size());
    QScreen* screen = QGuiApplication::screenAt(globalRect.center());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }

#ifdef Q_OS_WIN
    ScopedCaptureExclusion captureExclusion(widget);
#endif

    return screen ? screen->grabWindow(0, globalRect.x(), globalRect.y(), globalRect.width(), globalRect.height()).toImage()
                  : QImage();
}

QColor averageColor(const QImage& image) {
    if (image.isNull()) {
        return QColor(245, 248, 252);
    }

    qint64 r = 0, g = 0, b = 0, count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = line[x];
            r += qRed(px); g += qGreen(px); b += qBlue(px);
            ++count;
        }
    }

    count = std::max<qint64>(1, count);
    return QColor(static_cast<int>(r / count), static_cast<int>(g / count), static_cast<int>(b / count));
}

QColor dominantColor(const QImage& image) {
    if (image.isNull()) {
        return QColor(245, 248, 252);
    }

    constexpr int kBinsPerChannel = 8;
    constexpr int kBinCount = kBinsPerChannel * kBinsPerChannel * kBinsPerChannel;
    std::array<int, kBinCount> counts{};
    std::array<qint64, kBinCount> rs{};
    std::array<qint64, kBinCount> gs{};
    std::array<qint64, kBinCount> bs{};

    for (int y = 0; y < image.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = line[x];
            const int index = (qRed(px) >> 5) * 64 + (qGreen(px) >> 5) * 8 + (qBlue(px) >> 5);
            ++counts[index];
            rs[index] += qRed(px); gs[index] += qGreen(px); bs[index] += qBlue(px);
        }
    }

    const int best = static_cast<int>(std::distance(counts.begin(), std::max_element(counts.begin(), counts.end())));
    const int count = std::max(1, counts[best]);
    return QColor(static_cast<int>(rs[best] / count), static_cast<int>(gs[best] / count), static_cast<int>(bs[best] / count));
}

QColor makeMaterialColor(const QColor& average, const QColor& dominant, int opacityPercent) {
    QColor tint = LiquidGlassMaterialAnalyzer::blendColors(average, dominant, 0.72);
    int h = 0, s = 0, l = 0, a = 0;
    tint.getHsl(&h, &s, &l, &a);
    if (h >= 0) {
        tint.setHsl(h, std::clamp(static_cast<int>(s * 1.28), 28, 190), std::clamp(l, 54, 205), a);
    }

    const QColor base = relativeLuminance(average) < 0.42
        ? LiquidGlassMaterialAnalyzer::blendColors(tint, QColor(18, 24, 34), 0.38)
        : LiquidGlassMaterialAnalyzer::blendColors(tint, QColor(255, 255, 255), 0.24);
    const int alpha = std::clamp(118 + opacityPercent, 152, 210);
    return QColor(base.red(), base.green(), base.blue(), alpha);
}

QColor chooseReadableTextColor(const QColor& materialColor, const QImage& thumbnail, const QColor& currentTextColor) {
    const QColor effective = LiquidGlassMaterialAnalyzer::blendColors(averageColor(thumbnail), materialColor, materialColor.alphaF());
    const qreal blackContrast = contrastRatio(Qt::black, effective);
    const qreal whiteContrast = contrastRatio(Qt::white, effective);

    if ((currentTextColor == Qt::black && blackContrast >= kWcagAaContrast && blackContrast + 0.65 >= whiteContrast)
        || (currentTextColor == Qt::white && whiteContrast >= kWcagAaContrast && whiteContrast + 0.65 >= blackContrast)) {
        return currentTextColor;
    }

    if (blackContrast >= kWcagAaContrast || whiteContrast >= kWcagAaContrast) {
        return blackContrast >= whiteContrast ? Qt::black : Qt::white;
    }
    return relativeLuminance(effective) > 0.5 ? Qt::black : Qt::white;
}
} // namespace

LiquidGlassMaterialSample LiquidGlassMaterialAnalyzer::analyze(QWidget* widget, int opacityPercent, const QColor& currentTextColor) {
    if (!widget || widget->size().isEmpty()) {
        return {};
    }

    const QImage captured = captureBackground(widget).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (captured.isNull()) {
        return {};
    }

    const QSize blurSize(std::max(24, widget->width() / 2), std::max(24, widget->height() / 2));
    const QImage thumbnail = captured.scaled(QSize(kSampleMaxSize, kSampleMaxSize), Qt::KeepAspectRatio, Qt::FastTransformation);
    const QColor average = averageColor(thumbnail);
    const QColor material = makeMaterialColor(average, dominantColor(thumbnail), opacityPercent);

    return {
        boxBlur(captured.scaled(blurSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), std::max(3, kBlurRadius / 2))
            .scaled(widget->size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
        material,
        chooseReadableTextColor(material, thumbnail, currentTextColor),
        true
    };
}

QColor LiquidGlassMaterialAnalyzer::blendColors(const QColor& a, const QColor& b, qreal t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor(static_cast<int>(std::round(a.red() + (b.red() - a.red()) * t)),
                  static_cast<int>(std::round(a.green() + (b.green() - a.green()) * t)),
                  static_cast<int>(std::round(a.blue() + (b.blue() - a.blue()) * t)),
                  static_cast<int>(std::round(a.alpha() + (b.alpha() - a.alpha()) * t)));
}

qreal LiquidGlassMaterialAnalyzer::colorDistance(const QColor& a, const QColor& b) {
    const qreal dr = a.red() - b.red();
    const qreal dg = a.green() - b.green();
    const qreal db = a.blue() - b.blue();
    const qreal da = (a.alpha() - b.alpha()) * 0.35;
    return std::sqrt(dr * dr + dg * dg + db * db + da * da);
}
