#include "ui/LanguageManager.h"

#include "core/SettingsRepo.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QWidget>

#include <functional>

namespace app::ui {

namespace {

// Dicționar central. Doar chrome-ul vizibil (nu date). Stringurile lipsă rămân
// în engleză. en = sursa (cheia). Ordine: es, fr, de, ru.
struct Tr { const char* en; const char* es; const char* fr; const char* de; const char* ru; };

const Tr kTr[] = {
    // ---- GUI kit chrome (title bar, dialogs, tables, feedback) ----
    { "Settings", "Ajustes", "Paramètres", "Einstellungen", "Настройки" },
    { "About", "Acerca de", "À propos", "Über", "О программе" },
    { "Close", "Cerrar", "Fermer", "Schließen", "Закрыть" },
    { "Restore", "Restaurar", "Restaurer", "Wiederherstellen", "Восстановить" },
    { "enabled", "activado", "activé", "aktiviert", "включено" },
    { "disabled", "desactivado", "désactivé", "deaktiviert", "отключено" },
    { "ok", "ok", "ok", "ok", "ок" },
    { "blocked", "bloqueado", "bloqué", "blockiert", "заблокировано" },
    { "off", "apagado", "arrêté", "aus", "выкл" },
    { "active", "activo", "actif", "aktiv", "активно" },
    { "inactive", "inactivo", "inactif", "inaktiv", "неактивно" },
    { "No results", "Sin resultados", "Aucun résultat", "Keine Ergebnisse", "Ничего не найдено" },
    { "No results for “%1”", "Sin resultados para «%1»", "Aucun résultat pour « %1 »", "Keine Ergebnisse für „%1“", "Ничего не найдено по запросу «%1»" },
    { "Try a different search or clear the filter.", "Prueba otra búsqueda o quita el filtro.", "Essayez une autre recherche ou effacez le filtre.", "Versuchen Sie eine andere Suche oder entfernen Sie den Filter.", "Измените запрос или сбросьте фильтр." },
    // ---- GitWallet ----
    { "Wallet", "Billetera", "Portefeuille", "Wallet", "Кошелёк" },
    { "Overview", "Resumen", "Vue d'ensemble", "Übersicht", "Обзор" },
    { "Addresses", "Direcciones", "Adresses", "Adressen", "Адреса" },
    { "Send", "Enviar", "Envoyer", "Senden", "Отправить" },
    { "Total balance across every address, in BTC, ETH and USD.", "Saldo total de todas las direcciones, en BTC, ETH y USD.", "Solde total de toutes les adresses, en BTC, ETH et USD.", "Gesamtsaldo aller Adressen in BTC, ETH und USD.", "Общий баланс всех адресов в BTC, ETH и USD." },
    { "Every address derived from your seed, with its balance.", "Todas las direcciones derivadas de tu semilla, con su saldo.", "Toutes les adresses dérivées de votre phrase de récupération, avec leur solde.", "Jede aus deiner Seed-Phrase abgeleitete Adresse mit ihrem Saldo.", "Все адреса, полученные из вашей сид-фразы, с балансом." },
    { "Consolidate funds to one address.", "Reúne los fondos en una sola dirección.", "Regroupez les fonds sur une seule adresse.", "Guthaben auf eine Adresse zusammenführen.", "Соберите средства на один адрес." },
    { "Network, currency and appearance.", "Red, moneda y apariencia.", "Réseau, devise et apparence.", "Netzwerk, Währung und Darstellung.", "Сеть, валюта и оформление." },
};

} // namespace

LanguageManager* LanguageManager::instance()
{
    static LanguageManager mgr;
    return &mgr;
}

LanguageManager::LanguageManager()
{
    buildDictionary();
    if (qApp)
        qApp->installEventFilter(this);
}

void LanguageManager::buildDictionary()
{
    for (const Tr& t : kTr) {
        const QString en = QString::fromUtf8(t.en);
        dict_[QStringLiteral("es")].insert(en, QString::fromUtf8(t.es));
        dict_[QStringLiteral("fr")].insert(en, QString::fromUtf8(t.fr));
        dict_[QStringLiteral("de")].insert(en, QString::fromUtf8(t.de));
        dict_[QStringLiteral("ru")].insert(en, QString::fromUtf8(t.ru));
    }
}

void LanguageManager::loadFromSettings()
{
    QString code = SettingsRepo().get(QStringLiteral("ui.language"));
    static const QStringList ok = { QStringLiteral("en"), QStringLiteral("es"),
                                    QStringLiteral("fr"), QStringLiteral("de"),
                                    QStringLiteral("ru") };
    current_ = ok.contains(code) ? code : QStringLiteral("en");
}

void LanguageManager::setLanguage(const QString& code)
{
    if (code == current_)
        return;
    current_ = code;
    SettingsRepo().set(QStringLiteral("ui.language"), code);
    applyToApp();
    emit changed();
}

QString LanguageManager::translate(const QString& english) const
{
    if (current_ == QLatin1String("en"))
        return english;
    const auto langIt = dict_.constFind(current_);
    if (langIt == dict_.constEnd())
        return english;
    return langIt->value(english, english);
}

void LanguageManager::applyToApp() const
{
    const auto widgets = qApp->topLevelWidgets();
    for (QWidget* w : widgets)
        applyTo(w);
}

void LanguageManager::applyTo(QWidget* root) const
{
    if (!root)
        return;
    translateWidget(root);
    const auto kids = root->findChildren<QWidget*>();
    for (QWidget* w : kids)
        translateWidget(w);
    // Acțiuni din meniuri (QMenu/QToolButton).
    const auto acts = root->findChildren<QAction*>();
    for (QAction* a : acts) {
        if (a->text().isEmpty())
            continue;
        const QByteArray pk = "i18nOrig_act";
        QVariant v = a->property(pk.constData());
        QString orig = v.isValid() ? v.toString() : a->text();
        if (!v.isValid())
            a->setProperty(pk.constData(), orig);
        a->setText(translate(orig));
    }
}

void LanguageManager::translateWidget(QWidget* w) const
{
    if (!w)
        return;

    // Butoane din bara de titlu: nu le atingem (cod limbă / temă).
    const QString on = w->objectName();
    if (on == QLatin1String("tbLang") || on == QLatin1String("tbTheme"))
        return;
    // QHeaderView e un QAbstractItemView — nu-l tratăm ca view de date.
    if (qobject_cast<QHeaderView*>(w))
        return;

    // Traduce o proprietate text, păstrând actualizările dinamice ale aplicației.
    // Memorăm ultima valoare pe care AM aplicat-o noi; dacă textul curent diferă,
    // aplicația l-a schimbat (date/contor) → re-capturăm acel text ca sursă.
    // Setăm doar când traducerea chiar diferă (evită clobber la „4" → „4").
    auto applyText = [this, w](const char* key, const QString& current,
                               std::function<void(const QString&)> setter) {
        if (current.isEmpty())
            return;
        const QByteArray okOrig = QByteArray("i18nOrig_") + key;
        const QByteArray okLast = QByteArray("i18nLast_") + key;
        const QVariant last = w->property(okLast.constData());
        QString orig;
        if (!last.isValid() || current != last.toString()) {
            orig = current;                       // sursă nouă (prima dată / dinamic)
            w->setProperty(okOrig.constData(), orig);
        } else {
            orig = w->property(okOrig.constData()).toString();
        }
        const QString tr = translate(orig);
        w->setProperty(okLast.constData(), tr);
        if (tr != current)
            setter(tr);
    };

    if (auto* lbl = qobject_cast<QLabel*>(w)) {
        applyText("text", lbl->text(), [lbl](const QString& s) { lbl->setText(s); });
    } else if (auto* btn = qobject_cast<QAbstractButton*>(w)) {
        applyText("text", btn->text(), [btn](const QString& s) { btn->setText(s); });
    } else if (auto* gb = qobject_cast<QGroupBox*>(w)) {
        applyText("title", gb->title(), [gb](const QString& s) { gb->setTitle(s); });
    }

    if (auto* le = qobject_cast<QLineEdit*>(w))
        applyText("ph", le->placeholderText(),
                  [le](const QString& s) { le->setPlaceholderText(s); });

    if (auto* combo = qobject_cast<QComboBox*>(w)) {
        const QByteArray pk = "i18nOrig_items";
        QVariant v = combo->property(pk.constData());
        QStringList orig;
        if (v.isValid())
            orig = v.toStringList();
        else {
            for (int i = 0; i < combo->count(); ++i)
                orig << combo->itemText(i);
            combo->setProperty(pk.constData(), orig);
        }
        for (int i = 0; i < combo->count() && i < orig.size(); ++i) {
            const QString tr = translate(orig[i]);
            if (tr != combo->itemText(i))
                combo->setItemText(i, tr);
        }
    }

    if (auto* tabs = qobject_cast<QTabWidget*>(w)) {
        const QByteArray pk = "i18nOrig_tabs";
        QVariant v = tabs->property(pk.constData());
        QStringList orig;
        if (v.isValid())
            orig = v.toStringList();
        else {
            for (int i = 0; i < tabs->count(); ++i)
                orig << tabs->tabText(i);
            tabs->setProperty(pk.constData(), orig);
        }
        for (int i = 0; i < tabs->count() && i < orig.size(); ++i) {
            const QString tr = translate(orig[i]);
            if (tr != tabs->tabText(i))
                tabs->setTabText(i, tr);
        }
    }

    // Antete de tabel QTableWidget: traducem doar textele găsite în dicționar
    // (altfel lăsăm neschimbat — fără a le șterge în numere).
    if (auto* tw = qobject_cast<QTableWidget*>(w)) {
        const QByteArray pk = "i18nOrig_hdr";
        QVariant v = tw->property(pk.constData());
        QStringList orig;
        if (v.isValid())
            orig = v.toStringList();
        else {
            for (int c = 0; c < tw->columnCount(); ++c)
                orig << (tw->horizontalHeaderItem(c)
                             ? tw->horizontalHeaderItem(c)->text() : QString());
            tw->setProperty(pk.constData(), orig);
        }
        for (int c = 0; c < tw->columnCount() && c < orig.size(); ++c) {
            if (orig[c].isEmpty())
                continue;
            const QString tr = translate(orig[c]);
            if (tr == orig[c])
                continue; // nu e în dicționar → lasă neschimbat
            if (auto* it = tw->horizontalHeaderItem(c))
                it->setText(tr);
        }
    }
}

bool LanguageManager::eventFilter(QObject* watched, QEvent* event)
{
    // Widget nou afișat/stilizat → tradu-l (prinde dialoguri deschise ulterior).
    if (current_ != QLatin1String("en")
        && (event->type() == QEvent::Polish || event->type() == QEvent::Show)) {
        if (auto* w = qobject_cast<QWidget*>(watched))
            translateWidget(w);
    }
    return QObject::eventFilter(watched, event);
}

} // namespace app::ui
