#pragma once

#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

class QTimer;
class QWidget;

namespace app {

namespace ui {
class MainWindow;
}

// End-to-end check of the real UI, driven like a user would: it types into the
// add-wallet dialog, adds seeds and public keys, presses the buttons, switches
// pages, changes a setting, closes and reopens the window, removes wallets — and
// after every step checks what the app shows and saves a screenshot. Started by
// --walkthrough <dir>, which main() only accepts together with --data-dir, so it
// never touches the real wallets; the only seeds it ever types are public BIP39
// test mnemonics. Writes <dir>/walkthrough.json, addresses.csv and NN-step.png
// files; quits with 0 when every check passed, 1 otherwise.
class Walkthrough : public QObject
{
public:
    using WindowFactory = std::function<ui::MainWindow*()>;

    Walkthrough(WindowFactory makeWindow, const QString& outDir, QObject* parent = nullptr);

    void start();

private:
    struct Step {
        QString name;
        std::function<void()> act;      // what the "user" does
        std::function<bool()> ready;    // the app has reacted (polled every 100 ms)
        int timeoutMs = 10000;
        std::function<void()> check;    // records pass / fail through expect()
        std::function<QWidget*()> shot; // what to photograph (null = the main window)
    };

    void buildSteps();
    void runStep(int index);
    void finishStep(int index, bool timedOut);
    void finish();

    void expect(bool ok, const QString& what);
    QWidget* findButton(QWidget* root, const QString& text) const;
    QString visibleText(QWidget* root) const; // every visible label / button text, for checks

    WindowFactory makeWindow_;
    QString outDir_;
    QPointer<ui::MainWindow> window_;
    QList<Step> steps_;
    QTimer* poll_ = nullptr;
    QJsonArray results_;
    QJsonArray currentChecks_;
    int failures_ = 0;
    bool scanFinished_ = false;

    // Values remembered across steps.
    int rowsBeforeReopen_ = 0;
    QString givenOutAddress_;
    std::optional<bool> hideZeroWas_;
    bool hideDustWas_ = true;
    quint32 givenOutIndex_ = 0;
    QString totalsBeforeReopen_;
    quint32 removedWallet_ = 0;
};

} // namespace app
