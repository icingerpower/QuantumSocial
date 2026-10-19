#ifndef PRODUCTSHOWCASEMULTIVIDEOWORKFLOW_H
#define PRODUCTSHOWCASEMULTIVIDEOWORKFLOW_H

#include <memory>
#include <QPointer>
#include <QCoro/QCoroTask>

#include "AbstractMultiVideoWorkflow.h"
#include "VideoGenerationWorkflow.h"

// Multi-video workflow for multi-product showcases:
// - 1 Intro step + N After-Intro steps (each displaying a product)
// - 1 prompt for intro step + 1 prompt for after-intro steps
// - Dedicated video counts (takes) for intro step and after-intro steps
// - Up to 4 images per step with optional CLI regeneration / white-background isolation
// - Ordered file naming for Dropbox upload and sorting:
//     01_step1_intro_take01.mp4, 01_step1_intro_take02.mp4...
//     02_step2_product1_take01.mp4...
// - CLI validation for each video using extracted 5 to 10 frames
// - Resumable if blocked due to limitation usage / quota
class ProductShowcaseMultiVideoWorkflow : public AbstractMultiVideoWorkflow
{
    Q_OBJECT

public:
    explicit ProductShowcaseMultiVideoWorkflow(QObject *parent = nullptr);
    ~ProductShowcaseMultiVideoWorkflow() override;

    QString workflowName() const override;
    QString workflowId() const override;

    const QList<Step> &steps() const override { return m_steps; }
    QList<Step> &steps() override { return m_steps; }

    QString videoFileName(int stepIndex, int takeIndex) const override;

    // Helper to configure the showcase steps
    void configure(int totalSteps,
                   const QString &introPrompt, int introTakes,
                   const QString &afterIntroPrompt, int afterIntroTakes,
                   const QList<QList<ImageInput>> &stepImages = {});

    void start(const QString &projectDir, const QString &outputDir,
               const QString &generatorId, const QVariantMap &settings,
               AbstractCli *decisionCli, AbstractCli *imageCli,
               const QString &videoFormatLabel) override;
    void resume() override;
    void cancel() override;
    void selectIntroTake(int takeIndex) override;
    void selectTake(int stepIndex, int takeIndex) override;

    void resetStep(int stepIndex, bool resetSubsequentSteps = false);
    void regenerateStep(int stepIndex, int newTakesCount, bool replaceExisting = false);
    QJsonObject toJson() const override;
    bool fromJson(const QJsonObject &json) override;
    // Also called after the pane moves completed takes into their final folder.
    void updateMergedVideo();
    bool isMerging() const { return m_merging; }
    QString mergedVideoPath() const { return m_mergedVideoPath; }
    QString generationShortCode() const { return m_generationShortCode; }
    bool prepareGeneration(const QString &shortCode, QString *error);

signals:
    void mergedVideoUpdated(const QString &path, const QString &error);

public:
    enum class ImagePrepStatus
    {
        Success,
        Failed,
        QuotaExceeded
    };

    struct ImageRegenResult
    {
        QString filePath;
        bool quotaExceeded = false;
        QString errorMessage;
    };

private:
    QList<Step> m_steps;
    bool m_cancelled = false;
    QPointer<VideoGenerationWorkflow> m_currentVideoWorkflow;
    QCoro::Task<void> m_executionTask;
    QCoro::Task<void> m_mergeTask;
    bool m_merging = false;
    int m_mergeRevision = 0;
    QString m_mergedVideoPath;
    QStringList m_mergedSources;
    QString m_generationShortCode;

    QStringList _selectedVideoPaths() const;
    QDir _workingDirectory() const;
    bool _organizeWorkingFiles(QString *error);
    QCoro::Task<void> _mergeSelectedVideos();

    QCoro::Task<void> _runWorkflow();
    bool _pauseForStepSelection(int stepIndex);
    void _saveState();
    QCoro::Task<ImagePrepStatus> _ensureStepImagesReady(Step &step, const QString &stepWorkingDir);
    QCoro::Task<ImageRegenResult> _regenerateImage(const QString &originalImage,
                                                  AbstractMultiVideoWorkflow::ImageRegenOption option,
                                                  const QString &outputDir, int imgIndex);
};

#endif // PRODUCTSHOWCASEMULTIVIDEOWORKFLOW_H
