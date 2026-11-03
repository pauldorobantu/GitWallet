#include "ui/pages/LockPage.h"

#include "ui/dialogs/PasswordDialog.h"

#include "ui/AppIcon.h"
#include "ui/Icons.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/ThemeManager.h"
#include "ui/design/Style.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace app::ui {

namespace {

constexpr int kFieldWidth = 340;

} // namespace

LockPage::LockPage(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("lockPage"));
    auto* root = new QVBoxLayout(this);
    root->addStretch(1);

    auto* mark = new QLabel(this);
    const auto paintMark = [mark] {
        mark->setPixmap(makeIcon(icon::Kind::Lock, page::muted(), 48, 1.5).pixmap(48, 48));
    };
    paintMark();
    connect(theme::ThemeManager::instance(), &theme::ThemeManager::changed, mark, paintMark);
    root->addWidget(mark, 0, Qt::AlignHCenter);
    root->addSpacing(page::kFormVSpacing);

    auto* title = new QLabel(QStringLiteral("GitWallet is locked"), this);
    title->setObjectName(QStringLiteral("lockTitle"));
    QFont bigger = title->font();
    bigger.setPixelSize(bigger.pixelSize() > 0 ? bigger.pixelSize() + 6 : 20);
    bigger.setWeight(QFont::DemiBold);
    title->setFont(bigger);
    root->addWidget(title, 0, Qt::AlignHCenter);

    auto* hint = new QLabel(QStringLiteral("Enter the password to see your wallets."), this);
    setTone(hint, Tone::Muted);
    root->addWidget(hint, 0, Qt::AlignHCenter);
    root->addSpacing(page::kPageSpacing);

    field_ = new QLineEdit(this);
    field_->setObjectName(QStringLiteral("lockPassword"));
    field_->setEchoMode(QLineEdit::Password);
    field_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    field_->setPlaceholderText(QStringLiteral("Password"));
    field_->setAccessibleName(QStringLiteral("Password"));
    field_->setFixedWidth(kFieldWidth);
    field_->setMaxLength(PasswordDialog::kMostChars);
    root->addWidget(field_, 0, Qt::AlignHCenter);
    root->addSpacing(page::kFormVSpacing);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("lockStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setAlignment(Qt::AlignHCenter);
    status_->setFixedWidth(kFieldWidth);
    status_->hide();
    root->addWidget(status_, 0, Qt::AlignHCenter);

    unlock_ = new QPushButton(QStringLiteral("Unlock"), this);
    unlock_->setObjectName(QStringLiteral("primaryButton"));
    unlock_->setDefault(true);
    unlock_->setFixedWidth(kFieldWidth);
    root->addWidget(unlock_, 0, Qt::AlignHCenter);

    forgot_ = new QPushButton(QStringLiteral("Forgot the password?"), this);
    forgot_->setObjectName(QStringLiteral("lockForgot"));
    forgot_->setFlat(true);
    root->addWidget(forgot_, 0, Qt::AlignHCenter);
    root->addStretch(2);

    connect(unlock_, &QPushButton::clicked, this, &LockPage::tryNow);
    connect(field_, &QLineEdit::returnPressed, this, &LockPage::tryNow);
    connect(forgot_, &QPushButton::clicked, this, &LockPage::forgotRequested);
}

void LockPage::tryNow()
{
    if (field_->text().isEmpty() || !unlock_->isEnabled())
        return;
    emit unlockRequested(field_->text());
}

void LockPage::reset()
{
    {
        const QSignalBlocker quiet(field_);
        field_->setText(QString(field_->text().size(), QLatin1Char(' ')));
        field_->clear();
    }
    setBusy(false);
    setError(QString());
}

void LockPage::setBusy(bool busy)
{
    field_->setEnabled(!busy);
    unlock_->setEnabled(!busy);
    forgot_->setEnabled(!busy);
    unlock_->setText(busy ? trText(QStringLiteral("Opening…")) : trText(QStringLiteral("Unlock")));
    if (!busy && isVisible())
        field_->setFocus();
}

void LockPage::setError(const QString& error)
{
    status_->setVisible(!error.isEmpty());
    if (!error.isEmpty())
        setStatus(status_, error, Tone::Error);
}

} // namespace app::ui
