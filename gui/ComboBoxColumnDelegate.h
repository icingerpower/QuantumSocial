#ifndef COMBOBOXCOLUMNDELEGATE_H
#define COMBOBOXCOLUMNDELEGATE_H

#include <QStyledItemDelegate>
#include <QHash>
#include <QStringList>

class ComboBoxColumnDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ComboBoxColumnDelegate(const QHash<int, QStringList> &columnItems,
                                    QObject *parent = nullptr);

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override;
    void setEditorData(QWidget *editor, const QModelIndex &index) const override;
    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override;
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override;

private:
    QHash<int, QStringList> m_columnItems;
};

#endif // COMBOBOXCOLUMNDELEGATE_H
