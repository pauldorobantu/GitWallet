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
    // ---- GitWallet: saved balances, networks ----
    { "Saved balances from %1, being checked", "Saldos guardados de %1, en comprobación", "Soldes enregistrés de %1, en vérification", "Gespeicherte Salden von %1, werden geprüft", "Сохранённые балансы от %1, проверяются" },
    { "Saved balances, being checked", "Saldos guardados, en comprobación", "Soldes enregistrés, en vérification", "Gespeicherte Salden, werden geprüft", "Сохранённые балансы, проверяются" },
    { "saved, being checked", "guardado, en comprobación", "enregistré, en vérification", "gespeichert, wird geprüft", "сохранено, проверяется" },
    { "Checking saved balances…", "Comprobando saldos guardados…", "Vérification des soldes enregistrés…", "Gespeicherte Salden werden geprüft…", "Проверка сохранённых балансов…" },
    { "Prices: %1 · %2", "Precios: %1 · %2", "Prix : %1 · %2", "Preise: %1 · %2", "Цены: %1 · %2" },
    { "Prices: not loaded yet", "Precios: aún sin cargar", "Prix : pas encore chargés", "Preise: noch nicht geladen", "Цены: ещё не загружены" },
    { "%1 endpoint answers for another network", "El punto de acceso de %1 responde por otra red", "Le point d'accès %1 répond pour un autre réseau", "%1-Endpunkt antwortet für ein anderes Netz", "Узел %1 отвечает за другую сеть" },
    { "%1 balances from %2", "Saldos de %1 de %2", "Soldes %1 de %2", "%1-Salden von %2", "Балансы %1 от %2" },
    { "%1 lookups failing", "Fallan las consultas de %1", "Échec des requêtes %1", "%1-Abfragen schlagen fehl", "Запросы %1 не проходят" },
    { "server on another chain", "servidor en otra cadena", "serveur sur une autre chaîne", "Server auf einer anderen Chain", "сервер в другой сети" },
    { "No wallet has a key on %1", "Ninguna billetera tiene clave de %1", "Aucun portefeuille n'a de clé %1", "Kein Wallet hat einen %1-Schlüssel", "Ни у одного кошелька нет ключа %1" },
    { "%1 live, %2 read every %3 min", "%1 en directo, %2 leídas cada %3 min", "%1 en direct, %2 lues toutes les %3 min", "%1 live, %2 alle %3 Min. gelesen", "%1 в реальном времени, %2 читаются каждые %3 мин" },
    { "No server in use", "Ningún servidor en uso", "Aucun serveur utilisé", "Kein Server in Benutzung", "Сервер не используется" },
    { "A server answers for another network", "Un servidor responde por otra red", "Un serveur répond pour un autre réseau", "Ein Server antwortet für ein anderes Netz", "Сервер отвечает за другую сеть" },
    { "Prices: could not be loaded", "Precios: no se pudieron cargar", "Prix : chargement impossible", "Preise: konnten nicht geladen werden", "Цены: не удалось загрузить" },
    { "no price for %1", "sin precio para %1", "pas de prix pour %1", "kein Preis für %1", "нет цены для %1" },
    { "Loading saved balances…", "Cargando saldos guardados…", "Chargement des soldes enregistrés…", "Gespeicherte Salden werden geladen…", "Загрузка сохранённых балансов…" },
    { "%1 saved", "%1 guardadas", "%1 enregistrées", "%1 gespeichert", "сохранено: %1" },
    { "Account", "Cuenta", "Compte", "Konto", "Аккаунт" },
    { "Addresses exported, with saved balances still being checked", "Direcciones exportadas, con saldos guardados aún en comprobación", "Adresses exportées, avec des soldes enregistrés encore en vérification", "Adressen exportiert, gespeicherte Salden werden noch geprüft", "Адреса экспортированы, сохранённые балансы ещё проверяются" },
    { "Refreshing prices and balances", "Actualizando precios y saldos", "Actualisation des prix et des soldes", "Preise und Salden werden aktualisiert", "Обновление цен и балансов" },
    { "Saved balances are being read again", "Los saldos guardados se están leyendo de nuevo", "Les soldes enregistrés sont relus", "Gespeicherte Salden werden neu gelesen", "Сохранённые балансы читаются заново" },
    { "not scanned by this version", "esta versión no lo escanea", "non analysé par cette version", "von dieser Version nicht gescannt", "эта версия не сканирует" },
    { "refresh failing", "la actualización falla", "l'actualisation échoue", "Aktualisierung schlägt fehl", "обновление не удаётся" },
    { "sources disagree on %1", "las fuentes no coinciden en %1", "les sources divergent sur %1", "Quellen uneinig über %1", "источники расходятся по %1" },
    { "Some files could not be deleted", "Algunos archivos no se pudieron eliminar", "Certains fichiers n'ont pas pu être supprimés", "Einige Dateien konnten nicht gelöscht werden", "Некоторые файлы не удалось удалить" },
    { "nonces from %1", "nonces de %1", "nonces du %1", "Nonces von %1", "nonce от %1" },
    { "History", "Historial", "Historique", "Verlauf", "История" },
    { "Date", "Fecha", "Date", "Datum", "Дата" },
    { "Amount", "Importe", "Montant", "Betrag", "Сумма" },
    { "USD now", "USD ahora", "USD maintenant", "USD jetzt", "USD сейчас" },
    { "USD then", "USD entonces", "USD alors", "USD damals", "USD тогда" },
    { "Fee", "Comisión", "Frais", "Gebühr", "Комиссия" },
    { "Label", "Etiqueta", "Libellé", "Bezeichnung", "Метка" },
    { "Note", "Nota", "Note", "Notiz", "Заметка" },
    { "Transaction", "Transacción", "Transaction", "Transaktion", "Транзакция" },
    { "Received", "Recibido", "Reçu", "Empfangen", "Получено" },
    { "Sent", "Enviado", "Envoyé", "Gesendet", "Отправлено" },
    { "Moved", "Movido", "Déplacé", "Verschoben", "Перемещено" },
    { "Pending", "Pendiente", "En attente", "Ausstehend", "В ожидании" },
    { "Confirmed", "Confirmado", "Confirmé", "Bestätigt", "Подтверждено" },
    { "Replaced", "Reemplazado", "Remplacé", "Ersetzt", "Заменено" },
    { "Dropped", "Descartado", "Abandonné", "Verworfen", "Отброшено" },
    { "1 confirmation", "1 confirmación", "1 confirmation", "1 Bestätigung", "1 подтверждение" },
    { "%1 confirmations", "%1 confirmaciones", "%1 confirmations", "%1 Bestätigungen", "Подтверждений: %1" },
    { "In a block, proof on its way", "En un bloque, prueba en camino", "Dans un bloc, preuve en cours", "In einem Block, Nachweis unterwegs", "В блоке, доказательство в пути" },
    { "All types", "Todos los tipos", "Tous les types", "Alle Arten", "Все типы" },
    { "In a block", "En un bloque", "Dans un bloc", "In einem Block", "В блоке" },
    { "Replaced or dropped", "Reemplazado o descartado", "Remplacé ou abandonné", "Ersetzt oder verworfen", "Заменено или отброшено" },
    { "All time", "Todo el tiempo", "Toute la période", "Gesamter Zeitraum", "За всё время" },
    { "Last 24 hours", "Últimas 24 horas", "Dernières 24 heures", "Letzte 24 Stunden", "Последние 24 часа" },
    { "Last 7 days", "Últimos 7 días", "7 derniers jours", "Letzte 7 Tage", "Последние 7 дней" },
    { "Last 30 days", "Últimos 30 días", "30 derniers jours", "Letzte 30 Tage", "Последние 30 дней" },
    { "This year", "Este año", "Cette année", "Dieses Jahr", "Этот год" },
    { "Last year", "El año pasado", "L'année dernière", "Letztes Jahr", "Прошлый год" },
    { "Details", "Detalles", "Détails", "Details", "Подробности" },
    { "Show all", "Mostrar todo", "Tout afficher", "Alle anzeigen", "Показать всё" },
    { "Copy address", "Copiar dirección", "Copier l'adresse", "Adresse kopieren", "Копировать адрес" },
    { "Copy", "Copiar", "Copier", "Kopieren", "Копировать" },
    { "Open in explorer", "Abrir en el explorador", "Ouvrir dans l'explorateur", "Im Explorer öffnen", "Открыть в обозревателе" },
    { "Address copied", "Dirección copiada", "Adresse copiée", "Adresse kopiert", "Адрес скопирован" },
    { "Transaction id copied", "Id de transacción copiado", "Id de transaction copié", "Transaktions-ID kopiert", "Id транзакции скопирован" },
    { "From", "De", "De", "Von", "Откуда" },
    { "To", "A", "À", "An", "Куда" },
    { "Replaced by", "Reemplazada por", "Remplacée par", "Ersetzt durch", "Заменена на" },
    { "block %1", "bloque %1", "bloc %1", "Block %1", "блок %1" },
    { "not known", "no se sabe", "inconnu", "unbekannt", "неизвестно" },
    { "(no address)", "(sin dirección)", "(sans adresse)", "(keine Adresse)", "(нет адреса)" },
    { "in", "entra", "entrée", "ein", "вход" },
    { "out", "sale", "sortie", "aus", "выход" },
    { "%1 transaction", "%1 transacción", "%1 transaction", "%1 Transaktion", "Транзакций: %1" },
    { "%1 transactions", "%1 transacciones", "%1 transactions", "%1 Transaktionen", "Транзакций: %1" },
    { "%1 of %2 transactions", "%1 de %2 transacciones", "%1 sur %2 transactions", "%1 von %2 Transaktionen", "%1 из %2 транзакций" },
    { "No transactions yet", "Aún no hay transacciones", "Pas encore de transactions", "Noch keine Transaktionen", "Транзакций пока нет" },
    { "Search id, address, label or note", "Buscar id, dirección, etiqueta o nota", "Chercher un id, une adresse, un libellé ou une note", "ID, Adresse, Bezeichnung oder Notiz suchen", "Искать id, адрес, метку или заметку" },
    { "Search address, label or path", "Buscar dirección, etiqueta o ruta", "Chercher une adresse, un libellé ou un chemin", "Adresse, Bezeichnung oder Pfad suchen", "Искать адрес, метку или путь" },
    { "Export CSV", "Exportar CSV", "Exporter en CSV", "CSV exportieren", "Экспорт в CSV" },
    { "Export transactions", "Exportar transacciones", "Exporter les transactions", "Transaktionen exportieren", "Экспорт транзакций" },
    { "Transactions exported", "Transacciones exportadas", "Transactions exportées", "Transaktionen exportiert", "Транзакции экспортированы" },
    { "Reading the history: %1 addresses, %2 transactions and %3 proofs to go", "Leyendo el historial: faltan %1 direcciones, %2 transacciones y %3 pruebas", "Lecture de l'historique : reste %1 adresses, %2 transactions et %3 preuves", "Verlauf wird gelesen: noch %1 Adressen, %2 Transaktionen und %3 Nachweise", "Чтение истории: осталось адресов %1, транзакций %2, доказательств %3" },
    { "Every transaction is read, and what is in a block has its proof checked on this PC", "Todas las transacciones están leídas, y lo que está en un bloque tiene su prueba comprobada en este PC", "Toutes les transactions sont lues, et ce qui est dans un bloc a sa preuve vérifiée sur ce PC", "Alle Transaktionen sind gelesen, und was in einem Block ist, wurde auf diesem PC nachgewiesen", "Все транзакции прочитаны, а для тех, что в блоке, доказательство проверено на этом ПК" },
    { "%1 addresses could not be read whole from this server", "%1 direcciones no se pudieron leer enteras de este servidor", "%1 adresses n'ont pas pu être lues en entier sur ce serveur", "%1 Adressen konnten von diesem Server nicht vollständig gelesen werden", "Адресов, не прочитанных полностью с этого сервера: %1" },
    { "%1 proofs did not check out", "%1 pruebas no se confirmaron", "%1 preuves n'ont pas abouti", "%1 Nachweise stimmten nicht", "Не подтвердилось доказательств: %1" },
    { "%1 addresses do not add up to their balance", "%1 direcciones no cuadran con su saldo", "%1 adresses ne correspondent pas à leur solde", "%1 Adressen stimmen nicht mit ihrem Saldo überein", "Адресов, не сходящихся с балансом: %1" },
    { "Not in a block yet. It can still be replaced: do not count it as paid.", "Aún no está en un bloque. Todavía puede reemplazarse: no la cuentes como pagada.", "Pas encore dans un bloc. Elle peut encore être remplacée : ne la comptez pas comme payée.", "Noch in keinem Block. Sie kann noch ersetzt werden: nicht als bezahlt werten.", "Ещё не в блоке. Её ещё могут заменить: не считайте оплаченной." },
    { "In block %1. Its Merkle proof and the block's work were checked on this PC.", "En el bloque %1. Su prueba Merkle y el trabajo del bloque se comprobaron en este PC.", "Dans le bloc %1. Sa preuve de Merkle et le travail du bloc ont été vérifiés sur ce PC.", "In Block %1. Merkle-Nachweis und Arbeit des Blocks wurden auf diesem PC geprüft.", "В блоке %1. Доказательство Меркла и работа блока проверены на этом ПК." },
    { "A server says it is in block %1. Its proof has not been checked yet.", "Un servidor dice que está en el bloque %1. Su prueba aún no se ha comprobado.", "Un serveur dit qu'elle est dans le bloc %1. Sa preuve n'a pas encore été vérifiée.", "Ein Server sagt, sie sei in Block %1. Der Nachweis wurde noch nicht geprüft.", "Сервер сообщает, что она в блоке %1. Доказательство ещё не проверено." },
    { "Another transaction spends the same coins. This one no longer counts.", "Otra transacción gasta las mismas monedas. Esta ya no cuenta.", "Une autre transaction dépense les mêmes pièces. Celle-ci ne compte plus.", "Eine andere Transaktion gibt dieselben Coins aus. Diese zählt nicht mehr.", "Другая транзакция тратит те же монеты. Эта больше не считается." },
    { "Servers no longer list it. Unless it comes back, it does not count.", "Los servidores ya no la listan. Si no vuelve, no cuenta.", "Les serveurs ne la listent plus. Si elle ne revient pas, elle ne compte pas.", "Server führen sie nicht mehr auf. Kommt sie nicht zurück, zählt sie nicht.", "Серверы больше её не показывают. Если не вернётся, она не считается." },
    { "Proven on this PC: a Merkle proof ties it to this block, and the block's header and work were checked.", "Demostrada en este PC: una prueba Merkle la une a este bloque, y se comprobaron su cabecera y su trabajo.", "Prouvée sur ce PC : une preuve de Merkle la lie à ce bloc, et son en-tête et son travail ont été vérifiés.", "Auf diesem PC nachgewiesen: Ein Merkle-Nachweis bindet sie an diesen Block, dessen Kopf und Arbeit geprüft wurden.", "Доказано на этом ПК: доказательство Меркла связывает её с этим блоком, заголовок и работа блока проверены." },
    { "A server says it is in this block. Its proof has not been checked yet.", "Un servidor dice que está en este bloque. Su prueba aún no se ha comprobado.", "Un serveur dit qu'elle est dans ce bloc. Sa preuve n'a pas encore été vérifiée.", "Ein Server sagt, sie sei in diesem Block. Der Nachweis wurde noch nicht geprüft.", "Сервер сообщает, что она в этом блоке. Доказательство ещё не проверено." },
    { "The price at that moment is not known yet", "El precio de ese momento aún no se conoce", "Le prix à ce moment n'est pas encore connu", "Der Preis zu diesem Zeitpunkt ist noch nicht bekannt", "Цена на тот момент пока неизвестна" },
    { "What this did to the balance shown: %1", "Lo que esto hizo al saldo mostrado: %1", "Effet sur le solde affiché : %1", "Wirkung auf den angezeigten Saldo: %1", "Как это изменило показанный баланс: %1" },
    { "1 %1 = %2 at %3", "1 %1 = %2 a las %3", "1 %1 = %2 à %3", "1 %1 = %2 um %3", "1 %1 = %2 в %3" },
    { "What it was worth when it happened, from DefiLlama: to the minute in recent years, to the day further back, nothing before 2015.", "Lo que valía cuando ocurrió, según DefiLlama: al minuto en los últimos años, al día más atrás, nada antes de 2015.", "Sa valeur au moment des faits, selon DefiLlama : à la minute ces dernières années, au jour plus loin, rien avant 2015.", "Der Wert zum Zeitpunkt, von DefiLlama: minutengenau in den letzten Jahren, tagesgenau davor, nichts vor 2015.", "Сколько это стоило тогда, по DefiLlama: с точностью до минуты в последние годы, до дня раньше, ничего до 2015." },
    { "Reading where its coins came from…", "Leyendo de dónde vinieron sus monedas…", "Lecture de la provenance de ses pièces…", "Herkunft der Coins wird gelesen…", "Чтение, откуда пришли монеты…" },
    { "Where its coins came from is read from a server, and none is connected.", "De dónde vinieron sus monedas se lee de un servidor, y no hay ninguno conectado.", "La provenance de ses pièces se lit sur un serveur, et aucun n'est connecté.", "Die Herkunft der Coins wird von einem Server gelesen, und keiner ist verbunden.", "Откуда пришли монеты, читается с сервера, а ни один не подключён." },
    { "Newly mined coins: they come from no address.", "Monedas recién minadas: no vienen de ninguna dirección.", "Pièces fraîchement minées : elles ne viennent d'aucune adresse.", "Frisch geschürfte Coins: Sie kommen von keiner Adresse.", "Только что добытые монеты: они ни с какого адреса." },
    { "It spends %1 more than are listed here.", "Gasta %1 más de los que se listan aquí.", "Elle en dépense %1 de plus que ceux listés ici.", "Sie gibt %1 mehr aus, als hier aufgeführt sind.", "Она тратит ещё %1 сверх перечисленных здесь." },
    { "One of your addresses", "Una de tus direcciones", "Une de vos adresses", "Eine Ihrer Adressen", "Один из ваших адресов" },
    { "An order, a customer, an invoice", "Un pedido, un cliente, una factura", "Une commande, un client, une facture", "Eine Bestellung, ein Kunde, eine Rechnung", "Заказ, клиент, счёт" },
    { "A customer, an order, a till", "Un cliente, un pedido, una caja", "Un client, une commande, une caisse", "Ein Kunde, eine Bestellung, eine Kasse", "Клиент, заказ, касса" },
    { "Every transaction of this wallet", "Todas las transacciones de esta billetera", "Toutes les transactions de ce portefeuille", "Alle Transaktionen dieses Wallets", "Все транзакции этого кошелька" },
    { "Every transaction of this address. A double click opens it too.", "Todas las transacciones de esta dirección. También se abre con doble clic.", "Toutes les transactions de cette adresse. Un double-clic l'ouvre aussi.", "Alle Transaktionen dieser Adresse. Auch per Doppelklick.", "Все транзакции этого адреса. Открывается и двойным щелчком." },
    { "A name for this address that only this PC knows", "Un nombre para esta dirección que solo este PC conoce", "Un nom pour cette adresse que seul ce PC connaît", "Ein Name für diese Adresse, den nur dieser PC kennt", "Имя этого адреса, известное только этому ПК" },
    { "Ethereum transactions come in the next stage", "Las transacciones de Ethereum llegan en la siguiente etapa", "Les transactions Ethereum arrivent à la prochaine étape", "Ethereum-Transaktionen kommen in der nächsten Stufe", "Транзакции Ethereum появятся на следующем этапе" },
    { "Add a wallet on the Wallets page, and its transactions show here.", "Añade una billetera en la página Billeteras y sus transacciones aparecerán aquí.", "Ajoutez un portefeuille sur la page Portefeuilles, et ses transactions s'afficheront ici.", "Fügen Sie auf der Seite Wallets ein Wallet hinzu, dann erscheinen hier seine Transaktionen.", "Добавьте кошелёк на странице Кошельки, и здесь появятся его транзакции." },
    { "Every transaction of every address shows here, as servers tell of it and proofs check out.", "Aquí aparece cada transacción de cada dirección, según la cuentan los servidores y se comprueban las pruebas.", "Chaque transaction de chaque adresse s'affiche ici, à mesure que les serveurs la signalent et que les preuves sont vérifiées.", "Jede Transaktion jeder Adresse erscheint hier, sobald Server sie melden und Nachweise stimmen.", "Здесь появляется каждая транзакция каждого адреса, когда о ней сообщают серверы и проверяются доказательства." },
    { "Moved: every output came back to the addresses shown, so only a fee left.", "Movido: todas las salidas volvieron a las direcciones mostradas, así que solo se fue una comisión.", "Déplacé : toutes les sorties sont revenues aux adresses affichées, seuls des frais sont partis.", "Verschoben: Alle Ausgaben kamen zu den angezeigten Adressen zurück, nur eine Gebühr ging ab.", "Перемещено: все выходы вернулись на показанные адреса, ушла только комиссия." },
    { "Opens the address on a public block explorer, in your browser", "Abre la dirección en un explorador de bloques público, en tu navegador", "Ouvre l'adresse sur un explorateur de blocs public, dans votre navigateur", "Öffnet die Adresse in einem öffentlichen Block-Explorer in Ihrem Browser", "Открывает адрес в публичном обозревателе блоков, в вашем браузере" },
    { "Opens this transaction on a public block explorer, in your browser", "Abre esta transacción en un explorador de bloques público, en tu navegador", "Ouvre cette transaction sur un explorateur de blocs public, dans votre navigateur", "Öffnet diese Transaktion in einem öffentlichen Block-Explorer in Ihrem Browser", "Открывает эту транзакцию в публичном обозревателе блоков, в вашем браузере" },
    { "Save the lines shown to a spreadsheet file. It is not encrypted: anyone who reads it sees these transactions, labels and notes.", "Guarda las líneas mostradas en una hoja de cálculo. No está cifrada: quien la lea verá estas transacciones, etiquetas y notas.", "Enregistre les lignes affichées dans un fichier tableur. Il n'est pas chiffré : quiconque le lit voit ces transactions, libellés et notes.", "Speichert die angezeigten Zeilen in einer Tabellendatei. Sie ist nicht verschlüsselt: Wer sie liest, sieht diese Transaktionen, Bezeichnungen und Notizen.", "Сохраняет показанные строки в файл таблицы. Он не зашифрован: кто его прочтёт, увидит эти транзакции, метки и заметки." },
    { "Labels and notes", "Etiquetas y notas", "Libellés et notes", "Bezeichnungen und Notizen", "Метки и заметки" },
    { "The labels and notes you give transactions and addresses are on this PC alone, in its encrypted database: no server can give them back. Keep a copy elsewhere. The file is in the form other wallets read (BIP-329), and it is not encrypted.", "Las etiquetas y notas de tus transacciones y direcciones están solo en este PC, en su base de datos cifrada: ningún servidor puede devolverlas. Guarda una copia en otro lugar. El archivo tiene el formato que leen otras billeteras (BIP-329) y no está cifrado.", "Les libellés et notes de vos transactions et adresses sont uniquement sur ce PC, dans sa base chiffrée : aucun serveur ne peut les rendre. Gardez-en une copie ailleurs. Le fichier est au format lu par d'autres portefeuilles (BIP-329) et n'est pas chiffré.", "Bezeichnungen und Notizen zu Transaktionen und Adressen liegen nur auf diesem PC in seiner verschlüsselten Datenbank: Kein Server kann sie zurückgeben. Bewahren Sie anderswo eine Kopie auf. Die Datei hat das Format anderer Wallets (BIP-329) und ist nicht verschlüsselt.", "Метки и заметки транзакций и адресов есть только на этом ПК, в его зашифрованной базе: ни один сервер их не вернёт. Храните копию в другом месте. Файл в формате других кошельков (BIP-329), он не зашифрован." },
    { "Export labels", "Exportar etiquetas", "Exporter les libellés", "Bezeichnungen exportieren", "Экспорт меток" },
    { "Import labels", "Importar etiquetas", "Importer des libellés", "Bezeichnungen importieren", "Импорт меток" },
    { "Labels exported", "Etiquetas exportadas", "Libellés exportés", "Bezeichnungen exportiert", "Метки экспортированы" },
    { "No label or note to export yet", "Aún no hay etiquetas ni notas que exportar", "Aucun libellé ni note à exporter pour l'instant", "Noch keine Bezeichnungen oder Notizen zum Exportieren", "Пока нет меток и заметок для экспорта" },
    { "That file could not be read.", "Ese archivo no se pudo leer.", "Ce fichier n'a pas pu être lu.", "Diese Datei konnte nicht gelesen werden.", "Этот файл не удалось прочитать." },
    { "%1 labels imported", "%1 etiquetas importadas", "%1 libellés importés", "%1 Bezeichnungen importiert", "Импортировано меток: %1" },
    { "%1 labels imported, %2 lines left out", "%1 etiquetas importadas, %2 líneas omitidas", "%1 libellés importés, %2 lignes ignorées", "%1 Bezeichnungen importiert, %2 Zeilen ausgelassen", "Импортировано меток: %1, пропущено строк: %2" },
    { "Exporting %1%", "Exportando %1%", "Export %1%", "Export %1%", "Экспорт %1%" },
    { "The list changed while it was being exported. Try again.", "La lista cambió mientras se exportaba. Inténtalo de nuevo.", "La liste a changé pendant l'export. Réessayez.", "Die Liste änderte sich beim Exportieren. Bitte erneut versuchen.", "Список изменился во время экспорта. Попробуйте ещё раз." },
    { "Type", "Tipo", "Type", "Art", "Тип" },
    { "State", "Estado", "État", "Status", "Статус" },
    { "Address", "Dirección", "Adresse", "Adresse", "Адрес" },
    { "All states", "Todos los estados", "Tous les états", "Alle Status", "Все статусы" },
    { "All wallets", "Todas las billeteras", "Tous les portefeuilles", "Alle Wallets", "Все кошельки" },
    { "No wallet yet", "Aún no hay billetera", "Pas encore de portefeuille", "Noch keine Wallet", "Кошелька пока нет" },
    { "Save", "Guardar", "Enregistrer", "Speichern", "Сохранить" },
    { "Cancel", "Cancelar", "Annuler", "Abbrechen", "Отмена" },
    { "Could not write the file: %1", "No se pudo escribir el archivo: %1", "Impossible d'écrire le fichier : %1", "Die Datei konnte nicht geschrieben werden: %1", "Не удалось записать файл: %1" },
    { "In a block, proof did not hold", "En un bloque, la prueba no es válida", "Dans un bloc, la preuve n'est pas valable", "In einem Block, Nachweis ungültig", "В блоке, доказательство не подтвердилось" },
    { "Proof did not hold", "La prueba no es válida", "La preuve n'est pas valable", "Nachweis ungültig", "Доказательство не подтвердилось" },
    { "A server says it is in block %1, and the proof it gave does not hold, or it gives none. Do not count it as paid.", "Un servidor dice que está en el bloque %1, y la prueba que dio no es válida, o no da ninguna. No la cuentes como pagada.", "Un serveur dit qu'elle est dans le bloc %1, et la preuve donnée n'est pas valable, ou il n'en donne aucune. Ne la comptez pas comme payée.", "Ein Server sagt, sie sei in Block %1, und sein Nachweis ist ungültig oder fehlt. Nicht als bezahlt werten.", "Сервер говорит, что она в блоке %1, а его доказательство не подтвердилось или его нет. Не считайте её оплаченной." },
    { "A server says it is in this block, and the proof it gave does not hold, or it gives none. Do not count it as paid.", "Un servidor dice que está en este bloque, y la prueba que dio no es válida, o no da ninguna. No la cuentes como pagada.", "Un serveur dit qu'elle est dans ce bloc, et la preuve donnée n'est pas valable, ou il n'en donne aucune. Ne la comptez pas comme payée.", "Ein Server sagt, sie sei in diesem Block, und sein Nachweis ist ungültig oder fehlt. Nicht als bezahlt werten.", "Сервер говорит, что она в этом блоке, а его доказательство не подтвердилось или его нет. Не считайте её оплаченной." },
    { "waiting for a server", "esperando un servidor", "en attente d'un serveur", "wartet auf einen Server", "ожидание сервера" },
    { "In and out: what the lines shown came to, of those proven in a block.", "Entradas y salidas: lo que suman las líneas mostradas, de las probadas en un bloque.", "Entrées et sorties : ce que font les lignes affichées, parmi celles prouvées dans un bloc.", "Ein und aus: was die gezeigten Zeilen ergeben, von denen, die in einem Block nachgewiesen sind.", "Приход и расход: итог показанных строк, из тех, что доказаны в блоке." },
    { "No longer in the history", "Ya no está en el historial", "N'est plus dans l'historique", "Nicht mehr im Verlauf", "Больше нет в истории" },
    { "paid by the sender", "pagada por el remitente", "payés par l'expéditeur", "vom Absender bezahlt", "оплачено отправителем" },
    { "The size a rate goes by is as a server gives it: no proof covers a transaction's witness.", "El tamaño de la tarifa es el que da un servidor: ninguna prueba cubre el witness de una transacción.", "La taille utilisée pour le taux est celle donnée par un serveur : aucune preuve ne couvre le witness d'une transaction.", "Die Größe für die Rate stammt von einem Server: Kein Nachweis deckt den Witness einer Transaktion ab.", "Размер для ставки указан сервером: ни одно доказательство не покрывает witness транзакции." },
    { "(not read from the server)", "(no leído del servidor)", "(non lu depuis le serveur)", "(nicht vom Server gelesen)", "(не прочитано с сервера)" },
    { "%1 of what it spends could not be read from the server.", "No se pudieron leer del servidor %1 de lo que gasta.", "%1 de ce qu'elle dépense n'ont pas pu être lus depuis le serveur.", "%1 von dem, was sie ausgibt, konnten nicht vom Server gelesen werden.", "Не удалось прочитать с сервера: %1 из того, что она тратит." },
    { "Discard changes", "Descartar los cambios", "Annuler les modifications", "Änderungen verwerfen", "Отменить изменения" },
    { "The server did not give this transaction.", "El servidor no dio esta transacción.", "Le serveur n'a pas donné cette transaction.", "Der Server hat diese Transaktion nicht geliefert.", "Сервер не выдал эту транзакцию." },
    { "What the server gave does not hash to this transaction.", "Lo que dio el servidor no corresponde al hash de esta transacción.", "Ce que le serveur a donné ne correspond pas au hash de cette transaction.", "Was der Server geliefert hat, ergibt nicht den Hash dieser Transaktion.", "То, что выдал сервер, не даёт хеш этой транзакции." },
    { "That looks like seed words or a private key, so it was not kept.", "Parece una semilla o una clave privada, así que no se guardó.", "Cela ressemble à une phrase secrète ou une clé privée, cela n'a donc pas été conservé.", "Das sieht nach Seed-Wörtern oder einem privaten Schlüssel aus und wurde daher nicht behalten.", "Похоже на seed-фразу или приватный ключ, поэтому не сохранено." },
    { "Add a wallet first: labels and notes are kept with it.", "Añade primero una billetera: las etiquetas y notas se guardan con ella.", "Ajoutez d'abord un portefeuille : les étiquettes et notes sont conservées avec lui.", "Fügen Sie zuerst eine Wallet hinzu: Etiketten und Notizen werden mit ihr gespeichert.", "Сначала добавьте кошелёк: метки и заметки хранятся вместе с ним." },
    { "What is saved is still being read. Try again in a moment.", "Lo guardado aún se está leyendo. Inténtalo de nuevo en un momento.", "Ce qui est enregistré est encore en cours de lecture. Réessayez dans un instant.", "Das Gespeicherte wird noch gelesen. Versuchen Sie es gleich noch einmal.", "Сохранённое ещё читается. Повторите через минуту." },
    { "A file of labels is still going in. Try again in a moment.", "Aún se está importando un archivo de etiquetas. Inténtalo de nuevo en un momento.", "Un fichier d'étiquettes est encore en cours d'import. Réessayez dans un instant.", "Eine Etikettendatei wird noch eingelesen. Versuchen Sie es gleich noch einmal.", "Файл меток ещё загружается. Повторите через минуту." },
    { "Labels and notes are not being kept: %1.", "Las etiquetas y notas no se están guardando: %1.", "Les étiquettes et notes ne sont pas conservées : %1.", "Etiketten und Notizen werden nicht behalten: %1.", "Метки и заметки не сохраняются: %1." },
    { "another GitWallet window holds this folder", "otra ventana de GitWallet tiene esta carpeta", "une autre fenêtre GitWallet détient ce dossier", "ein anderes GitWallet-Fenster hält diesen Ordner", "этой папкой владеет другое окно GitWallet" },
    { "That file is too long to be a file of labels.", "Ese archivo es demasiado largo para ser de etiquetas.", "Ce fichier est trop long pour être un fichier d'étiquettes.", "Diese Datei ist zu lang für eine Etikettendatei.", "Этот файл слишком велик для файла меток." },
    { "Labels and notes are not being saved", "Las etiquetas y notas no se están guardando", "Les étiquettes et notes ne sont pas enregistrées", "Etiketten und Notizen werden nicht gespeichert", "Метки и заметки не сохраняются" },
    { "Copied", "Copiado", "Copié", "Kopiert", "Скопировано" },
    { "%1 lines name a transaction these wallets have not read yet, and look like a private key until they have. Keep the file, and import it again once the history is read. What is still refused then is left out for good.", "%1 líneas nombran una transacción que estas billeteras aún no han leído, y hasta entonces parecen una clave privada. Conserva el archivo e impórtalo de nuevo cuando el historial esté leído. Lo que entonces siga rechazado queda fuera definitivamente.", "%1 lignes nomment une transaction que ces portefeuilles n'ont pas encore lue, et ressemblent à une clé privée d'ici là. Gardez le fichier et importez-le de nouveau une fois l'historique lu. Ce qui sera encore refusé alors restera écarté pour de bon.", "%1 Zeilen nennen eine Transaktion, die diese Wallets noch nicht gelesen haben, und sehen bis dahin wie ein privater Schlüssel aus. Behalten Sie die Datei und importieren Sie sie erneut, sobald der Verlauf gelesen ist. Was dann noch abgelehnt wird, bleibt endgültig draußen.", "В %1 строках указана транзакция, которую эти кошельки ещё не прочитали, и до тех пор они похожи на приватный ключ. Сохраните файл и импортируйте его снова, когда история будет прочитана. То, что и тогда будет отклонено, не войдёт окончательно." },
    { "The labels and notes of an older file have yet to come over", "Las etiquetas y notas de un archivo anterior aún no se han traído", "Les étiquettes et notes d'un ancien fichier n'ont pas encore été reprises", "Die Etiketten und Notizen einer älteren Datei wurden noch nicht übernommen", "Метки и заметки из старого файла ещё не перенесены" },
    { "The labels and notes of an older file have yet to come over, and until they have none can be changed. This is tried again every minute: free some disk space if the disk is full.", "Las etiquetas y notas de un archivo anterior aún no se han traído, y hasta entonces no se puede cambiar ninguna. Se reintenta cada minuto: libera espacio si el disco está lleno.", "Les étiquettes et notes d'un ancien fichier n'ont pas encore été reprises, et d'ici là aucune ne peut être modifiée. Nouvel essai chaque minute : libérez de la place si le disque est plein.", "Die Etiketten und Notizen einer älteren Datei wurden noch nicht übernommen, und bis dahin kann keine geändert werden. Das wird jede Minute erneut versucht: Geben Sie Speicherplatz frei, falls die Festplatte voll ist.", "Метки и заметки из старого файла ещё не перенесены, и до этого их нельзя менять. Попытка повторяется каждую минуту: освободите место, если диск заполнен." },
    { "Saved balances are not being kept", "Los saldos guardados no se conservan", "Les soldes enregistrés ne sont pas conservés", "Gespeicherte Salden werden nicht behalten", "Сохранённые балансы не сохраняются" },
    { "%1 Electrum servers, line %2: use host:port", "Servidores Electrum de %1, línea %2: usa host:puerto", "Serveurs Electrum %1, ligne %2 : utilisez hôte:port", "%1 Electrum-Server, Zeile %2: host:port verwenden", "Серверы Electrum %1, строка %2: укажите host:port" },
    { "%1 RPC URLs, line %2: use an https:// URL", "URL RPC de %1, línea %2: usa una URL https://", "URL RPC %1, ligne %2 : utilisez une URL https://", "%1 RPC-URLs, Zeile %2: https://-URL verwenden", "RPC-адреса %1, строка %2: укажите адрес https://" },
    { "not updated since", "sin actualizar desde entonces", "pas mis à jour depuis", "seitdem nicht aktualisiert", "с тех пор не обновлялись" },
    // ---- GitWallet ----
    { "Wallet", "Billetera", "Portefeuille", "Wallet", "Кошелёк" },
    { "Wallets", "Billeteras", "Portefeuilles", "Wallets", "Кошельки" },
    { "Add wallet", "Añadir billetera", "Ajouter un portefeuille", "Wallet hinzufügen", "Добавить кошелёк" },
    { "Add wallets", "Añadir billeteras", "Ajouter les portefeuilles", "Wallets hinzufügen", "Добавить кошельки" },
    { "Add many", "Añadir varias", "En ajouter plusieurs", "Mehrere hinzufügen", "Добавить несколько" },
    { "Overview", "Resumen", "Vue d'ensemble", "Übersicht", "Обзор" },
    { "An address given out could not be written down", "No se pudo guardar una dirección entregada", "Une adresse donnée n'a pas pu être enregistrée", "Eine ausgegebene Adresse konnte nicht gespeichert werden", "Не удалось записать выданный адрес" },
    { "An address given out could not be written down yet, so another start may offer it again. This is tried again every half a minute.", "Una dirección entregada aún no se pudo guardar, así que otro inicio podría ofrecerla de nuevo. Se reintenta cada medio minuto.", "Une adresse donnée n'a pas encore pu être enregistrée : un autre démarrage pourrait la proposer de nouveau. Nouvel essai toutes les demi-minutes.", "Eine ausgegebene Adresse konnte noch nicht gespeichert werden, ein weiterer Start könnte sie erneut anbieten. Das wird jede halbe Minute erneut versucht.", "Выданный адрес пока не удалось записать, поэтому при следующем запуске он может быть предложен снова. Попытка повторяется каждые полминуты." },
    { "Has been used; what it holds is being read", "Se ha usado; se está leyendo lo que tiene", "A été utilisée ; ce qu'elle détient est en cours de lecture", "Wurde benutzt; was sie hält, wird gelesen", "Использовался; баланс читается" },
    { "Given out; no server has answered for it yet", "Entregada; ningún servidor ha respondido aún", "Donnée ; aucun serveur n'a encore répondu", "Ausgegeben; noch kein Server hat geantwortet", "Выдан; ни один сервер ещё не ответил" },
    { "No server has answered for it yet", "Ningún servidor ha respondido aún", "Aucun serveur n'a encore répondu", "Noch kein Server hat geantwortet", "Ни один сервер ещё не ответил" },
    { "Given out; nothing had come in as far as was saved, being checked", "Entregada; no había llegado nada según lo guardado, en comprobación", "Donnée ; rien n'était arrivé d'après ce qui est enregistré, en vérification", "Ausgegeben; laut Gespeichertem war nichts eingegangen, wird geprüft", "Выдан; по сохранённым данным поступлений не было, идёт проверка" },
    { "Saved, being checked", "Guardado, en comprobación", "Enregistré, en vérification", "Gespeichert, wird geprüft", "Сохранено, идёт проверка" },
    { "Nothing here holds it", "Nada aquí lo tiene", "Rien ici n'en détient", "Hier hält es nichts", "Здесь этого нет" },
    { "No address of this holds any. All states, below, shows them all.", "Ninguna dirección de esto tiene nada. Todos los estados, abajo, las muestra todas.", "Aucune adresse ici n'en détient. Tous les états, ci-dessous, les affiche toutes.", "Keine Adresse hiervon hält etwas. Alle Status unten zeigt sie alle.", "Ни на одном адресе этого нет. «Все статусы» ниже покажет все адреса." },
    { "This wallet has no key on %1", "Esta billetera no tiene clave en %1", "Ce portefeuille n'a pas de clé sur %1", "Dieses Wallet hat keinen Schlüssel auf %1", "У этого кошелька нет ключа в сети %1" },
    { "saved figures still being read", "las cifras guardadas aún se están leyendo", "les chiffres enregistrés sont encore en lecture", "gespeicherte Zahlen werden noch gelesen", "сохранённые данные ещё читаются" },
    { "a key this version cannot read", "una clave que esta versión no puede leer", "une clé que cette version ne peut pas lire", "ein Schlüssel, den diese Version nicht lesen kann", "ключ, который эта версия не может прочитать" },
    { "%1 hold something so far", "%1 tienen algo hasta ahora", "%1 détiennent quelque chose pour l'instant", "%1 halten bisher etwas", "Пока с балансом: %1" },
    { "Every %1 transaction is read, and what is in a block has its proof checked on this PC", "Se ha leído cada transacción de %1, y lo que está en un bloque tiene su prueba comprobada en este PC", "Chaque transaction %1 est lue, et ce qui est dans un bloc a sa preuve vérifiée sur ce PC", "Jede %1-Transaktion ist gelesen, und was in einem Block ist, hat seinen Beweis auf diesem PC geprüft", "Каждая транзакция %1 прочитана, а для тех, что в блоке, доказательство проверено на этом ПК" },
    { "The history of %1 is not read yet", "El historial de %1 aún no se lee", "L'historique de %1 n'est pas encore lu", "Der Verlauf von %1 wird noch nicht gelesen", "История %1 пока не читается" },
    { "%1 of the addresses it gave out have had nothing come in yet. If its seed is added again, they will be offered to someone else: only this PC knows they were given out.", "%1 de las direcciones que entregó aún no han recibido nada. Si se vuelve a añadir su semilla, se ofrecerán a otra persona: solo este PC sabe que se entregaron.", "%1 des adresses qu'il a données n'ont encore rien reçu. Si sa phrase est de nouveau ajoutée, elles seront proposées à quelqu'un d'autre : seul ce PC sait qu'elles ont été données.", "%1 der ausgegebenen Adressen haben noch nichts erhalten. Wird die Seed-Phrase erneut hinzugefügt, werden sie jemand anderem angeboten: Nur dieser PC weiß, dass sie ausgegeben wurden.", "На %1 из выданных адресов ещё ничего не поступило. Если сид-фразу добавить снова, они будут предложены кому-то другому: только этот ПК знает, что они выданы." },
    { "%1 of the addresses they gave out have had nothing come in yet. If those seeds are added again, they will be offered to someone else: only this PC knows they were given out.", "%1 de las direcciones que entregaron aún no han recibido nada. Si se vuelven a añadir esas semillas, se ofrecerán a otra persona: solo este PC sabe que se entregaron.", "%1 des adresses qu'ils ont données n'ont encore rien reçu. Si ces phrases sont de nouveau ajoutées, elles seront proposées à quelqu'un d'autre : seul ce PC sait qu'elles ont été données.", "%1 der ausgegebenen Adressen haben noch nichts erhalten. Werden diese Seed-Phrasen erneut hinzugefügt, werden sie jemand anderem angeboten: Nur dieser PC weiß, dass sie ausgegeben wurden.", "На %1 из выданных адресов ещё ничего не поступило. Если эти сид-фразы добавить снова, они будут предложены кому-то другому: только этот ПК знает, что они выданы." },
    { "Portfolio", "Cartera", "Portefeuille global", "Portfolio", "Портфель" },
    { "Receive", "Recibir", "Recevoir", "Empfangen", "Получить" },
    { "Coin", "Moneda", "Monnaie", "Coin", "Монета" },
    { "Coins", "Monedas", "Monnaies", "Coins", "Монеты" },
    { "Assets", "Activos", "Actifs", "Assets", "Активы" },
    { "Price", "Precio", "Prix", "Preis", "Цена" },
    { "Holdings", "Tenencias", "Avoirs", "Bestand", "Баланс" },
    { "Value", "Valor", "Valeur", "Wert", "Стоимость" },
    { "Share", "Parte", "Part", "Anteil", "Доля" },
    { "Hide zero balances", "Ocultar saldos en cero", "Masquer les soldes nuls", "Nullsalden ausblenden", "Скрыть нулевые балансы" },
    { "A coin that holds nothing leaves this list. One nobody has answered for yet stays.", "Una moneda sin saldo sale de esta lista. La que aún nadie ha respondido se queda.", "Une monnaie sans solde quitte cette liste. Celle pour laquelle personne n'a encore répondu reste.", "Ein Coin ohne Bestand verschwindet aus dieser Liste. Einer, für den noch niemand geantwortet hat, bleibt.", "Монета без баланса исчезает из списка. Та, о которой ещё никто не ответил, остаётся." },
    { "How the scan stands, and on which servers", "Cómo va el escaneo, y en qué servidores", "Où en est l'analyse, et sur quels serveurs", "Wie der Scan steht und auf welchen Servern", "Как идёт сканирование и на каких серверах" },
    { "%1 wallets · %2 funded of %3 addresses", "%1 billeteras · %2 con fondos de %3 direcciones", "%1 portefeuilles · %2 approvisionnées sur %3 adresses", "%1 Wallets · %2 von %3 Adressen mit Guthaben", "Кошельков: %1 · с балансом %2 из %3 адресов" },
    { "%1 of %2 hold something", "%1 de %2 tienen algo", "%1 sur %2 détiennent quelque chose", "%1 von %2 halten etwas", "С балансом: %1 из %2" },
    { "Addresses that hold some of it", "Direcciones que tienen algo de esto", "Adresses qui en détiennent", "Adressen, die etwas davon halten", "Адреса, на которых это есть" },
    { "Show QR", "Mostrar QR", "Afficher le QR", "QR anzeigen", "Показать QR" },
    { "%1 address holds it", "%1 dirección lo tiene", "%1 adresse en détient", "%1 Adresse hält es", "Адресов с балансом: %1" },
    { "%1 addresses hold it", "%1 direcciones lo tienen", "%1 adresses en détiennent", "%1 Adressen halten es", "Адресов с балансом: %1" },
    { "%1 funded of %2 addresses", "%1 con fondos de %2 direcciones", "%1 approvisionnées sur %2 adresses", "%1 von %2 Adressen mit Guthaben", "С балансом %1 из %2 адресов" },
    { "Not read yet on this network", "Aún no se lee en esta red", "Pas encore lu sur ce réseau", "Auf diesem Netzwerk noch nicht gelesen", "В этой сети ещё не читается" },
    { "The history of Ethereum, USDT and USDC comes in the next stage. Balances are read already, and an address opens on a block explorer.", "El historial de Ethereum, USDT y USDC llega en la próxima etapa. Los saldos ya se leen, y una dirección se abre en un explorador de bloques.", "L'historique d'Ethereum, USDT et USDC arrive à la prochaine étape. Les soldes sont déjà lus, et une adresse s'ouvre dans un explorateur de blocs.", "Der Verlauf von Ethereum, USDT und USDC kommt in der nächsten Stufe. Salden werden bereits gelesen, und eine Adresse öffnet sich in einem Block-Explorer.", "История Ethereum, USDT и USDC появится на следующем этапе. Балансы уже читаются, а адрес открывается в обозревателе блоков." },
    { "Every transaction of this address", "Todas las transacciones de esta dirección", "Toutes les transactions de cette adresse", "Alle Transaktionen dieser Adresse", "Все транзакции этого адреса" },
    { "Add a wallet on the Wallets page, and its addresses to receive on show here.", "Añade una billetera en la página Billeteras, y aquí se mostrarán sus direcciones para recibir.", "Ajoutez un portefeuille sur la page Portefeuilles, et ses adresses de réception s'afficheront ici.", "Fügen Sie auf der Seite Wallets ein Wallet hinzu, dann erscheinen hier seine Empfangsadressen.", "Добавьте кошелёк на странице «Кошельки», и здесь появятся адреса для получения." },
    { "Optional: goes into the QR code", "Opcional: va en el código QR", "Facultatif : va dans le code QR", "Optional: kommt in den QR-Code", "Необязательно: попадёт в QR-код" },
    { "Only this PC knows it. It does not go into the QR code.", "Solo este PC la conoce. No va en el código QR.", "Seul ce PC la connaît. Elle ne va pas dans le code QR.", "Nur dieser PC kennt es. Es kommt nicht in den QR-Code.", "Её знает только этот ПК. В QR-код она не попадает." },
    { "Address type", "Tipo de dirección", "Type d'adresse", "Adresstyp", "Тип адреса" },
    { "Save QR", "Guardar QR", "Enregistrer le QR", "QR speichern", "Сохранить QR" },
    { "New address", "Dirección nueva", "Nouvelle adresse", "Neue Adresse", "Новый адрес" },
    { "This one counts as given out, and the next unused one comes up", "Esta cuenta como entregada, y aparece la siguiente sin usar", "Celle-ci compte comme donnée, et la suivante inutilisée apparaît", "Diese gilt als ausgegeben, und die nächste unbenutzte erscheint", "Этот считается выданным, и появляется следующий неиспользованный" },
    { "Given out", "Entregadas", "Données", "Ausgegeben", "Выданные" },
    { "Copied, but it has been used before", "Copiada, pero ya se ha usado antes", "Copiée, mais elle a déjà été utilisée", "Kopiert, aber sie wurde schon benutzt", "Скопировано, но этот адрес уже использовался" },
    { "Copied, but it was given out before, and nothing has come in yet", "Copiada, pero ya se entregó antes y aún no ha llegado nada", "Copiée, mais elle a déjà été donnée et rien n'est encore arrivé", "Kopiert, aber sie wurde schon ausgegeben, und noch ist nichts eingegangen", "Скопировано, но этот адрес уже выдан, и поступлений ещё не было" },
    { "Copied, but no server has answered for it yet: it may have been used", "Copiada, pero ningún servidor ha respondido aún: puede que ya se haya usado", "Copiée, mais aucun serveur n'a encore répondu : elle a peut-être été utilisée", "Kopiert, aber noch hat kein Server geantwortet: Sie könnte schon benutzt worden sein", "Скопировано, но сервер ещё не ответил: адрес мог использоваться" },
    { "Copied. A change address is not kept as given out", "Copiada. Una dirección de cambio no se guarda como entregada", "Copiée. Une adresse de monnaie n'est pas enregistrée comme donnée", "Kopiert. Eine Wechselgeldadresse wird nicht als ausgegeben vermerkt", "Скопировано. Адрес сдачи не отмечается как выданный" },
    { "Copies the address chosen, and counts it as given out", "Copia la dirección elegida y la cuenta como entregada", "Copie l'adresse choisie et la compte comme donnée", "Kopiert die gewählte Adresse und zählt sie als ausgegeben", "Копирует выбранный адрес и отмечает его как выданный" },
    { "Not in a block yet", "Aún no está en un bloque", "Pas encore dans un bloc", "Noch in keinem Block", "Ещё не в блоке" },
    { "Handed to someone, and nothing has come in yet", "Entregada a alguien, y aún no ha llegado nada", "Donnée à quelqu'un, et rien n'est encore arrivé", "An jemanden ausgegeben, und noch ist nichts eingegangen", "Выдан кому-то, поступлений ещё не было" },
    { "It can still be replaced: do not count it as paid yet", "Aún puede reemplazarse: no lo cuentes como pagado todavía", "Elle peut encore être remplacée : ne la comptez pas encore comme payée", "Sie kann noch ersetzt werden: Noch nicht als bezahlt zählen", "Её ещё могут заменить: пока не считайте оплаченной" },
    { "A scan cannot be seen from here: once it has been scanned, press New address, so that it is not offered again.", "Desde aquí no se ve si se ha escaneado: una vez escaneado, pulsa Dirección nueva para que no vuelva a ofrecerse.", "Un scan ne se voit pas d'ici : une fois le code scanné, appuyez sur Nouvelle adresse pour qu'elle ne soit plus proposée.", "Ein Scan ist von hier aus nicht sichtbar: Drücken Sie danach Neue Adresse, damit sie nicht erneut angeboten wird.", "Отсюда не видно, отсканирован ли код: после сканирования нажмите «Новый адрес», чтобы он больше не предлагался." },
    { "The address moved on, and this one has a label of its own, so what was typed was not kept.", "La dirección ha cambiado y esta ya tiene su propia etiqueta, así que lo escrito no se guardó.", "L'adresse a changé et celle-ci a déjà son propre libellé : ce qui a été saisi n'a pas été gardé.", "Die Adresse ist weitergerückt und diese hat schon eine eigene Bezeichnung, daher wurde die Eingabe nicht übernommen.", "Адрес сменился, а у этого уже есть своя метка, поэтому введённое не сохранено." },
    { "The next unused address lies past the most GitWallet follows. A server that calls every address used does that, so try another one in Settings; fewer wallets also leave more room.", "La siguiente dirección sin usar está más allá de lo que GitWallet sigue. Eso lo provoca un servidor que da todas las direcciones por usadas, así que prueba otro en Ajustes; menos billeteras también dejan más espacio.", "La prochaine adresse inutilisée se trouve au-delà de ce que GitWallet suit. Un serveur qui dit toutes les adresses utilisées provoque cela : essayez-en un autre dans les paramètres ; moins de portefeuilles laissent aussi plus de place.", "Die nächste unbenutzte Adresse liegt jenseits dessen, was GitWallet verfolgt. Das verursacht ein Server, der jede Adresse als benutzt meldet: Versuchen Sie in den Einstellungen einen anderen; weniger Wallets lassen ebenfalls mehr Platz.", "Следующий неиспользованный адрес лежит дальше того, что отслеживает GitWallet. Так бывает, когда сервер называет все адреса использованными: попробуйте другой в настройках; меньшее число кошельков тоже оставляет больше места." },
    { "Held %1 as far as was saved, being checked", "Tenía %1 según lo guardado, en comprobación", "Détenait %1 d'après ce qui est enregistré, en vérification", "Hielt laut Gespeichertem %1, wird geprüft", "По сохранённым данным хранил %1, идёт проверка" },
    { "Had been used, and held nothing as far as was saved, being checked", "Se había usado y no tenía nada según lo guardado, en comprobación", "Avait été utilisée et ne détenait rien d'après ce qui est enregistré, en vérification", "War laut Gespeichertem benutzt und hielt nichts, wird geprüft", "По сохранённым данным использовался и был пуст, идёт проверка" },
    { "Holds %1, of which %2 is not in a block yet", "Tiene %1, de los cuales %2 aún no están en un bloque", "Détient %1, dont %2 pas encore dans un bloc", "Hält %1, davon %2 noch in keinem Block", "Хранит %1, из них %2 ещё не в блоке" },
    { "Held %1 as of %2; no server is answering now", "Tenía %1 a fecha de %2; ahora no responde ningún servidor", "Détenait %1 au %2 ; aucun serveur ne répond maintenant", "Hielt %1 am %2; gerade antwortet kein Server", "Хранил %1 на %2; сейчас ни один сервер не отвечает" },
    { "Held %1 when last read; no server is answering now", "Tenía %1 en la última lectura; ahora no responde ningún servidor", "Détenait %1 à la dernière lecture ; aucun serveur ne répond maintenant", "Hielt %1 beim letzten Lesen; gerade antwortet kein Server", "При последнем чтении хранил %1; сейчас ни один сервер не отвечает" },
    { "Had been used, and held nothing as of %1; no server is answering now", "Se había usado y no tenía nada a fecha de %1; ahora no responde ningún servidor", "Avait été utilisée et ne détenait rien au %1 ; aucun serveur ne répond maintenant", "War benutzt und hielt am %1 nichts; gerade antwortet kein Server", "Использовался и был пуст на %1; сейчас ни один сервер не отвечает" },
    { "Had been used, and held nothing when last read; no server is answering now", "Se había usado y no tenía nada en la última lectura; ahora no responde ningún servidor", "Avait été utilisée et ne détenait rien à la dernière lecture ; aucun serveur ne répond maintenant", "War benutzt und hielt beim letzten Lesen nichts; gerade antwortet kein Server", "Использовался и при последнем чтении был пуст; сейчас ни один сервер не отвечает" },
    { "Given out; nothing had come in as of %1, and no server is answering now", "Entregada; no había llegado nada a fecha de %1, y ahora no responde ningún servidor", "Donnée ; rien n'était arrivé au %1, et aucun serveur ne répond maintenant", "Ausgegeben; bis %1 war nichts eingegangen, und gerade antwortet kein Server", "Выдан; на %1 поступлений не было, и сейчас ни один сервер не отвечает" },
    { "Given out; no server is answering now, so nothing can be said of it", "Entregada; ahora no responde ningún servidor, así que no se puede decir nada de ella", "Donnée ; aucun serveur ne répond maintenant, on ne peut donc rien en dire", "Ausgegeben; gerade antwortet kein Server, daher lässt sich nichts dazu sagen", "Выдан; сейчас ни один сервер не отвечает, поэтому о нём ничего нельзя сказать" },
    { "Unused as of %1; no server is answering now", "Sin usar a fecha de %1; ahora no responde ningún servidor", "Inutilisée au %1 ; aucun serveur ne répond maintenant", "Am %1 unbenutzt; gerade antwortet kein Server", "Не использовался на %1; сейчас ни один сервер не отвечает" },
    { "No server is answering now, so nothing can be said of it", "Ahora no responde ningún servidor, así que no se puede decir nada de ella", "Aucun serveur ne répond maintenant, on ne peut donc rien en dire", "Gerade antwortet kein Server, daher lässt sich nichts dazu sagen", "Сейчас ни один сервер не отвечает, поэтому о нём ничего нельзя сказать" },
    { "As of %1", "A fecha de %1", "Au %1", "Stand %1", "На %1" },
    { "No server answering", "Ningún servidor responde", "Aucun serveur ne répond", "Kein Server antwortet", "Серверы не отвечают" },
    { "Given out: the latest %1 of %2", "Entregadas: las últimas %1 de %2", "Données : les %1 dernières sur %2", "Ausgegeben: die letzten %1 von %2", "Выданные: последние %1 из %2" },
    { "%1 not in a block yet", "%1 aún no en un bloque", "%1 pas encore dans un bloc", "%1 noch in keinem Block", "%1 ещё не в блоке" },
    { "sources disagree on its price", "las fuentes no coinciden en su precio", "les sources divergent sur son prix", "die Quellen sind sich beim Preis uneins", "источники расходятся в цене" },
    { "price from %1", "precio de %1", "prix du %1", "Preis vom %1", "цена от %1" },
    { "No scan follows this address now", "Ahora ningún escaneo sigue esta dirección", "Aucune analyse ne suit cette adresse maintenant", "Kein Scan verfolgt diese Adresse mehr", "Сейчас этот адрес не отслеживается" },
    { "Balances in doubt", "Saldos en duda", "Soldes incertains", "Salden zweifelhaft", "Балансы под сомнением" },
    { "%1 do not add up to their transactions", "%1 no cuadran con sus transacciones", "%1 ne correspondent pas à leurs transactions", "%1 stimmen nicht mit ihren Transaktionen überein", "%1 не сходятся со своими транзакциями" },
    { "%1 wallets", "%1 billeteras", "%1 portefeuilles", "%1 Wallets", "Кошельков: %1" },
    { "Funded: holds a balance now. Used: has transactions or a balance. Given out: handed to someone, and nothing has come in yet.", "Con fondos: tiene saldo ahora. Usada: tiene transacciones o saldo. Entregada: dada a alguien, y aún no ha llegado nada.", "Approvisionnée : détient un solde maintenant. Utilisée : a des transactions ou un solde. Donnée : remise à quelqu'un, et rien n'est encore arrivé.", "Mit Guthaben: hält jetzt ein Guthaben. Benutzt: hat Transaktionen oder ein Guthaben. Ausgegeben: an jemanden gegeben, und noch ist nichts eingegangen.", "С балансом: хранит средства сейчас. Использован: есть транзакции или баланс. Выдан: передан кому-то, поступлений ещё не было." },
    { "GitWallet forgets its public keys and stops watching its addresses. The other wallets stay as they are. Nothing changes on the blockchain, but this PC has no seed for it: to add it back you need these keys, which Public keys shows now.", "GitWallet olvida sus claves públicas y deja de vigilar sus direcciones. Las demás billeteras siguen igual. En la blockchain no cambia nada, pero este PC no tiene su semilla: para volver a añadirla necesitas estas claves, que ahora muestra Claves públicas.", "GitWallet oublie ses clés publiques et cesse de surveiller ses adresses. Les autres portefeuilles restent tels quels. Rien ne change sur la blockchain, mais ce PC n'a pas sa phrase : pour le rajouter, il vous faut ces clés, que Clés publiques affiche maintenant.", "GitWallet vergisst seine öffentlichen Schlüssel und beobachtet seine Adressen nicht mehr. Die anderen Wallets bleiben, wie sie sind. In der Blockchain ändert sich nichts, aber dieser PC hat keine Seed-Phrase dafür: Um es wieder hinzuzufügen, brauchen Sie diese Schlüssel, die Öffentliche Schlüssel jetzt anzeigt.", "GitWallet забудет его открытые ключи и перестанет следить за его адресами. Остальные кошельки останутся как есть. В блокчейне ничего не изменится, но на этом ПК нет его сид-фразы: чтобы добавить его снова, нужны эти ключи, которые сейчас показывает «Открытые ключи»." },
    { "GitWallet forgets its public keys and stops watching its addresses. The other wallets stay as they are. Nothing changes on the blockchain: adding its seed again shows the same addresses.", "GitWallet olvida sus claves públicas y deja de vigilar sus direcciones. Las demás billeteras siguen igual. En la blockchain no cambia nada: al volver a añadir su semilla aparecen las mismas direcciones.", "GitWallet oublie ses clés publiques et cesse de surveiller ses adresses. Les autres portefeuilles restent tels quels. Rien ne change sur la blockchain : ajouter de nouveau sa phrase affiche les mêmes adresses.", "GitWallet vergisst seine öffentlichen Schlüssel und beobachtet seine Adressen nicht mehr. Die anderen Wallets bleiben, wie sie sind. In der Blockchain ändert sich nichts: Wird die Seed-Phrase erneut hinzugefügt, erscheinen dieselben Adressen.", "GitWallet забудет его открытые ключи и перестанет следить за его адресами. Остальные кошельки останутся как есть. В блокчейне ничего не изменится: если снова добавить сид-фразу, появятся те же адреса." },
    { "An older wallet file kept beside wallet.dat may still hold its keys, until Remove all wallets in Settings.", "Un archivo de billetera más antiguo junto a wallet.dat puede conservar aún sus claves, hasta usar Eliminar todas las billeteras en Ajustes.", "Un ancien fichier de portefeuille gardé à côté de wallet.dat peut encore contenir ses clés, jusqu'à Supprimer tous les portefeuilles dans les paramètres.", "Eine ältere Wallet-Datei neben wallet.dat kann seine Schlüssel noch enthalten, bis Alle Wallets entfernen in den Einstellungen.", "Старый файл кошелька рядом с wallet.dat может ещё хранить его ключи, пока в настройках не выбрано «Удалить все кошельки»." },
    { "Its %1 labels and notes go too, for good: only this PC has them, and no seed brings them back. Keep a copy first, under Labels and notes in Settings.", "Sus %1 etiquetas y notas también se van, para siempre: solo este PC las tiene y ninguna semilla las recupera. Guarda antes una copia, en Etiquetas y notas en Ajustes.", "Ses %1 libellés et notes partent aussi, définitivement : seul ce PC les possède, et aucune phrase ne les rétablit. Gardez d'abord une copie, sous Libellés et notes dans les paramètres.", "Auch seine %1 Bezeichnungen und Notizen gehen endgültig verloren: Nur dieser PC hat sie, und keine Seed-Phrase bringt sie zurück. Sichern Sie zuerst eine Kopie unter Bezeichnungen und Notizen in den Einstellungen.", "Его метки и заметки (%1) тоже удалятся навсегда: они есть только на этом ПК, и сид-фраза их не вернёт. Сначала сохраните копию в разделе «Метки и заметки» в настройках." },
    { "Send only %1 on the %2 network to this address. Anything else may be lost.", "Envía solo %1 en la red %2 a esta dirección. Cualquier otra cosa puede perderse.", "N'envoyez que des %1 sur le réseau %2 à cette adresse. Tout le reste peut être perdu.", "Senden Sie nur %1 im %2-Netzwerk an diese Adresse. Alles andere kann verloren gehen.", "Отправляйте на этот адрес только %1 в сети %2. Всё остальное может быть потеряно." },
    { "Send only %1 on the %2 network to this address. Anything else, or another network, may be lost.", "Envía solo %1 en la red %2 a esta dirección. Cualquier otra cosa, u otra red, puede perderse.", "N'envoyez que des %1 sur le réseau %2 à cette adresse. Tout le reste, ou un autre réseau, peut être perdu.", "Senden Sie nur %1 im %2-Netzwerk an diese Adresse. Alles andere oder ein anderes Netzwerk kann verloren gehen.", "Отправляйте на этот адрес только %1 в сети %2. Всё остальное или другая сеть могут привести к потере." },
    { "That amount cannot go into the QR code: write it like 0.015", "Ese importe no puede ir en el código QR: escríbelo como 0.015", "Ce montant ne peut pas aller dans le code QR : écrivez-le comme 0.015", "Dieser Betrag kann nicht in den QR-Code: Schreiben Sie ihn wie 0.015", "Эту сумму нельзя поместить в QR-код: напишите её как 0.015" },
    { "Looking for the next unused address: a server has to answer for it first.", "Buscando la siguiente dirección sin usar: primero tiene que responder un servidor.", "Recherche de la prochaine adresse inutilisée : un serveur doit d'abord répondre.", "Die nächste unbenutzte Adresse wird gesucht: Zuerst muss ein Server antworten.", "Поиск следующего неиспользованного адреса: сначала должен ответить сервер." },
    { "This wallet has no key to receive on.", "Esta billetera no tiene una clave para recibir.", "Ce portefeuille n'a pas de clé pour recevoir.", "Dieses Wallet hat keinen Schlüssel zum Empfangen.", "У этого кошелька нет ключа для получения." },
    { "This wallet has no such key.", "Esta billetera no tiene esa clave.", "Ce portefeuille n'a pas cette clé.", "Dieses Wallet hat keinen solchen Schlüssel.", "У этого кошелька нет такого ключа." },
    { "Holds %1", "Tiene %1", "Détient %1", "Hält %1", "Хранит %1" },
    { "Has been used, and holds nothing now", "Se ha usado, y ahora no tiene nada", "A été utilisée, et ne détient plus rien", "Wurde benutzt und hält jetzt nichts", "Использовался, сейчас пуст" },
    { "Given out, nothing has come in yet", "Entregada, aún no ha llegado nada", "Donnée, rien n'est encore arrivé", "Ausgegeben, noch nichts eingegangen", "Выдан, поступлений пока нет" },
    { "Unused as far as was saved, being checked", "Sin usar según lo guardado, en comprobación", "Inutilisée d'après ce qui est enregistré, en vérification", "Laut Gespeichertem unbenutzt, wird geprüft", "Не использовался по сохранённым данным, идёт проверка" },
    { "Waiting", "Esperando", "En attente", "Wartet", "Ожидание" },
    { "Could not write the file", "No se pudo escribir el archivo", "Impossible d'écrire le fichier", "Die Datei konnte nicht geschrieben werden", "Не удалось записать файл" },
    { "QR saved", "QR guardado", "QR enregistré", "QR gespeichert", "QR сохранён" },
    { "What is kept for this address is not what the key of this wallet gives, so it is not offered.", "Lo guardado para esta dirección no es lo que da la clave de esta billetera, así que no se ofrece.", "Ce qui est conservé pour cette adresse n'est pas ce que donne la clé de ce portefeuille : elle n'est donc pas proposée.", "Was für diese Adresse gespeichert ist, entspricht nicht dem, was der Schlüssel dieses Wallets ergibt, daher wird sie nicht angeboten.", "Сохранённое для этого адреса не совпадает с тем, что даёт ключ кошелька, поэтому он не предлагается." },
    { "Used", "Usada", "Utilisée", "Benutzt", "Использован" },
    { "Total balance", "Saldo total", "Solde total", "Gesamtsaldo", "Общий баланс" },
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
    // A path holds what people named: its own words come translated already.
    if (on == QLatin1String("pathLink") || on == QLatin1String("pathHere") || on == QLatin1String("placeTitle"))
        return;
    // So does a list of wallets: a wallet called Send stays called Send.
    if (on == QLatin1String("walletFilter") || on == QLatin1String("historyWallet") || on == QLatin1String("receiveWallet"))
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
