#ifndef PANESETTINGS_H
#define PANESETTINGS_H

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class PaneSettings; }
QT_END_NAMESPACE

class AvailableCliTable;
class TableGenerationSettings;

// Settings tab: the AI CLIs found on this machine (common's
// AvailableCliTable, with async availability + membership checks) and the
// video generation settings table (one section per registered backend, rows
// come from each backend's availableSettings).
class PaneSettings : public QWidget
{
    Q_OBJECT

public:
    explicit PaneSettings(TableGenerationSettings *generationSettings,
                          AvailableCliTable *availableClis = nullptr,
                          QWidget *parent = nullptr);
    ~PaneSettings() override;

private:
    Ui::PaneSettings *ui;
    TableGenerationSettings *m_generationSettings;
    AvailableCliTable *m_availableClis;
};

#endif // PANESETTINGS_H
