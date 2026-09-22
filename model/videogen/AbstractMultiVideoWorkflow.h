#ifndef ABSTRACTMULTIVIDEOWORKFLOW_H
#define ABSTRACTMULTIVIDEOWORKFLOW_H

#include <QDir>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class AbstractCli;
class AbstractVideoGenerator;

// Base class for multi-video generation workflows.
//
// A multi-video workflow orchestrates the generation of multiple video parts
// that will later be merged or selected from (e.g., displaying several products,
// comparison videos, or hook+story+offer structures).
//
// Subclasses define:
//   - the step sequence (intro, products, outro, etc.)
//   - the file naming conventions (ordered for clean Dropbox upload & sorting)
//   - step-specific prompts, takes, and image handling
//   - serialization and resume capability for interrupted runs (e.g. quota limits)
class AbstractMultiVideoWorkflow : public QObject
{
    Q_OBJECT

public:
    enum class StepType
    {
        Intro,
        AfterIntro,
        Custom
    };

    enum class ImageRegenOption
    {
        KeepOriginal,
        Regenerate,
        IsolateOnWhite
    };

    struct ImageInput
    {
        QString originalPath;
        ImageRegenOption regenOption = ImageRegenOption::KeepOriginal;
        QString processedPath;

        QJsonObject toJson() const;
        static ImageInput fromJson(const QJsonObject &obj);
    };

    struct Step
    {
        StepType type = StepType::Intro;
        QString title;
        QString prompt;
        int videoCount = 1; // number of variations/takes to generate
        QList<ImageInput> images; // up to 4 images

        QJsonObject toJson() const;
        static Step fromJson(const QJsonObject &obj);
    };

    struct TakeInfo
    {
        int stepIndex = 0;
        int takeIndex = 0;
        QString expectedFileName; // e.g. "01_step1_intro_take01.mp4"
        QString filePath;
        bool completed = false;
        bool failed = false;
        QString errorMessage;
        QString conversationUrl;
        bool promptSubmitted = false;

        QJsonObject toJson() const;
        static TakeInfo fromJson(const QJsonObject &obj);
    };

    enum class State
    {
        Idle,
        Running,
        Paused,      // paused e.g. on quota / rate limit; can be resumed!
        Completed,
        Cancelled,
        Error
    };

    explicit AbstractMultiVideoWorkflow(QObject *parent = nullptr);
    ~AbstractMultiVideoWorkflow() override = default;

    virtual QString workflowName() const = 0;
    virtual QString workflowId() const = 0;

    virtual const QList<Step> &steps() const = 0;
    virtual QList<Step> &steps() = 0;

    // Ordered, human-readable file name for Dropbox upload / sorting
    // e.g. "01_step1_intro_take01.mp4", "02_step2_product1_take01.mp4"
    virtual QString videoFileName(int stepIndex, int takeIndex) const = 0;

    // Total count of videos across all steps
    int totalVideoCount() const;
    // Number of successfully generated videos
    int completedVideoCount() const;

    State state() const { return m_state; }
    QString pauseReason() const { return m_pauseReason; }

    const QList<TakeInfo> &takes() const { return m_takes; }
    QList<TakeInfo> &takes() { return m_takes; }

    QString conversationUrl() const { return m_conversationUrl; }
    void setConversationUrl(const QString &url) { m_conversationUrl = url; }

    int selectedIntroTakeIndex() const { return selectedTakeIndex(0); }
    void setSelectedIntroTakeIndex(int idx) { setSelectedTakeIndex(0, idx); }

    int selectedTakeIndex(int stepIndex) const {
        if (m_selectedTakeIndices.contains(stepIndex))
        {
            return m_selectedTakeIndices.value(stepIndex);
        }
        if (stepIndex == 0)
        {
            return m_selectedIntroTakeIndex;
        }
        return -1;
    }
    void setSelectedTakeIndex(int stepIndex, int idx) {
        m_selectedTakeIndices[stepIndex] = idx;
        if (stepIndex == 0)
        {
            m_selectedIntroTakeIndex = idx;
        }
    }
    const QMap<int, int> &selectedTakeIndices() const { return m_selectedTakeIndices; }

    // Serialization for state persistence and resume
    virtual QJsonObject toJson() const;
    virtual bool fromJson(const QJsonObject &json);

    bool saveToFile(const QString &filePath) const;
    bool loadFromFile(const QString &filePath);

    // Workflow control
    virtual void start(const QString &projectDir, const QString &outputDir,
                       const QString &generatorId, const QVariantMap &settings,
                       AbstractCli *decisionCli, AbstractCli *imageCli,
                       const QString &videoFormatLabel) = 0;
    virtual void resume() = 0;
    virtual void cancel() = 0;
    virtual void selectIntroTake(int takeIndex) = 0;
    virtual void selectTake(int stepIndex, int takeIndex) = 0;

signals:
    void progress(const QString &message);
    void takeStarted(int stepIndex, int takeIndex, const QString &fileName);
    void takeCompleted(int stepIndex, int takeIndex, const QString &videoPath);
    void stepCompleted(int stepIndex);
    void introSelectionRequested(const QList<AbstractMultiVideoWorkflow::TakeInfo> &introTakes);
    void takeSelectionRequested(int stepIndex, const QList<AbstractMultiVideoWorkflow::TakeInfo> &takes);
    void paused(const QString &reason, int completedCount, int totalCount);
    void allCompleted(const QStringList &generatedVideoPaths);
    void failed(const QString &errorMessage);

protected:
    void setState(State state, const QString &reason = QString{});
    void initializeTakes();

    State m_state = State::Idle;
    QString m_pauseReason;
    QList<TakeInfo> m_takes;
    int m_selectedIntroTakeIndex = -1;
    QMap<int, int> m_selectedTakeIndices;

    QString m_projectDir;
    QString m_outputDir;
    QString m_generatorId;
    QVariantMap m_settings;
    AbstractCli *m_decisionCli = nullptr;
    AbstractCli *m_imageCli = nullptr;
    QString m_videoFormatLabel;
    QString m_conversationUrl;
};

#endif // ABSTRACTMULTIVIDEOWORKFLOW_H
