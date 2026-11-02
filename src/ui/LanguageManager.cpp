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
    { "%1 lines name a transaction these wallets have not read yet, and look like a private key until they have. Keep the file, and import it again once the history is read.", "%1 líneas nombran una transacción que estas billeteras aún no han leído, y hasta entonces parecen una clave privada. Conserva el archivo e impórtalo de nuevo cuando el historial esté leído.", "%1 lignes nomment une transaction que ces portefeuilles n'ont pas encore lue, et ressemblent à une clé privée d'ici là. Gardez le fichier et importez-le de nouveau une fois l'historique lu.", "%1 Zeilen nennen eine Transaktion, die diese Wallets noch nicht gelesen haben, und sehen bis dahin wie ein privater Schlüssel aus. Behalten Sie die Datei und importieren Sie sie erneut, sobald der Verlauf gelesen ist.", "В %1 строках указана транзакция, которую эти кошельки ещё не прочитали, и до тех пор они похожи на приватный ключ. Сохраните файл и импортируйте его снова, когда история будет прочитана." },
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
