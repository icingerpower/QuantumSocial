#ifndef TABLEACCOUNTS_H
#define TABLEACCOUNTS_H

#include <QAbstractTableModel>

class TableAccounts : public QAbstractTableModel
{
    Q_OBJECT

public:
    static const int IND_TYPE;
    static const int IND_URL;
    static const int IND_VIEW;
    static const int IND_FOLLOWERS;
    static const int IND_LIKES;

    explicit TableAccounts(const QString &workingDirectory, QObject *parent = nullptr);

    // Header:
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // Basic functionality:
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // Editable:
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void addAccount();
    void removeAccount(int row);

private:
    static const QStringList HEADER;
    QString m_filePath;
    QList<QVariantList> m_listOfVariantList;

    void _loadFromFile();
    void _saveInFile();
};

#endif // TABLEACCOUNTS_H
