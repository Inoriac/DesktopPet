#include "streaming_text_paginator.h"

#include <QTextBoundaryFinder>
#include <QVector>

#include <algorithm>

namespace {

int scalarCount(const QString& text) {
    return text.toUcs4().size();
}

QVector<int> graphemeEnds(const QString& text) {
    QVector<int> ends;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.toStart();
    int boundary = finder.toNextBoundary();
    while (boundary >= 0) {
        if (boundary > 0) ends.append(boundary);
        boundary = finder.toNextBoundary();
    }
    return ends;
}

int paragraphBreakIndex(const QString& text) {
    const int newline = text.indexOf(QLatin1Char('\n'));
    if (newline < 0) return -1;
    const int boundary = newline + 1;
    return scalarCount(text.left(boundary)) <= StreamingTextPaginator::hardMaximum()
        ? boundary
        : -1;
}

int sentenceBreakIndex(const QString& text) {
    static const QString punctuation = QStringLiteral("。！？!?；;");
    int previous = 0;
    int scalars = 0;
    const QVector<int> ends = graphemeEnds(text);
    for (const int boundary : ends) {
        scalars += scalarCount(text.mid(previous, boundary - previous));
        previous = boundary;
        if (scalars > StreamingTextPaginator::hardMaximum()) break;
        if (scalars >= StreamingTextPaginator::softTarget()
            && punctuation.contains(text.at(boundary - 1))) {
            return boundary;
        }
    }
    return -1;
}

int hardBreakIndex(const QString& text) {
    int previous = 0;
    int scalars = 0;
    int safeBoundary = 0;
    int preferredBoundary = 0;
    const QVector<int> ends = graphemeEnds(text);
    for (const int boundary : ends) {
        const int clusterScalars = scalarCount(
            text.mid(previous, boundary - previous));
        if (scalars + clusterScalars > StreamingTextPaginator::hardMaximum()) {
            break;
        }
        scalars += clusterScalars;
        previous = boundary;
        safeBoundary = boundary;
        const QChar last = text.at(boundary - 1);
        if (scalars >= StreamingTextPaginator::softTarget()
            && (last == QLatin1Char(',') || last == QChar(u'，')
                || last.isSpace())) {
            preferredBoundary = boundary;
        }
    }
    return preferredBoundary > 0 ? preferredBoundary : safeBoundary;
}

} // namespace

void StreamingTextPaginator::reset() {
    m_buffer.clear();
}

PaginationUpdate StreamingTextPaginator::feed(const QString& delta) {
    if (!delta.isEmpty()) m_buffer += delta;
    return takeAvailablePages(false);
}

PaginationUpdate StreamingTextPaginator::finish() {
    return takeAvailablePages(true);
}

PaginationUpdate StreamingTextPaginator::takeAvailablePages(bool finishing) {
    PaginationUpdate update;
    while (!m_buffer.isEmpty()) {
        int splitAt = paragraphBreakIndex(m_buffer);
        if (splitAt < 0) splitAt = sentenceBreakIndex(m_buffer);
        if (splitAt < 0
            && scalarCount(m_buffer) > hardMaximum()) {
            splitAt = hardBreakIndex(m_buffer);
        }
        if (splitAt <= 0) break;
        update.newlySealedPages.append(m_buffer.left(splitAt));
        m_buffer.remove(0, splitAt);
    }

    if (finishing && !m_buffer.isEmpty()) {
        while (scalarCount(m_buffer) > hardMaximum()) {
            const int splitAt = hardBreakIndex(m_buffer);
            if (splitAt <= 0) break;
            update.newlySealedPages.append(m_buffer.left(splitAt));
            m_buffer.remove(0, splitAt);
        }
        if (!m_buffer.isEmpty()) {
            update.newlySealedPages.append(m_buffer);
            m_buffer.clear();
        }
    }

    update.draftPage = m_buffer;
    return update;
}
