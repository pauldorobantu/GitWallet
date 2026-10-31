#pragma once

#include <QWidget>

class QVBoxLayout;
class QStackedWidget;

namespace app::ui {

class TitleBar;
class SideNav;

// The application window: the same frameless shell, title bar and sidebar as
// FlowSuite (reused design system), hosting the wallet's pages. Stage 1 wires
// the shell and navigation; the pages fill in as the wallet grows.
class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void refreshFrame();

    QVBoxLayout* root_ = nullptr;
    TitleBar* title_ = nullptr;
    SideNav* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;
};

} // namespace app::ui
