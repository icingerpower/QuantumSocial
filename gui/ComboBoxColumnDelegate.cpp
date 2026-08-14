#include <QComboBox>
#include "ComboBoxColumnDelegate.h"

ComboBoxColumnDelegate::ComboBoxColumnDelegate(
        const QHash<int, QStringList> &columnItems, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_columnItems(columnItems)
{
}

QWidget *ComboBoxColumnDelegate::createEditor(
        QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (m_columnItems.contains(index.column()))
    {
        auto *combo = new QComboBox(parent);
        combo->addItems(m_columnItems[index.column()]);
        return combo;
    }
    return QStyledItemDelegate::createEditor(parent, option, index);
}

void ComboBoxColumnDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
    if (auto *combo = qobject_cast<QComboBox *>(editor))
    {
        const int idx = combo->findText(index.data(Qt::EditRole).toString());
        if (idx >= 0) combo->setCurrentIndex(idx);
        return;
    }
    QStyledItemDelegate::setEditorData(editor, index);
}

void ComboBoxColumnDelegate::setModelData(
        QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const
{
    if (auto *combo = qobject_cast<QComboBox *>(editor))
    {
        model->setData(index, combo->currentText(), Qt::EditRole);
        return;
    }
    QStyledItemDelegate::setModelData(editor, model, index);
}

void ComboBoxColumnDelegate::updateEditorGeometry(
        QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &) const
{
    editor->setGeometry(option.rect);
}
