#ifndef VIDEOPLANPROXY_H
#define VIDEOPLANPROXY_H

#include <QSet>
#include <QSortFilterProxyModel>
#include <QUuid>

#include "../videos/TableVideos.h"

// View of the active TreeProperties restricted to one video's plan: only
// the property values the video is testing (and their parent properties)
// are visible, and the Generated/Stats columns become checkboxes.
//
// The checkbox state lives in the bound VideoRecord, not in the tree — the
// same value can be "generated" for one video and still pending for
// another. The source model deliberately leaves those two columns empty.
class VideoPlanProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit VideoPlanProxy(QObject *parent = nullptr);

    // Binds the proxy to one video record; a null id shows nothing.
    void setVideo(TableVideos *videos, const QUuid &videoId);
    QUuid videoId() const;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    TableVideos *m_videos = nullptr;
    QUuid m_videoId;
    QSet<QUuid> m_plannedIds;

    bool _isCheckColumn(int column) const;
    bool _rowOrDescendantPlanned(const QModelIndex &sourceIndex) const;
    void _emitChecksChanged(const QModelIndex &parent);
};

#endif // VIDEOPLANPROXY_H
