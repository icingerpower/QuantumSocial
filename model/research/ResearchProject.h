#ifndef RESEARCHPROJECT_H
#define RESEARCHPROJECT_H
#include <QJsonObject>
#include <QString>
#include <QDir>
class TableProjects;
class ResearchProject
{
public:
    static int create(TableProjects &projects, QJsonObject context, const QString &imagePath, QString *error);
    static bool snapshot(const QDir &projectDirectory, const QDir &generationDirectory, QString *error);
};
#endif
