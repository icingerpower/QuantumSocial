#ifndef TABLEPROJECTS_H
#define TABLEPROJECTS_H

#include <QAbstractTableModel>
#include <QDir>
#include <QUuid>

// One row per video project. Visible columns: Name (renameable in place)
// and Created. The trailing columns are hidden in the view: Keyword and
// Hook idea (edited through the Source form via QDataWidgetMapper), the
// source image path, and — always LAST — the project id. Rows are matched
// by that id on save/reload, so renames and row reordering never matter;
// new columns must be inserted before it.
//
// Everything lives in a subfolder of the working directory:
//   <workingDir>/projects/projects.csv        this table
//   <workingDir>/projects/<id>/               the project's own files
// The folder is named by id (not by name) so renaming a project never
// touches the filesystem. Image paths are stored relative to the working
// directory, so the whole directory stays movable.
class TableProjects : public QAbstractTableModel
{
    Q_OBJECT

public:
    static const int IND_NAME;
    static const int IND_CREATED;
    static const int IND_KEYWORD;
    static const int IND_HOOK;
    static const int IND_IMAGE;
    static const int IND_GEN_PROMPT;
    static const int IND_GEN_HOOK;
    static const int IND_GEN_DESC;
    // Optional second source image (e.g. a different angle of the same
    // product) — appended AFTER every pre-existing column (never inserted
    // in the middle) so rows written before it existed still load correctly
    // via _loadFromFile()'s "pad missing trailing fields" logic. Never
    // itself regenerated/bootstrapped by the image step; it rides along
    // unchanged as extra reference material.
    static const int IND_IMAGE2;
    static const int IND_ID;

    explicit TableProjects(const QString &workingDirectory, QObject *parent = nullptr);

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // Creates the project folder and copies the source image(s) into it (the
    // project stays self-contained even if the originals move). imagePath2
    // is optional ("" for none) — a second angle/reference of the same
    // product, never itself regenerated/bootstrapped, always available as
    // extra reference material during generation. Keyword and hook are
    // optional; the name defaults to the keyword when given, else
    // "Project <n>" — renameable in the table afterwards. Returns the new row.
    int addProject(const QString &imagePath, const QString &imagePath2,
                   const QString &keyword, const QString &hookIdea);

    // Removes the row AND deletes the project's folder on disk — callers
    // must confirm with the user first.
    void removeProject(int row);

    QUuid projectId(int row) const;
    int rowOfId(const QUuid &projectId) const;
    // The project's own folder (<workingDir>/projects/<id>).
    QDir projectDir(int row) const;
    // Absolute path of the source image ("" when the row has none).
    QString absoluteImagePath(int row) const;
    // Absolute path of the optional second source image ("" when none).
    QString absoluteImagePath2(int row) const;

    // One folder per generation (<workingDir>/projects/<id>/generations/
    // <shortCode>/, created on first access): its top level holds only what
    // matters for publishing (the video/images, hook-description.txt); a
    // "temp" subfolder (see generationTempDir) holds everything else, so
    // browsing this folder alone is enough to review and publish.
    QDir generationDir(int row, const QString &shortCode) const;
    // The "temp" subfolder of one generation: source-image variant, prompt
    // files, suggestions/hooks JSON, rejected takes, extracted frames.
    QDir generationTempDir(int row, const QString &shortCode) const;
    // Scratch folder for the image + suggestion steps, which run BEFORE the
    // generation's short code is known. Callers should clear it (see
    // resetStagingDir) at the start of every Generate run.
    QDir stagingDir(int row) const;
    void resetStagingDir(int row) const;

private:
    static const QStringList HEADER;

    QDir m_workingDir;
    QString m_filePath;
    QList<QVariantList> m_listOfVariantList;

    void _loadFromFile();
    void _saveInFile();
};

#endif // TABLEPROJECTS_H
