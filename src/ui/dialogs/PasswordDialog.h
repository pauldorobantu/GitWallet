#pragma once

#include <QDialog>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace app::ui {

// Asks for a password, and waits with the work it is for: making a key takes about a second, and what
// goes wrong is said here, with what was typed still there. Which fields it has depends on what it is
// for. What is typed is wiped as it closes.
class PasswordDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Mode {
        SetApp,     // a new password for the app
        ChangeApp,  // the one in use, and a new one
        RemoveApp,  // the one in use
        MakeBackup, // a new password for one file
        OpenBackup, // the password of one file
    };
    struct Filled {
        QString current;
        QString fresh;
        bool deleteOldFiles = false;
    };
    // `done` is called with an empty string when the work has gone well, and the dialog then closes;
    // otherwise it says why, and the dialog stays.
    using Finish = std::function<void(const QString& error)>;
    using Work = std::function<void(const Filled& filled, const Finish& finish)>;

    // `oldFiles`: older wallet files kept beside this one, which a password would not reach.
    PasswordDialog(Mode mode, int oldFiles, const QString& about, const Work& work, QWidget* parent = nullptr);
    ~PasswordDialog() override;

    void done(int result) override;

private:
    void updateState();
    void go();
    QLineEdit* addField(const QString& title, const char* name);

    Mode mode_;
    Work work_;
    QLineEdit* current_ = nullptr;
    QLineEdit* fresh_ = nullptr;
    QLineEdit* repeat_ = nullptr;
    QCheckBox* understood_ = nullptr;
    QCheckBox* deleteOld_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* ok_ = nullptr;
    QPushButton* cancel_ = nullptr;
    bool busy_ = false;
};

} // namespace app::ui
