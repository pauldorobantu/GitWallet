#include "ui/PageKit.h"

#include "ui/DialogChrome.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/TableUtil.h"
#include "ui/ThemeManager.h"

#include <QAbstractButton>
#include <QAction>
#include <QBoxLayout>
#include <QDialog>
#include <QApplication>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QTableView>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace app::ui::page {

namespace {
QColor role(Color c)
{
    return theme::ThemeManager::instance()->color(c);
}

// Gazda celulei de acțiuni. Tabelul o așază în rect-ul de TEXT al celulei
// (QStyledItemDelegate::updateEditorGeometry), adică micșorat cu padding-ul QSS
// al item-ului (kCellPadX stânga + dreapta). Coloanele ResizeToContents se
// dimensionează după sizeHint → gazda primea 20px mai puțin decât cerea și
// iconițele se înghesuiau până se atingeau. Raportăm hint-ul cu padding inclus.
class ActionsCell : public QWidget
{
public:
    using QWidget::QWidget;
    QSize sizeHint() const override
    {
        return QWidget::sizeHint() + QSize(2 * kCellPadX, 0);
    }
    QSize minimumSizeHint() const override
    {
        return QWidget::minimumSizeHint() + QSize(2 * kCellPadX, 0);
    }
};
} // namespace

QColor accent() { return role(Color::Accent); }
QColor success() { return role(Color::Success); }
QColor danger() { return role(Color::Error); }
QColor warning() { return role(Color::Warning); }
QColor muted() { return role(Color::Muted); }
QColor text() { return role(Color::Text); }

qreal corner(qreal natural)
{
    return theme::ThemeManager::instance()->corner(natural);
}

void applyPageLayout(QVBoxLayout* root)
{
    if (!root)
        return;
    root->setContentsMargins(kPageMarginX, kPageMarginTop, kPageMarginX, kPageMarginBot);
    root->setSpacing(kPageSpacing);
}

ActionBar makeActionBar(QWidget* parent)
{
    ActionBar bar;
    bar.widget = new QWidget(parent);
    bar.widget->setObjectName(QStringLiteral("pageActionBar"));
    bar.widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    // Aceeași înălțime pe toate paginile, chiar dacă bara are doar un count.
    bar.widget->setMinimumHeight(kControlH);
    auto* row = new QHBoxLayout(bar.widget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(kToolbarSpacing);
    bar.left = new QHBoxLayout;
    bar.left->setContentsMargins(0, 0, 0, 0);
    bar.left->setSpacing(kToolbarSpacing);
    bar.right = new QHBoxLayout;
    bar.right->setContentsMargins(0, 0, 0, 0);
    bar.right->setSpacing(kToolbarSpacing);
    row->addLayout(bar.left);
    row->addStretch(1);
    row->addLayout(bar.right);
    return bar;
}

void applyFormLayout(QFormLayout* form)
{
    if (!form)
        return;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(kFormHSpacing);
    form->setVerticalSpacing(kFormVSpacing);
}

void alignFormLabels(const QList<QFormLayout*>& forms, int minWidth)
{
    QList<QWidget*> labels;
    int widest = minWidth;
    for (QFormLayout* form : forms) {
        if (!form)
            continue;
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem* item = form->itemAt(row, QFormLayout::LabelRole);
            QWidget* label = item ? item->widget() : nullptr;
            if (!label)
                continue;
            labels << label;
            widest = qMax(widest, label->sizeHint().width());
        }
    }
    for (QWidget* label : labels)
        label->setMinimumWidth(widest);
}

Section makeSection(QWidget* parent, const QString& title, const QString& description)
{
    Section s;
    s.card = new QFrame(parent);
    s.card->setObjectName(QStringLiteral("cardPanel"));
    s.body = new QVBoxLayout(s.card);
    s.body->setContentsMargins(kSectionPadX, kSectionPadY, kSectionPadX, kSectionPadY);
    s.body->setSpacing(kFormVSpacing);
    if (!title.isEmpty()) {
        auto* label = new QLabel(title, s.card);
        label->setObjectName(QStringLiteral("sectionTitle"));
        s.body->addWidget(label);
    }
    if (!description.isEmpty()) {
        auto* label = new QLabel(description, s.card);
        label->setObjectName(QStringLiteral("sectionDesc"));
        label->setWordWrap(true);
        s.body->addWidget(label);
    }
    if (!title.isEmpty() || !description.isEmpty())
        s.body->addSpacing(4); // antet → conținut
    return s;
}

QFormLayout* addSectionForm(const Section& section)
{
    auto* form = new QFormLayout; // fără părinte: îl adoptă body-ul
    applyFormLayout(form);
    section.body->addLayout(form);
    return form;
}

void finishForms(const QList<QFormLayout*>& forms)
{
    alignFormLabels(forms, kFormLabelW);
    for (QFormLayout* form : forms) {
        if (!form)
            continue;
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem* item = form->itemAt(row, QFormLayout::FieldRole);
            QWidget* field = item ? item->widget() : nullptr;
            if (qobject_cast<QLineEdit*>(field) || qobject_cast<QComboBox*>(field)
                || qobject_cast<QAbstractSpinBox*>(field))
                field->setMaximumWidth(kFieldMaxW);
        }
    }
}

QLineEdit* makeSearch(QWidget* parent, const QString& placeholder)
{
    auto* edit = new QLineEdit(parent);
    edit->setObjectName(QStringLiteral("mimSearch"));
    edit->setPlaceholderText(placeholder);
    edit->setClearButtonEnabled(true);
    edit->setMinimumWidth(kSearchMin);
    edit->setMaximumWidth(kSearchMax);
    QAction* lupa = edit->addAction(makeIcon(icon::Kind::Search, muted(), 15),
                                    QLineEdit::LeadingPosition);
    // Re-tintare la schimbarea temei (icon-ul e un pixmap fix).
    QObject::connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed,
                     lupa, [lupa]() {
                         lupa->setIcon(makeIcon(icon::Kind::Search, muted(), 15));
                     });
    return edit;
}

QWidget* makeEmptyState(QWidget* parent, int iconKind, const QString& title,
                        const QString& subtitle)
{
    auto* host = new QWidget(parent);
    host->setObjectName(QStringLiteral("emptyState"));
    auto* lay = new QVBoxLayout(host);
    lay->setAlignment(Qt::AlignCenter);
    lay->setSpacing(10);

    // Icon într-un disc tentat (badge) — arată intenționat/„designed", nu un
    // icon care plutește singur în gol.
    auto* iconLabel = new QLabel(host);
    iconLabel->setAlignment(Qt::AlignCenter);
    const auto paintBadge = [iconLabel, iconKind]() {
        const int d = 68;
        const qreal dpr = iconLabel->devicePixelRatioF();
        QPixmap pm(qRound(d * dpr), qRound(d * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        // Disc pe tonul de hover al temei (alb translucid pe dark, umbră caldă
        // pe light) — albul fix dispărea complet pe fundalul light.
        QColor disc = theme::ThemeManager::instance()->color(QStringLiteral("hover"));
        disc.setAlpha(qMin(255, disc.alpha() + 6));
        p.setBrush(disc);
        // Disc pe temele rotunjite, pătrat pe „Square" (ca restul formelor).
        p.drawRoundedRect(QRectF(0, 0, d, d), corner(d / 2.0), corner(d / 2.0));
        const int is = 30;
        p.drawPixmap((d - is) / 2, (d - is) / 2,
                     makeIcon(static_cast<icon::Kind>(iconKind), muted(), is, 1.6)
                         .pixmap(is, is));
        p.end();
        iconLabel->setPixmap(pm);
    };
    paintBadge();
    QObject::connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed,
                     iconLabel, paintBadge);

    auto* titleLabel = new QLabel(title, host);
    titleLabel->setObjectName(QStringLiteral("emptyTitle"));
    titleLabel->setAlignment(Qt::AlignCenter);

    lay->addWidget(iconLabel);
    lay->addWidget(titleLabel);
    if (!subtitle.isEmpty()) {
        auto* subLabel = new QLabel(subtitle, host);
        subLabel->setObjectName(QStringLiteral("emptySub"));
        subLabel->setAlignment(Qt::AlignCenter);
        subLabel->setWordWrap(true);
        lay->addWidget(subLabel);
    }
    return host;
}

QWidget* makeNoResultsState(QWidget* parent)
{
    return makeEmptyState(parent, static_cast<int>(icon::Kind::Search),
                          QStringLiteral("No results"),
                          QStringLiteral("Try a different search or clear the filter."));
}

void showListState(QWidget* table, QWidget* emptyState, QWidget* noResults, int rows,
                   const QString& query, bool filtered)
{
    const bool narrowed = filtered || !query.isEmpty();
    const bool showNoResults = rows == 0 && narrowed;
    if (showNoResults) {
        if (auto* title = noResults->findChild<QLabel*>(QStringLiteral("emptyTitle"))) {
            // The search text is user input: plain text, and short enough that
            // the centered title never widens the page.
            title->setTextFormat(Qt::PlainText);
            const QString shown = query.size() > 40 ? query.left(39) + QStringLiteral("…")
                                                    : query;
            title->setText(shown.isEmpty()
                               ? trText(QStringLiteral("No results"))
                               : trText(QStringLiteral("No results for “%1”")).arg(shown));
        }
    }
    table->setVisible(rows > 0);
    emptyState->setVisible(rows == 0 && !narrowed);
    noResults->setVisible(showNoResults);
}

void configureTable(QAbstractItemView* view, int rowHeight)
{
    if (!view)
        return;
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setAlternatingRowColors(true);

    if (auto* table = qobject_cast<QTableView*>(view)) {
        table->setShowGrid(false);
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(rowHeight);
        table->horizontalHeader()->setSectionsClickable(false);
    } else if (auto* tree = qobject_cast<QTreeView*>(view)) {
        tree->header()->setSectionsClickable(false);
    }
}

QWidget* rowActions(QWidget* parent, QHBoxLayout** outLayout)
{
    auto* cell = new ActionsCell(parent);
    cell->setObjectName(QStringLiteral("mimOpsCell"));
    auto* lay = new QHBoxLayout(cell);
    lay->setContentsMargins(6, 0, 6, 0); // vertical 0 → centrat, fără overflow
    lay->setSpacing(6);
    lay->setAlignment(Qt::AlignVCenter);
    if (outLayout)
        *outLayout = lay;
    return cell;
}

void fitDialog(QDialog* dialog, DialogSize size, int minHeight)
{
    if (!dialog)
        return;
    // Aceeași formă ca fereastra principală (bară de titlu integrată + ramă),
    // aplicată înainte de măsurare ca bara să intre în calculul înălțimii.
    applyDialogChrome(dialog);

    // Mărimi STANDARD pe clase (lățimea e fixă pe clasă, nu după conținut).
    int baseW = 760;
    int baseH = 0;
    switch (size) {
    case DialogSize::Compact: baseW = 600; break;
    case DialogSize::Form: baseW = 760; break;
    case DialogSize::Wide: baseW = 980; baseH = 700; break;
    case DialogSize::Large: baseW = 1240; baseH = 820; break;
    }
    baseH = qMax(baseH, minHeight);

    // Spațiul disponibil: fereastra principală (dacă e), altfel ecranul.
    QSize avail;
    if (QWidget* p = dialog->parentWidget())
        avail = p->window()->size();
    if (QScreen* screen = dialog->screen()) {
        const QSize scr = screen->availableGeometry().size();
        avail = avail.isValid() ? avail.boundedTo(scr) : scr;
    }
    if (!avail.isValid())
        avail = kWindowSize;
    const int maxW = qRound(avail.width() * 0.92);
    const int maxH = qRound(avail.height() * 0.92);

    // Lățimea = cea standard a clasei; crește doar dacă minimul conținutului
    // chiar nu încape. Înălțimea = conținutul (sizeHint), cel puțin clasa.
    dialog->adjustSize();
    const QSize hint = dialog->sizeHint();
    const int w = qMin(maxW, qMax(baseW, dialog->minimumSizeHint().width()));
    const int h = qMin(maxH, qMax(baseH, hint.height()));
    dialog->resize(w, h);
    // Lățime minimă: nu lăsăm userul să-l strângă până taie conținutul.
    // Înălțimea minimă o lasă layout-ului (SetDefaultConstraint): un mesaj
    // care apare după deschidere (rezultat Test, eroare) poate mări fereastra.
    dialog->setMinimumWidth(qMin(w, qMax(dialog->minimumSizeHint().width(), 460)));
}

void applyDialogLayout(QBoxLayout* root)
{
    if (!root)
        return;
    namespace layout = tokens::layout;
    root->setContentsMargins(layout::kDialogPadX, layout::kDialogPadTop, layout::kDialogPadX,
                             layout::kDialogPadBot);
    root->setSpacing(layout::kDialogSpacing);
}

void setLabeledIcon(QAbstractButton* button, const QIcon& icon)
{
    if (!button)
        return;
    constexpr int kGap = 5;  // + cei ~2-4px ai lui Qt ≈ 7px vizual
    constexpr int kDrop = 1; // aliniere optică cu linia textului
    // Același DPR ca makeIcon (qApp, cel mai mare dintre ecrane): cu DPR-ul
    // butonului, pe monitoare cu scalări diferite pixmap-ul nativ era
    // reinterpretat la alt DPR și glyph-ul ieșea mărit.
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    // Cerem mai mult decât există: pixmap-ul nativ (makeIcon) nu se mărește.
    const QPixmap glyph = icon.pixmap(QSize(64, 64), dpr);
    if (glyph.isNull()) {
        button->setIcon(icon);
        return;
    }
    const QSize g = glyph.deviceIndependentSize().toSize();
    const QSize canvasSize(g.width() + kGap, g.height() + 2 * kDrop);
    QPixmap canvas(qRound(canvasSize.width() * dpr), qRound(canvasSize.height() * dpr));
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.drawPixmap(QPointF(0, 2 * kDrop), glyph);
    p.end();
    button->setIcon(QIcon(canvas));
    button->setIconSize(canvasSize);
}

QToolButton* rowIconButton(QWidget* parent, int iconKind, const QColor& color,
                           const QString& tooltip)
{
    auto* b = new QToolButton(parent);
    b->setObjectName(QStringLiteral("mimIconBtn"));
    b->setIcon(ui::makeIcon(static_cast<ui::icon::Kind>(iconKind), color, 16));
    b->setToolTip(tooltip);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedSize(kIconBtn, kIconBtn);
    return b;
}

} // namespace app::ui::page
