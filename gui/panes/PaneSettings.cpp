#include "PaneSettings.h"
#include "ui_PaneSettings.h"

#include <QHeaderView>
#include <QSignalBlocker>

#include "AvailableCliTable.h"

#include "model/FavoriteHooks.h"
#include "model/FavoriteVideoPrompts.h"
#include "model/PreferredHashtags.h"
#include "model/videogen/TableGenerationSettings.h"

PaneSettings::PaneSettings(TableGenerationSettings *generationSettings,
                          PreferredHashtags *hashtags, FavoriteHooks *favoriteHooks,
                          FavoriteVideoPrompts *favoriteVideoPrompts, QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneSettings)
    , m_generationSettings(generationSettings)
    , m_hashtags(hashtags)
    , m_favoriteHooks(favoriteHooks)
    , m_favoriteVideoPrompts(favoriteVideoPrompts)
    , m_availableClis(new AvailableCliTable(this))
{
    ui->setupUi(this);

    ui->tableViewClis->setModel(m_availableClis);
    ui->tableViewClis->horizontalHeader()->setStretchLastSection(true);
    ui->tableViewClis->resizeColumnToContents(AvailableCliTable::ColName);

    ui->tableViewGenerationSettings->setModel(m_generationSettings);
    ui->tableViewGenerationSettings->horizontalHeader()->setSectionResizeMode(
        TableGenerationSettings::IND_SETTING, QHeaderView::Stretch);
    ui->tableViewGenerationSettings->resizeColumnToContents(
        TableGenerationSettings::IND_GENERATOR);

    {
        const QSignalBlocker blocker{ui->plainTextEditHashtags};
        ui->plainTextEditHashtags->setPlainText(m_hashtags->hashtags().join(QStringLiteral("\n")));
    }
    connect(ui->plainTextEditHashtags, &QPlainTextEdit::textChanged,
            this, &PaneSettings::_saveHashtags);

    {
        const QSignalBlocker blocker{ui->plainTextEditFavoriteHooks};
        ui->plainTextEditFavoriteHooks->setPlainText(
            m_favoriteHooks->hooks().join(QStringLiteral("\n")));
    }
    connect(ui->plainTextEditFavoriteHooks, &QPlainTextEdit::textChanged,
            this, &PaneSettings::_saveFavoriteHooks);

    {
        const QSignalBlocker blocker{ui->plainTextEditFavoriteVideoPrompts};
        ui->plainTextEditFavoriteVideoPrompts->setPlainText(
            m_favoriteVideoPrompts->prompts().join(QStringLiteral("\n")));
    }
    connect(ui->plainTextEditFavoriteVideoPrompts, &QPlainTextEdit::textChanged,
            this, &PaneSettings::_saveFavoriteVideoPrompts);
}

PaneSettings::~PaneSettings()
{
    delete ui;
}

void PaneSettings::_saveHashtags()
{
    m_hashtags->setHashtags(PreferredHashtags::parse(ui->plainTextEditHashtags->toPlainText()));
}

void PaneSettings::_saveFavoriteHooks()
{
    m_favoriteHooks->setHooks(ui->plainTextEditFavoriteHooks->toPlainText()
        .split(QLatin1Char('\n')));
}

void PaneSettings::_saveFavoriteVideoPrompts()
{
    m_favoriteVideoPrompts->setPrompts(ui->plainTextEditFavoriteVideoPrompts->toPlainText()
        .split(QLatin1Char('\n')));
}
