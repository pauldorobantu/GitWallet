#include "ui/dialogs/PasswordDialog.h"

#include "ui/DialogChrome.h"
#include "ui/LanguageManager.h"
#include "ui/PageKit.h"
#include "ui/design/Style.h"
#include "wallet/BulkImport.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace app::ui {

namespace {

constexpr int kLeastChars = 8;

void wipeField(QLineEdit* edit)
{
    if (!edit)
        return;
    const QSignalBlocker quiet(edit);
    edit->setText(QString(edit->text().size(), QLatin1Char(' ')));
    edit->clear();
}

} // namespace

QLineEdit* PasswordDialog::addField(const QString& title, const char* name, bool newOne)
{
    auto* column = static_cast<QVBoxLayout*>(layout());
    auto* label = new QLabel(title, this);
    column->addWidget(label);
    auto* edit = new QLineEdit(this);
    edit->setObjectName(QString::fromLatin1(name));
    edit->setEchoMode(QLineEdit::Password);
    edit->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    // A new one with room for one more, to say so rather than cut it where nothing shows: what is
    // typed where it is asked for afterwards would not be what was set.
    edit->setMaxLength(newOne ? kMostChars + 1 : kMostChars);
    edit->setAccessibleName(title);
    column->addWidget(edit);
    connect(edit, &QLineEdit::textChanged, this, &PasswordDialog::updateState);
    return edit;
}

PasswordDialog::PasswordDialog(Mode mode, const OldFiles& oldFiles, const QString& about, const Work& work, QWidget* parent)
    : QDialog(parent), mode_(mode), work_(work)
{
    setObjectName(QStringLiteral("passwordDialog"));
    QString title;
    QString okText;
    QString warning;
    switch (mode_) {
    case Mode::SetApp:
        title = QStringLiteral("Set a password");
        okText = QStringLiteral("Set password");
        warning = QStringLiteral("If you forget it, nobody can open what is saved here, GitWallet included. Your coins are safe either way: they are on their seeds. You would add those again, and lose the labels, notes and addresses given out that only this PC knows, unless a backup holds them.");
        break;
    case Mode::ChangeApp:
        title = QStringLiteral("Change the password");
        okText = QStringLiteral("Change password");
        warning = QStringLiteral("If you forget the new one, nobody can open what is saved here, GitWallet included.");
        break;
    case Mode::RemoveApp:
        title = QStringLiteral("Remove the password");
        okText = QStringLiteral("Remove password");
        warning = QStringLiteral("Anyone using this Windows account can then open GitWallet and see your wallets, and so can any program running under it.");
        break;
    case Mode::MakeBackup:
        title = QStringLiteral("Make a backup");
        okText = QStringLiteral("Make backup");
        warning = QStringLiteral("Only this password opens the file. It can differ from the app password. Keep it separate from the file: if you forget it, nobody can open it, GitWallet included.");
        break;
    case Mode::OpenBackup:
        title = QStringLiteral("Restore from a backup");
        okText = QStringLiteral("Restore");
        warning = QStringLiteral("Nothing here is removed: wallets the file holds and this PC does not are added, and addresses given out, labels and notes come along.");
        break;
    }
    setWindowTitle(trText(title));
    applyDialogChrome(this);
    auto* column = new QVBoxLayout(this);
    page::applyDialogLayout(column);

    if (!about.isEmpty()) {
        auto* where = new QLabel(about, this);
        where->setTextFormat(Qt::PlainText);
        where->setWordWrap(true);
        setTone(where, Tone::Muted);
        column->addWidget(where);
    }
    auto* note = new QLabel(trText(warning), this);
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    column->addWidget(note);

    if (mode_ == Mode::ChangeApp || mode_ == Mode::RemoveApp)
        current_ = addField(trText(QStringLiteral("Current password")), "passwordCurrent");
    if (mode_ == Mode::OpenBackup)
        current_ = addField(trText(QStringLiteral("Password of the backup")), "passwordCurrent");
    if (mode_ == Mode::SetApp || mode_ == Mode::ChangeApp || mode_ == Mode::MakeBackup) {
        fresh_ = addField(trText(QStringLiteral("New password")), "passwordNew", true);
        repeat_ = addField(trText(QStringLiteral("Repeat the new password")), "passwordRepeat", true);
        auto* hint = new QLabel(trText(QStringLiteral("At least %1 characters. A sentence of five or six words is long and easy to remember.")).arg(kLeastChars), this);
        hint->setWordWrap(true);
        setTone(hint, Tone::Muted);
        column->addWidget(hint);
    }
    if (mode_ == Mode::SetApp || mode_ == Mode::MakeBackup) {
        understood_ = new QCheckBox(trText(QStringLiteral("I understand that nobody can get it back for me")), this);
        understood_->setObjectName(QStringLiteral("passwordUnderstood"));
        column->addWidget(understood_);
        connect(understood_, &QCheckBox::toggled, this, &PasswordDialog::updateState);
    }
    if ((mode_ == Mode::SetApp || mode_ == Mode::ChangeApp) && oldFiles.formatCopies > 0) {
        // On by default: left there, such a copy opens today's database with no password at all.
        deleteCopies_ = new QCheckBox(trText(QStringLiteral("Also delete the %1 older copies of this wallet file: no password reaches them, and they open today's database")).arg(oldFiles.formatCopies), this);
        deleteCopies_->setObjectName(QStringLiteral("passwordDeleteCopies"));
        deleteCopies_->setChecked(true);
        column->addWidget(deleteCopies_);
    }
    if ((mode_ == Mode::SetApp || mode_ == Mode::ChangeApp) && oldFiles.unread > 0) {
        deleteUnread_ = new QCheckBox(trText(QStringLiteral("Also delete the %1 older files that could not be read: no password reaches them, and they may be the only copy of other wallets")).arg(oldFiles.unread), this);
        deleteUnread_->setObjectName(QStringLiteral("passwordDeleteUnread"));
        column->addWidget(deleteUnread_);
    }

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("passwordStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    column->addWidget(status_);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(page::kToolbarSpacing);
    buttons->addStretch(1);
    cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
    ok_ = new QPushButton(trText(okText), this);
    ok_->setObjectName(mode_ == Mode::RemoveApp ? QStringLiteral("dangerButton") : QStringLiteral("primaryButton"));
    ok_->setDefault(true);
    buttons->addWidget(cancel_);
    buttons->addWidget(ok_);
    column->addLayout(buttons);
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    connect(ok_, &QPushButton::clicked, this, &PasswordDialog::go);

    page::fitDialog(this, page::DialogSize::Compact);
    updateState();
}

PasswordDialog::~PasswordDialog()
{
    wipeField(current_);
    wipeField(fresh_);
    wipeField(repeat_);
}

void PasswordDialog::updateState()
{
    QString why;
    bool ready = !busy_;
    if (current_ && current_->text().isEmpty())
        ready = false;
    if (fresh_) {
        const QString fresh = fresh_->text();
        if (fresh.isEmpty()) {
            ready = false;
        } else if (fresh.size() < kLeastChars) {
            ready = false;
            why = trText(QStringLiteral("At least %1 characters.")).arg(kLeastChars);
        } else if (fresh.size() > kMostChars) {
            ready = false;
            why = trText(QStringLiteral("At most %1 characters.")).arg(kMostChars);
        } else if (wallet::looksLikeSecret(fresh)) {
            // A seed in the field would be one more place for it to be typed.
            ready = false;
            why = trText(QStringLiteral("That looks like seed words or a private key. Choose something else: it should not be typed here."));
        } else if (repeat_->text() != fresh) {
            ready = false;
            if (!repeat_->text().isEmpty())
                why = trText(QStringLiteral("The two do not match."));
        } else if (current_ && current_->text() == fresh) {
            ready = false;
            why = trText(QStringLiteral("That is already the current password."));
        }
    }
    if (understood_ && !understood_->isChecked())
        ready = false;
    ok_->setEnabled(ready);
    if (!busy_)
        setStatus(status_, why, Tone::Warning);
}

void PasswordDialog::go()
{
    if (!ok_->isEnabled() || busy_)
        return;
    busy_ = true;
    ok_->setEnabled(false);
    cancel_->setEnabled(false);
    setStatus(status_, trText(QStringLiteral("Working… checking a password takes about a second, on purpose.")), Tone::Muted);
    Filled filled;
    filled.current = current_ ? current_->text() : QString();
    filled.fresh = fresh_ ? fresh_->text() : QString();
    filled.deleteFormatCopies = deleteCopies_ && deleteCopies_->isChecked();
    filled.deleteUnreadFiles = deleteUnread_ && deleteUnread_->isChecked();
    const QPointer<PasswordDialog> self(this);
    work_(filled, [self](const QString& error) {
        if (!self)
            return;
        self->busy_ = false;
        self->cancel_->setEnabled(true);
        if (error.isEmpty()) {
            self->accept();
            return;
        }
        self->updateState();
        setStatus(self->status_, error, Tone::Error);
    });
}

void PasswordDialog::done(int result)
{
    // The work has its own end, which goes by this.
    if (busy_ && result != QDialog::Accepted)
        return;
    wipeField(current_);
    wipeField(fresh_);
    wipeField(repeat_);
    QDialog::done(result);
}

} // namespace app::ui
