#ifndef REFERENCEASSESSOR_H
#define REFERENCEASSESSOR_H

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <functional>

class AbstractCli;

class ReferenceAssessor
{
public:
    using Completion = std::function<void(QJsonObject)>;
    virtual ~ReferenceAssessor() = default;
    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual bool needsCli() const { return false; }
    virtual void assess(const QJsonObject &input, AbstractCli *cli, const QString &prompt,
                        const QString &workingDirectory, QObject *context, Completion done) const = 0;
    static const QMap<QString, const ReferenceAssessor *> &all();
    static QString defaultPrompt();
    class Recorder
    {
    public:
        explicit Recorder(const ReferenceAssessor *assessor);
    };
};

#endif
