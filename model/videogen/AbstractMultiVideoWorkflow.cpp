#include "AbstractMultiVideoWorkflow.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

QJsonObject AbstractMultiVideoWorkflow::ImageInput::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("originalPath")] = originalPath;
    obj[QStringLiteral("regenOption")] = static_cast<int>(regenOption);
    obj[QStringLiteral("processedPath")] = processedPath;
    return obj;
}

AbstractMultiVideoWorkflow::ImageInput AbstractMultiVideoWorkflow::ImageInput::fromJson(const QJsonObject &obj)
{
    ImageInput img;
    img.originalPath = obj.value(QStringLiteral("originalPath")).toString();
    img.regenOption = static_cast<ImageRegenOption>(
        obj.value(QStringLiteral("regenOption")).toInt(static_cast<int>(ImageRegenOption::KeepOriginal)));
    img.processedPath = obj.value(QStringLiteral("processedPath")).toString();
    if (img.regenOption != ImageRegenOption::KeepOriginal && img.processedPath == img.originalPath)
    {
        img.processedPath.clear();
    }
    return img;
}

QJsonObject AbstractMultiVideoWorkflow::Step::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("type")] = static_cast<int>(type);
    obj[QStringLiteral("title")] = title;
    obj[QStringLiteral("prompt")] = prompt;
    obj[QStringLiteral("videoCount")] = videoCount;

    QJsonArray imgArray;
    for (const auto &img : images)
    {
        imgArray.append(img.toJson());
    }
    obj[QStringLiteral("images")] = imgArray;
    return obj;
}

AbstractMultiVideoWorkflow::Step AbstractMultiVideoWorkflow::Step::fromJson(const QJsonObject &obj)
{
    Step step;
    step.type = static_cast<StepType>(obj.value(QStringLiteral("type")).toInt(static_cast<int>(StepType::Intro)));
    step.title = obj.value(QStringLiteral("title")).toString();
    step.prompt = obj.value(QStringLiteral("prompt")).toString();
    step.videoCount = obj.value(QStringLiteral("videoCount")).toInt(1);

    const QJsonArray imgArray = obj.value(QStringLiteral("images")).toArray();
    for (const auto &val : imgArray)
    {
        step.images.append(ImageInput::fromJson(val.toObject()));
    }
    return step;
}

QJsonObject AbstractMultiVideoWorkflow::TakeInfo::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("stepIndex")] = stepIndex;
    obj[QStringLiteral("takeIndex")] = takeIndex;
    obj[QStringLiteral("expectedFileName")] = expectedFileName;
    obj[QStringLiteral("filePath")] = filePath;
    obj[QStringLiteral("completed")] = completed;
    obj[QStringLiteral("failed")] = failed;
    obj[QStringLiteral("errorMessage")] = errorMessage;
    obj[QStringLiteral("conversationUrl")] = conversationUrl;
    obj[QStringLiteral("promptSubmitted")] = promptSubmitted;
    return obj;
}

AbstractMultiVideoWorkflow::TakeInfo AbstractMultiVideoWorkflow::TakeInfo::fromJson(const QJsonObject &obj)
{
    TakeInfo take;
    take.stepIndex = obj.value(QStringLiteral("stepIndex")).toInt();
    take.takeIndex = obj.value(QStringLiteral("takeIndex")).toInt();
    take.expectedFileName = obj.value(QStringLiteral("expectedFileName")).toString();
    take.filePath = obj.value(QStringLiteral("filePath")).toString();
    take.completed = obj.value(QStringLiteral("completed")).toBool();
    take.failed = obj.value(QStringLiteral("failed")).toBool();
    take.errorMessage = obj.value(QStringLiteral("errorMessage")).toString();
    take.conversationUrl = obj.value(QStringLiteral("conversationUrl")).toString();
    take.promptSubmitted = obj.value(QStringLiteral("promptSubmitted")).toBool(take.failed && !take.conversationUrl.isEmpty());
    return take;
}

AbstractMultiVideoWorkflow::AbstractMultiVideoWorkflow(QObject *parent)
    : QObject(parent)
{
}

int AbstractMultiVideoWorkflow::totalVideoCount() const
{
    int count = 0;
    for (const auto &step : steps())
    {
        count += qMax(1, step.videoCount);
    }
    return count;
}

int AbstractMultiVideoWorkflow::completedVideoCount() const
{
    int count = 0;
    for (const auto &take : m_takes)
    {
        if (take.completed)
        {
            ++count;
        }
    }
    return count;
}

void AbstractMultiVideoWorkflow::setState(State state, const QString &reason)
{
    m_state = state;
    m_pauseReason = reason;
}

void AbstractMultiVideoWorkflow::initializeTakes()
{
    m_takes.clear();
    const auto &stepList = steps();
    for (int stepIdx = 0; stepIdx < stepList.size(); ++stepIdx)
    {
        const auto &step = stepList.at(stepIdx);
        const int takesCount = qMax(1, step.videoCount);
        for (int takeIdx = 0; takeIdx < takesCount; ++takeIdx)
        {
            TakeInfo info;
            info.stepIndex = stepIdx;
            info.takeIndex = takeIdx;
            info.expectedFileName = videoFileName(stepIdx, takeIdx);
            m_takes.append(info);
        }
    }
}

QJsonObject AbstractMultiVideoWorkflow::toJson() const
{
    QJsonObject root;
    root[QStringLiteral("workflowId")] = workflowId();
    root[QStringLiteral("state")] = static_cast<int>(m_state);
    root[QStringLiteral("pauseReason")] = m_pauseReason;
    root[QStringLiteral("projectDir")] = m_projectDir;
    root[QStringLiteral("outputDir")] = m_outputDir;
    root[QStringLiteral("generatorId")] = m_generatorId;
    root[QStringLiteral("settings")] = QJsonObject::fromVariantMap(m_settings);
    root[QStringLiteral("videoFormatLabel")] = m_videoFormatLabel;
    root[QStringLiteral("conversationUrl")] = m_conversationUrl;
    root[QStringLiteral("selectedIntroTakeIndex")] = m_selectedIntroTakeIndex;

    QJsonObject selectedTakesObj;
    for (auto it = m_selectedTakeIndices.cbegin(); it != m_selectedTakeIndices.cend(); ++it)
    {
        selectedTakesObj[QString::number(it.key())] = it.value();
    }
    root[QStringLiteral("selectedTakes")] = selectedTakesObj;

    QJsonArray stepsArray;
    for (const auto &step : steps())
    {
        stepsArray.append(step.toJson());
    }
    root[QStringLiteral("steps")] = stepsArray;

    QJsonArray takesArray;
    for (const auto &take : m_takes)
    {
        takesArray.append(take.toJson());
    }
    root[QStringLiteral("takes")] = takesArray;

    return root;
}

bool AbstractMultiVideoWorkflow::fromJson(const QJsonObject &json)
{
    m_state = static_cast<State>(json.value(QStringLiteral("state")).toInt(static_cast<int>(State::Idle)));
    m_pauseReason = json.value(QStringLiteral("pauseReason")).toString();
    m_projectDir = json.value(QStringLiteral("projectDir")).toString();
    m_outputDir = json.value(QStringLiteral("outputDir")).toString();
    m_generatorId = json.value(QStringLiteral("generatorId")).toString();
    m_settings = json.value(QStringLiteral("settings")).toObject().toVariantMap();
    m_videoFormatLabel = json.value(QStringLiteral("videoFormatLabel")).toString();
    m_conversationUrl = json.value(QStringLiteral("conversationUrl")).toString();
    m_selectedIntroTakeIndex = json.value(QStringLiteral("selectedIntroTakeIndex")).toInt(-1);

    m_selectedTakeIndices.clear();
    const QJsonObject selectedTakesObj = json.value(QStringLiteral("selectedTakes")).toObject();
    for (auto it = selectedTakesObj.begin(); it != selectedTakesObj.end(); ++it)
    {
        m_selectedTakeIndices[it.key().toInt()] = it.value().toInt();
    }
    if (m_selectedIntroTakeIndex >= 0 && !m_selectedTakeIndices.contains(0))
    {
        m_selectedTakeIndices[0] = m_selectedIntroTakeIndex;
    }

    QList<Step> loadedSteps;
    const QJsonArray stepsArray = json.value(QStringLiteral("steps")).toArray();
    for (const auto &val : stepsArray)
    {
        loadedSteps.append(Step::fromJson(val.toObject()));
    }
    steps() = loadedSteps;

    m_takes.clear();
    const QJsonArray takesArray = json.value(QStringLiteral("takes")).toArray();
    for (const auto &val : takesArray)
    {
        m_takes.append(TakeInfo::fromJson(val.toObject()));
    }

    if (m_takes.isEmpty() && !steps().isEmpty())
    {
        initializeTakes();
    }

    return true;
}

bool AbstractMultiVideoWorkflow::saveToFile(const QString &filePath) const
{
    QFile file{filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return false;
    }
    file.write(QJsonDocument(toJson()).toJson());
    return true;
}

bool AbstractMultiVideoWorkflow::loadFromFile(const QString &filePath)
{
    QFile file{filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
    {
        return false;
    }
    return fromJson(doc.object());
}
