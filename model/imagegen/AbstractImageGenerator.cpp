#include "AbstractImageGenerator.h"

AbstractImageGenerator::Recorder::Recorder(AbstractImageGenerator *generator)
{
    getGenerators().insert(generator->getId(), generator);
}

const QMap<QString, AbstractImageGenerator *> &AbstractImageGenerator::ALL_IMAGE_GENERATORS()
{
    return getGenerators();
}

QMap<QString, AbstractImageGenerator *> &AbstractImageGenerator::getGenerators()
{
    static QMap<QString, AbstractImageGenerator *> map;
    return map;
}

QList<AbstractImageGenerator::SettingSpec> AbstractImageGenerator::availableSettings() const
{
    return {};
}
