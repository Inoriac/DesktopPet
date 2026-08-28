#include <QtTest>

#include <QTextBoundaryFinder>

#include "ui/streaming_text_paginator.h"

namespace {

QString joined(const QStringList& sealed, const QString& draft) {
    QString result = sealed.join(QString());
    result += draft;
    return result;
}

} // namespace

class TestStreamingTextPaginator : public QObject {
    Q_OBJECT

private slots:
    void feed_whenChineseSentencesAndParagraphsArriveIncrementally_shouldSealAtNaturalBoundaries();
    void feed_whenDeltaIsEmpty_shouldNotCreateEmptyPage();
    void finish_whenUnpunctuatedUnicodeExceedsHardLimit_shouldPreserveGraphemesAndExactText();
};

void TestStreamingTextPaginator::feed_whenChineseSentencesAndParagraphsArriveIncrementally_shouldSealAtNaturalBoundaries() {
    StreamingTextPaginator paginator;
    const QString paragraph = QStringLiteral("第一段很短。\n");

    const PaginationUpdate paragraphUpdate = paginator.feed(paragraph);

    QCOMPARE(paragraphUpdate.newlySealedPages, QStringList{paragraph});
    QVERIFY(paragraphUpdate.draftPage.isEmpty());

    const QString sentencePrefix = QString(90, QChar(u'中'))
        + QStringLiteral("。");
    const PaginationUpdate prefixUpdate = paginator.feed(sentencePrefix);
    QVERIFY(prefixUpdate.newlySealedPages.isEmpty());
    QCOMPARE(prefixUpdate.draftPage, sentencePrefix);

    const QString sentenceSuffix = QString(8, QChar(u'文'))
        + QStringLiteral("！尾巴");
    const PaginationUpdate sentenceUpdate = paginator.feed(sentenceSuffix);
    QCOMPARE(sentenceUpdate.newlySealedPages.size(), 1);
    QVERIFY(sentenceUpdate.newlySealedPages.first().endsWith(
        QStringLiteral("！")));
    QCOMPARE(sentenceUpdate.draftPage, QStringLiteral("尾巴"));
    QCOMPARE(joined(paragraphUpdate.newlySealedPages
                        + sentenceUpdate.newlySealedPages,
                    sentenceUpdate.draftPage),
             paragraph + sentencePrefix + sentenceSuffix);
}

void TestStreamingTextPaginator::feed_whenDeltaIsEmpty_shouldNotCreateEmptyPage() {
    StreamingTextPaginator paginator;

    const PaginationUpdate update = paginator.feed(QString());
    const PaginationUpdate finished = paginator.finish();

    QVERIFY(update.newlySealedPages.isEmpty());
    QVERIFY(update.draftPage.isEmpty());
    QVERIFY(finished.newlySealedPages.isEmpty());
    QVERIFY(finished.draftPage.isEmpty());
}

void TestStreamingTextPaginator::finish_whenUnpunctuatedUnicodeExceedsHardLimit_shouldPreserveGraphemesAndExactText() {
    StreamingTextPaginator paginator;
    QString original;
    for (int i = 0; i < 100; ++i) {
        original += QStringLiteral("A\U0001F642e\u0301");
    }

    const PaginationUpdate update = paginator.feed(original.left(173));
    const PaginationUpdate second = paginator.feed(original.mid(173));
    const PaginationUpdate finished = paginator.finish();
    const QStringList pages = update.newlySealedPages
        + second.newlySealedPages + finished.newlySealedPages;

    QVERIFY(pages.size() >= 3);
    QCOMPARE(joined(pages, finished.draftPage), original);
    int utf16Offset = 0;
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, original);
    for (const QString& page : pages) {
        QVERIFY(!page.isEmpty());
        QVERIFY(page.toUcs4().size() <= StreamingTextPaginator::hardMaximum());
        utf16Offset += page.size();
        boundaries.setPosition(utf16Offset);
        QVERIFY(boundaries.isAtBoundary());
    }
    QCOMPARE(utf16Offset, original.size());
}

QTEST_GUILESS_MAIN(TestStreamingTextPaginator)
#include "test_streaming_text_paginator.moc"
