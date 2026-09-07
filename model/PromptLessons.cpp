#include "PromptLessons.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace {
constexpr int kMaxLessonLength = 220;
constexpr int kMaxLessons = 30; // keeps the injected prompt section bounded
// Above this share of significant words in common, two lessons are treated
// as the same underlying pitfall even when phrased differently. Needed
// because the checking CLI writes a fresh "reason" every time — observed
// live: 11 near-duplicate rephrasings of "a full-body shot shrinks the
// product to a tiny detail" all got recorded as if they were distinct,
// since the old exact-string dedup never caught reworded repeats.
constexpr double kSimilarityThreshold = 0.3;

const QSet<QString> &stopWords()
{
    static const QSet<QString> words{
        QStringLiteral("the"), QStringLiteral("a"), QStringLiteral("an"),
        QStringLiteral("is"), QStringLiteral("in"), QStringLiteral("to"),
        QStringLiteral("of"), QStringLiteral("and"), QStringLiteral("or"),
        QStringLiteral("as"), QStringLiteral("it"), QStringLiteral("its"),
        QStringLiteral("this"), QStringLiteral("that"), QStringLiteral("but"),
        QStringLiteral("rather"), QStringLiteral("than"), QStringLiteral("for"),
        QStringLiteral("on"), QStringLiteral("with"), QStringLiteral("by"),
        QStringLiteral("be"), QStringLiteral("being"), QStringLiteral("not"),
        QStringLiteral("was"), QStringLiteral("were"), QStringLiteral("are"),
        QStringLiteral("at"), QStringLiteral("into"), QStringLiteral("from"),
    };
    return words;
}

QSet<QString> significantWords(const QString &text)
{
    static const QRegularExpression splitter{QStringLiteral("[^a-z0-9']+")};
    QSet<QString> words;
    for (const QString &word : text.toLower().split(splitter, Qt::SkipEmptyParts))
    {
        if (word.size() > 2 && !stopWords().contains(word))
        {
            words.insert(word);
        }
    }
    return words;
}

double jaccardSimilarity(const QSet<QString> &a, const QSet<QString> &b)
{
    if (a.isEmpty() || b.isEmpty())
    {
        return 0.0;
    }
    QSet<QString> intersection = a;
    intersection.intersect(b);
    QSet<QString> unioned = a;
    unioned.unite(b);
    return unioned.isEmpty() ? 0.0
        : static_cast<double>(intersection.size()) / unioned.size();
}
}

PromptLessons::PromptLessons(const QString &workingDirectory)
    : m_filePath(QDir(workingDirectory).absoluteFilePath(
          QStringLiteral("prompt_lessons.md")))
{
    _loadFromFile();
}

QStringList PromptLessons::lessons() const
{
    return m_lessons;
}

void PromptLessons::addLesson(const QString &lesson)
{
    const QString clean = lesson.trimmed().left(kMaxLessonLength);
    if (clean.isEmpty())
    {
        return;
    }
    const QSet<QString> newWords = significantWords(clean);
    for (const QString &existing : m_lessons)
    {
        if (existing.compare(clean, Qt::CaseInsensitive) == 0
            || jaccardSimilarity(newWords, significantWords(existing))
                >= kSimilarityThreshold)
        {
            return; // exact duplicate, or the same pitfall reworded
        }
    }
    m_lessons << clean;
    while (m_lessons.size() > kMaxLessons)
    {
        m_lessons.removeFirst(); // drop the oldest — recent failures matter most
    }
    _saveInFile();
}

QString PromptLessons::asPromptSection() const
{
    if (m_lessons.isEmpty())
    {
        return QString{};
    }
    QStringList bullets;
    for (const QString &lesson : m_lessons)
    {
        bullets << QStringLiteral("- %1").arg(lesson);
    }
    return QStringLiteral(
        "Known pitfalls from past generations — do NOT repeat these:\n%1\n")
        .arg(bullets.join(QStringLiteral("\n")));
}

void PromptLessons::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    QTextStream stream{&file};
    for (const QString &rawLine : stream.readAll().split(QLatin1Char('\n')))
    {
        const QString trimmed = rawLine.trimmed();
        // Only lines actually written as bullets are lessons — everything
        // else (the "# Prompt lessons" heading, the explanatory paragraph,
        // blank lines) must never be mistaken for one. A previous version
        // of this check accepted any non-empty, non-"#" line, which quietly
        // turned the explanatory paragraph itself into a permanent "lesson"
        // injected into every prompt.
        if (!trimmed.startsWith(QLatin1String("- ")))
        {
            continue;
        }
        const QString lesson = trimmed.mid(2).trimmed();
        if (!lesson.isEmpty())
        {
            m_lessons << lesson;
        }
    }
}

void PromptLessons::_saveInFile() const
{
    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    QTextStream stream{&file};
    stream << "# Prompt lessons\n\n"
              "Auto-recorded from real generation failures (content checks, "
              "clip refusals) and fed back into every future prompt so the "
              "same mistake isn't repeated. Edit or delete lines freely — "
              "this is just a growing list, order doesn't matter.\n\n";
    for (const QString &lesson : m_lessons)
    {
        stream << "- " << lesson << "\n";
    }
}
