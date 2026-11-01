#pragma once

#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QTimer;
class QWidget;

namespace app {

namespace ui {
class MainWindow;
}

// End-to-end check of the real UI, driven like a user would: it types into the
// import dialog, presses the buttons, switches pages, changes a setting, closes
// and reopens the window, removes the wallet — and after every step checks what
// the app shows and saves a screenshot. Started by --walkthrough <dir>, which
// main() only accepts together with --data-dir, so it never touches the real
// wallet; the only seed it ever types is the public BIP39 test mnemonic.
// Writes <dir>/walkthrough.json and NN-step.png files; quits with 0 when every
// check passed, 1 otherwise.
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
    QString totalsBeforeReopen_;
};

} // namespace app
