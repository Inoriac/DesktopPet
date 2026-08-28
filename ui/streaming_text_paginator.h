#ifndef DESKTOP_PET_STREAMING_TEXT_PAGINATOR_H
#define DESKTOP_PET_STREAMING_TEXT_PAGINATOR_H

#include <QString>
#include <QStringList>

struct PaginationUpdate {
    QStringList newlySealedPages;
    QString draftPage;
};

class StreamingTextPaginator {
public:
    void reset();
    PaginationUpdate feed(const QString& delta);
    PaginationUpdate finish();

    static constexpr int softTarget() { return 96; }
    static constexpr int hardMaximum() { return 180; }

private:
    PaginationUpdate takeAvailablePages(bool finishing);

    QString m_buffer;
};

#endif // DESKTOP_PET_STREAMING_TEXT_PAGINATOR_H
