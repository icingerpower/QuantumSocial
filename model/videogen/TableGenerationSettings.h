#ifndef TABLEGENERATIONSETTINGS_H
#define TABLEGENERATIONSETTINGS_H

#include <QAbstractTableModel>
#include <QVariantMap>

// Editable table of every registered video backend's settings: rows are
// built from AbstractVideoGenerator::availableSettings() of each backend,
// so implementing a new backend (with different settings) automatically
// adds its rows here — nothing else to write.
//
// Values are stored in video_generation_settings.json in the working
// directory (they are project-context settings, not machine settings, so
// QSettings would be wrong). settingsFor() hands the merged
// defaults+overrides of one backend to the generation workflow.
class TableGenerationSettings : public QAbstractTableModel
{
    Q_OBJECT

public:
    static const int IND_GENERATOR;
    static const int IND_SETTING;
    static const int IND_VALUE;

    explicit TableGenerationSettings(const QString &workingDirectory,
                                     QObject *parent = nullptr);

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // Current values (defaults overlaid with saved edits) for one backend,
    // keyed by setting key — what AbstractVideoGenerator::generate() takes.
    QVariantMap settingsFor(const QString &generatorId) const;

private:
    struct Row
    {
        QString generatorId;
        QString generatorName;
        QString key;
        QString label;
        QVariant defaultValue;
        QVariant value;
    };

    static const QStringList HEADER;

    QString m_filePath;
    QList<Row> m_rows;

    void _loadFromFile();
    void _saveInFile();
};

#endif // TABLEGENERATIONSETTINGS_H
