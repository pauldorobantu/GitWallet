#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace app::ui {

// What the window shows in place of everything else while it is locked: nothing of the wallets, and
// a field for the password.
class LockPage : public QWidget
{
    Q_OBJECT
public:
    explicit LockPage(QWidget* parent = nullptr);

    void reset();                  // ready for the next try, with nothing in the field
    void setBusy(bool busy);       // a key takes about a second to make
    void setError(const QString& error);
    QLineEdit* field() const { return field_; }

signals:
    void unlockRequested(const QString& password);
    void forgotRequested();

private:
    void tryNow();

    QLineEdit* field_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* unlock_ = nullptr;
    QPushButton* forgot_ = nullptr;
};

} // namespace app::ui
