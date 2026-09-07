#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QList>
#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class AbstractCli;
class TreeProperties;
class TableGenerationSettings;
class TableVideos;
class StatisticsScheduler;
class PreferredHashtags;
class PromptLessons;
class FavoriteHooks;
class FavoriteVideoPrompts;
class SavedPrompts;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    Ui::MainWindow *ui;
    // Shared by the panes: one ground truth (videos + snapshots), one active
    // property tree, one archive tree, and the background snapshot fetcher.
    TableVideos *m_videos;
    TreeProperties *m_properties;
    TreeProperties *m_archive;
    StatisticsScheduler *m_scheduler;
    TableGenerationSettings *m_generationSettings;
    // Not QObjects (plain data + file persistence) — owned directly,
    // deleted in ~MainWindow.
    PreferredHashtags *m_hashtags;
    PromptLessons *m_promptLessons;
    FavoriteHooks *m_favoriteHooks;
    FavoriteVideoPrompts *m_favoriteVideoPrompts;
    SavedPrompts *m_savedPrompts;
    QList<AbstractCli *> m_availableClis;
};
#endif // MAINWINDOW_H
