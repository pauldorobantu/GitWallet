#pragma once

#include <QHash>
#include <QObject>
#include <QString>

class QWidget;

namespace app::ui {

// Traducere a GUI-ului la rulare, fără a rescrie fiecare apel de string.
// La schimbarea limbii, parcurge arborele de widget-uri și traduce textele
// vizibile (etichete, butoane, placeholdere, taburi, antete de tabel, titluri)
// căutându-le într-un dicționar central. Stringurile necunoscute (date: email,
// IP, nume) rămân neschimbate. Originalul (engleză) e păstrat într-o proprietate
// dinamică, așa că trecerea înapoi la English și între limbi funcționează.
// Limbi: en (sursă), es, fr, de, ru.
class LanguageManager : public QObject
{
    Q_OBJECT
public:
    static LanguageManager* instance();

    QString language() const { return current_; }   // cod: en/es/fr/de/ru
    void loadFromSettings();                         // citește ui.language
    void setLanguage(const QString& code);           // salvează + aplică + emite

    // Traduce un string din engleză în limba curentă (identitate pt en/necunoscut).
    QString translate(const QString& english) const;

    // Aplică traducerea pe tot arborele unui widget (recursiv). Apelat automat
    // la schimbarea limbii și când un widget nou e afișat.
    void applyTo(QWidget* root) const;
    void applyToApp() const; // toate ferestrele top-level

signals:
    void changed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    LanguageManager();
    void buildDictionary();
    void translateWidget(QWidget* w) const;

    QString current_ = QStringLiteral("en");
    // dict_[lang][englishSource] = translated.
    QHash<QString, QHash<QString, QString>> dict_;
};

// Dictionary lookup for text built at runtime (identity for en / unknown keys).
// Translate the English template FIRST, then fill it in with .arg().
inline QString trText(const QString& english)
{
    return LanguageManager::instance()->translate(english);
}

} // namespace app::ui
