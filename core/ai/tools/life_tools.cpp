//
// 生活助理工具
//

#include "life_tools.h"

#include <QDate>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {
constexpr int kWeatherTimeoutMs = 8000;

QJsonObject makeStringProperty(const QString& description, const QString& defaultValue = {}) {
    QJsonObject obj;
    obj["type"] = "string";
    obj["description"] = description;
    if (!defaultValue.isEmpty()) {
        obj["default"] = defaultValue;
    }
    return obj;
}

QJsonObject queryWttr(const QString& location, const QString& language) {
    QUrl url(QString("https://wttr.in/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(location))));
    QUrlQuery query;
    query.addQueryItem("format", "j1");
    query.addQueryItem("lang", language.isEmpty() ? "zh" : language);
    url.setQuery(query);

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Desktop-Pet-LifeAssistant/1.0");

    QEventLoop loop;
    QNetworkReply* reply = manager.get(request);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    timer.start(kWeatherTimeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        return {{"error", QString("天气查询超时")}};
    }

    if (reply->error() != QNetworkReply::NoError) {
        const QString error = reply->errorString();
        reply->deleteLater();
        return {{"error", QString("天气查询失败: %1").arg(error)}};
    }

    const QByteArray bytes = reply->readAll();
    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) {
        return {{"error", QString("天气响应格式无效")}};
    }
    return doc.object();
}

QString weatherDesc(const QJsonObject& obj) {
    const QJsonArray desc = obj.value("weatherDesc").toArray();
    if (!desc.isEmpty()) {
        return desc.first().toObject().value("value").toString();
    }
    return {};
}

QString chineseHolidayName(const QDate& date) {
    const int month = date.month();
    const int day = date.day();
    if (month == 1 && day == 1) return "元旦";
    if (month == 2 && day == 14) return "情人节";
    if (month == 3 && day == 8) return "妇女节";
    if (month == 4 && day == 1) return "愚人节";
    if (month == 5 && day == 1) return "劳动节";
    if (month == 5 && day == 4) return "青年节";
    if (month == 6 && day == 1) return "儿童节";
    if (month == 10 && day == 1) return "国庆节";
    if (month == 12 && day == 24) return "平安夜";
    if (month == 12 && day == 25) return "圣诞节";
    return {};
}

QStringList briefingTips(const QJsonObject& weatherData) {
    QStringList tips;
    const QJsonArray current = weatherData.value("current_condition").toArray();
    if (!current.isEmpty()) {
        const QJsonObject now = current.first().toObject();
        const int precip = now.value("precipMM").toString().toDouble() > 0.0 ? 1 : 0;
        const int humidity = now.value("humidity").toString().toInt();
        const int tempC = now.value("temp_C").toString().toInt();
        const QString desc = weatherDesc(now);
        if (precip || desc.contains("雨") || desc.contains("snow", Qt::CaseInsensitive)) {
            tips.append("可能有降水，出门记得带伞。 ");
        }
        if (tempC >= 30) {
            tips.append("天气偏热，注意补水和防晒。 ");
        } else if (tempC <= 5) {
            tips.append("气温较低，记得保暖。 ");
        }
        if (humidity >= 80) {
            tips.append("湿度较高，体感可能会闷。 ");
        }
    }
    if (tips.isEmpty()) {
        tips.append("今天也要记得喝水和适当休息。 ");
    }
    return tips;
}
}

WeatherQueryTool::WeatherQueryTool()
    : AITool(
          "weather_query",
          "查询指定城市的当前天气和简要预报。用于出门提醒、穿衣建议和每日简报。",
          ToolCategory::Query) {}

QJsonObject WeatherQueryTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    properties["location"] = makeStringProperty("城市或地点，如 Beijing、Shanghai、北京；默认 auto:ip", "auto:ip");
    properties["language"] = makeStringProperty("返回语言，默认 zh", "zh");
    schema["properties"] = properties;
    return schema;
}

bool WeatherQueryTool::validate(const QJsonObject& params) const {
    Q_UNUSED(params)
    return true;
}

ToolResult WeatherQueryTool::execute(const QJsonObject& params) {
    const QString location = params.value("location").toString("auto:ip").trimmed();
    const QString language = params.value("language").toString("zh").trimmed();
    const QJsonObject raw = queryWttr(location.isEmpty() ? QString("auto:ip") : location, language);
    if (raw.contains("error")) {
        return ToolResult::fail(raw.value("error").toString());
    }

    QJsonObject result;
    result["location"] = location.isEmpty() ? QString("auto:ip") : location;

    const QJsonArray current = raw.value("current_condition").toArray();
    if (!current.isEmpty()) {
        const QJsonObject now = current.first().toObject();
        result["temperature_c"] = now.value("temp_C").toString();
        result["feels_like_c"] = now.value("FeelsLikeC").toString();
        result["humidity"] = now.value("humidity").toString();
        result["wind_kmph"] = now.value("windspeedKmph").toString();
        result["description"] = weatherDesc(now);
        result["precip_mm"] = now.value("precipMM").toString();
    }

    const QJsonArray weather = raw.value("weather").toArray();
    if (!weather.isEmpty()) {
        const QJsonObject today = weather.first().toObject();
        result["today_max_c"] = today.value("maxtempC").toString();
        result["today_min_c"] = today.value("mintempC").toString();
        result["sunrise"] = today.value("astronomy").toArray().isEmpty()
            ? QString()
            : today.value("astronomy").toArray().first().toObject().value("sunrise").toString();
        result["sunset"] = today.value("astronomy").toArray().isEmpty()
            ? QString()
            : today.value("astronomy").toArray().first().toObject().value("sunset").toString();
    }

    result["source"] = "wttr.in";
    return ToolResult::ok(result);
}

HolidayQueryTool::HolidayQueryTool()
    : AITool(
          "holiday_query",
          "查询指定日期是否是周末或内置常见节日。第一版使用本地规则，不联网。",
          ToolCategory::Query) {}

QJsonObject HolidayQueryTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    properties["date"] = makeStringProperty("日期 yyyy-MM-dd，默认今天");
    schema["properties"] = properties;
    return schema;
}

ToolResult HolidayQueryTool::execute(const QJsonObject& params) {
    QDate date = QDate::currentDate();
    const QString dateText = params.value("date").toString().trimmed();
    if (!dateText.isEmpty()) {
        const QDate parsed = QDate::fromString(dateText, Qt::ISODate);
        if (!parsed.isValid()) {
            return ToolResult::fail("日期格式无效，请使用 yyyy-MM-dd");
        }
        date = parsed;
    }

    const QString holidayName = chineseHolidayName(date);
    const bool weekend = date.dayOfWeek() >= 6;

    QJsonObject result;
    result["date"] = date.toString(Qt::ISODate);
    result["day_of_week"] = date.toString("dddd");
    result["is_weekend"] = weekend;
    result["holiday_name"] = holidayName;
    result["is_holiday_hint"] = weekend || !holidayName.isEmpty();
    result["note"] = holidayName.isEmpty()
        ? QString("第一版仅包含周末和少量固定公历节日，不含调休与农历节日。")
        : QString("今天是%1。第一版不含调休判断。 ").arg(holidayName);
    return ToolResult::ok(result);
}

DailyBriefingTool::DailyBriefingTool()
    : AITool(
          "daily_briefing",
          "生成轻量每日简报：当前日期、节假日提示、天气摘要和生活建议。",
          ToolCategory::Query) {}

QJsonObject DailyBriefingTool::parameterSchema() const {
    QJsonObject schema;
    schema["type"] = "object";
    QJsonObject properties;
    properties["location"] = makeStringProperty("城市或地点，如 Beijing、北京；默认 auto:ip", "auto:ip");
    schema["properties"] = properties;
    return schema;
}

ToolResult DailyBriefingTool::execute(const QJsonObject& params) {
    const QString location = params.value("location").toString("auto:ip").trimmed();
    const QDate today = QDate::currentDate();
    const QString holidayName = chineseHolidayName(today);
    const QJsonObject weatherData = queryWttr(location.isEmpty() ? QString("auto:ip") : location, "zh");

    QJsonObject result;
    result["date"] = today.toString(Qt::ISODate);
    result["day_of_week"] = today.toString("dddd");
    result["is_weekend"] = today.dayOfWeek() >= 6;
    result["holiday_name"] = holidayName;

    QString weatherLine = "天气暂时没查到。";
    QStringList tips;
    if (!weatherData.contains("error")) {
        const QJsonArray current = weatherData.value("current_condition").toArray();
        if (!current.isEmpty()) {
            const QJsonObject now = current.first().toObject();
            weatherLine = QString("现在 %1℃，体感 %2℃，%3，湿度 %4%。")
                .arg(now.value("temp_C").toString(),
                     now.value("FeelsLikeC").toString(),
                     weatherDesc(now),
                     now.value("humidity").toString());
        }
        tips = briefingTips(weatherData);
    } else {
        result["weather_error"] = weatherData.value("error").toString();
        tips.append("今天也要记得喝水和适当休息。 ");
    }

    QStringList lines;
    lines.append(QString("今天是 %1，%2。").arg(today.toString("yyyy-MM-dd"), today.toString("dddd")));
    if (!holidayName.isEmpty()) {
        lines.append(QString("今天是%1。 ").arg(holidayName));
    }
    lines.append(weatherLine);
    lines.append(tips.join(""));

    result["weather_summary"] = weatherLine;
    result["tips"] = QJsonArray::fromStringList(tips);
    result["briefing"] = lines.join("\n");
    return ToolResult::ok(result);
}
