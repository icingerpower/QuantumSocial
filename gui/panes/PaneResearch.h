#ifndef PANERESEARCH_H
#define PANERESEARCH_H

#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QSet>
#include <QMap>

#include <QCoro/QCoroTask>

#include "model/accounts/AbstractSocialAccount.h"

class AbstractCli;
class TableAccounts;
class ResearchStore;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStandardItemModel;
class QSortFilterProxyModel;
class QTableView;
class QTabWidget;
class QPushButton;
class QCheckBox;
class QMediaPlayer;
class QScrollArea;
class QStackedWidget;
class QSplitter;
class QGridLayout;

class PaneResearch : public QWidget
{
    Q_OBJECT
public:
    PaneResearch(TableAccounts *accounts, ResearchStore *store,
                 const QList<AbstractCli *> &clis, QWidget *parent = nullptr);
    ~PaneResearch() override;

    bool eventFilter(QObject *watched, QEvent *event) override;

    static QString defaultSpyQueryForProduct(const QJsonObject &product);
    static QString formatInfluencerOptionLabel(const QJsonObject &influencer);
    int importSpyResult(
        const AbstractSocialAccount::SpyResult &result,
        const QString &linkProductId = {},
        bool autoApprove = false,
        const QString &platformId = {},
        int *outMatchedInfluencerPicks = nullptr);
    QCoro::Task<void> runScrollAndSave(
        QString platformId,
        QString queryOrUrl,
        int maxPosts,
        bool autoScroll,
        QString linkProductId = {},
        int minEngagement = 0,
        int maxDaysOld = 0);
    QCoro::Task<void> runScrollAndSaveMode(
        QString platformId,
        QString queryOrUrl,
        int maxPosts,
        AbstractSocialAccount::SpyMode mode,
        QString linkProductId = {},
        int minEngagement = 0,
        int maxDaysOld = 0,
        QString influencerId = {});
    QJsonObject buildAutoBatchSpec(
        const QStringList &productIds,
        int generationsPerProduct,
        bool includeImage,
        bool includeCarousel,
        bool includeVideo,
        bool carouselHookOverlay = false,
        const QString &videoFormatLabel = QStringLiteral("vertical (9:16)"),
        const QString &videoGeneratorId = {}) const;
    bool hasConfiguredProductDirectory() const;
    void importProducts();

signals:
    void createProjectRequested(const QJsonObject &context, const QString &imagePath);
    void autoBatchRequested(const QJsonObject &batchSpec);
    void newlyImportedProductsChecked(const QStringList &productIds);

private:
    TableAccounts *m_accounts;
    ResearchStore *m_store;
    QList<AbstractCli *> m_clis;
    QComboBox *m_account;
    QTabWidget *m_tabs;
    QLabel *m_status;
    QLineEdit *m_directory;
    QLineEdit *m_productSearch;
    QCheckBox *m_onlyAllowed;
    QStandardItemModel *m_products;
    QSortFilterProxyModel *m_productProxy;
    QTableView *m_productTable;
    QLabel *m_productImage;
    QLabel *m_productDetails;
    QLabel *m_chosenProduct;
    QComboBox *m_sortReferences = nullptr;
    QString m_activeFilter = QStringLiteral("pending");
    QMap<QString, QPushButton *> m_filterButtons;
    QLabel *m_metricsSummary = nullptr;
    QScrollArea *m_cardsScroll = nullptr;
    QWidget *m_cards;
    QGridLayout *m_cardsLayout;
    QLineEdit *m_referenceSearch;
    QMap<QString, QCheckBox *> m_cardChecks;
    QStringList m_displayedRefIds;
    int m_pendingScrollRestoreY = -1;
    int m_visibleCards = 24;
    QPushButton *m_playPauseBtn = nullptr;
    QPushButton *m_starAuthorBtn = nullptr;
    QLineEdit *m_title;
    QPlainTextEdit *m_notes;
    QPlainTextEdit *m_evidence;
    QLabel *m_referenceImage;
    QStackedWidget *m_preview;
    QMediaPlayer *m_player;
    QComboBox *m_projectProductCombo = nullptr;
    QPushButton *m_createProjectBtn = nullptr;
    QCheckBox *m_nextAfterReview;
    QComboBox *m_cli;
    QPushButton *m_assess;
    QPushButton *m_scrollAndSave = nullptr;
    QList<QSplitter *> m_splitters;
    QString m_productId;
    QSet<QString> m_checkedProductIds;
    QString m_referenceId;
    bool m_refreshing = false;
    bool m_assessing = false;
    bool m_spying = false;
    bool m_backfilling = false;
    QSet<QString> m_backfillAttemptedIds;
    QCoro::Task<void> m_spyTask;
    QStringList m_assessmentQueue;
    QString m_runAssessor;
    AbstractCli *m_runCli = nullptr;
    QString m_runPrompt;
    QJsonArray m_lastProducts;
    QJsonObject m_lastProfile;
    bool m_lastOnlyAllowed = false;

    QString accountId() const;
    QString effectiveReferenceId() const;
    void refreshAccounts();
    void refresh();
    void refreshProducts();
    void refreshProjectProducts();
    void refreshReferences();
    void updateCreateProjectButton();
    void updateStarAuthorButton();
    void starCurrentReferenceAuthor();
    void backfillMissingPinterestMetadata();
    void showProduct();
    void showReference();
    void editProfile();
    void _showNewlyImportedProductsDialog(const QList<QJsonObject> &newProducts, int totalCatalogCount);
    void scrollAndSave();
    void addUrls();
    void addFiles();
    bool saveReference();
    void deleteSelection();
    void deleteAll();
    bool deleteReferenceIds(const QStringList &ids);
    void review(const QString &status);
    void reviewIds(const QStringList &ids, const QString &status);
    void assessNext();
    void createProject();
    void openAutoBatchDialog();
    QStringList selectedReferences() const;
    void selectReference(const QString &id);
    void setFilter(const QString &filter);
    void report(bool success, const QString &message = {});
    QWidget *buildProducts();
    QWidget *buildReferences();
};

#endif
