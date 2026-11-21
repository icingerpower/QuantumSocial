#ifndef PRODUCTSOURCE_H
#define PRODUCTSOURCE_H

#include <QJsonArray>
#include <QMap>
#include <QStringList>

// Sources produce snapshots; they never modify the source working directory.
class ProductSource
{
public:
    struct Result
    {
        QJsonArray products;
        int archivedRecords = 0;
        int skippedWithoutAPlus = 0;
        QString error;
    };
    virtual ~ProductSource() = default;
    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual Result read(const QString &directory) const = 0;
    static const QMap<QString, const ProductSource *> &all();
    class Recorder
    {
    public:
        explicit Recorder(const ProductSource *source);
    };
};

#endif
