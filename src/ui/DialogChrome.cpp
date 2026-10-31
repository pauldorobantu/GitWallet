#include "ui/DialogChrome.h"

#include "ui/Icons.h"
#include "ui/ThemeManager.h"

#include <QDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QWindow>

namespace app::ui {

namespace {

constexpr int kEdge = 6; // zona de redimensionare pe laturi (px)

// ThemeManager aplică chrome-ul la PRIMUL ChildAdded — încă în constructorul
// QMessageBox/QProgressDialog. setWindowFlags() trimite atunci StyleChange/
// FontChange (cu QSS-ul aplicației activ), iar changeEvent()-ul lor folosește
// membri încă necreați (eticheta iconiței / a progresului) → crash la ORICE
// confirmare (ex. toate butoanele Delete). Notificările se înghit cât durează
// schimbarea flag-urilor; stilul real se aplică oricum la Polish, la afișare.
class ChangeEventMute : public QObject
{
public:
    bool eventFilter(QObject*, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::StyleChange:
        case QEvent::FontChange:
        case QEvent::PaletteChange:
        case QEvent::LanguageChange:
        case QEvent::LocaleChange:
        case QEvent::LayoutDirectionChange:
            return true;
        default:
            return false;
        }
    }
};

QColor themeColor(const char* role)
{
    return theme::ThemeManager::instance()->color(QLatin1String(role));
}

// Rama de 1px desenată PESTE conținut (transparentă la mouse), ca rama
// #appRoot a ferestrei principale.
class ChromeFrame : public QWidget
{
public:
    explicit ChromeFrame(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(QPen(themeColor("frame"), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};

// Bara de titlu: același stil ca bara ferestrei principale (#titleBar,
// butonul de închidere #tbClose cu hover roșu), tras cu mouse-ul mută fereastra.
class ChromeTitle : public QWidget
{
public:
    ChromeTitle(QDialog* dialog)
        : QWidget(dialog)
        , dialog_(dialog)
    {
        setObjectName(QStringLiteral("titleBar"));
        setAttribute(Qt::WA_StyledBackground, true);
        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(16, 0, 0, 0);
        lay->setSpacing(8);

        title_ = new QLabel(this);
        title_->setObjectName(QStringLiteral("appName"));
        lay->addWidget(title_, 1, Qt::AlignVCenter);

        close_ = new QToolButton(this);
        close_->setObjectName(QStringLiteral("tbClose"));
        close_->setFixedSize(46, kDialogTitleHeight);
        close_->setIconSize(QSize(16, 16));
        close_->setToolTip(QStringLiteral("Close"));
        close_->setFocusPolicy(Qt::NoFocus);
        QObject::connect(close_, &QToolButton::clicked, dialog, &QDialog::reject);
        lay->addWidget(close_, 0, Qt::AlignTop);

        refresh();
    }

    void refresh()
    {
        const QString t = dialog_->windowTitle().trimmed();
        title_->setText(t);
        close_->setIcon(makeCaptionIcon(icon::Kind::Close, themeColor("textSoft"), 16));
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) {
            if (QWindow* w = window()->windowHandle())
                w->startSystemMove();
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

private:
    QDialog* dialog_;
    QLabel* title_ = nullptr;
    QToolButton* close_ = nullptr;
};

// Ține bara + rama la mărimea dialogului, titlul sincron și redimensionarea
// de pe laturi (fără rama nativă Windows nu mai există margini de resize).
class ChromeController : public QObject
{
public:
    ChromeController(QDialog* dialog, ChromeTitle* title, ChromeFrame* frame)
        : QObject(dialog)
        , dialog_(dialog)
        , title_(title)
        , frame_(frame)
    {
        dialog->installEventFilter(this);
        dialog->setMouseTracking(true);
        QObject::connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed,
                         this, [this]() {
                             title_->refresh();
                             frame_->update();
                         });
        layoutChrome();
    }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (obj != dialog_)
            return false;
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
            layoutChrome();
            break;
        case QEvent::WindowTitleChange:
            title_->refresh();
            break;
        case QEvent::MouseMove: {
            const auto* me = static_cast<QMouseEvent*>(event);
            if (me->buttons() == Qt::NoButton)
                updateCursor(edgesAt(me->position().toPoint()));
            break;
        }
        case QEvent::MouseButtonPress: {
            const auto* me = static_cast<QMouseEvent*>(event);
            const Qt::Edges edges = edgesAt(me->position().toPoint());
            if (me->button() == Qt::LeftButton && edges) {
                if (QWindow* w = dialog_->windowHandle())
                    w->startSystemResize(edges);
                return true;
            }
            break;
        }
        case QEvent::Leave:
            if (resizeCursor_) {
                dialog_->unsetCursor();
                resizeCursor_ = false;
            }
            break;
        default:
            break;
        }
        return false;
    }

private:
    void layoutChrome()
    {
        const int w = dialog_->width();
        title_->setGeometry(1, 1, qMax(0, w - 2), kDialogTitleHeight);
        frame_->setGeometry(0, 0, w, dialog_->height());
        title_->raise();
        frame_->raise();
    }

    bool resizable() const
    {
        return dialog_->minimumSize() != dialog_->maximumSize();
    }

    Qt::Edges edgesAt(const QPoint& pos) const
    {
        if (!resizable() || dialog_->isMaximized())
            return {};
        Qt::Edges e;
        if (pos.x() < kEdge)
            e |= Qt::LeftEdge;
        if (pos.x() >= dialog_->width() - kEdge)
            e |= Qt::RightEdge;
        if (pos.y() >= dialog_->height() - kEdge)
            e |= Qt::BottomEdge;
        return e; // sus = bara de titlu (mutare), nu redimensionare
    }

    void updateCursor(Qt::Edges e)
    {
        Qt::CursorShape shape = Qt::ArrowCursor;
        if ((e & Qt::LeftEdge) && (e & Qt::BottomEdge))
            shape = Qt::SizeBDiagCursor;
        else if ((e & Qt::RightEdge) && (e & Qt::BottomEdge))
            shape = Qt::SizeFDiagCursor;
        else if (e & (Qt::LeftEdge | Qt::RightEdge))
            shape = Qt::SizeHorCursor;
        else if (e & Qt::BottomEdge)
            shape = Qt::SizeVerCursor;
        if (shape != Qt::ArrowCursor) {
            dialog_->setCursor(shape);
            resizeCursor_ = true;
        } else if (resizeCursor_) {
            dialog_->unsetCursor();
            resizeCursor_ = false;
        }
    }

    QDialog* dialog_;
    ChromeTitle* title_;
    ChromeFrame* frame_;
    bool resizeCursor_ = false;
};

} // namespace

void applyDialogChrome(QDialog* dialog)
{
    if (!dialog || dialog->property("appChrome").toBool())
        return;
    // Fereastra nativă există deja (afișată): schimbarea flag-urilor ar
    // recrea-o și ascunde-o — o lăsăm cum e.
    if (dialog->testAttribute(Qt::WA_WState_Created) && dialog->isVisible())
        return;
    dialog->setProperty("appChrome", true);

    {
        ChangeEventMute mute;
        dialog->installEventFilter(&mute);
        dialog->setWindowFlags((dialog->windowFlags() & ~Qt::WindowContextHelpButtonHint)
                               | Qt::Dialog | Qt::FramelessWindowHint);
        dialog->removeEventFilter(&mute);
    }

    // Conținutul (layout-ul dialogului) începe sub bara de titlu, în interiorul ramei.
    const QMargins m = dialog->contentsMargins();
    dialog->setContentsMargins(m.left() + 1, m.top() + kDialogTitleHeight + 1,
                               m.right() + 1, m.bottom() + 1);

    auto* title = new ChromeTitle(dialog);
    auto* frame = new ChromeFrame(dialog);
    new ChromeController(dialog, title, frame);
}

} // namespace app::ui
