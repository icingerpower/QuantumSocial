// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "PaneResearch.h"

#include "AbstractCli.h"
#include "model/TableAccounts.h"
#include "model/research/ProductSource.h"
#include "model/research/ResearchStore.h"
#include "model/research/ReferenceAssessor.h"

#include <QApplication>
#include <QAudioOutput>
#include <algorithm>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTableView>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVideoWidget>
#include <QVBoxLayout>

#ifndef SOCIAL_STATS_SCRIPT_DIR
#define SOCIAL_STATS_SCRIPT_DIR ""
#endif

namespace {
class StableCardsScrollArea : public QScrollArea
{
public:
    using QScrollArea::QScrollArea;

protected:
    bool focusNextPrevChild(bool next) override
    {
        return QWidget::focusNextPrevChild(next);
    }
};

QPushButton *button(QHBoxLayout *layout, const QString &text, const QString &name = {})
{
    auto *b = new QPushButton(text);
    if (!name.isEmpty()) b->setObjectName(name);
    layout->addWidget(b);
    return b;
}
QStringList strings(const QJsonArray &array)
{
    QStringList result;
    for (const auto &v : array) result << v.toString();
    return result;
}
bool isVideoMediaPath(const QString &path)
{
    if (path.isEmpty())
    {
        return false;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("mp4")
        || suffix == QLatin1String("mov")
        || suffix == QLatin1String("webm");
}

QPixmap loadCompatiblePixmap(const QString &path)
{
    if (path.isEmpty())
    {
        return {};
    }
    QPixmap pixmap(path);
    if (!pixmap.isNull() || !QFileInfo::exists(path))
    {
        return pixmap;
    }

    // Fallback for formats missing a Qt imageformat plugin (such as .webp when
    // qtimageformats / libqwebp.so is not installed) and first-frame thumbnails
    // for video references (.mp4, .mov, .webm).
    const QString previewCache = path + QStringLiteral(".preview.png");
    if (QFileInfo::exists(previewCache))
    {
        QPixmap cached(previewCache);
        if (!cached.isNull())
        {
            return cached;
        }
    }

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!ffmpeg.isEmpty())
    {
        QProcess proc;
        proc.start(ffmpeg, {
            QStringLiteral("-y"),
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-i"), path,
            QStringLiteral("-frames:v"), QStringLiteral("1"),
            previewCache
        });
        if (proc.waitForFinished(5'000) && proc.exitCode() == 0)
        {
            return QPixmap(previewCache);
        }
    }
    return {};
}

void previewImage(QLabel *label, const QString &path, const QSize &size, const QString &fallback)
{
    label->setTextFormat(Qt::PlainText);
    const QPixmap pixmap = loadCompatiblePixmap(path);
    if (pixmap.isNull()) { label->setPixmap({}); label->setText(fallback); }
    else label->setPixmap(pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
QString text(const QJsonObject &object, const char *key) { return object.value(QLatin1String(key)).toString(); }
bool hasAccountRules(const QJsonObject &profile)
{
    return !profile.value("brands").toArray().isEmpty()
        && !profile.value("categories").toArray().isEmpty();
}

QString formatMetricCount(qint64 value)
{
    if (value < 0)
    {
        return QString{};
    }
    if (value >= 1'000'000)
    {
        return QStringLiteral("%1M").arg(double(value) / 1'000'000.0, 0, 'f', 1);
    }
    if (value >= 10'000)
    {
        return QStringLiteral("%1K").arg(double(value) / 1'000.0, 0, 'f', 1);
    }
    return QLocale(QLocale::English).toString(value);
}

qint64 statField(const QJsonObject &stats, const char *key)
{
    const QJsonValue v = stats.value(QLatin1String(key));
    return v.isDouble() ? static_cast<qint64>(v.toDouble(-1)) : -1;
}

qint64 referenceEngagementScore(const QJsonObject &ref)
{
    const QJsonObject stats = ref.value(QStringLiteral("stats")).toObject();
    if (stats.isEmpty())
    {
        return -1;
    }
    const QString platform = ref.value(QStringLiteral("platform")).toString();
    const qint64 saves = qMax(statField(stats, "saves"), statField(stats, "shares"));
    const qint64 likes = statField(stats, "likes");
    const qint64 comments = statField(stats, "comments");
    const qint64 views = statField(stats, "views");
    if (platform == QLatin1String("pinterest"))
    {
        if (saves < 0 && likes < 0 && comments < 0)
        {
            return -1;
        }
        return qMax<qint64>(0, saves) + qMax<qint64>(0, likes) + qMax<qint64>(0, comments);
    }
    if (views >= 0)
    {
        return views;
    }
    if (likes >= 0 || saves >= 0 || comments >= 0)
    {
        return qMax<qint64>(0, likes) + qMax<qint64>(0, saves) + qMax<qint64>(0, comments);
    }
    return -1;
}

qint64 referenceAgeDays(const QJsonObject &ref)
{
    const QString publishedAt = ref.value(QStringLiteral("stats")).toObject()
        .value(QStringLiteral("publishedAt")).toString().trimmed();
    if (publishedAt.size() < 10)
    {
        return -1;
    }
    const QDate pubDate = QDate::fromString(publishedAt.left(10), Qt::ISODate);
    if (!pubDate.isValid())
    {
        return -1;
    }
    return qMax<qint64>(0, pubDate.daysTo(QDate::currentDate()));
}

QString formatDailyRate(double perDay, const QString &unitLabel)
{
    QString num;
    if (perDay <= 0.0)
    {
        num = QStringLiteral("0");
    }
    else if (perDay >= 100.0)
    {
        num = formatMetricCount(qRound64(perDay));
    }
    else if (perDay >= 1.0)
    {
        num = QString::number(perDay, 'f', 1);
    }
    else
    {
        num = QString::number(perDay, 'f', 2);
    }
    return QStringLiteral("%1 %2").arg(num, unitLabel);
}

double referenceDailyVelocity(const QJsonObject &ref)
{
    const qint64 ageDays = referenceAgeDays(ref);
    if (ageDays < 0)
    {
        return -1.0;
    }
    const double denom = double(qMax<qint64>(1, ageDays));
    const QJsonObject stats = ref.value(QStringLiteral("stats")).toObject();
    const QString platform = ref.value(QStringLiteral("platform")).toString();
    const qint64 saves = qMax(statField(stats, "saves"), statField(stats, "shares"));
    const qint64 views = statField(stats, "views");
    const qint64 likes = statField(stats, "likes");
    if (platform == QLatin1String("pinterest"))
    {
        if (saves >= 0)
        {
            return double(saves) / denom;
        }
        if (likes >= 0)
        {
            return double(likes) / denom;
        }
        return -1.0;
    }
    if (views >= 0)
    {
        return double(views) / denom;
    }
    if (saves >= 0 || likes >= 0)
    {
        return double(qMax<qint64>(0, saves) + qMax<qint64>(0, likes)) / denom;
    }
    return -1.0;
}

QString formatReferenceStatsLine(const QJsonObject &ref)
{
    const QJsonObject stats = ref.value(QStringLiteral("stats")).toObject();
    const QString platform = ref.value(QStringLiteral("platform")).toString();
    const bool isAd = stats.value(QStringLiteral("isAd")).toBool(false);
    const qint64 saves = qMax(statField(stats, "saves"), statField(stats, "shares"));
    const qint64 likes = statField(stats, "likes");
    const qint64 views = statField(stats, "views");
    const qint64 comments = statField(stats, "comments");
    const QString author = stats.value(QStringLiteral("author")).toString().trimmed();
    const QString publishedAt = stats.value(QStringLiteral("publishedAt")).toString().trimmed();
    const qint64 ageDays = referenceAgeDays(ref);
    const double denomDays = double(qMax<qint64>(1, ageDays));

    QStringList parts;
    if (isAd)
    {
        parts << QStringLiteral("ads");
    }
    if (platform == QLatin1String("pinterest"))
    {
        if (saves >= 0)
        {
            parts << QObject::tr("%1 saves").arg(formatMetricCount(saves));
            if (ageDays >= 0)
            {
                parts << formatDailyRate(double(saves) / denomDays, QObject::tr("saves/day"));
            }
        }
        if (likes >= 0)
        {
            parts << QObject::tr("%1 likes").arg(formatMetricCount(likes));
        }
        if (comments > 0)
        {
            parts << QObject::tr("%1 comments").arg(formatMetricCount(comments));
        }
    }
    else
    {
        if (views >= 0)
        {
            parts << QObject::tr("%1 views").arg(formatMetricCount(views));
            if (ageDays >= 0)
            {
                parts << formatDailyRate(double(views) / denomDays, QObject::tr("views/day"));
            }
        }
        if (likes >= 0)
        {
            parts << QObject::tr("%1 likes").arg(formatMetricCount(likes));
        }
        if (saves >= 0)
        {
            parts << QObject::tr("%1 shares").arg(formatMetricCount(saves));
        }
        if (comments > 0)
        {
            parts << QObject::tr("%1 comments").arg(formatMetricCount(comments));
        }
    }
    if (!publishedAt.isEmpty())
    {
        if (ageDays >= 0)
        {
            parts << QObject::tr("%1 (%2d old)").arg(publishedAt.left(10)).arg(ageDays);
        }
        else
        {
            parts << publishedAt.left(10);
        }
    }
    if (!author.isEmpty())
    {
        parts << author;
    }
    return parts.join(QStringLiteral(" · "));
}

QString formatReferenceStatsRichHtml(const QJsonObject &ref)
{
    const QJsonObject stats = ref.value(QStringLiteral("stats")).toObject();
    const bool isAd = stats.value(QStringLiteral("isAd")).toBool(false);
    const QString plain = formatReferenceStatsLine(ref);
    if (plain.isEmpty())
    {
        return QString{};
    }
    if (!isAd)
    {
        return plain.toHtmlEscaped();
    }
    QStringList parts = plain.split(QStringLiteral(" · "));
    if (!parts.isEmpty() && parts.first() == QLatin1String("ads"))
    {
        parts.removeFirst();
    }
    QStringList escaped;
    escaped << QStringLiteral("<b style=\"color:#fbbf24;\">ads</b>");
    for (const QString &p : parts)
    {
        escaped << p.toHtmlEscaped();
    }
    return escaped.join(QStringLiteral(" &middot; "));
}

QString cleanDisplayTitle(const QJsonObject &ref)
{
    QString t = text(ref, "title").trimmed();
    t.remove(QRegularExpression(QStringLiteral("\\s*\\bpin\\s+page\\s*$"), QRegularExpression::CaseInsensitiveOption));
    t = t.trimmed();
    if (t.isEmpty() || t.compare(QStringLiteral("Untitled"), Qt::CaseInsensitive) == 0)
    {
        return QObject::tr("Untitled reference");
    }
    return t;
}

int defaultMinEngagementForPlatform(const QString &platformId)
{
    if (platformId == QLatin1String("pinterest"))
    {
        return 10;
    }
    if (platformId == QLatin1String("instagram"))
    {
        return 20;
    }
    if (platformId == QLatin1String("tiktok") || platformId == QLatin1String("youtube"))
    {
        return 1000;
    }
    return 0;
}
}

PaneResearch::PaneResearch(TableAccounts *accounts, ResearchStore *store,
                           const QList<AbstractCli *> &clis, QWidget *parent)
    : QWidget(parent), m_accounts(accounts), m_store(store), m_clis(clis)
{
    auto *layout = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    layout->addLayout(top);
    top->addWidget(new QLabel(tr("Account:")));
    m_account = new QComboBox;
    m_account->setObjectName("researchAccount");
    m_account->setMinimumContentsLength(25);
    top->addWidget(m_account, 1);
    auto *rules = button(top, tr("Account rules…"), "researchAccountRules");
    m_chosenProduct = new QLabel(tr("Choose a product in Products, then review its references."));
    m_chosenProduct->setTextFormat(Qt::PlainText);
    m_chosenProduct->setWordWrap(true);
    layout->addWidget(m_chosenProduct);
    m_tabs = new QTabWidget;
    layout->addWidget(m_tabs, 1);
    m_tabs->addTab(buildProducts(), tr("Products"));
    m_tabs->addTab(buildReferences(), tr("References"));
    m_status = new QLabel;
    m_status->setObjectName("researchStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    connect(rules, &QPushButton::clicked, this, &PaneResearch::editProfile);
    connect(m_account, &QComboBox::currentIndexChanged, this, [this] {
        if (!saveReference()) return;
        m_productId.clear(); m_referenceId.clear(); m_player->stop();
        {
            const QSignalBlocker blockAllowed(m_onlyAllowed);
            m_onlyAllowed->setChecked(hasAccountRules(m_store->profile(accountId())));
        }
        refresh();
    });
    connect(m_accounts, &QAbstractItemModel::rowsInserted, this, &PaneResearch::refreshAccounts);
    connect(m_accounts, &QAbstractItemModel::rowsRemoved, this, &PaneResearch::refreshAccounts);
    connect(m_accounts, &QAbstractItemModel::dataChanged, this, &PaneResearch::refreshAccounts);
    connect(m_store, &ResearchStore::changed, this, [this] { if (!m_refreshing) refresh(); });
    refreshAccounts();
    QSettings settings(m_store->directory().filePath("ui.ini"), QSettings::IniFormat);
    QString savedDir = settings.value("productDirectory").toString();
    if (savedDir.isEmpty())
        savedDir = m_store->lastProductDirectory();
    m_directory->setText(savedDir);
    if (!savedDir.isEmpty() && QDir(savedDir).exists())
    {
        bool hasLegacyNonAPlus = false;
        for (const auto &v : m_store->products())
        {
            if (text(v.toObject(), "sizingFolder").isEmpty())
            {
                hasLegacyNonAPlus = true;
                break;
            }
        }
        if (hasLegacyNonAPlus)
        {
            if (const auto *src = ProductSource::all().value("amazon-template3"))
            {
                const auto res = src->read(savedDir);
                if (res.error.isEmpty())
                {
                    m_store->replaceProducts(res.products, QFileInfo(savedDir).canonicalFilePath());
                    m_lastProducts = QJsonArray{};
                    refreshProducts();
                }
            }
        }
    }
    m_nextAfterReview->setChecked(settings.value("nextAfterReview", true).toBool());
    for (int i = 0; i < m_splitters.size(); ++i)
        m_splitters[i]->restoreState(settings.value(QString("splitter%1").arg(i)).toByteArray());
    if (!m_store->lastError().isEmpty()) report(false);
}

PaneResearch::~PaneResearch()
{
    m_store->directory().mkpath(".");
    QSettings settings(m_store->directory().filePath("ui.ini"), QSettings::IniFormat);
    settings.setValue("productDirectory", m_directory->text());
    settings.setValue("nextAfterReview", m_nextAfterReview->isChecked());
    for (int i = 0; i < m_splitters.size(); ++i) settings.setValue(QString("splitter%1").arg(i), m_splitters[i]->saveState());
}

bool PaneResearch::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease)
    {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton)
        {
            const QString refId = watched->property("referenceId").toString();
            if (!refId.isEmpty() && refId != m_referenceId)
            {
                QTimer::singleShot(0, this, [this, refId] { selectReference(refId); });
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

QWidget *PaneResearch::buildProducts()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *source = new QHBoxLayout;
    layout->addLayout(source);
    m_directory = new QLineEdit;
    m_directory->setObjectName("amazonSourceDirectory");
    m_directory->setPlaceholderText(tr("AmazonTemplate3 working directory"));
    source->addWidget(m_directory, 1);
    auto *browse = button(source, tr("Browse…"));
    auto *import = button(source, tr("Import / refresh"), "importAmazonProducts");
    auto *autoBatchTop = button(source, tr("Auto-generate batch…"), "researchAutoBatchProducts");
    autoBatchTop->setStyleSheet(QStringLiteral("font-weight: 600;"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, tr("AmazonTemplate3 working directory"), m_directory->text());
        if (!path.isEmpty()) m_directory->setText(path);
    });
    connect(import, &QPushButton::clicked, this, &PaneResearch::importProducts);
    connect(autoBatchTop, &QPushButton::clicked, this, &PaneResearch::openAutoBatchDialog);
    auto *filters = new QHBoxLayout;
    layout->addLayout(filters);
    m_productSearch = new QLineEdit;
    m_productSearch->setPlaceholderText(tr("Search title, brand, category, color, ASIN or SKU…"));
    filters->addWidget(m_productSearch, 1);
    m_onlyAllowed = new QCheckBox(tr("Allowed for this account"));
    m_onlyAllowed->setChecked(true);
    filters->addWidget(m_onlyAllowed);
    auto *split = new QSplitter;
    m_splitters << split;
    layout->addWidget(split, 1);
    m_productTable = new QTableView;
    m_productTable->setObjectName("researchProducts");
    m_productTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_productTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_productTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_products = new QStandardItemModel(this);
    m_products->setHorizontalHeaderLabels({tr("Product"), tr("Brand"), tr("Categories"), tr("Color"), tr("Variants"), tr("ASIN / SKU")});
    m_productProxy = new QSortFilterProxyModel(this);
    m_productProxy->setSourceModel(m_products);
    m_productProxy->setFilterKeyColumn(-1);
    m_productProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_productTable->setModel(m_productProxy);
    m_productTable->setSortingEnabled(true);
    m_productTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    split->addWidget(m_productTable);
    auto *detail = new QWidget;
    auto *detailLayout = new QVBoxLayout(detail);
    m_productImage = new QLabel;
    m_productImage->setAlignment(Qt::AlignCenter);
    m_productImage->setMinimumSize(250, 220);
    detailLayout->addWidget(m_productImage, 1);
    m_productDetails = new QLabel;
    m_productDetails->setTextFormat(Qt::PlainText);
    m_productDetails->setWordWrap(true);
    m_productDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLayout->addWidget(m_productDetails);
    auto *use = new QPushButton(tr("Use product → References"));
    detailLayout->addWidget(use);
    connect(use, &QPushButton::clicked, this, [this] {
        if (m_productId.isEmpty())
        {
            const QString msg = tr("Select a product first.");
            report(false, msg);
            QMessageBox::warning(this, tr("No product selected"), msg);
            return;
        }
        const auto reason = ResearchStore::productRestriction(m_store->product(m_productId), m_store->profile(accountId()));
        if (!reason.isEmpty())
        {
            report(false, reason);
            QMessageBox::warning(this, tr("Product not allowed for account"),
                tr("%1\n\nClick \"Account rules…\" (top right) to configure allowed brands and categories for this account.")
                    .arg(reason));
            return;
        }
        m_tabs->setCurrentIndex(1);
    });
    split->addWidget(detail);
    split->setSizes({700, 320});
    connect(m_productSearch, &QLineEdit::textChanged, m_productProxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(m_onlyAllowed, &QCheckBox::toggled, this, &PaneResearch::refreshProducts);
    connect(m_productTable->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { if (!m_refreshing) showProduct(); });
    connect(m_products, &QStandardItemModel::itemChanged, this, [this](QStandardItem *item) {
        if (m_refreshing || !item || item->column() != 0) return;
        const QString pid = item->data(Qt::UserRole).toString();
        if (pid.isEmpty()) return;
        if (item->checkState() == Qt::Checked) m_checkedProductIds.insert(pid);
        else m_checkedProductIds.remove(pid);
    });
    return page;
}

QWidget *PaneResearch::buildReferences()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    // Top bar: collection & cleanup on the left, 1-click view pills + sort on the right
    auto *actions = new QHBoxLayout;
    layout->addLayout(actions);
    m_scrollAndSave = button(actions, tr("Scroll & save…"), "researchScrollAndSave");
    connect(m_scrollAndSave, &QPushButton::clicked, this, &PaneResearch::scrollAndSave);

    auto *addBtn = button(actions, tr("Add…"), "researchAddMenu");
    auto *addMenu = new QMenu(addBtn);
    addMenu->addAction(tr("Paste post URLs…"), this, &PaneResearch::addUrls);
    addMenu->addAction(tr("From local images / videos…"), this, &PaneResearch::addFiles);
    addBtn->setMenu(addMenu);

    connect(button(actions, tr("Delete selection"), "researchDeleteSelection"), &QPushButton::clicked, this, &PaneResearch::deleteSelection);
    connect(button(actions, tr("Delete all"), "researchDeleteAll"), &QPushButton::clicked, this, &PaneResearch::deleteAll);
    actions->addStretch();

    auto *filterGroup = new QButtonGroup(this);
    filterGroup->setExclusive(true);
    const QList<QPair<QString, QString>> filterSpecs{
        {QStringLiteral("pending"), tr("Pending")},
        {QStringLiteral("approved"), tr("Approved")},
        {QStringLiteral("rejected"), tr("Rejected")},
        {QStringLiteral("auto_discarded"), tr("Auto-discarded")},
        {QString{}, tr("All")}
    };
    for (const auto &spec : filterSpecs)
    {
        const QString key = spec.first;
        const QString objSuffix = key.isEmpty() ? QStringLiteral("all") : key;
        auto *fb = new QPushButton(spec.second);
        fb->setObjectName(QStringLiteral("researchFilter_") + objSuffix);
        fb->setCheckable(true);
        fb->setChecked(key == m_activeFilter);
        fb->setStyleSheet(QStringLiteral(
            "QPushButton { padding: 4px 10px; border-radius: 4px; }"
            "QPushButton:checked { background: palette(highlight); color: palette(highlighted-text); font-weight: 600; }"));
        filterGroup->addButton(fb);
        actions->addWidget(fb);
        m_filterButtons.insert(key, fb);
        connect(fb, &QPushButton::clicked, this, [this, key] { setFilter(key); });
    }

    m_sortReferences = new QComboBox;
    m_sortReferences->setObjectName("researchSortCombo");
    m_sortReferences->addItem(tr("Sort: Best stats first"), "stats");
    m_sortReferences->addItem(tr("Sort: Highest saves/day"), "velocity");
    m_sortReferences->addItem(tr("Sort: Newest post date"), "published");
    m_sortReferences->addItem(tr("Sort: Recently added"), "newest");
    actions->addWidget(m_sortReferences);

    m_referenceSearch = new QLineEdit;
    m_referenceSearch->setPlaceholderText(tr("Search references (title, @author, ads)…"));
    layout->addWidget(m_referenceSearch);

    m_metricsSummary = new QLabel;
    m_metricsSummary->setObjectName("researchMetricsSummary");
    m_metricsSummary->setTextFormat(Qt::PlainText);
    m_metricsSummary->setWordWrap(true);
    m_metricsSummary->setStyleSheet(QStringLiteral(
        "QLabel#researchMetricsSummary { background: rgba(128, 140, 160, 0.12); "
        "border: 1px solid rgba(128, 140, 160, 0.28); border-radius: 5px; padding: 6px 10px; }"));
    layout->addWidget(m_metricsSummary);

    auto *split = new QSplitter;
    m_splitters << split;
    layout->addWidget(split, 1);

    m_cardsScroll = new StableCardsScrollArea;
    m_cardsScroll->setObjectName("referenceScrollArea");
    m_cardsScroll->setWidgetResizable(true);
    m_cards = new QWidget;
    m_cards->setObjectName("referenceCards");
    m_cardsLayout = new QGridLayout(m_cards);
    m_cardsLayout->setAlignment(Qt::AlignTop);
    m_cardsLayout->setColumnStretch(0, 1);
    m_cardsLayout->setColumnStretch(1, 1);
    m_cardsScroll->setWidget(m_cards);
    connect(m_cardsScroll->verticalScrollBar(), &QAbstractSlider::rangeChanged, this, [this](int, int max) {
        if (m_pendingScrollRestoreY >= 0 && max > 0)
        {
            const int target = qMin(m_pendingScrollRestoreY, max);
            QTimer::singleShot(0, this, [this, target]() {
                if (m_cardsScroll && m_cardsScroll->verticalScrollBar())
                {
                    auto *vb = m_cardsScroll->verticalScrollBar();
                    vb->setValue(qMin(target, vb->maximum()));
                }
                m_pendingScrollRestoreY = -1;
            });
        }
    });
    split->addWidget(m_cardsScroll);

    auto *detail = new QWidget;
    auto *details = new QVBoxLayout(detail);
    m_preview = new QStackedWidget;
    m_referenceImage = new QLabel;
    m_referenceImage->setAlignment(Qt::AlignCenter);
    m_referenceImage->setWordWrap(true);
    m_referenceImage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_preview->addWidget(m_referenceImage);
    auto *video = new QVideoWidget;
    m_preview->addWidget(video);
    m_preview->setMinimumSize(250, 180);
    details->addWidget(m_preview, 2);

    m_player = new QMediaPlayer(this);
    auto *audio = new QAudioOutput(this);
    m_player->setAudioOutput(audio);
    m_player->setVideoOutput(video);
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this] { report(false, m_player->errorString()); });

    auto *media = new QHBoxLayout;
    details->addLayout(media);
    m_playPauseBtn = button(media, tr("Play / pause"));
    m_playPauseBtn->setVisible(false);
    connect(m_playPauseBtn, &QPushButton::clicked, this, [this] {
        if (m_player->playbackState() == QMediaPlayer::PlayingState) m_player->pause(); else m_player->play();
    });
    connect(button(media, tr("Open in Chrome")), &QPushButton::clicked, this, [this] {
        const auto reference = m_store->reference(effectiveReferenceId());
        const QString url = ResearchStore::canonicalUrl(text(reference, "url"));
        if (!url.isEmpty()) QDesktopServices::openUrl(QUrl(url));
    });
    connect(button(media, tr("Replace media…")), &QPushButton::clicked, this, [this] {
        const QString refId = effectiveReferenceId();
        if (refId.isEmpty()) { report(false, tr("Click a reference card first.")); return; }
        if (!saveReference()) return;
        const QString path = QFileDialog::getOpenFileName(this, tr("Replace reference media"), {}, tr("Media (*.jpg *.jpeg *.png *.webp *.mp4 *.mov *.webm)"));
        if (path.isEmpty()) return;
        const bool ok = m_store->attachMedia(refId, path);
        report(ok, tr("Media replaced. The reference is pending review."));
        if (ok) selectReference(refId);
    });
    m_starAuthorBtn = button(media, tr("☆ Star @author"), "researchStarAuthor");
    m_starAuthorBtn->setVisible(false);
    connect(m_starAuthorBtn, &QPushButton::clicked, this, &PaneResearch::starCurrentReferenceAuthor);
    media->addStretch();

    m_title = new QLineEdit;
    m_title->setObjectName("researchReferenceTitle");
    m_title->setPlaceholderText(tr("Reference title (auto-saved)"));
    details->addWidget(m_title);

    m_notes = new QPlainTextEdit;
    auto *saveNotes = new QTimer(this);
    saveNotes->setSingleShot(true);
    connect(m_notes, &QPlainTextEdit::textChanged, saveNotes, [saveNotes] { saveNotes->start(800); });
    connect(saveNotes, &QTimer::timeout, this, [this] { saveReference(); });
    connect(m_title, &QLineEdit::editingFinished, this, [this] { saveReference(); });
    m_notes->setPlaceholderText(tr("Optional adaptation notes (e.g. camera angle, outfit styling, pacing)…"));

    auto *detailTabs = new QTabWidget;
    detailTabs->addTab(m_notes, tr("Adaptation notes"));
    m_evidence = new QPlainTextEdit;
    m_evidence->setReadOnly(true);
    detailTabs->addTab(m_evidence, tr("History & AI"));
    details->addWidget(detailTabs, 1);

    // Self-contained Project Creation box: pick ANY allowed product right here
    auto *projectBox = new QFrame;
    projectBox->setObjectName("researchProjectBox");
    projectBox->setFrameShape(QFrame::StyledPanel);
    projectBox->setStyleSheet(QStringLiteral(
        "QFrame#researchProjectBox { background: rgba(88, 166, 255, 0.08); "
        "border: 1px solid rgba(88, 166, 255, 0.35); border-radius: 6px; padding: 6px; }"));
    auto *projectBoxLayout = new QVBoxLayout(projectBox);
    projectBoxLayout->setContentsMargins(8, 6, 8, 6);
    projectBoxLayout->setSpacing(6);

    auto *productRow = new QHBoxLayout;
    productRow->addWidget(new QLabel(tr("Product:")));
    m_projectProductCombo = new QComboBox;
    m_projectProductCombo->setObjectName("researchProjectProduct");
    m_projectProductCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    productRow->addWidget(m_projectProductCombo, 1);
    projectBoxLayout->addLayout(productRow);

    connect(m_projectProductCombo, &QComboBox::currentIndexChanged, this, [this] {
        const QString pid = m_projectProductCombo->currentData().toString();
        if (!pid.isEmpty())
        {
            m_productId = pid;
            const auto prod = m_store->product(m_productId);
            if (!prod.isEmpty())
            {
                m_chosenProduct->setText(tr("Selected product: %1").arg(text(prod, "title")));
            }
        }
        updateCreateProjectButton();
    });

    auto *projectBtnsRow = new QHBoxLayout;
    m_createProjectBtn = new QPushButton(tr("Create generation project"));
    m_createProjectBtn->setObjectName("researchCreateProject");
    m_createProjectBtn->setMinimumHeight(32);
    m_createProjectBtn->setStyleSheet(QStringLiteral("font-weight: 600;"));
    projectBtnsRow->addWidget(m_createProjectBtn, 1);
    auto *autoBatchBtn = new QPushButton(tr("Auto-batch…"));
    autoBatchBtn->setObjectName("researchAutoBatch");
    autoBatchBtn->setMinimumHeight(32);
    autoBatchBtn->setToolTip(tr("Select 1 or more products and generate N diverse images, carousels, and videos automatically."));
    projectBtnsRow->addWidget(autoBatchBtn);
    projectBoxLayout->addLayout(projectBtnsRow);
    connect(m_createProjectBtn, &QPushButton::clicked, this, &PaneResearch::createProject);
    connect(autoBatchBtn, &QPushButton::clicked, this, &PaneResearch::openAutoBatchDialog);
    details->addWidget(projectBox);

    split->addWidget(detail);
    split->setSizes({660, 360});

    // Single compact bottom bar for optional batch / AI actions on checked cards
    auto *bottomBar = new QHBoxLayout;
    layout->addLayout(bottomBar);
    m_nextAfterReview = new QCheckBox(tr("Next after review"));
    m_nextAfterReview->setChecked(true);
    m_nextAfterReview->setToolTip(tr("Automatically advance to the next pending card after approving or rejecting."));
    bottomBar->addWidget(m_nextAfterReview);
    bottomBar->addStretch();
    connect(button(bottomBar, tr("Approve checked")), &QPushButton::clicked, this, [this] { review("approved"); });
    connect(button(bottomBar, tr("Reject checked")), &QPushButton::clicked, this, [this] { review("rejected"); });
    m_cli = new QComboBox;
    for (auto *cli : m_clis) m_cli->addItem(cli->getName());
    bottomBar->addWidget(m_cli);
    m_assess = button(bottomBar, tr("AI assess"));
    connect(m_assess, &QPushButton::clicked, this, [this] {
        if (m_assessing) { m_assessmentQueue.clear(); report(true, tr("Stopping after the current assessment.")); return; }
        if (!saveReference()) return;
        m_assessmentQueue = selectedReferences();
        if (m_assessmentQueue.isEmpty()) { report(false, tr("Click or check reference cards first.")); return; }
        m_runAssessor = m_clis.isEmpty() ? QStringLiteral("account-rules") : QStringLiteral("cli-suitability");
        m_runCli = m_cli->currentIndex() >= 0 ? m_clis.value(m_cli->currentIndex()) : nullptr;
        m_runPrompt = m_store->profile(accountId()).value("assessmentPrompt").toString(ReferenceAssessor::defaultPrompt());
        m_assessing = true;
        m_assess->setText(tr("Stop AI"));
        assessNext();
    });

    connect(m_sortReferences, &QComboBox::currentIndexChanged, this, [this] {
        m_visibleCards = 24;
        m_pendingScrollRestoreY = 0;
        if (m_cardsScroll && m_cardsScroll->verticalScrollBar()) m_cardsScroll->verticalScrollBar()->setValue(0);
        refreshReferences();
    });
    connect(m_referenceSearch, &QLineEdit::textChanged, this, [this] {
        m_visibleCards = 24;
        m_pendingScrollRestoreY = 0;
        if (m_cardsScroll && m_cardsScroll->verticalScrollBar()) m_cardsScroll->verticalScrollBar()->setValue(0);
        refreshReferences();
    });
    auto *approveShortcut = new QShortcut(QKeySequence("Ctrl+Return"), page);
    approveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(approveShortcut, &QShortcut::activated, this, [this] { review("approved"); });
    auto *deleteShortcut = new QShortcut(QKeySequence::Delete, m_cards);
    deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, &PaneResearch::deleteSelection);
    return page;
}

QString PaneResearch::accountId() const { return m_account->currentData().toString(); }

QString PaneResearch::effectiveReferenceId() const
{
    if (!m_referenceId.isEmpty() && !m_store->reference(m_referenceId).isEmpty())
    {
        return m_referenceId;
    }
    const QStringList checked = selectedReferences();
    return checked.isEmpty() ? QString{} : checked.first();
}

void PaneResearch::setFilter(const QString &filter)
{
    m_activeFilter = filter;
    for (auto it = m_filterButtons.cbegin(); it != m_filterButtons.cend(); ++it)
    {
        const QSignalBlocker block(it.value());
        it.value()->setChecked(it.key() == m_activeFilter);
    }
    m_visibleCards = 24;
    m_pendingScrollRestoreY = 0;
    if (m_cardsScroll && m_cardsScroll->verticalScrollBar())
    {
        m_cardsScroll->verticalScrollBar()->setValue(0);
    }
    refreshReferences();
}

void PaneResearch::refreshAccounts()
{
    const QString selected = accountId();
    const QSignalBlocker block(m_account);
    m_account->clear();
    for (int i = 0; i < m_accounts->rowCount(); ++i)
    {
        const QString label = m_accounts->data(m_accounts->index(i, TableAccounts::IND_TYPE)).toString()
            + " — " + m_accounts->data(m_accounts->index(i, TableAccounts::IND_URL)).toString();
        m_account->addItem(label, m_accounts->accountId(i).toString(QUuid::WithoutBraces));
    }
    const int index = m_account->findData(selected);
    if (index >= 0) m_account->setCurrentIndex(index);
    if (selected != accountId() || selected.isEmpty())
    {
        m_productId.clear();
        m_referenceId.clear();
        const QSignalBlocker blockAllowed(m_onlyAllowed);
        m_onlyAllowed->setChecked(hasAccountRules(m_store->profile(accountId())));
    }
    refresh();
}

void PaneResearch::refresh()
{
    refreshProducts();
    refreshProjectProducts();
    refreshReferences();
}

void PaneResearch::refreshProjectProducts()
{
    if (!m_projectProductCombo)
    {
        return;
    }
    const auto profile = m_store->profile(accountId());
    const auto products = m_store->products();
    const QString previousId = m_productId;

    const QSignalBlocker block(m_projectProductCombo);
    m_projectProductCombo->clear();

    for (const auto &v : products)
    {
        const auto p = v.toObject();
        if (!ResearchStore::productRestriction(p, profile).isEmpty())
        {
            continue;
        }
        const QString brand = text(p, "brand");
        const QString color = text(p, "color");
        const QString title = text(p, "title");
        QString label = brand;
        if (!color.isEmpty())
        {
            label += (label.isEmpty() ? QString{} : QStringLiteral(" · ")) + color;
        }
        if (!title.isEmpty())
        {
            label += (label.isEmpty() ? QString{} : QStringLiteral(" — ")) + title;
        }
        m_projectProductCombo->addItem(label.left(110), text(p, "id"));
    }

    if (m_projectProductCombo->count() == 0)
    {
        m_projectProductCombo->addItem(
            products.isEmpty()
                ? tr("Import products in Products tab first…")
                : tr("Click \"Account rules…\" (top right) to allow products…"),
            QString{});
        m_projectProductCombo->setEnabled(false);
    }
    else
    {
        m_projectProductCombo->setEnabled(true);
        int idx = m_projectProductCombo->findData(previousId);
        if (idx < 0)
        {
            idx = 0;
        }
        m_projectProductCombo->setCurrentIndex(idx);
        m_productId = m_projectProductCombo->currentData().toString();
        if (!m_productId.isEmpty())
        {
            m_chosenProduct->setText(tr("Selected product: %1").arg(text(m_store->product(m_productId), "title")));
        }
    }
    updateCreateProjectButton();
}

void PaneResearch::refreshProducts()
{
    const auto profile = m_store->profile(accountId());
    const auto products = m_store->products();
    if (m_lastProducts == products && m_lastProfile == profile && m_lastOnlyAllowed == m_onlyAllowed->isChecked()) return;
    m_lastProducts = products; m_lastProfile = profile; m_lastOnlyAllowed = m_onlyAllowed->isChecked();
    QSet<QString> validProductIds;
    for (const auto &value : products)
    {
        const QString pid = text(value.toObject(), "id");
        if (!pid.isEmpty())
        {
            validProductIds.insert(pid);
        }
    }
    m_checkedProductIds.intersect(validProductIds);
    m_refreshing = true;
    m_products->removeRows(0, m_products->rowCount());
    for (const auto &value : products)
    {
        const auto product = value.toObject();
        const auto restriction = ResearchStore::productRestriction(product, profile);
        if (m_onlyAllowed->isChecked() && !restriction.isEmpty()) continue;
        QStringList identifiers;
        for (const auto &v : product.value("variants").toArray()) identifiers << text(v.toObject(), "asin") << text(v.toObject(), "sku");
        const QStringList columns{text(product, "title"), text(product, "brand"), strings(product.value("categories").toArray()).join(", "),
            text(product, "color"), QString::number(product.value("variants").toArray().size()), identifiers.join(" ")};
        QList<QStandardItem *> row;
        for (const auto &column : columns) { auto *item = new QStandardItem(column); item->setToolTip(restriction); row << item; }
        const QString pid = text(product, "id");
        row[0]->setData(pid, Qt::UserRole);
        row[0]->setCheckable(true);
        row[0]->setCheckState(m_checkedProductIds.contains(pid) ? Qt::Checked : Qt::Unchecked);
        m_products->appendRow(row);
    }
    for (int row = 0; row < m_productProxy->rowCount(); ++row)
        if (m_productProxy->index(row, 0).data(Qt::UserRole).toString() == m_productId)
            m_productTable->setCurrentIndex(m_productProxy->index(row, 0));
    m_refreshing = false;
    if (m_store->product(m_productId).isEmpty()) m_productId.clear();
    refreshProjectProducts();
    if (!m_productId.isEmpty())
    {
        m_chosenProduct->setText(tr("Selected product: %1").arg(text(m_store->product(m_productId), "title")));
    }
    else if (products.isEmpty())
    {
        m_chosenProduct->setText(tr("Select an AmazonTemplate3 working directory below and click \"Import / refresh\"."));
    }
    else if (!hasAccountRules(profile))
    {
        m_chosenProduct->setText(tr("Showing all %1 imported products. Click \"Account rules…\" to allow brands and categories for this account.")
            .arg(products.size()));
    }
    else
    {
        m_chosenProduct->setText(tr("Showing %1 of %2 imported products allowed for this account. Choose a product to review references.")
            .arg(m_products->rowCount()).arg(products.size()));
    }
}

void PaneResearch::showProduct()
{
    const auto index = m_productTable->currentIndex();
    if (!index.isValid()) return;
    m_productId = index.siblingAtColumn(0).data(Qt::UserRole).toString();
    const auto product = m_store->product(m_productId);
    previewImage(m_productImage, text(product, "localImage"), QSize(350, 350), tr("No local image cached. You can choose a product photo when creating a project."));
    const int photoCount = qMax(1, int(product.value("localImages").toArray().size()));
    const QString resolution = text(product, "imageResolution");
    const QString photoInfo = resolution.isEmpty()
        ? tr("%1 high-res photo(s)").arg(photoCount)
        : tr("%1 high-res photo(s) (%2)").arg(photoCount).arg(resolution);
    m_productDetails->setText(tr("%1\nBrand: %2\nCategories: %3\nMarketplace: %4\n%5 variants · %6\nCatalog snapshot: %7\n%8")
        .arg(text(product, "title"), text(product, "brand"), strings(product.value("categories").toArray()).join(", "),
             text(product, "marketplace"), QString::number(product.value("variants").toArray().size()),
             photoInfo, text(product, "sourceModifiedAt"),
             ResearchStore::productRestriction(product, m_store->profile(accountId()))));
    m_chosenProduct->setText(tr("Selected product: %1").arg(text(product, "title")));
    if (m_projectProductCombo)
    {
        const int idx = m_projectProductCombo->findData(m_productId);
        if (idx >= 0)
        {
            const QSignalBlocker block(m_projectProductCombo);
            m_projectProductCombo->setCurrentIndex(idx);
        }
    }
    updateCreateProjectButton();
}

void PaneResearch::refreshReferences()
{
    const int targetScrollY = (m_pendingScrollRestoreY >= 0)
        ? m_pendingScrollRestoreY
        : ((m_cardsScroll && m_cardsScroll->verticalScrollBar())
               ? m_cardsScroll->verticalScrollBar()->value()
               : 0);

    // Move focus onto m_cardsScroll (outside m_cards) BEFORE hiding/deleting cards:
    // QWidget::hide() checks window()->focusWidget(), and if focus is inside m_cards,
    // focusNextPrevChild hops across cards and triggers QScrollArea::ensureWidgetVisible!
    QWidget *fw = window() ? window()->focusWidget() : QApplication::focusWidget();
    if (fw && m_cards && m_cards->isAncestorOf(fw) && m_cardsScroll)
    {
        m_cardsScroll->setFocus(Qt::OtherFocusReason);
    }

    const int prevCardsH = m_cards ? m_cards->height() : 0;
    if (m_cards && prevCardsH > 0 && targetScrollY > 0)
    {
        m_cards->setMinimumHeight(prevCardsH);
    }

    QStringList selected;
    for (auto it = m_cardChecks.cbegin(); it != m_cardChecks.cend(); ++it)
        if (it.value()->isChecked()) selected << it.key();
    m_cardChecks.clear();
    m_displayedRefIds.clear();
    while (auto *item = m_cardsLayout->takeAt(0))
    {
        if (item->widget()) { item->widget()->hide(); item->widget()->deleteLater(); }
        delete item;
    }

    const QString curAccount = accountId();
    const QString filter = m_activeFilter;
    const QString search = m_referenceSearch->text().trimmed();
    const QString sortMode = m_sortReferences ? m_sortReferences->currentData().toString() : QStringLiteral("stats");

    int totalForAccount = 0;
    int pendingCount = 0;
    int approvedCount = 0;
    int rejectedCount = 0;
    int autoDiscardedCount = 0;
    QList<QJsonObject> filteredRefs;

    for (const auto &v : m_store->references())
    {
        const auto r = v.toObject();
        if (text(r, "accountId") != curAccount)
        {
            continue;
        }
        ++totalForAccount;
        const QString st = text(r, "status");
        if (st == QLatin1String("approved")) ++approvedCount;
        else if (st == QLatin1String("rejected")) ++rejectedCount;
        else if (st == QLatin1String("auto_discarded")) ++autoDiscardedCount;
        else ++pendingCount;

        if (!filter.isEmpty() && st != filter) continue;
        const QString statsText = formatReferenceStatsLine(r);
        if (!search.isEmpty()
            && !(text(r, "title") + " " + text(r, "url") + " " + text(r, "notes") + " " + statsText)
                   .contains(search, Qt::CaseInsensitive))
        {
            continue;
        }
        filteredRefs.append(r);
    }

    if (auto *b = m_filterButtons.value(QStringLiteral("pending"))) b->setText(tr("Pending (%1)").arg(pendingCount));
    if (auto *b = m_filterButtons.value(QStringLiteral("approved"))) b->setText(tr("Approved (%1)").arg(approvedCount));
    if (auto *b = m_filterButtons.value(QStringLiteral("rejected"))) b->setText(tr("Rejected (%1)").arg(rejectedCount));
    if (auto *b = m_filterButtons.value(QStringLiteral("auto_discarded"))) b->setText(tr("Auto-discarded (%1)").arg(autoDiscardedCount));
    if (auto *b = m_filterButtons.value(QString{})) b->setText(tr("All (%1)").arg(totalForAccount));

    if (sortMode == QLatin1String("newest"))
    {
        std::reverse(filteredRefs.begin(), filteredRefs.end());
    }
    else if (sortMode == QLatin1String("velocity"))
    {
        std::stable_sort(filteredRefs.begin(), filteredRefs.end(),
                         [](const QJsonObject &a, const QJsonObject &b) {
                             const double va = referenceDailyVelocity(a);
                             const double vb = referenceDailyVelocity(b);
                             if (va != vb)
                             {
                                 return va > vb;
                             }
                             return referenceEngagementScore(a) > referenceEngagementScore(b);
                         });
    }
    else if (sortMode == QLatin1String("published"))
    {
        std::stable_sort(filteredRefs.begin(), filteredRefs.end(),
                         [](const QJsonObject &a, const QJsonObject &b) {
                             const QString da = a.value(QStringLiteral("stats")).toObject().value(QStringLiteral("publishedAt")).toString();
                             const QString db = b.value(QStringLiteral("stats")).toObject().value(QStringLiteral("publishedAt")).toString();
                             if (da != db)
                             {
                                 return da > db;
                             }
                             return referenceEngagementScore(a) > referenceEngagementScore(b);
                         });
    }
    else
    {
        std::stable_sort(filteredRefs.begin(), filteredRefs.end(),
                         [](const QJsonObject &a, const QJsonObject &b) {
                             return referenceEngagementScore(a) > referenceEngagementScore(b);
                         });
    }

    if (m_metricsSummary)
    {
        QStringList summaryLines;
        const QJsonObject spy = m_store->lastSpySession(curAccount);
        const int browsed = spy.value(QStringLiteral("browsedCount")).toInt(0);
        const int saved = spy.value(QStringLiteral("savedCount")).toInt(0);
        const int autoRejected = spy.value(QStringLiteral("rejectedCount")).toInt(0);
        const int minEng = spy.value(QStringLiteral("minEngagement")).toInt(0);
        const int maxAge = spy.value(QStringLiteral("maxDaysOld")).toInt(0);
        if (browsed > 0)
        {
            const double savePct = 100.0 * double(saved) / double(browsed);
            const double rejectPct = 100.0 * double(autoRejected) / double(browsed);
            const QString plat = text(spy, "platform");
            const QString q = text(spy, "query");
            QStringList activeFilters;
            if (minEng > 0) activeFilters << tr("min score ≥ %1").arg(minEng);
            if (maxAge > 0) activeFilters << tr("max age ≤ %1d").arg(maxAge);
            const QString filterSuffix = activeFilters.isEmpty()
                ? QString{}
                : QStringLiteral(", ") + activeFilters.join(QStringLiteral(", "));
            summaryLines << tr("Last Scroll & save (%1 · \"%2\"): Browsed %3 · Saved %4 (%5%) · Rejected %6 (%7% reject rate%8)")
                .arg(plat.isEmpty() ? tr("browser") : plat,
                     q,
                     QString::number(browsed),
                     QString::number(saved),
                     QString::number(savePct, 'f', 1),
                     QString::number(autoRejected),
                     QString::number(rejectPct, 'f', 1),
                     filterSuffix);
        }

        const int humanDecided = approvedCount + rejectedCount;
        const QString autoDiscPart = autoDiscardedCount > 0
            ? tr(" · %1 auto-discarded (≥4 flops, ≥2 products)").arg(autoDiscardedCount)
            : QString{};
        if (humanDecided > 0)
        {
            const double humanRejectPct = 100.0 * double(rejectedCount) / double(humanDecided);
            summaryLines << tr("Review queue: %1 total — %2 pending · %3 approved · %4 rejected (%5% human reject rate)%6")
                .arg(totalForAccount)
                .arg(pendingCount)
                .arg(approvedCount)
                .arg(rejectedCount)
                .arg(QString::number(humanRejectPct, 'f', 1),
                     autoDiscPart);
        }
        else if (totalForAccount > 0)
        {
            summaryLines << tr("Review queue: %1 total — %2 pending · 0 approved · 0 rejected%3")
                .arg(totalForAccount)
                .arg(pendingCount)
                .arg(autoDiscPart);
        }

        m_metricsSummary->setVisible(!summaryLines.isEmpty());
        m_metricsSummary->setText(summaryLines.join(QLatin1Char('\n')));
    }

    int count = 0;
    for (const auto &r : filteredRefs)
    {
        if (++count > m_visibleCards) continue;
        const QString id = text(r, "id");
        const QString st = text(r, "status");
        auto *card = new QFrame;
        card->setObjectName("referenceCard_" + id);
        card->setProperty("referenceId", id);
        card->setCursor(Qt::PointingHandCursor);
        card->installEventFilter(this);
        card->setFrameShape(QFrame::StyledPanel);
        if (id == m_referenceId)
        {
            card->setStyleSheet(QStringLiteral(
                "QFrame#referenceCard_%1 { border: 2px solid #58a6ff; background: rgba(88, 166, 255, 0.10); border-radius: 5px; }").arg(id));
        }
        card->setMinimumWidth(230);
        auto *layout = new QVBoxLayout(card);
        auto *check = new QCheckBox(cleanDisplayTitle(r));
        check->setChecked(selected.contains(id));
        m_cardChecks.insert(id, check);
        connect(check, &QCheckBox::toggled, this, [this, id](bool checked) {
            if (checked && m_referenceId != id)
            {
                QTimer::singleShot(0, this, [this, id] {
                    if (m_referenceId != id) selectReference(id);
                });
            }
            else
            {
                updateCreateProjectButton();
            }
        });
        layout->addWidget(check);

        auto *image = new QLabel;
        image->setProperty("referenceId", id);
        image->installEventFilter(this);
        image->setAlignment(Qt::AlignCenter);
        image->setFixedHeight(170);
        image->setWordWrap(true);
        const QString path = m_store->mediaPath(r);
        const QString cardFallback = path.isEmpty()
            ? tr("URL reference\nClick card & open in Chrome")
            : (isVideoMediaPath(path)
                   ? tr("Video reference\nClick card to watch")
                   : tr("Image preview unavailable\nOpen in Chrome"));
        previewImage(image, path, QSize(280, 170), cardFallback);
        layout->addWidget(image);

        const QString statsHtml = formatReferenceStatsRichHtml(r);
        auto *statsLabel = new QLabel(statsHtml.isEmpty() ? tr("No stats recorded").toHtmlEscaped() : statsHtml);
        statsLabel->setObjectName("referenceStats_" + id);
        statsLabel->setProperty("referenceId", id);
        statsLabel->installEventFilter(this);
        statsLabel->setTextFormat(Qt::RichText);
        statsLabel->setWordWrap(true);
        statsLabel->setStyleSheet(statsHtml.isEmpty()
            ? QStringLiteral("color: rgba(180, 190, 205, 0.65); font-size: 12px;")
            : QStringLiteral("color: #8fd19e; font-size: 12px;"));
        layout->addWidget(statsLabel);

        const int usedCount = r.value(QStringLiteral("usedProducts")).toArray().size();
        QString statusLine = text(r, "platform");
        if (isVideoMediaPath(path))
        {
            statusLine += QStringLiteral(" · video");
        }
        const QString statusLabelText = (st == QLatin1String("auto_discarded"))
            ? tr("⚡ auto-discarded (≥4 flops, ≥2 products)")
            : st;
        statusLine += QStringLiteral(" · ") + statusLabelText;
        if (usedCount > 0)
        {
            statusLine += tr(" · Used for %1 product(s)").arg(usedCount);
        }
        if (id == m_referenceId)
        {
            statusLine += tr(" · Selected");
        }
        auto *caption = new QLabel(statusLine);
        caption->setProperty("referenceId", id);
        caption->installEventFilter(this);
        caption->setTextFormat(Qt::PlainText);
        caption->setWordWrap(true);
        if (st == QLatin1String("auto_discarded"))
        {
            caption->setStyleSheet(QStringLiteral("color: #f59e0b; font-weight: 600;"));
        }
        layout->addWidget(caption);

        auto *actions = new QHBoxLayout;
        layout->addLayout(actions);
        auto *approve = button(actions, st == QLatin1String("approved") ? tr("✓ Approved") : tr("✓ Approve"));
        approve->setObjectName("approveReference_" + id);
        if (st == QLatin1String("approved"))
        {
            approve->setStyleSheet(QStringLiteral("color: #8fd19e; font-weight: 600;"));
        }
        connect(approve, &QPushButton::clicked, this, [this, id] {
            reviewIds({id}, "approved");
        });
        auto *reject = button(actions, st == QLatin1String("rejected") ? tr("✕ Rejected") : tr("✕ Reject"));
        reject->setObjectName("rejectReference_" + id);
        if (st == QLatin1String("rejected"))
        {
            reject->setStyleSheet(QStringLiteral("color: #e89774; font-weight: 600;"));
        }
        connect(reject, &QPushButton::clicked, this, [this, id] {
            reviewIds({id}, "rejected");
        });
        m_displayedRefIds.append(id);
        m_cardsLayout->addWidget(card, (count - 1) / 2, (count - 1) % 2);
    }
    if (count == 0)
    {
        auto *empty = new QLabel(tr("No references in this view. Click \"Scroll & save…\" to browse & capture posts in Chrome, or add post URLs / local media."));
        empty->setWordWrap(true);
        m_cardsLayout->addWidget(empty, 0, 0, 1, 2);
    }
    else if (count > m_visibleCards)
    {
        auto *more = new QPushButton(tr("Show more (%1 remaining)").arg(count - m_visibleCards));
        m_cardsLayout->addWidget(more, (m_visibleCards + 1) / 2, 0, 1, 2);
        connect(more, &QPushButton::clicked, this, [this] { m_visibleCards += 24; refreshReferences(); });
    }

    m_pendingScrollRestoreY = targetScrollY;
    if (m_cards)
    {
        m_cards->setMinimumHeight(0);
        m_cardsLayout->activate();
    }
    if (m_cardsScroll && m_cardsScroll->verticalScrollBar())
    {
        auto *vbar = m_cardsScroll->verticalScrollBar();
        if (vbar->maximum() > 0 || targetScrollY == 0)
        {
            vbar->setValue(qMin(targetScrollY, vbar->maximum()));
        }
        QTimer::singleShot(30, this, [this]() {
            if (m_pendingScrollRestoreY >= 0 && m_cardsScroll && m_cardsScroll->verticalScrollBar())
            {
                auto *vb = m_cardsScroll->verticalScrollBar();
                vb->setValue(qMin(m_pendingScrollRestoreY, vb->maximum()));
                m_pendingScrollRestoreY = -1;
            }
        });
    }
    // Refresh evidence without replacing unsaved text or restarting playback.
    const auto current = m_store->reference(m_referenceId);
    if (current.isEmpty() || text(current, "accountId") != curAccount) { m_referenceId.clear(); showReference(); }
    else
    {
        const QString statsSummary = formatReferenceStatsLine(current);
        const QString pubDate = current.value(QStringLiteral("stats")).toObject().value(QStringLiteral("publishedAt")).toString();
        const qint64 ageDays = referenceAgeDays(current);
        const QString createdDisplay = pubDate.isEmpty()
            ? tr("—")
            : (ageDays >= 0 ? tr("%1 (%2 days old)").arg(pubDate.left(10)).arg(ageDays) : pubDate);
        QStringList usedNames;
        for (const auto &pidVal : current.value(QStringLiteral("usedProducts")).toArray())
        {
            const auto up = m_store->product(pidVal.toString());
            if (!up.isEmpty()) usedNames << (text(up, "color") + QStringLiteral(" — ") + text(up, "title")).left(60);
        }
        const QString curStatusStr = text(current, "status");
        const QString statusExplain = (curStatusStr == QLatin1String("auto_discarded"))
            ? tr("auto_discarded (Automatically discarded after ≥4 low-performing posts across ≥2 products)")
            : ((curStatusStr == QLatin1String("rejected"))
                   ? tr("rejected (Rejected by human)")
                   : curStatusStr);
        QString evidence = tr("Status: %1\nSource: %2\nCreated: %3\nStatistics: %4\nProjects created for: %5\nReview: %6")
            .arg(statusExplain,
                 text(current, "url"),
                 createdDisplay,
                 statsSummary.isEmpty() ? tr("—") : statsSummary,
                 usedNames.isEmpty() ? tr("None yet (reusable with any allowed product)") : usedNames.join(QStringLiteral("; ")),
                 text(current, "reviewReason"));
        for (const auto &v : current.value("assessments").toArray())
        {
            const auto a = v.toObject();
            evidence += "\n\n" + text(a, "method") + " · " + text(a, "verdict") + " · " + text(a, "at") + "\n" + text(a, "reason");
        }
        for (const auto &v : current.value("history").toArray())
        {
            const auto h = v.toObject();
            evidence += "\n\n" + text(h, "actor") + " · " + text(h, "status") + " · " + text(h, "at") + "\n" + text(h, "reason");
        }
        m_evidence->setPlainText(evidence);
    }

    updateCreateProjectButton();
    backfillMissingPinterestMetadata();
}

void PaneResearch::updateCreateProjectButton()
{
    if (!m_createProjectBtn)
    {
        return;
    }
    const QString refId = effectiveReferenceId();
    const auto ref = m_store->reference(refId);
    const QString chosenProdId = m_projectProductCombo ? m_projectProductCombo->currentData().toString() : m_productId;

    if (ref.isEmpty())
    {
        m_createProjectBtn->setEnabled(false);
        m_createProjectBtn->setText(tr("Click a reference card on the left to create a project"));
        return;
    }
    if (chosenProdId.isEmpty())
    {
        m_createProjectBtn->setEnabled(true);
        m_createProjectBtn->setText(tr("Create generation project (configure product first…)"));
        return;
    }
    const QString st = text(ref, "status");
    m_createProjectBtn->setEnabled(true);
    if (st == QLatin1String("approved"))
    {
        m_createProjectBtn->setText(tr("Create generation project"));
    }
    else
    {
        m_createProjectBtn->setText(tr("Approve & create generation project"));
    }
}

void PaneResearch::backfillMissingPinterestMetadata()
{
    if (m_backfilling || qEnvironmentVariableIntValue("QS_SPY_HEADLESS") == 1)
    {
        return;
    }
    const QString curAccount = accountId();
    if (curAccount.isEmpty())
    {
        return;
    }

    QMap<QString, QString> urlToRefId;
    for (const auto &v : m_store->references())
    {
        const auto r = v.toObject();
        if (text(r, "accountId") != curAccount || text(r, "platform") != QLatin1String("pinterest"))
        {
            continue;
        }
        const QString id = text(r, "id");
        if (m_backfillAttemptedIds.contains(id))
        {
            continue;
        }
        const QString url = text(r, "url");
        const QJsonObject st = r.value(QStringLiteral("stats")).toObject();
        if (url.contains(QStringLiteral("/pin/")) && st.value(QStringLiteral("publishedAt")).toString().isEmpty())
        {
            m_backfillAttemptedIds.insert(id);
            urlToRefId.insert(url, id);
            if (urlToRefId.size() >= 30)
            {
                break;
            }
        }
    }
    if (urlToRefId.isEmpty())
    {
        return;
    }

    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    const QString scriptPath = QDir(QString::fromUtf8(SOCIAL_STATS_SCRIPT_DIR))
        .filePath(QStringLiteral("spy_scroll_pinterest.py"));
    if (python.isEmpty() || !QFileInfo::exists(scriptPath))
    {
        return;
    }

    m_backfilling = true;
    auto *proc = new QProcess(this);
    QStringList args{scriptPath, QStringLiteral("--backfill")};
    args << urlToRefId.keys();

    connect(proc, &QProcess::finished, this, [this, proc, urlToRefId](int, QProcess::ExitStatus) {
        m_backfilling = false;
        const QByteArray out = proc->readAllStandardOutput().trimmed();
        proc->deleteLater();
        const QJsonObject items = QJsonDocument::fromJson(out).object().value(QStringLiteral("items")).toObject();
        if (items.isEmpty())
        {
            return;
        }
        bool anyUpdated = false;
        m_refreshing = true;
        for (auto it = urlToRefId.cbegin(); it != urlToRefId.cend(); ++it)
        {
            const QJsonObject meta = items.value(it.key()).toObject();
            if (meta.isEmpty())
            {
                continue;
            }
            const QJsonObject existing = m_store->reference(it.value());
            if (existing.isEmpty())
            {
                continue;
            }
            QJsonObject stats = existing.value(QStringLiteral("stats")).toObject();
            const QString pub = meta.value(QStringLiteral("published_at")).toString();
            if (!pub.isEmpty()) stats.insert(QStringLiteral("publishedAt"), pub);
            if (meta.contains(QStringLiteral("shares")) && !stats.contains(QStringLiteral("saves")))
            {
                const qint64 sv = static_cast<qint64>(meta.value(QStringLiteral("shares")).toDouble(-1));
                if (sv >= 0)
                {
                    stats.insert(QStringLiteral("shares"), sv);
                    stats.insert(QStringLiteral("saves"), sv);
                }
            }
            if (meta.contains(QStringLiteral("likes")) && !stats.contains(QStringLiteral("likes")))
            {
                const qint64 lk = static_cast<qint64>(meta.value(QStringLiteral("likes")).toDouble(-1));
                if (lk >= 0) stats.insert(QStringLiteral("likes"), lk);
            }
            const QString auth = meta.value(QStringLiteral("author")).toString();
            if (!auth.isEmpty() && !stats.contains(QStringLiteral("author")))
            {
                stats.insert(QStringLiteral("author"), auth);
            }
            if (meta.value(QStringLiteral("is_ad")).toBool(false))
            {
                stats.insert(QStringLiteral("isAd"), true);
            }
            if (stats != existing.value(QStringLiteral("stats")).toObject())
            {
                if (m_store->updateReference(it.value(), QJsonObject{{QStringLiteral("stats"), stats}}))
                {
                    anyUpdated = true;
                }
            }
        }
        m_refreshing = false;
        if (anyUpdated)
        {
            refreshReferences();
            // Run again if more than 30 pins needed backfill
            QTimer::singleShot(100, this, &PaneResearch::backfillMissingPinterestMetadata);
        }
    });
    proc->start(python, args);
}

QStringList PaneResearch::selectedReferences() const
{
    QStringList ids;
    for (auto it = m_cardChecks.cbegin(); it != m_cardChecks.cend(); ++it) if (it.value()->isChecked()) ids << it.key();
    if (ids.isEmpty() && !m_referenceId.isEmpty() && m_cardChecks.contains(m_referenceId)) ids << m_referenceId;
    return ids;
}

void PaneResearch::selectReference(const QString &id)
{
    if (!saveReference()) return;
    m_referenceId = id;
    showReference();
    refreshReferences();
}

void PaneResearch::showReference()
{
    const auto r = m_store->reference(m_referenceId);
    const QSignalBlocker blockTitle(m_title);
    const QSignalBlocker blockNotes(m_notes);
    m_title->setText(text(r, "title"));
    m_notes->setPlainText(text(r, "notes"));
    m_title->setEnabled(!r.isEmpty()); m_notes->setEnabled(!r.isEmpty());
    m_player->stop();
    const QString path = m_store->mediaPath(r);
    const bool isVideo = !path.isEmpty() && isVideoMediaPath(path);
    if (m_playPauseBtn)
    {
        m_playPauseBtn->setVisible(isVideo);
    }
    if (isVideo)
    {
        m_preview->setCurrentIndex(1);
        m_player->setSource(QUrl::fromLocalFile(path));
    }
    else
    {
        m_player->setSource({});
        m_preview->setCurrentIndex(0);
        previewImage(m_referenceImage, path, QSize(420, 330),
                     r.isEmpty()
                         ? tr("Click any reference card on the left to preview it here.")
                         : tr("Open in Chrome to view this post."));
    }
    if (r.isEmpty()) m_evidence->clear();
    updateCreateProjectButton();
    updateStarAuthorButton();
}

void PaneResearch::updateStarAuthorButton()
{
    if (!m_starAuthorBtn)
    {
        return;
    }
    const auto r = m_store->reference(effectiveReferenceId());
    if (r.isEmpty())
    {
        m_starAuthorBtn->setVisible(false);
        return;
    }
    const QString plat = text(r, "platform").trimmed().toLower();
    if (!QStringList{"pinterest", "tiktok", "instagram", "youtube"}.contains(plat))
    {
        m_starAuthorBtn->setVisible(false);
        return;
    }
    QString handle = ResearchStore::normalizeInfluencerHandle(
        r.value(QStringLiteral("stats")).toObject().value(QStringLiteral("author")).toString(),
        plat);
    if (handle.isEmpty())
    {
        handle = ResearchStore::normalizeInfluencerHandle(text(r, "url"), plat);
    }
    if (handle.isEmpty())
    {
        m_starAuthorBtn->setVisible(false);
        return;
    }

    bool alreadySaved = false;
    for (const auto &v : m_store->influencers(accountId(), plat))
    {
        if (text(v.toObject(), "handle").compare(handle, Qt::CaseInsensitive) == 0)
        {
            alreadySaved = true;
            break;
        }
    }
    m_starAuthorBtn->setVisible(true);
    m_starAuthorBtn->setText(alreadySaved ? tr("★ %1").arg(handle) : tr("☆ Star %1").arg(handle));
    m_starAuthorBtn->setToolTip(alreadySaved
        ? tr("%1 is in your Favorite influencers for %2.").arg(handle, plat)
        : tr("Save %1 as a Favorite influencer for %2.").arg(handle, plat));
}

void PaneResearch::starCurrentReferenceAuthor()
{
    const auto r = m_store->reference(effectiveReferenceId());
    if (r.isEmpty())
    {
        return;
    }
    const QString plat = text(r, "platform").trimmed().toLower();
    QString handle = ResearchStore::normalizeInfluencerHandle(
        r.value(QStringLiteral("stats")).toObject().value(QStringLiteral("author")).toString(),
        plat);
    if (handle.isEmpty())
    {
        handle = ResearchStore::normalizeInfluencerHandle(text(r, "url"), plat);
    }
    if (handle.isEmpty())
    {
        return;
    }
    const QString id = m_store->addInfluencer(accountId(), plat, handle);
    updateStarAuthorButton();
    report(!id.isEmpty(), tr("Saved %1 to Favorite influencers (%2).").arg(handle, plat));
}

void PaneResearch::editProfile()
{
    const QString id = accountId();
    if (id.isEmpty()) { report(false, tr("Add a social account in Accounts first.")); return; }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Account product rules"));
    dialog.resize(750, 650);
    auto *layout = new QVBoxLayout(&dialog);
    auto *help = new QLabel(tr("Only checked brands and categories are eligible. Empty selections allow no products. Changes apply to this account only."));
    help->setWordWrap(true); layout->addWidget(help);
    auto *lists = new QHBoxLayout; layout->addLayout(lists, 1);
    auto *brands = new QListWidget;
    auto *categories = new QListWidget;
    const auto profile = m_store->profile(id);
    QStringList brandNames = strings(profile.value("brands").toArray());
    QStringList categoryNames = strings(profile.value("categories").toArray());
    for (const auto &v : m_store->products())
    {
        brandNames << text(v.toObject(), "brand");
        categoryNames << strings(v.toObject().value("categories").toArray());
    }
    const auto fill = [&](QListWidget *list, QStringList names, const QJsonArray &checked, const QString &label) {
        auto *column = new QVBoxLayout; lists->addLayout(column);
        column->addWidget(new QLabel(label));
        auto *search = new QLineEdit; search->setPlaceholderText(tr("Filter…")); column->addWidget(search);
        column->addWidget(list, 1);
        names.removeAll(""); names.removeDuplicates(); names.sort(Qt::CaseInsensitive);
        for (const auto &name : names)
        {
            auto *item = new QListWidgetItem(name, list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(strings(checked).contains(name, Qt::CaseInsensitive) ? Qt::Checked : Qt::Unchecked);
        }
        connect(search, &QLineEdit::textChanged, list, [list](const QString &query) {
            for (int i = 0; i < list->count(); ++i) list->item(i)->setHidden(!list->item(i)->text().contains(query, Qt::CaseInsensitive));
        });
    };
    fill(brands, brandNames, profile.value("brands").toArray(), tr("Allowed brands"));
    fill(categories, categoryNames, profile.value("categories").toArray(), tr("Allowed categories"));
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *audience = new QLineEdit(text(profile, "audience")); form->addRow(tr("Audience / account theme"), audience);
    auto *language = new QLineEdit(text(profile, "language")); form->addRow(tr("Language"), language);
    auto *prompt = new QPlainTextEdit(profile.value("assessmentPrompt").toString(ReferenceAssessor::defaultPrompt()));
    prompt->setMaximumHeight(125); form->addRow(tr("CLI assessment criteria"), prompt);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto checked = [](QListWidget *list) {
        QJsonArray values;
        for (int i = 0; i < list->count(); ++i) if (list->item(i)->checkState() == Qt::Checked) values.append(list->item(i)->text());
        return values;
    };
    auto updated = profile;
    updated.insert("brands", checked(brands)); updated.insert("categories", checked(categories));
    updated.insert("audience", audience->text().trimmed()); updated.insert("language", language->text().trimmed());
    updated.insert("assessmentPrompt", prompt->toPlainText());
    if (hasAccountRules(updated) && !m_onlyAllowed->isChecked())
    {
        const QSignalBlocker blockAllowed(m_onlyAllowed);
        m_onlyAllowed->setChecked(true);
    }
    report(m_store->setProfile(id, updated), tr("Account rules saved."));
}

bool PaneResearch::hasConfiguredProductDirectory() const
{
    return m_directory && !m_directory->text().trimmed().isEmpty();
}

void PaneResearch::_showNewlyImportedProductsDialog(const QList<QJsonObject> &newProducts, int totalCatalogCount)
{
    if (newProducts.isEmpty())
    {
        return;
    }

    const QJsonObject profile = m_store ? m_store->profile(accountId()) : QJsonObject{};
    const bool rulesConfigured = hasAccountRules(profile);
    QStringList allowedNewIds;
    for (const QJsonObject &p : newProducts)
    {
        const QString pid = text(p, "id");
        if (pid.isEmpty())
        {
            continue;
        }
        if (!rulesConfigured || ResearchStore::productRestriction(p, profile).isEmpty())
        {
            allowedNewIds << pid;
        }
    }

    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("dialogNewlyImportedProducts"));
    dlg.setWindowTitle(tr("New Products Imported (%1)").arg(newProducts.size()));
    dlg.resize(880, 620);

    auto *rootLayout = new QVBoxLayout(&dlg);
    rootLayout->setContentsMargins(14, 14, 14, 14);
    rootLayout->setSpacing(10);

    auto *summaryLabel = new QLabel(&dlg);
    summaryLabel->setObjectName(QStringLiteral("labelNewProductsSummary"));
    summaryLabel->setWordWrap(true);
    summaryLabel->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600; color: #8fd19e;"));
    summaryLabel->setText(rulesConfigured
        ? tr("✨ Found %1 new A+ product/color group(s) (%2 allowed for this account · %3 total in catalog).")
              .arg(newProducts.size()).arg(allowedNewIds.size()).arg(totalCatalogCount)
        : tr("✨ Found %1 new A+ product/color group(s) (%2 total in catalog).")
              .arg(newProducts.size()).arg(totalCatalogCount));
    rootLayout->addWidget(summaryLabel);

    auto *scroll = new QScrollArea(&dlg);
    scroll->setObjectName(QStringLiteral("scrollNewProducts"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *gridHost = new QWidget(scroll);
    gridHost->setObjectName(QStringLiteral("gridNewProducts"));
    auto *grid = new QGridLayout(gridHost);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(10);
    const int kColumns = 3;
    for (int c = 0; c < kColumns; ++c)
    {
        grid->setColumnStretch(c, 1);
    }

    const int maxVisualCards = 120;
    const int shownCount = qMin(int(newProducts.size()), maxVisualCards);
    for (int i = 0; i < shownCount; ++i)
    {
        const QJsonObject &p = newProducts.at(i);
        const QString pid = text(p, "id");
        const QString title = text(p, "title");
        const QString color = text(p, "color");
        const QString brand = text(p, "brand");
        const QString asin = text(p, "asin");
        const QString imgPath = text(p, "localImage");
        const QStringList cats = strings(p.value(QStringLiteral("categories")).toArray());
        const int variantCount = qMax(1, int(p.value(QStringLiteral("variants")).toArray().size()));
        const QString restriction = rulesConfigured ? ResearchStore::productRestriction(p, profile) : QString{};
        const bool isAllowed = restriction.isEmpty();

        auto *card = new QFrame(gridHost);
        card->setObjectName(QStringLiteral("newProductCard_") + pid);
        card->setFrameShape(QFrame::StyledPanel);
        card->setStyleSheet(isAllowed
            ? QStringLiteral("QFrame { background: rgba(30, 38, 54, 0.72); border: 1px solid rgba(74, 222, 128, 0.42); border-radius: 8px; }")
            : QStringLiteral("QFrame { background: rgba(30, 34, 44, 0.55); border: 1px solid rgba(148, 163, 184, 0.28); border-radius: 8px; }"));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(8, 8, 8, 8);
        cardLayout->setSpacing(5);

        auto *imgLbl = new QLabel(card);
        imgLbl->setObjectName(QStringLiteral("newProductImage_") + pid);
        imgLbl->setAlignment(Qt::AlignCenter);
        imgLbl->setMinimumSize(180, 180);
        imgLbl->setMaximumHeight(200);
        imgLbl->setStyleSheet(QStringLiteral("background: #11151d; border: 1px solid #262e40; border-radius: 6px;"));
        previewImage(imgLbl, imgPath, QSize(220, 185), tr("No image"));
        cardLayout->addWidget(imgLbl);

        auto *badgeLbl = new QLabel(card);
        badgeLbl->setWordWrap(true);
        badgeLbl->setStyleSheet(isAllowed
            ? QStringLiteral("border: none; background: transparent; color: #86efac; font-size: 11px; font-weight: 600;")
            : QStringLiteral("border: none; background: transparent; color: #fbbf24; font-size: 11px;"));
        badgeLbl->setText(isAllowed
            ? tr("✓ Allowed for account")
            : tr("⚠ Filtered by account rules"));
        if (!restriction.isEmpty())
        {
            badgeLbl->setToolTip(restriction);
        }
        cardLayout->addWidget(badgeLbl);

        const QString heading = color.isEmpty() ? title : QStringLiteral("%1 — %2").arg(color, title);
        auto *titleLbl = new QLabel(heading, card);
        titleLbl->setWordWrap(true);
        titleLbl->setStyleSheet(QStringLiteral("border: none; background: transparent; font-weight: 700; font-size: 12px;"));
        titleLbl->setToolTip(heading);
        cardLayout->addWidget(titleLbl);

        QStringList metaParts;
        if (!brand.isEmpty()) metaParts << brand;
        if (!cats.isEmpty()) metaParts << cats.join(QStringLiteral(", "));
        if (!asin.isEmpty()) metaParts << asin;
        metaParts << tr("%1 variant(s)").arg(variantCount);
        auto *metaLbl = new QLabel(metaParts.join(QStringLiteral(" · ")), card);
        metaLbl->setWordWrap(true);
        metaLbl->setStyleSheet(QStringLiteral("border: none; background: transparent; color: #9ca3af; font-size: 11px;"));
        cardLayout->addWidget(metaLbl);
        cardLayout->addStretch();

        grid->addWidget(card, i / kColumns, i % kColumns);
    }
    if (newProducts.size() > maxVisualCards)
    {
        auto *moreLbl = new QLabel(
            tr("+ %1 additional new product(s) imported into the catalog.")
                .arg(newProducts.size() - maxVisualCards),
            gridHost);
        moreLbl->setStyleSheet(QStringLiteral("color: #93c5fd; font-weight: 600; padding: 6px;"));
        grid->addWidget(moreLbl, (shownCount + kColumns - 1) / kColumns, 0, 1, kColumns);
    }

    scroll->setWidget(gridHost);
    rootLayout->addWidget(scroll, 1);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    if (!allowedNewIds.isEmpty())
    {
        auto *btnCheckNew = new QPushButton(
            tr("✓ Check New Products (%1)").arg(allowedNewIds.size()), &dlg);
        btnCheckNew->setObjectName(QStringLiteral("buttonCheckNewProducts"));
        btnCheckNew->setMinimumHeight(34);
        btnCheckNew->setStyleSheet(QStringLiteral(
            "QPushButton { background: #16a34a; color: white; font-weight: 700; padding: 6px 14px; border-radius: 6px; }"));
        connect(btnCheckNew, &QPushButton::clicked, &dlg, [this, &dlg, allowedNewIds]() {
            for (const QString &pid : allowedNewIds)
            {
                m_checkedProductIds.insert(pid);
            }
            m_lastProducts = QJsonArray{};
            refreshProducts();
            emit newlyImportedProductsChecked(allowedNewIds);
            dlg.accept();
        });
        btnRow->addWidget(btnCheckNew);
    }
    auto *btnClose = new QPushButton(tr("Close"), &dlg);
    btnClose->setObjectName(QStringLiteral("buttonCloseNewProducts"));
    btnClose->setMinimumHeight(34);
    btnClose->setDefault(true);
    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnRow->addWidget(btnClose);
    rootLayout->addLayout(btnRow);

    dlg.exec();
}

void PaneResearch::importProducts()
{
    const QString directory = m_directory->text().trimmed();
    const auto *source = ProductSource::all().value("amazon-template3");
    const auto result = source->read(directory);
    if (!result.error.isEmpty())
    {
        report(false, result.error);
        QMessageBox::warning(this, tr("Invalid working directory"), result.error);
        return;
    }

    QSet<QString> existingIds;
    for (const QJsonValue &v : m_store->products())
    {
        const QString pid = text(v.toObject(), "id");
        if (!pid.isEmpty())
        {
            existingIds.insert(pid);
        }
    }

    const QString canonicalDir = QFileInfo(directory).canonicalFilePath();
    if (!m_store->replaceProducts(result.products, canonicalDir))
    {
        report(false);
        QMessageBox::warning(this, tr("Could not save imported products"), m_store->lastError());
        return;
    }
    m_store->directory().mkpath(".");
    QSettings settings(m_store->directory().filePath("ui.ini"), QSettings::IniFormat);
    settings.setValue("productDirectory", canonicalDir);

    QList<QJsonObject> newlyImported;
    for (const QJsonValue &v : result.products)
    {
        const QJsonObject p = v.toObject();
        const QString pid = text(p, "id");
        if (!pid.isEmpty() && !existingIds.contains(pid))
        {
            newlyImported.append(p);
        }
    }

    const auto profile = m_store->profile(accountId());
    if (!hasAccountRules(profile) && m_onlyAllowed->isChecked())
    {
        const QSignalBlocker blockAllowed(m_onlyAllowed);
        m_onlyAllowed->setChecked(false);
    }
    m_lastProducts = QJsonArray{};
    refreshProducts();

    const QString newSummaryTag = newlyImported.isEmpty()
        ? tr("no new products")
        : tr("%1 new").arg(newlyImported.size());

    if (!hasAccountRules(profile))
    {
        report(true, tr("Imported %1 A+ product/color groups (%2 · excluded %3 archived, %4 without A+ / high-res photos). Showing all A+ products — click \"Account rules…\" to allow brands and categories for this account.")
            .arg(result.products.size()).arg(newSummaryTag).arg(result.archivedRecords).arg(result.skippedWithoutAPlus));
    }
    else
    {
        report(true, tr("Imported %1 A+ product/color groups (%2 · excluded %3 archived, %4 without A+ / high-res photos); %5 allowed for this account.")
            .arg(result.products.size()).arg(newSummaryTag).arg(result.archivedRecords).arg(result.skippedWithoutAPlus).arg(m_products->rowCount()));
    }

    if (!newlyImported.isEmpty())
    {
        _showNewlyImportedProductsDialog(newlyImported, result.products.size());
    }
    else if (hasAccountRules(profile) && m_onlyAllowed->isChecked() && m_products->rowCount() == 0 && !result.products.isEmpty())
    {
        QMessageBox::information(this, tr("No matching products for account rules"),
            tr("Imported %1 A+ product/color groups, but none match the current Account rules.\n\nUpdate \"Account rules…\" or uncheck \"Allowed for this account\" to view all imported A+ products.")
                .arg(result.products.size()));
    }
}

QString PaneResearch::defaultSpyQueryForProduct(const QJsonObject &product)
{
    if (product.isEmpty())
    {
        return QString{};
    }

    QString title = text(product, "title").trimmed();
    // Cut Amazon subtitle / spec clauses ("with 7 Cm...", " - ...", "(Cheetah, 37=...)")
    static const QRegularExpression clauseBreakRe(
        QStringLiteral("(\\(|\\[|,|\\s+[-–—|]\\s+|\\s+(?:with|featuring|for|including)\\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch brk = clauseBreakRe.match(title);
    if (brk.hasMatch() && brk.capturedStart() > 0)
    {
        title = title.left(brk.capturedStart()).trimmed();
    }

    // Remove measurements/numbers ("7 cm", "3.5 in") and possessives ("Women's")
    title.remove(QRegularExpression(
        QStringLiteral("\\b\\d+(?:[.,]\\d+)?\\s*(?:cm|mm|in|inch|inches|oz|ml|g|kg|pack|pcs|pair|pairs)?\\b"),
        QRegularExpression::CaseInsensitiveOption));
    title.replace(QRegularExpression(QStringLiteral("['’]s\\b"), QRegularExpression::CaseInsensitiveOption), QString{});
    title.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9\\s-]")), QStringLiteral(" "));

    static const QSet<QString> stopWords{
        QStringLiteral("women"), QStringLiteral("womens"), QStringLiteral("woman"),
        QStringLiteral("men"), QStringLiteral("mens"), QStringLiteral("ladies"),
        QStringLiteral("girls"), QStringLiteral("boys"), QStringLiteral("unisex"),
        QStringLiteral("print"), QStringLiteral("pattern"), QStringLiteral("printed"),
        QStringLiteral("comfortable"), QStringLiteral("comfy"), QStringLiteral("adjustable"),
        QStringLiteral("elasticated"), QStringLiteral("elastic"), QStringLiteral("classic"),
        QStringLiteral("elegant"), QStringLiteral("sexy"), QStringLiteral("casual"),
        QStringLiteral("dressy"), QStringLiteral("stylish"), QStringLiteral("fashion"),
        QStringLiteral("new"), QStringLiteral("pair"), QStringLiteral("pairs"),
        QStringLiteral("set"), QStringLiteral("style"), QStringLiteral("design"),
        QStringLiteral("fit"), QStringLiteral("size"), QStringLiteral("the"),
        QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("and"),
        QStringLiteral("or"), QStringLiteral("of"), QStringLiteral("in"),
        QStringLiteral("on"), QStringLiteral("to"), QStringLiteral("by"),
        QStringLiteral("from")
    };

    QSet<QString> brandTokens;
    for (const QString &bt : text(product, "brand").toLower().split(QRegularExpression(QStringLiteral("\\W+")), Qt::SkipEmptyParts))
    {
        brandTokens.insert(bt);
    }

    QStringList rawWords = title.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QStringList coreWords;
    for (const QString &w : rawWords)
    {
        const QString lw = w.toLower();
        if (lw.size() <= 1 || stopWords.contains(lw))
        {
            continue;
        }
        if (!coreWords.contains(w, Qt::CaseInsensitive))
        {
            coreWords << w;
        }
    }

    // Strip private-label brand words unless doing so leaves zero product nouns
    QStringList withoutBrand;
    for (const QString &w : coreWords)
    {
        if (!brandTokens.contains(w.toLower()))
        {
            withoutBrand << w;
        }
    }
    if (!withoutBrand.isEmpty())
    {
        coreWords = withoutBrand;
    }

    // Clean variant color and avoid redundant animal-print doubling (e.g. "Cheetah" + "Leopard")
    QString rawColor = text(product, "color").trimmed();
    const int colorSep = rawColor.indexOf(QRegularExpression(QStringLiteral("[/(,\\-]")));
    if (colorSep > 0)
    {
        rawColor = rawColor.left(colorSep).trimmed();
    }
    QStringList colorWords;
    for (const QString &cw : rawColor.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
    {
        if (!stopWords.contains(cw.toLower()) && colorWords.size() < 2)
        {
            colorWords << cw;
        }
    }

    static const QSet<QString> animalPatterns{
        QStringLiteral("leopard"), QStringLiteral("cheetah"), QStringLiteral("panther"),
        QStringLiteral("animal"), QStringLiteral("zebra"), QStringLiteral("snake"),
        QStringLiteral("snakeskin"), QStringLiteral("python"), QStringLiteral("croc"),
        QStringLiteral("crocodile")
    };
    bool titleHasAnimalPattern = false;
    for (const QString &w : coreWords)
    {
        if (animalPatterns.contains(w.toLower()))
        {
            titleHasAnimalPattern = true;
            break;
        }
    }
    bool colorIsAnimalPattern = false;
    for (const QString &cw : colorWords)
    {
        if (animalPatterns.contains(cw.toLower()))
        {
            colorIsAnimalPattern = true;
            break;
        }
    }

    QStringList combined;
    if (!colorWords.isEmpty() && !(titleHasAnimalPattern && colorIsAnimalPattern))
    {
        for (const QString &cw : colorWords)
        {
            if (!coreWords.contains(cw, Qt::CaseInsensitive))
            {
                combined << cw;
            }
        }
    }
    combined << coreWords;

    const QStringList cats = strings(product.value("categories").toArray());
    bool isFashion = false;
    for (const QString &cat : cats)
    {
        if (cat.contains(QStringLiteral("SHOE"), Qt::CaseInsensitive)
            || cat.contains(QStringLiteral("HEEL"), Qt::CaseInsensitive)
            || cat.contains(QStringLiteral("BOOT"), Qt::CaseInsensitive)
            || cat.contains(QStringLiteral("DRESS"), Qt::CaseInsensitive)
            || cat.contains(QStringLiteral("SKIRT"), Qt::CaseInsensitive)
            || cat.contains(QStringLiteral("COAT"), Qt::CaseInsensitive)
            || cat.contains(QStringLiteral("PANT"), Qt::CaseInsensitive)
            || cat.contains(QStringLiteral("TOP"), Qt::CaseInsensitive)
            || cat.contains(QStringLiteral("WEAR"), Qt::CaseInsensitive))
        {
            isFashion = true;
            break;
        }
    }

    const bool hasOutfit = combined.contains(QStringLiteral("outfit"), Qt::CaseInsensitive);
    const int maxCoreWords = (isFashion && !hasOutfit) ? 4 : 5;
    if (combined.size() > maxCoreWords)
    {
        // Keep primary color/pattern word(s) at the start + the specific silhouette/noun at the end
        QStringList compact;
        compact << combined.first();
        const int tailCount = maxCoreWords - 1;
        for (int i = combined.size() - tailCount; i < combined.size(); ++i)
        {
            compact << combined.at(i);
        }
        combined = compact;
    }

    if (isFashion && !hasOutfit)
    {
        combined << QStringLiteral("outfit");
    }
    return combined.join(QLatin1Char(' ')).simplified();
}

QString PaneResearch::formatInfluencerOptionLabel(const QJsonObject &influencer)
{
    const QString handle = text(influencer, "handle");
    const QString notes = text(influencer, "notes").trimmed();
    const QString checkedIso = text(influencer, "lastCheckedAt").trimmed();
    const QString pickedIso = text(influencer, "lastPickedAt").trimmed();
    const int pickedCount = influencer.value(QStringLiteral("pickedCount")).toInt(0);

    const auto formatRelativeDate = [](const QString &iso) -> QString {
        if (iso.size() < 10)
        {
            return {};
        }
        const QDate d = QDate::fromString(iso.left(10), Qt::ISODate);
        if (!d.isValid())
        {
            return iso.left(10);
        }
        const qint64 days = qMax<qint64>(0, d.daysTo(QDate::currentDate()));
        if (days == 0)
        {
            return QObject::tr("today (%1)").arg(iso.left(10));
        }
        if (days == 1)
        {
            return QObject::tr("1d ago (%1)").arg(iso.left(10));
        }
        return QObject::tr("%1d ago (%2)").arg(days).arg(iso.left(10));
    };

    QStringList metaParts;
    if (checkedIso.isEmpty() && pickedIso.isEmpty())
    {
        metaParts << QObject::tr("Never checked");
    }
    else
    {
        if (!checkedIso.isEmpty())
        {
            metaParts << QObject::tr("Checked %1").arg(formatRelativeDate(checkedIso));
        }
        if (!pickedIso.isEmpty())
        {
            metaParts << QObject::tr("Last pick %1").arg(formatRelativeDate(pickedIso));
        }
        else
        {
            metaParts << QObject::tr("No picks yet");
        }
    }
    if (pickedCount > 0)
    {
        metaParts << QObject::tr("%1 saved").arg(pickedCount);
    }

    QString label = handle;
    if (!notes.isEmpty())
    {
        label += QStringLiteral(" (%1)").arg(notes.left(32));
    }
    label += QStringLiteral(" — ") + metaParts.join(QStringLiteral(" · "));
    return label;
}

void PaneResearch::scrollAndSave()
{
    if (accountId().isEmpty())
    {
        report(false, tr("Choose an account first."));
        return;
    }
    if (m_spying)
    {
        report(false, tr("A browser Scroll & save session is already running."));
        return;
    }

    QSettings settings(m_store->directory().filePath("ui.ini"), QSettings::IniFormat);

    QDialog dialog(this);
    dialog.setObjectName("dialogScrollAndSave");
    dialog.setWindowTitle(tr("Scroll & save references from browser"));
    dialog.resize(680, 420);
    auto *layout = new QVBoxLayout(&dialog);

    auto *help = new QLabel;
    help->setObjectName("spyDialogHelp");
    help->setWordWrap(true);
    layout->addWidget(help);

    auto *form = new QFormLayout;
    layout->addLayout(form);

    auto *platformCombo = new QComboBox;
    platformCombo->setObjectName("spyPlatformCombo");
    const auto &allPlatforms = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS();
    const QStringList order{"pinterest", "tiktok", "instagram", "youtube"};
    for (const QString &id : order)
    {
        if (auto *platform = allPlatforms.value(id))
        {
            if (platform->supportsSpyScrollAndSave())
            {
                platformCombo->addItem(platform->getName(), platform->getId());
            }
        }
    }
    for (auto *platform : allPlatforms)
    {
        if (platform->supportsSpyScrollAndSave() && platformCombo->findData(platform->getId()) < 0)
        {
            platformCombo->addItem(platform->getName(), platform->getId());
        }
    }
    const QString savedPlatform = settings.value("spyPlatform", "pinterest").toString();
    const int savedPlatformIdx = platformCombo->findData(savedPlatform);
    if (savedPlatformIdx >= 0)
    {
        platformCombo->setCurrentIndex(savedPlatformIdx);
    }
    form->addRow(tr("Social platform:"), platformCombo);

    auto *targetToggleRow = new QHBoxLayout;
    auto *keywordRadio = new QRadioButton(tr("Keyword or URL"));
    keywordRadio->setObjectName("spyTargetKeywordRadio");
    auto *influencerRadio = new QRadioButton(tr("Favorite influencer"));
    influencerRadio->setObjectName("spyTargetInfluencerRadio");
    const bool startWithInfluencer = (settings.value("spyTargetKind", "keyword").toString() == QLatin1String("influencer"));
    keywordRadio->setChecked(!startWithInfluencer);
    influencerRadio->setChecked(startWithInfluencer);
    targetToggleRow->addWidget(keywordRadio);
    targetToggleRow->addWidget(influencerRadio);
    targetToggleRow->addStretch();
    form->addRow(tr("Target:"), targetToggleRow);

    const auto selectedProduct = m_store->product(m_productId);
    QString initialQuery = defaultSpyQueryForProduct(selectedProduct);
    if (initialQuery.isEmpty())
    {
        initialQuery = settings.value("spyLastQuery").toString();
    }

    auto *queryLabel = new QLabel(tr("Search query or URL:"));
    auto *queryEdit = new QLineEdit(initialQuery);
    queryEdit->setObjectName("spyQueryEdit");
    form->addRow(queryLabel, queryEdit);

    auto *influencerLabel = new QLabel(tr("Influencer:"));
    auto *influencerRowWidget = new QWidget;
    influencerRowWidget->setObjectName("spyInfluencerRowWidget");
    auto *influencerRow = new QHBoxLayout(influencerRowWidget);
    influencerRow->setContentsMargins(0, 0, 0, 0);
    influencerRow->setSpacing(6);
    auto *influencerCombo = new QComboBox;
    influencerCombo->setObjectName("spyInfluencerCombo");
    influencerCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *addInfluencerBtn = new QPushButton(tr("+ Add…"));
    addInfluencerBtn->setObjectName("spyAddInfluencerBtn");
    auto *removeInfluencerBtn = new QPushButton(tr("Remove"));
    removeInfluencerBtn->setObjectName("spyRemoveInfluencerBtn");
    influencerRow->addWidget(influencerCombo, 1);
    influencerRow->addWidget(addInfluencerBtn);
    influencerRow->addWidget(removeInfluencerBtn);
    form->addRow(influencerLabel, influencerRowWidget);

    auto *urlPreview = new QLabel;
    urlPreview->setObjectName("spyTargetUrlPreview");
    urlPreview->setTextFormat(Qt::PlainText);
    urlPreview->setWordWrap(true);
    form->addRow(tr("Target page:"), urlPreview);

    auto *modeCombo = new QComboBox;
    modeCombo->setObjectName("spyModeCombo");
    modeCombo->addItem(tr("Auto-scroll in Chrome & save"), "auto");
    modeCombo->addItem(tr("Manual scroll in Chrome (save when window closes)"), "manual");
    modeCombo->addItem(tr("Human pick in Chrome (click posts → Approved)"), "pick");
    const QString savedMode = settings.value("spyMode", "auto").toString();
    const int savedModeIdx = modeCombo->findData(savedMode);
    if (savedModeIdx >= 0)
    {
        modeCombo->setCurrentIndex(savedModeIdx);
    }
    form->addRow(tr("Scroll mode:"), modeCombo);

    auto *filtersBox = new QWidget;
    filtersBox->setObjectName("spyAutoFiltersBox");
    auto *filtersForm = new QFormLayout(filtersBox);
    filtersForm->setContentsMargins(0, 0, 0, 0);

    auto *maxPostsSpin = new QSpinBox;
    maxPostsSpin->setObjectName("spyMaxPostsSpin");
    maxPostsSpin->setRange(1, 200);
    maxPostsSpin->setValue(settings.value("spyMaxPosts", 24).toInt());
    filtersForm->addRow(tr("Max posts to save:"), maxPostsSpin);

    auto *minEngagementLabel = new QLabel;
    auto *minEngagementSpin = new QSpinBox;
    minEngagementSpin->setObjectName("spyMinEngagementSpin");
    minEngagementSpin->setRange(0, 1'000'000);
    minEngagementSpin->setSpecialValueText(tr("0 (save all — no stats filter)"));
    filtersForm->addRow(minEngagementLabel, minEngagementSpin);

    auto *maxDaysOldSpin = new QSpinBox;
    maxDaysOldSpin->setObjectName("spyMaxDaysOldSpin");
    maxDaysOldSpin->setRange(0, 3650);
    maxDaysOldSpin->setSingleStep(30);
    maxDaysOldSpin->setSuffix(tr(" days"));
    maxDaysOldSpin->setSpecialValueText(tr("0 (any age — no date limit)"));
    maxDaysOldSpin->setValue(settings.value("spyMaxDaysOld", 365).toInt());
    filtersForm->addRow(tr("Max post age (days):"), maxDaysOldSpin);

    layout->addWidget(filtersBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const auto reloadInfluencers = [this, platformCombo, influencerCombo, removeInfluencerBtn](const QString &preferId = {}) {
        const QString pid = platformCombo->currentData().toString();
        const QString prevId = preferId.isEmpty() ? influencerCombo->currentData().toString() : preferId;
        const QSignalBlocker block(influencerCombo);
        influencerCombo->clear();
        const QJsonArray list = m_store->influencers(accountId(), pid);
        for (const auto &v : list)
        {
            const auto inf = v.toObject();
            influencerCombo->addItem(formatInfluencerOptionLabel(inf), text(inf, "id"));
            influencerCombo->setItemData(influencerCombo->count() - 1, text(inf, "url"), Qt::UserRole + 1);
        }
        if (influencerCombo->count() == 0)
        {
            influencerCombo->addItem(tr("No favorite influencers saved for this platform — click \"+ Add…\""), QString{});
            influencerCombo->setEnabled(false);
            removeInfluencerBtn->setEnabled(false);
        }
        else
        {
            influencerCombo->setEnabled(true);
            removeInfluencerBtn->setEnabled(true);
            const int idx = prevId.isEmpty() ? 0 : influencerCombo->findData(prevId);
            influencerCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        }
    };

    const auto updateDialogUi = [&]() {
        const bool useInfluencer = influencerRadio->isChecked();
        const bool isPickMode = (modeCombo->currentData().toString() == QLatin1String("pick"));
        const QString pid = platformCombo->currentData().toString();
        const auto *platform = allPlatforms.value(pid);

        queryLabel->setVisible(!useInfluencer);
        queryEdit->setVisible(!useInfluencer);
        influencerLabel->setVisible(useInfluencer);
        influencerRowWidget->setVisible(useInfluencer);
        filtersBox->setVisible(!isPickMode);

        queryEdit->setPlaceholderText(isPickMode
            ? tr("Optional search keywords or URL (leave empty to browse platform feed)…")
            : tr("Search keywords (e.g. burgundy stiletto heels outfit) or full URL…"));

        help->setText(isPickMode
            ? tr("Opens Chrome with an interactive QuantumSocial overlay: hover any post to see its live stats "
                 "and click \"+ Pick\" (or Alt+Click a card / \"+ Pick open post\" while watching a video). "
                 "Click \"✓ Save & Finish\" (or close Chrome) — picked posts land directly in Approved!")
            : tr("Opens a visible Chrome window on the selected social platform, scrolls to discover "
                 "posts, downloads their media into your reference library, and queues them for review.\n"
                 "If a login screen or CAPTCHA appears, scrolling pauses automatically until you solve it."));

        if (auto *okBtn = buttons->button(QDialogButtonBox::Ok))
        {
            okBtn->setText(isPickMode ? tr("Open Chrome & pick") : tr("Open Chrome & scroll"));
            if (useInfluencer)
            {
                okBtn->setEnabled(!influencerCombo->currentData().toString().isEmpty());
            }
            else
            {
                okBtn->setEnabled(isPickMode || !queryEdit->text().trimmed().isEmpty());
            }
        }

        if (!platform)
        {
            urlPreview->setText(tr("Choose a social platform."));
            return;
        }
        if (useInfluencer)
        {
            const QString infUrl = influencerCombo->currentData(Qt::UserRole + 1).toString();
            urlPreview->setText(infUrl.isEmpty()
                ? tr("Add or select a favorite influencer above.")
                : infUrl);
        }
        else
        {
            const QString q = queryEdit->text().trimmed();
            if (q.isEmpty())
            {
                urlPreview->setText(isPickMode
                    ? platform->defaultSpyHomeUrl()
                    : tr("Enter search keywords or paste a page URL."));
            }
            else if (q.startsWith(QLatin1String("http://"))
                     || q.startsWith(QLatin1String("https://"))
                     || q.startsWith(QLatin1String("file://")))
            {
                urlPreview->setText(q);
            }
            else
            {
                urlPreview->setText(platform->defaultSpySearchUrl(q));
            }
        }
    };

    const auto syncPlatformThreshold = [&]() {
        const QString pid = platformCombo->currentData().toString();
        if (pid == QLatin1String("pinterest"))
        {
            minEngagementLabel->setText(QObject::tr("Min saves + likes per pin:"));
            minEngagementSpin->setSingleStep(5);
        }
        else if (pid == QLatin1String("instagram"))
        {
            minEngagementLabel->setText(QObject::tr("Min likes + comments per post:"));
            minEngagementSpin->setSingleStep(10);
        }
        else
        {
            minEngagementLabel->setText(QObject::tr("Min views per video:"));
            minEngagementSpin->setSingleStep(250);
        }
        const QString key = QStringLiteral("spyMinEngagement_%1").arg(pid);
        minEngagementSpin->setValue(settings.value(key, defaultMinEngagementForPlatform(pid)).toInt());
        reloadInfluencers();
        updateDialogUi();
    };

    connect(addInfluencerBtn, &QPushButton::clicked, &dialog, [&]() {
        QDialog addDlg(&dialog);
        addDlg.setObjectName("dialogAddInfluencer");
        addDlg.setWindowTitle(tr("Add favorite influencer (%1)").arg(platformCombo->currentText()));
        addDlg.resize(460, 160);
        auto *addLayout = new QVBoxLayout(&addDlg);
        auto *addForm = new QFormLayout;
        auto *handleEdit = new QLineEdit;
        handleEdit->setObjectName("addInfluencerInput");
        handleEdit->setPlaceholderText(tr("@handle or https://… profile / board URL"));
        auto *notesEdit = new QLineEdit;
        notesEdit->setObjectName("addInfluencerNotes");
        notesEdit->setPlaceholderText(tr("Optional note (e.g. Luxury heels runway)"));
        addForm->addRow(tr("Handle or URL:"), handleEdit);
        addForm->addRow(tr("Note:"), notesEdit);
        addLayout->addLayout(addForm);
        auto *addBtns = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        addLayout->addWidget(addBtns);
        connect(addBtns, &QDialogButtonBox::accepted, &addDlg, &QDialog::accept);
        connect(addBtns, &QDialogButtonBox::rejected, &addDlg, &QDialog::reject);
        if (addDlg.exec() != QDialog::Accepted)
        {
            return;
        }
        const QString pid = platformCombo->currentData().toString();
        const QString addedId = m_store->addInfluencer(accountId(), pid, handleEdit->text(), notesEdit->text());
        if (addedId.isEmpty())
        {
            QMessageBox::warning(&dialog, tr("Invalid influencer"), m_store->lastError());
            return;
        }
        reloadInfluencers(addedId);
        updateDialogUi();
    });

    connect(removeInfluencerBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString infId = influencerCombo->currentData().toString();
        if (infId.isEmpty())
        {
            return;
        }
        m_store->removeInfluencer(infId);
        reloadInfluencers();
        updateDialogUi();
    });

    connect(platformCombo, &QComboBox::currentIndexChanged, &dialog, syncPlatformThreshold);
    connect(keywordRadio, &QRadioButton::toggled, &dialog, updateDialogUi);
    connect(influencerRadio, &QRadioButton::toggled, &dialog, updateDialogUi);
    connect(influencerCombo, &QComboBox::currentIndexChanged, &dialog, updateDialogUi);
    connect(queryEdit, &QLineEdit::textChanged, &dialog, updateDialogUi);
    connect(modeCombo, &QComboBox::currentIndexChanged, &dialog, updateDialogUi);
    syncPlatformThreshold();

    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const QString platformId = platformCombo->currentData().toString();
    const QString modeKey = modeCombo->currentData().toString();
    const auto spyMode = AbstractSocialAccount::spyModeFromKey(modeKey);
    const bool useInfluencer = influencerRadio->isChecked();

    QString influencerId;
    QString queryOrUrl;
    if (useInfluencer)
    {
        influencerId = influencerCombo->currentData().toString();
        queryOrUrl = influencerCombo->currentData(Qt::UserRole + 1).toString().trimmed();
        if (influencerId.isEmpty() || queryOrUrl.isEmpty())
        {
            report(false, tr("Select or add a favorite influencer first."));
            return;
        }
    }
    else
    {
        queryOrUrl = queryEdit->text().trimmed();
        if (queryOrUrl.isEmpty() && spyMode != AbstractSocialAccount::SpyMode::HumanPick)
        {
            report(false, tr("Enter a search query or URL to scroll."));
            return;
        }
    }

    const int maxPosts = maxPostsSpin->value();
    const int minEngagement = minEngagementSpin->value();
    const int maxDaysOld = maxDaysOldSpin->value();

    settings.setValue("spyPlatform", platformId);
    settings.setValue("spyTargetKind", useInfluencer ? QStringLiteral("influencer") : QStringLiteral("keyword"));
    if (!useInfluencer)
    {
        settings.setValue("spyLastQuery", queryOrUrl);
    }
    settings.setValue("spyMode", modeKey);
    settings.setValue("spyMaxPosts", maxPosts);
    settings.setValue(QStringLiteral("spyMinEngagement_%1").arg(platformId), minEngagement);
    settings.setValue("spyMaxDaysOld", maxDaysOld);

    m_spyTask = runScrollAndSaveMode(
        platformId, queryOrUrl, maxPosts, spyMode, QString{}, minEngagement, maxDaysOld, influencerId);
}

QCoro::Task<void> PaneResearch::runScrollAndSave(
    QString platformId,
    QString queryOrUrl,
    int maxPosts,
    bool autoScroll,
    QString linkProductId,
    int minEngagement,
    int maxDaysOld)
{
    return runScrollAndSaveMode(
        std::move(platformId),
        std::move(queryOrUrl),
        maxPosts,
        autoScroll ? AbstractSocialAccount::SpyMode::AutoScroll : AbstractSocialAccount::SpyMode::ManualScroll,
        std::move(linkProductId),
        minEngagement,
        maxDaysOld,
        QString{});
}

QCoro::Task<void> PaneResearch::runScrollAndSaveMode(
    QString platformId,
    QString queryOrUrl,
    int maxPosts,
    AbstractSocialAccount::SpyMode mode,
    QString linkProductId,
    int minEngagement,
    int maxDaysOld,
    QString influencerId)
{
    const auto *platform = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS().value(platformId);
    if (!platform || !platform->supportsSpyScrollAndSave())
    {
        report(false, tr("Platform %1 does not support Scroll & save.").arg(platformId));
        co_return;
    }
    if (m_spying)
    {
        co_return;
    }

    const bool isHumanPick = (mode == AbstractSocialAccount::SpyMode::HumanPick);
    m_spying = true;
    if (m_scrollAndSave)
    {
        m_scrollAndSave->setEnabled(false);
        m_scrollAndSave->setText(isHumanPick ? tr("Picking in Chrome…") : tr("Scrolling in Chrome…"));
    }

    if (isHumanPick)
    {
        report(true, tr("Opening %1 in Chrome for human picking… "
                        "Click \"+ Pick\" on any inspiring post, then click \"✓ Save & Finish\" (lands directly in Approved).")
            .arg(platform->getName()));
    }
    else
    {
        QStringList filterHints;
        if (minEngagement > 0) filterHints << tr("min stats ≥ %1").arg(minEngagement);
        if (maxDaysOld > 0) filterHints << tr("≤ %1d old").arg(maxDaysOld);
        const QString filterHint = filterHints.isEmpty()
            ? QString{}
            : QStringLiteral(" (%1)").arg(filterHints.join(QStringLiteral(", ")));
        report(true, tr("Opening %1 in Chrome to scroll & save up to %2 posts%3… "
                        "(pauses automatically for login or CAPTCHA; close Chrome anytime to finish and save)")
            .arg(platform->getName())
            .arg(maxPosts)
            .arg(filterHint));
    }

    const QString downloadDir = m_store->directory().filePath(QStringLiteral("spy_downloads"));
    QDir().mkpath(downloadDir);
    const QJsonObject spyContext = m_store->spyContextPayload(accountId(), platformId);

    QPointer<PaneResearch> self(this);
    AbstractSocialAccount::SpyResult result;
    result = co_await platform->spyScrollAndSaveMode(
        queryOrUrl, downloadDir, maxPosts, mode, minEngagement, spyContext, maxDaysOld);
    if (!self)
    {
        co_return;
    }

    m_spying = false;
    if (m_scrollAndSave)
    {
        m_scrollAndSave->setEnabled(true);
        m_scrollAndSave->setText(tr("Scroll & save…"));
    }

    if (!result.errorMessage.isEmpty() && result.posts.isEmpty())
    {
        report(false, result.errorMessage);
        co_return;
    }

    if (!accountId().isEmpty())
    {
        for (const QString &starred : result.starredInfluencers)
        {
            const QString starredId = m_store->addInfluencer(accountId(), platformId, starred);
            if (!starredId.isEmpty())
            {
                m_store->recordInfluencerActivity(starredId, true, false, 0);
            }
        }
    }

    int matchedInfluencerPicks = 0;
    const int savedCount = importSpyResult(result, linkProductId, isHumanPick, platformId, &matchedInfluencerPicks);

    if (!accountId().isEmpty())
    {
        if (!influencerId.isEmpty())
        {
            const int extraCount = (matchedInfluencerPicks > 0) ? 0 : savedCount;
            m_store->recordInfluencerActivity(influencerId, true, savedCount > 0, extraCount);
        }
        for (const QString &vis : result.visitedProfiles)
        {
            const QString visHandle = ResearchStore::normalizeInfluencerHandle(vis, platformId);
            for (const auto &v : m_store->influencers(accountId(), platformId))
            {
                const auto inf = v.toObject();
                const QString id = text(inf, "id");
                if (id == influencerId)
                {
                    continue;
                }
                const bool sameHandle = !visHandle.isEmpty()
                    && text(inf, "handle").compare(visHandle, Qt::CaseInsensitive) == 0;
                const bool sameUrl = text(inf, "url").compare(vis, Qt::CaseInsensitive) == 0;
                if (sameHandle || sameUrl)
                {
                    m_store->recordInfluencerActivity(id, true, false, 0);
                }
            }
        }
        updateStarAuthorButton();
    }

    const int browsedCount = qMax(result.browsedCount, savedCount + result.rejectedCount);
    if (browsedCount > 0 && !accountId().isEmpty())
    {
        m_store->recordSpySession(accountId(), QJsonObject{
            {QStringLiteral("platform"), platform->getName()},
            {QStringLiteral("platformId"), platformId},
            {QStringLiteral("query"), queryOrUrl.isEmpty() ? platform->defaultSpyHomeUrl() : queryOrUrl},
            {QStringLiteral("mode"), AbstractSocialAccount::spyModeKey(mode)},
            {QStringLiteral("browsedCount"), browsedCount},
            {QStringLiteral("savedCount"), savedCount},
            {QStringLiteral("rejectedCount"), result.rejectedCount},
            {QStringLiteral("minEngagement"), result.minEngagement},
            {QStringLiteral("maxDaysOld"), result.maxDaysOld}
        });
        refreshReferences();
    }

    if (isHumanPick)
    {
        if (savedCount <= 0)
        {
            report(true, result.starredInfluencers.isEmpty()
                ? tr("Browser picking finished (0 posts picked).")
                : tr("Browser picking finished — saved %1 favorite influencer(s).")
                    .arg(result.starredInfluencers.size()));
        }
        else
        {
            report(true, tr("Picked & approved %1 reference post(s) from %2 — ready to create projects!")
                .arg(savedCount)
                .arg(platform->getName()));
        }
        co_return;
    }

    if (savedCount <= 0)
    {
        if (browsedCount > 0 && result.rejectedCount > 0)
        {
            report(false, tr("Browsed %1 post(s) on %2 for \"%3\", but 0 matched your stats/age filters (100% reject rate). Lower min stats or increase max days old.")
                .arg(browsedCount)
                .arg(platform->getName(), queryOrUrl));
        }
        else
        {
            report(false, tr("No candidate posts were found on %1 for \"%2\".")
                .arg(platform->getName(), queryOrUrl));
        }
    }
    else if (browsedCount > 0)
    {
        const double savePct = 100.0 * double(savedCount) / double(browsedCount);
        const double rejectPct = 100.0 * double(result.rejectedCount) / double(browsedCount);
        report(true, tr("Saved %1 / %2 browsed post(s) (%3% saved · %4% reject rate) from %5 into the review queue.")
            .arg(savedCount)
            .arg(browsedCount)
            .arg(QString::number(savePct, 'f', 1))
            .arg(QString::number(rejectPct, 'f', 1))
            .arg(platform->getName()));
    }
    else
    {
        report(true, tr("Saved %1 reference post(s) from %2 into the review queue.")
            .arg(savedCount)
            .arg(platform->getName()));
    }
}

int PaneResearch::importSpyResult(
    const AbstractSocialAccount::SpyResult &result,
    const QString &linkProductId,
    bool autoApprove,
    const QString &platformId,
    int *outMatchedInfluencerPicks)
{
    if (outMatchedInfluencerPicks)
    {
        *outMatchedInfluencerPicks = 0;
    }
    if (accountId().isEmpty())
    {
        return 0;
    }

    const bool canLinkProduct = !linkProductId.isEmpty()
        && ResearchStore::productRestriction(m_store->product(linkProductId), m_store->profile(accountId())).isEmpty();

    QString firstId;
    int imported = 0;
    int matchedInfPicks = 0;
    m_refreshing = true;
    for (const auto &post : result.posts)
    {
        if (!autoApprove && !post.postUrl.isEmpty() && m_store->isRejectedUrl(accountId(), post.postUrl))
        {
            continue;
        }
        const QString title = post.title.isEmpty() ? post.postUrl : post.title;
        const QString id = m_store->addReference(accountId(), post.postUrl, title, post.localMediaPath);
        if (id.isEmpty())
        {
            continue;
        }
        const auto existing = m_store->reference(id);
        const QString existingStatus = text(existing, "status");
        if (!autoApprove && (existingStatus == QLatin1String("rejected") || existingStatus == QLatin1String("approved")))
        {
            continue;
        }
        if (!post.localMediaPath.isEmpty()
            && QFileInfo::exists(post.localMediaPath)
            && m_store->mediaPath(existing).isEmpty())
        {
            m_store->attachMedia(id, post.localMediaPath, !autoApprove);
        }

        QJsonObject patch;
        if (canLinkProduct && text(existing, "productId").isEmpty())
        {
            patch.insert("productId", linkProductId);
        }

        const QString effectivePlat = !platformId.isEmpty() ? platformId : text(existing, "platform");

        // Persist structured post statistics on the reference record
        QJsonObject stats = existing.value(QStringLiteral("stats")).toObject();
        if (post.views >= 0) stats.insert(QStringLiteral("views"), post.views);
        if (post.likes >= 0) stats.insert(QStringLiteral("likes"), post.likes);
        if (post.comments >= 0) stats.insert(QStringLiteral("comments"), post.comments);
        if (post.shares >= 0)
        {
            stats.insert(QStringLiteral("shares"), post.shares);
            if (effectivePlat == QLatin1String("pinterest"))
            {
                stats.insert(QStringLiteral("saves"), post.shares);
            }
        }
        if (!post.author.isEmpty())
        {
            stats.insert(QStringLiteral("author"), post.author);
        }
        if (!post.publishedAt.isEmpty())
        {
            stats.insert(QStringLiteral("publishedAt"), post.publishedAt);
        }
        if (post.isAd)
        {
            stats.insert(QStringLiteral("isAd"), true);
        }
        if (!stats.isEmpty())
        {
            patch.insert(QStringLiteral("stats"), stats);
        }
        if (!post.title.isEmpty()
            && (text(existing, "title").endsWith(QStringLiteral("pin page"), Qt::CaseInsensitive)
                || text(existing, "title").startsWith(QStringLiteral("Untitled"), Qt::CaseInsensitive)))
        {
            patch.insert(QStringLiteral("title"), post.title);
        }

        if (!patch.isEmpty())
        {
            m_store->updateReference(id, patch);
        }
        if (autoApprove && text(m_store->reference(id), "status") != QLatin1String("approved"))
        {
            m_store->review({id}, QStringLiteral("approved"), tr("Picked in browser"));
        }

        matchedInfPicks += m_store->recordInfluencerPickByAuthorOrUrl(
            accountId(), effectivePlat, post.author, post.postUrl);

        if (firstId.isEmpty())
        {
            firstId = id;
        }
        ++imported;
    }
    m_refreshing = false;
    if (outMatchedInfluencerPicks)
    {
        *outMatchedInfluencerPicks = matchedInfPicks;
    }
    if (autoApprove && imported > 0 && m_activeFilter == QLatin1String("pending"))
    {
        setFilter(QStringLiteral("approved"));
    }
    else
    {
        refreshReferences();
    }
    if (!firstId.isEmpty())
    {
        selectReference(firstId);
    }
    return imported;
}

void PaneResearch::addUrls()
{
    if (accountId().isEmpty()) { report(false, tr("Choose an account first.")); return; }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add reference URLs")); dialog.resize(600, 360);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Paste one post URL per line. Duplicate URLs for this account are reused.")));
    auto *urls = new QPlainTextEdit; layout->addWidget(urls);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QString last;
    m_refreshing = true;
    for (const auto &url : urls->toPlainText().split('\n', Qt::SkipEmptyParts))
    {
        last = m_store->addReference(accountId(), url.trimmed(), url.trimmed());
        if (last.isEmpty()) break;
        // Do not replace an existing reference's product during duplicate import.
        if (!m_productId.isEmpty() && text(m_store->reference(last), "productId").isEmpty())
            if (!m_store->updateReference(last, {{"productId", m_productId}})) { last.clear(); break; }
    }
    m_refreshing = false;
    report(!last.isEmpty(), tr("References added to the review queue."));
    refreshReferences();
    if (!last.isEmpty()) selectReference(last);
}

void PaneResearch::addFiles()
{
    if (accountId().isEmpty()) { report(false, tr("Choose an account first.")); return; }
    const auto paths = QFileDialog::getOpenFileNames(this, tr("Reference images or videos"), {}, tr("Media (*.jpg *.jpeg *.png *.webp *.mp4 *.mov *.webm)"));
    if (paths.isEmpty()) return;
    QString last;
    m_refreshing = true;
    for (const auto &path : paths)
    {
        last = m_store->addReference(accountId(), {}, QFileInfo(path).completeBaseName(), path);
        if (last.isEmpty()) break;
        if (!m_productId.isEmpty() && !m_store->updateReference(last, {{"productId", m_productId}})) { last.clear(); break; }
    }
    m_refreshing = false;
    report(!last.isEmpty(), tr("Media copied into the reference library."));
    refreshReferences();
    if (!last.isEmpty()) selectReference(last);
}

bool PaneResearch::saveReference()
{
    if (m_referenceId.isEmpty()) return true;
    const QJsonObject cur = m_store->reference(m_referenceId);
    if (cur.isEmpty() || (text(cur, "title") == m_title->text() && text(cur, "notes") == m_notes->toPlainText()))
    {
        return true;
    }
    const bool ok = m_store->updateReference(m_referenceId, {{"title", m_title->text()}, {"notes", m_notes->toPlainText()}});
    if (!ok) report(false);
    return ok;
}

bool PaneResearch::deleteReferenceIds(const QStringList &ids)
{
    if (ids.isEmpty())
    {
        return false;
    }
    for (const QString &id : ids)
    {
        m_assessmentQueue.removeAll(id);
    }
    if (ids.contains(m_referenceId))
    {
        m_player->stop();
        m_player->setSource({});
        m_referenceId.clear();
    }
    const bool ok = m_store->removeReferences(ids);
    if (ok)
    {
        showReference();
        refreshReferences();
    }
    report(ok, tr("Deleted %1 reference(s).").arg(ids.size()));
    return ok;
}

void PaneResearch::deleteSelection()
{
    const auto ids = selectedReferences();
    if (ids.isEmpty())
    {
        report(false, tr("Select reference cards to delete."));
        return;
    }
    const auto answer = QMessageBox::question(
        this,
        tr("Delete selected references"),
        tr("Delete %1 selected reference(s) and their cached media?").arg(ids.size()),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
    {
        return;
    }
    deleteReferenceIds(ids);
}

void PaneResearch::deleteAll()
{
    const QString curAccount = accountId();
    if (curAccount.isEmpty())
    {
        report(false, tr("Choose an account first."));
        return;
    }

    const QString filter = m_activeFilter;
    const QString search = m_referenceSearch->text().trimmed();
    QStringList allAccountIds;
    QStringList currentViewIds;

    for (const auto &v : m_store->references())
    {
        const auto r = v.toObject();
        if (text(r, "accountId") != curAccount)
        {
            continue;
        }
        const QString id = text(r, "id");
        allAccountIds << id;

        if (!filter.isEmpty() && text(r, "status") != filter)
        {
            continue;
        }
        const QString statsText = formatReferenceStatsLine(r);
        if (!search.isEmpty()
            && !(text(r, "title") + " " + text(r, "url") + " " + text(r, "notes") + " " + statsText)
                   .contains(search, Qt::CaseInsensitive))
        {
            continue;
        }
        currentViewIds << id;
    }

    if (allAccountIds.isEmpty())
    {
        report(false, tr("No references to delete for this account."));
        return;
    }

    if (!currentViewIds.isEmpty() && currentViewIds.size() < allAccountIds.size())
    {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Delete references"));
        box.setText(tr("Delete all %1 reference(s) for this account, or only the %2 in the current filtered view?")
                        .arg(allAccountIds.size())
                        .arg(currentViewIds.size()));
        auto *deleteAllBtn = box.addButton(tr("Delete all (%1)").arg(allAccountIds.size()), QMessageBox::AcceptRole);
        auto *deleteViewBtn = box.addButton(tr("Delete current view (%1)").arg(currentViewIds.size()), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(deleteAllBtn);
        box.exec();
        if (box.clickedButton() == deleteAllBtn)
        {
            deleteReferenceIds(allAccountIds);
        }
        else if (box.clickedButton() == deleteViewBtn)
        {
            deleteReferenceIds(currentViewIds);
        }
        return;
    }

    const auto answer = QMessageBox::question(
        this,
        tr("Delete all references"),
        tr("Delete all %1 reference(s) for this account and their cached media?").arg(allAccountIds.size()),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
    {
        return;
    }
    deleteReferenceIds(allAccountIds);
}

void PaneResearch::review(const QString &status)
{
    const auto ids = selectedReferences();
    if (ids.isEmpty()) { report(false, tr("Select reference cards first.")); return; }
    reviewIds(ids, status);
}

void PaneResearch::reviewIds(const QStringList &ids, const QString &status)
{
    if (!saveReference()) return;

    const bool advanceNext = m_nextAfterReview->isChecked()
        && status != QLatin1String("pending")
        && m_activeFilter == QLatin1String("pending");

    QString nextCandidateId;
    if (advanceNext)
    {
        int anchorIdx = -1;
        for (const QString &rid : ids)
        {
            const int idx = m_displayedRefIds.indexOf(rid);
            if (idx >= 0 && (anchorIdx < 0 || idx < anchorIdx))
            {
                anchorIdx = idx;
            }
        }
        if (anchorIdx < 0)
        {
            anchorIdx = 0;
        }
        for (int i = anchorIdx; i < m_displayedRefIds.size(); ++i)
        {
            if (!ids.contains(m_displayedRefIds[i]))
            {
                nextCandidateId = m_displayedRefIds[i];
                break;
            }
        }
        if (nextCandidateId.isEmpty())
        {
            for (int i = anchorIdx - 1; i >= 0; --i)
            {
                if (!ids.contains(m_displayedRefIds[i]))
                {
                    nextCandidateId = m_displayedRefIds[i];
                    break;
                }
            }
        }
    }

    m_refreshing = true;
    const bool ok = m_store->review(ids, status, {});
    m_refreshing = false;
    report(ok, tr("%1 reference(s) marked %2.").arg(ids.size()).arg(status));
    if (!ok)
    {
        refreshReferences();
        return;
    }

    if (advanceNext)
    {
        m_referenceId = nextCandidateId;
        showReference();
        refreshReferences();
        if (m_referenceId.isEmpty() && !m_displayedRefIds.isEmpty())
        {
            selectReference(m_displayedRefIds.first());
        }
    }
    else
    {
        if (status != QLatin1String("pending")
            && m_activeFilter == QLatin1String("pending")
            && ids.contains(m_referenceId))
        {
            m_referenceId.clear();
            showReference();
        }
        refreshReferences();
        updateCreateProjectButton();
    }
}

void PaneResearch::assessNext()
{
    if (m_assessmentQueue.isEmpty())
    {
        m_assessing = false; m_assess->setText(tr("AI assess"));
        report(true, tr("AI assessments finished. Approve the winners you want to keep.")); return;
    }
    const QString id = m_assessmentQueue.takeFirst();
    const auto reference = m_store->reference(id);
    const QString chosenProdId = m_projectProductCombo ? m_projectProductCombo->currentData().toString() : m_productId;
    const auto product = m_store->product(chosenProdId.isEmpty() ? text(reference, "productId") : chosenProdId);
    auto referenceInput = reference;
    referenceInput.remove("assessments"); referenceInput.remove("history");
    referenceInput.insert("localMediaPath", m_store->mediaPath(reference));
    const qint64 ageDays = referenceAgeDays(reference);
    if (ageDays >= 0)
    {
        QJsonObject enrichedStats = referenceInput.value(QStringLiteral("stats")).toObject();
        enrichedStats.insert(QStringLiteral("ageDays"), ageDays);
        const double velocity = referenceDailyVelocity(reference);
        if (velocity >= 0.0)
        {
            enrichedStats.insert(QStringLiteral("dailyVelocity"), qRound(velocity * 100.0) / 100.0);
        }
        referenceInput.insert(QStringLiteral("stats"), enrichedStats);
    }
    const QJsonObject input{{"reference", referenceInput}, {"product", product},
        {"profile", m_store->profile(text(reference, "accountId"))}};
    const auto *method = ReferenceAssessor::all().value(m_runAssessor);
    if (!method) { m_assessmentQueue.clear(); assessNext(); return; }
    report(true, tr("Assessing %1 (%2 remaining)…").arg(text(reference, "title")).arg(m_assessmentQueue.size()));
    method->assess(input, m_runCli, m_runPrompt, m_store->directory().absolutePath(), this,
        [this, id](const QJsonObject &assessment) {
            if (!m_store->recordAssessment(id, assessment))
            {
                m_assessmentQueue.clear(); m_assessing = false; m_assess->setText(tr("AI assess")); report(false); return;
            }
            QTimer::singleShot(0, this, &PaneResearch::assessNext);
        });
}

void PaneResearch::createProject()
{
    if (!saveReference()) return;

    const QString refId = effectiveReferenceId();
    if (refId.isEmpty())
    {
        report(false, tr("Click a reference card on the left first."));
        return;
    }
    if (m_referenceId != refId)
    {
        m_referenceId = refId;
        showReference();
    }

    const QString chosenProdId = m_projectProductCombo ? m_projectProductCombo->currentData().toString() : m_productId;
    const auto product = m_store->product(chosenProdId);
    if (product.isEmpty())
    {
        const QString msg = m_store->products().isEmpty()
            ? tr("Import your product catalog in the Products tab first.")
            : tr("Click \"Account rules…\" (top right) to allow your brand and category for this account.");
        report(false, msg);
        QMessageBox::information(this, tr("Choose a product"), msg);
        return;
    }

    const QString restriction = ResearchStore::productRestriction(product, m_store->profile(accountId()));
    if (!restriction.isEmpty())
    {
        report(false, restriction);
        QMessageBox::warning(this, tr("Product not allowed for account"), restriction);
        return;
    }

    // Auto-approve a pending/selected reference when the user clicks Create generation project
    auto reference = m_store->reference(refId);
    if (text(reference, "status") != QLatin1String("approved"))
    {
        if (!m_store->review({refId}, QStringLiteral("approved"), tr("Approved when creating project")))
        {
            report(false);
            return;
        }
        reference = m_store->reference(refId);
    }

    QString image = text(product, "localImage");
    if (!QFileInfo::exists(image))
    {
        image = QFileDialog::getOpenFileName(this, tr("Choose an accurate photo of this product"), {}, tr("Images (*.jpg *.jpeg *.png *.webp)"));
    }
    if (image.isEmpty()) return;

    // Record that this product was generated from this reference (preserving approved status
    // so the same reference can be reused for any number of other products).
    QJsonArray usedProducts = reference.value(QStringLiteral("usedProducts")).toArray();
    if (!strings(usedProducts).contains(chosenProdId))
    {
        usedProducts.append(chosenProdId);
        m_store->updateReference(refId, QJsonObject{{QStringLiteral("usedProducts"), usedProducts}});
        reference = m_store->reference(refId);
    }

    // If this Pinterest reference was previously saved with only a static cover image
    // (or no media), check if the Pin actually has a direct .mp4 video stream so
    // Generation can extract frames & motion from the real video.
    if (text(reference, "platform") == QLatin1String("pinterest")
        && !isVideoMediaPath(m_store->mediaPath(reference))
        && qEnvironmentVariableIsEmpty("QS_SPY_HEADLESS"))
    {
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        const QString scriptPath = QDir(QStringLiteral(SOCIAL_STATS_SCRIPT_DIR))
            .filePath(QStringLiteral("spy_scroll_pinterest.py"));
        const QString pinUrl = text(reference, "url");
        if (!python.isEmpty() && QFileInfo::exists(scriptPath) && !pinUrl.isEmpty())
        {
            QTemporaryDir tmpVideoDir;
            if (tmpVideoDir.isValid())
            {
                QProcess dlProc;
                dlProc.start(python, {scriptPath, QStringLiteral("--download-video"), pinUrl, tmpVideoDir.path()});
                if (dlProc.waitForFinished(15'000) && dlProc.exitCode() == 0)
                {
                    const QString dlVideoPath = QJsonDocument::fromJson(dlProc.readAllStandardOutput().trimmed())
                        .object().value(QStringLiteral("video_path")).toString();
                    if (!dlVideoPath.isEmpty() && QFileInfo::exists(dlVideoPath))
                    {
                        if (m_store->attachMedia(refId, dlVideoPath, false))
                        {
                            reference = m_store->reference(refId);
                        }
                    }
                }
            }
        }
    }

    QJsonObject context{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("accountId"), accountId()},
        {QStringLiteral("accountLabel"), m_account->currentText()},
        {QStringLiteral("profile"), m_store->profile(accountId())},
        {QStringLiteral("product"), product},
        {QStringLiteral("reference"), reference}
    };
    context.insert(QStringLiteral("referenceMediaPath"), m_store->mediaPath(reference));
    emit createProjectRequested(context, image);
}

QJsonObject PaneResearch::buildAutoBatchSpec(
    const QStringList &productIds,
    int generationsPerProduct,
    bool includeImage,
    bool includeCarousel,
    bool includeVideo,
    bool carouselHookOverlay,
    const QString &videoFormatLabel,
    const QString &videoGeneratorId) const
{
    const QString curAccount = accountId();
    const QJsonObject profile = m_store->profile(curAccount);
    QJsonArray selectedProducts;
    for (const QString &pid : productIds)
    {
        const QJsonObject prod = m_store->product(pid);
        if (prod.isEmpty() || !ResearchStore::productRestriction(prod, profile).isEmpty())
        {
            continue;
        }
        if (text(prod, "localImage").isEmpty() || !QFileInfo::exists(text(prod, "localImage")))
        {
            continue;
        }
        selectedProducts.append(prod);
    }

    QList<QJsonObject> approvedRefs;
    for (const QJsonValue &v : m_store->references())
    {
        const QJsonObject r = v.toObject();
        if (text(r, "accountId") == curAccount && text(r, "status") == QLatin1String("approved"))
        {
            approvedRefs.append(r);
        }
    }
    std::stable_sort(approvedRefs.begin(), approvedRefs.end(),
                     [](const QJsonObject &a, const QJsonObject &b) {
                         const double va = referenceDailyVelocity(a);
                         const double vb = referenceDailyVelocity(b);
                         if (va != vb)
                         {
                             return va > vb;
                         }
                         return referenceEngagementScore(a) > referenceEngagementScore(b);
                     });

    QJsonArray refPool;
    for (const QJsonObject &r : approvedRefs)
    {
        QJsonObject entry;
        entry.insert(QStringLiteral("reference"), r);
        entry.insert(QStringLiteral("mediaPath"), m_store->mediaPath(r));
        refPool.append(entry);
    }

    return QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("accountId"), curAccount},
        {QStringLiteral("accountLabel"), m_account->currentText()},
        {QStringLiteral("profile"), profile},
        {QStringLiteral("products"), selectedProducts},
        {QStringLiteral("approvedReferences"), refPool},
        {QStringLiteral("generationsPerProduct"), qMax(1, generationsPerProduct)},
        {QStringLiteral("includeImage"), includeImage},
        {QStringLiteral("includeCarousel"), includeCarousel},
        {QStringLiteral("includeVideo"), includeVideo},
        {QStringLiteral("carouselHookOverlay"), carouselHookOverlay},
        {QStringLiteral("videoFormat"), videoFormatLabel},
        {QStringLiteral("videoGeneratorId"), videoGeneratorId}
    };
}

void PaneResearch::openAutoBatchDialog()
{
    const QString curAccount = accountId();
    if (curAccount.isEmpty())
    {
        report(false, tr("Add and choose a social account first."));
        return;
    }
    const QJsonObject profile = m_store->profile(curAccount);
    const QJsonArray allProducts = m_store->products();
    if (allProducts.isEmpty())
    {
        const QString msg = tr("Import your product catalog in the Products tab first.");
        report(false, msg);
        QMessageBox::information(this, tr("Import products first"), msg);
        return;
    }

    QList<QJsonObject> eligibleProducts;
    for (const QJsonValue &v : allProducts)
    {
        const QJsonObject p = v.toObject();
        if (ResearchStore::productRestriction(p, profile).isEmpty()
            && !text(p, "localImage").isEmpty()
            && QFileInfo::exists(text(p, "localImage")))
        {
            eligibleProducts.append(p);
        }
    }
    if (eligibleProducts.isEmpty())
    {
        const QString msg = tr("Click \"Account rules…\" (top right) to allow your brand and category for this account.");
        report(false, msg);
        QMessageBox::information(this, tr("Configure account rules"), msg);
        return;
    }

    int approvedRefCount = 0;
    for (const QJsonValue &v : m_store->references())
    {
        const QJsonObject r = v.toObject();
        if (text(r, "accountId") == curAccount && text(r, "status") == QLatin1String("approved"))
        {
            ++approvedRefCount;
        }
    }

    QSettings settings(m_store->directory().filePath("ui.ini"), QSettings::IniFormat);

    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("dialogAutoBatch"));
    dlg.setWindowTitle(tr("Auto-Generate Creative Batch"));
    dlg.setMaximumWidth(1600);
    dlg.resize(780, 580);
    auto *layout = new QVBoxLayout(&dlg);

    auto *intro = new QLabel(
        approvedRefCount > 0
            ? tr("Account: %1 · %2 approved reference(s) ready as inspiration.\n"
                 "Your selected product(s) are pre-checked at the top below — QuantumSocial will generate "
                 "diverse creatives unattended (80% top inspirations & winning A/B properties, 20% new exploration).")
                .arg(m_account->currentText())
                .arg(approvedRefCount)
            : tr("Account: %1 · 0 approved references yet (will generate fresh editorial A/B creatives "
                 "from your product photos; approve Pinterest/social cards in References anytime to include them!).")
                .arg(m_account->currentText()));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // Collect pre-selected products from checkboxes first, then table row selection, then active m_productId
    QSet<QString> initialChecked = m_checkedProductIds;
    for (int r = 0; r < m_products->rowCount(); ++r)
    {
        if (auto *it = m_products->item(r, 0))
        {
            if (it->checkState() == Qt::Checked)
            {
                initialChecked.insert(it->data(Qt::UserRole).toString());
            }
        }
    }
    if (initialChecked.isEmpty() && m_productTable && m_productTable->selectionModel())
    {
        for (const QModelIndex &selIdx : m_productTable->selectionModel()->selectedRows(0))
        {
            const QString pid = selIdx.data(Qt::UserRole).toString();
            if (!pid.isEmpty())
            {
                initialChecked.insert(pid);
            }
        }
    }
    if (initialChecked.isEmpty() && !m_productId.isEmpty())
    {
        initialChecked.insert(m_productId);
    }
    if (initialChecked.isEmpty() && !eligibleProducts.isEmpty())
    {
        initialChecked.insert(text(eligibleProducts.first(), "id"));
    }

    // Preserve the user's current proxy sort order, with all pre-selected products placed FIRST at the top!
    QHash<QString, int> proxyOrder;
    for (int r = 0; r < m_productProxy->rowCount(); ++r)
    {
        proxyOrder.insert(m_productProxy->index(r, 0).data(Qt::UserRole).toString(), r);
    }
    std::stable_sort(eligibleProducts.begin(), eligibleProducts.end(),
                     [&](const QJsonObject &a, const QJsonObject &b) {
                         const QString ida = text(a, "id");
                         const QString idb = text(b, "id");
                         const bool ca = initialChecked.contains(ida);
                         const bool cb = initialChecked.contains(idb);
                         if (ca != cb)
                         {
                             return ca;
                         }
                         return proxyOrder.value(ida, 999999) < proxyOrder.value(idb, 999999);
                     });

    auto *prodHeader = new QHBoxLayout;
    prodHeader->addWidget(new QLabel(tr("1. Products to generate for:")));
    prodHeader->addStretch();
    auto *onlySelectedChk = new QCheckBox(tr("Show only selected (%1)").arg(initialChecked.size()));
    onlySelectedChk->setObjectName(QStringLiteral("autoBatchOnlySelected"));
    onlySelectedChk->setChecked(initialChecked.size() < eligibleProducts.size());
    prodHeader->addWidget(onlySelectedChk);
    auto *checkAllBtn = new QPushButton(tr("Check all (%1)").arg(eligibleProducts.size()));
    auto *uncheckAllBtn = new QPushButton(tr("Clear"));
    prodHeader->addWidget(checkAllBtn);
    prodHeader->addWidget(uncheckAllBtn);
    layout->addLayout(prodHeader);

    auto *prodSplitRow = new QHBoxLayout;
    auto *prodList = new QListWidget;
    prodList->setObjectName(QStringLiteral("autoBatchProductsList"));
    prodList->setIconSize(QSize(44, 44));

    auto *prodPreviewBox = new QFrame;
    prodPreviewBox->setFrameShape(QFrame::StyledPanel);
    prodPreviewBox->setFixedWidth(185);
    auto *prodPreviewLayout = new QVBoxLayout(prodPreviewBox);
    prodPreviewLayout->setContentsMargins(6, 6, 6, 6);
    auto *prodPreviewImg = new QLabel;
    prodPreviewImg->setObjectName(QStringLiteral("autoBatchProductPreview"));
    prodPreviewImg->setAlignment(Qt::AlignCenter);
    prodPreviewImg->setFixedSize(170, 170);
    auto *prodPreviewCaption = new QLabel;
    prodPreviewCaption->setWordWrap(true);
    prodPreviewCaption->setAlignment(Qt::AlignCenter);
    prodPreviewCaption->setStyleSheet(QStringLiteral("font-size: 11px;"));
    prodPreviewLayout->addWidget(prodPreviewImg);
    prodPreviewLayout->addWidget(prodPreviewCaption, 1);

    const auto loadFastThumb = [](const QString &imgPath, int edge) -> QPixmap {
        if (imgPath.isEmpty())
        {
            return {};
        }
        QImageReader reader(imgPath);
        reader.setAutoTransform(true);
        const QSize orig = reader.size();
        if (orig.isValid() && (orig.width() > edge * 2 || orig.height() > edge * 2))
        {
            reader.setScaledSize(orig.scaled(edge, edge, Qt::KeepAspectRatio));
        }
        const QImage img = reader.read();
        if (img.isNull())
        {
            return loadCompatiblePixmap(imgPath).scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return QPixmap::fromImage(img).scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    };

    const auto ensureRowIconLoaded = [&](QListWidgetItem *item) {
        if (!item || item->data(Qt::UserRole + 2).toBool())
        {
            return;
        }
        item->setData(Qt::UserRole + 2, true);
        const QPixmap thumb = loadFastThumb(item->data(Qt::UserRole + 1).toString(), 44);
        if (!thumb.isNull())
        {
            item->setIcon(QIcon(thumb));
        }
    };

    for (const QJsonObject &p : eligibleProducts)
    {
        const QString pid = text(p, "id");
        const QString imgPath = text(p, "localImage");
        const bool isPreChecked = initialChecked.contains(pid);
        QString label = text(p, "color");
        if (!text(p, "title").isEmpty())
        {
            label += (label.isEmpty() ? QString{} : QStringLiteral(" — ")) + text(p, "title");
        }
        auto *item = new QListWidgetItem(label, prodList);
        item->setData(Qt::UserRole, pid);
        item->setData(Qt::UserRole + 1, imgPath);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(isPreChecked ? Qt::Checked : Qt::Unchecked);
        const bool hideInitially = onlySelectedChk->isChecked() && !isPreChecked;
        item->setHidden(hideInitially);
        if (!hideInitially)
        {
            ensureRowIconLoaded(item);
        }
    }

    prodSplitRow->addWidget(prodList, 1);
    prodSplitRow->addWidget(prodPreviewBox);
    layout->addLayout(prodSplitRow, 1);

    const auto updatePreviewForCurrent = [&]() {
        auto *cur = prodList->currentItem();
        if (!cur || cur->isHidden())
        {
            for (int i = 0; i < prodList->count(); ++i)
            {
                if (!prodList->item(i)->isHidden())
                {
                    cur = prodList->item(i);
                    prodList->setCurrentItem(cur);
                    break;
                }
            }
        }
        if (!cur)
        {
            prodPreviewImg->clear();
            prodPreviewCaption->setText(tr("No product selected"));
            return;
        }
        const QPixmap px = loadFastThumb(cur->data(Qt::UserRole + 1).toString(), 170);
        if (!px.isNull())
        {
            prodPreviewImg->setPixmap(px);
        }
        else
        {
            prodPreviewImg->setText(tr("No photo"));
        }
        prodPreviewCaption->setText(cur->text().left(85));
    };
    connect(prodList, &QListWidget::currentItemChanged, &dlg, [&](QListWidgetItem *, QListWidgetItem *) {
        updatePreviewForCurrent();
    });
    if (prodList->count() > 0)
    {
        prodList->setCurrentRow(0);
    }
    updatePreviewForCurrent();

    auto *form = new QFormLayout;
    auto *countSpin = new QSpinBox;
    countSpin->setObjectName(QStringLiteral("autoBatchCountPerProduct"));
    countSpin->setRange(1, 50);
    countSpin->setValue(settings.value(QStringLiteral("autoBatch/countPerProduct"), 3).toInt());
    form->addRow(tr("2. Generations per product (each format):"), countSpin);

    const bool isTikTokAccount = m_account->currentText().contains(
        QLatin1String("tiktok"), Qt::CaseInsensitive);
    const QString includeImageKey = isTikTokAccount
        ? QStringLiteral("autoBatch/tiktokIncludeImage")
        : QStringLiteral("autoBatch/includeImage");

    auto *formatsCol = new QVBoxLayout;
    auto *chkImage = new QCheckBox(
        isTikTokAccount
            ? tr("Single Image (Pinterest / Instagram only — skipped on TikTok)")
            : tr("Single Image (product-swapped scene / editorial)"));
    chkImage->setObjectName(QStringLiteral("autoBatchFormatImage"));
    chkImage->setChecked(settings.value(includeImageKey, !isTikTokAccount).toBool());

    auto *chkCarousel = new QCheckBox(tr("Carousel / Slideshow (4 coherent slides)"));
    chkCarousel->setObjectName(QStringLiteral("autoBatchFormatCarousel"));
    chkCarousel->setChecked(settings.value(QStringLiteral("autoBatch/includeCarousel"), true).toBool());

    auto *chkHookOverlay = new QCheckBox(tr("Overlay hook / story text over Carousel slides (recommended for TikTok)"));
    chkHookOverlay->setObjectName(QStringLiteral("autoBatchCarouselHookOverlay"));
    chkHookOverlay->setChecked(settings.value(QStringLiteral("autoBatch/carouselTextOverlayOn"), true).toBool());
    chkHookOverlay->setEnabled(chkCarousel->isChecked());
    connect(chkCarousel, &QCheckBox::toggled, chkHookOverlay, &QWidget::setEnabled);

    auto *overlayIndent = new QHBoxLayout;
    overlayIndent->addSpacing(22);
    overlayIndent->addWidget(chkHookOverlay);

    auto *chkVideo = new QCheckBox(tr("Video (auto product-swap + AI quality gate + motion animation, no overlaid text)"));
    chkVideo->setObjectName(QStringLiteral("autoBatchFormatVideo"));
    chkVideo->setChecked(settings.value(QStringLiteral("autoBatch/includeVideo"), true).toBool());

    formatsCol->addWidget(chkImage);
    formatsCol->addWidget(chkCarousel);
    formatsCol->addLayout(overlayIndent);
    formatsCol->addWidget(chkVideo);
    form->addRow(tr("3. Creative formats:"), formatsCol);

    auto *formatCombo = new QComboBox;
    formatCombo->addItem(tr("Vertical 9:16 (TikTok / Reels / Shorts / Pins)"), QStringLiteral("vertical (9:16)"));
    formatCombo->addItem(tr("Horizontal 16:9"), QStringLiteral("horizontal (16:9)"));
    formatCombo->addItem(tr("Square 1:1"), QStringLiteral("square (1:1)"));
    form->addRow(tr("4. Aspect ratio:"), formatCombo);
    layout->addLayout(form);

    auto *summaryLabel = new QLabel;
    summaryLabel->setWordWrap(true);
    summaryLabel->setStyleSheet(QStringLiteral("color: #8fd19e; font-weight: 600; padding: 4px 0;"));
    layout->addWidget(summaryLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    if (auto *okBtn = buttons->button(QDialogButtonBox::Ok))
    {
        okBtn->setText(tr("Start Auto-Generation"));
        okBtn->setObjectName(QStringLiteral("buttonStartAutoBatch"));
    }
    layout->addWidget(buttons);

    const auto applySelectedFilter = [&]() {
        const bool onlyChecked = onlySelectedChk->isChecked();
        for (int i = 0; i < prodList->count(); ++i)
        {
            auto *it = prodList->item(i);
            const bool hide = onlyChecked && (it->checkState() != Qt::Checked);
            it->setHidden(hide);
            if (!hide)
            {
                ensureRowIconLoaded(it);
            }
        }
        updatePreviewForCurrent();
    };

    const auto updateSummary = [&]() {
        int checkedProducts = 0;
        for (int i = 0; i < prodList->count(); ++i)
        {
            if (prodList->item(i)->checkState() == Qt::Checked) ++checkedProducts;
        }
        onlySelectedChk->setText(tr("Show only selected (%1)").arg(checkedProducts));
        const int formatCount = (chkImage->isChecked() ? 1 : 0)
            + (chkCarousel->isChecked() ? 1 : 0)
            + (chkVideo->isChecked() ? 1 : 0);
        const bool anyFormat = formatCount > 0;
        const int totalCreatives = checkedProducts * countSpin->value() * formatCount;
        summaryLabel->setText(
            (checkedProducts > 0 && anyFormat)
                ? tr("Ready: %1 product(s) × %2 per format × %3 format(s) = %4 total creative(s) — zero popups while running.")
                    .arg(checkedProducts).arg(countSpin->value()).arg(formatCount).arg(totalCreatives)
                : tr("Check at least 1 product and 1 creative format."));
        if (auto *okBtn = buttons->button(QDialogButtonBox::Ok))
        {
            okBtn->setEnabled(checkedProducts > 0 && anyFormat);
        }
    };

    connect(onlySelectedChk, &QCheckBox::toggled, &dlg, [&]() {
        applySelectedFilter();
    });
    connect(checkAllBtn, &QPushButton::clicked, &dlg, [&]() {
        const QSignalBlocker blockOnly(onlySelectedChk);
        onlySelectedChk->setChecked(false);
        const QSignalBlocker blockList(prodList);
        for (int i = 0; i < prodList->count(); ++i)
        {
            auto *it = prodList->item(i);
            it->setHidden(false);
            it->setCheckState(Qt::Checked);
            ensureRowIconLoaded(it);
        }
        updateSummary();
        updatePreviewForCurrent();
    });
    connect(uncheckAllBtn, &QPushButton::clicked, &dlg, [&]() {
        const QSignalBlocker blockOnly(onlySelectedChk);
        onlySelectedChk->setChecked(false);
        const QSignalBlocker blockList(prodList);
        for (int i = 0; i < prodList->count(); ++i)
        {
            auto *it = prodList->item(i);
            it->setHidden(false);
            it->setCheckState(Qt::Unchecked);
            ensureRowIconLoaded(it);
        }
        updateSummary();
        updatePreviewForCurrent();
    });
    connect(prodList, &QListWidget::itemChanged, &dlg, [&](QListWidgetItem *changedItem) {
        if (changedItem)
        {
            prodList->setCurrentItem(changedItem);
        }
        updateSummary();
    });
    connect(countSpin, &QSpinBox::valueChanged, &dlg, [&]() { updateSummary(); });
    connect(chkImage, &QCheckBox::toggled, &dlg, [&]() { updateSummary(); });
    connect(chkCarousel, &QCheckBox::toggled, &dlg, [&]() { updateSummary(); });
    connect(chkVideo, &QCheckBox::toggled, &dlg, [&]() { updateSummary(); });
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    updateSummary();

    if (dlg.exec() != QDialog::Accepted)
    {
        return;
    }

    QStringList chosenProductIds;
    m_checkedProductIds.clear();
    for (int i = 0; i < prodList->count(); ++i)
    {
        if (prodList->item(i)->checkState() == Qt::Checked)
        {
            const QString pid = prodList->item(i)->data(Qt::UserRole).toString();
            chosenProductIds << pid;
            m_checkedProductIds.insert(pid);
        }
    }
    m_refreshing = true;
    for (int r = 0; r < m_products->rowCount(); ++r)
    {
        if (auto *it = m_products->item(r, 0))
        {
            it->setCheckState(m_checkedProductIds.contains(it->data(Qt::UserRole).toString())
                                  ? Qt::Checked
                                  : Qt::Unchecked);
        }
    }
    m_refreshing = false;

    settings.setValue(QStringLiteral("autoBatch/countPerProduct"), countSpin->value());
    settings.setValue(includeImageKey, chkImage->isChecked());
    settings.setValue(QStringLiteral("autoBatch/includeCarousel"), chkCarousel->isChecked());
    settings.setValue(QStringLiteral("autoBatch/carouselTextOverlayOn"), chkHookOverlay->isChecked());
    settings.setValue(QStringLiteral("autoBatch/includeVideo"), chkVideo->isChecked());

    const QJsonObject spec = buildAutoBatchSpec(
        chosenProductIds,
        countSpin->value(),
        chkImage->isChecked(),
        chkCarousel->isChecked(),
        chkVideo->isChecked(),
        chkHookOverlay->isChecked(),
        formatCombo->currentData().toString());
    if (spec.value(QStringLiteral("products")).toArray().isEmpty())
    {
        report(false, tr("No valid product photos found for the selected products."));
        return;
    }
    emit autoBatchRequested(spec);
}

void PaneResearch::report(bool success, const QString &message)
{
    m_status->setText(success ? message : tr("Could not complete action: %1").arg(message.isEmpty() ? m_store->lastError() : message));
    m_status->setStyleSheet(success ? QString{} : QStringLiteral("color: #e89774;"));
}
