#ifndef DESKTOP_PET_DOMAIN_RESULT_H
#define DESKTOP_PET_DOMAIN_RESULT_H

#include <QJsonObject>
#include <QString>

#include <optional>
#include <utility>
#include <variant>

struct DomainError {
    QString code;
    QString message;
    QJsonObject details;
};

template <typename T, typename E = DomainError>
class Result {
public:
    static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result failure(E error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    bool isOk() const { return m_value.index() == 0; }
    explicit operator bool() const { return isOk(); }

    T& value() { return std::get<0>(m_value); }
    const T& value() const { return std::get<0>(m_value); }
    T takeValue() { return std::move(std::get<0>(m_value)); }

    E& error() { return std::get<1>(m_value); }
    const E& error() const { return std::get<1>(m_value); }

private:
    template <typename... Args>
    explicit Result(std::in_place_index_t<0>, Args&&... args)
        : m_value(std::in_place_index<0>, std::forward<Args>(args)...) {}

    template <typename... Args>
    explicit Result(std::in_place_index_t<1>, Args&&... args)
        : m_value(std::in_place_index<1>, std::forward<Args>(args)...) {}

    std::variant<T, E> m_value;
};

template <typename E>
class Result<void, E> {
public:
    static Result success() { return Result(std::nullopt); }
    static Result failure(E error) { return Result(std::move(error)); }

    bool isOk() const { return !m_error.has_value(); }
    explicit operator bool() const { return isOk(); }

    E& error() { return *m_error; }
    const E& error() const { return *m_error; }

private:
    explicit Result(std::optional<E> error) : m_error(std::move(error)) {}
    explicit Result(E error) : m_error(std::move(error)) {}

    std::optional<E> m_error;
};

inline DomainError domainError(const QString& code,
                               const QString& message,
                               QJsonObject details = {}) {
    return {code, message, std::move(details)};
}

#endif // DESKTOP_PET_DOMAIN_RESULT_H
