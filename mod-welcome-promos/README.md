# mod-welcome-promos

Schickt jedem neu erstellten Charakter einmalig eine Willkommensmail mit
historischen Aktions- und Sondergegenstaenden.

## Warum ein Modul

AzerothCore bringt mit `mail_server_template` bereits ein Serverpostsystem mit.
Es stellt allerdings bei **jedem Anmelden** zu und vermerkt den Charakter
anschliessend in `mail_server_character`. Auf einem Server mit Playerbots
betrifft das saemtliche Bot-Charaktere. Bei rund 1500 Bots gegenueber zwei
echten Charakteren entstuenden zehntausende Gegenstaende in Bot-Postfaechern,
und jeder neu angelegte Bot bekaeme laufend weitere.

`OnPlayerCreate` loest dagegen nur bei tatsaechlicher Neuerstellung aus.
Bestehende Charaktere bleiben dadurch unberuehrt, ohne dass man sie vorher
irgendwo eintragen muesste.

## Arbeitsweise

| Zeitpunkt | Hook | Vorgang |
|---|---|---|
| Serverstart | `OnBeforeWorldInitialized` | legt die Merktabelle an |
| Charaktererstellung | `OnPlayerCreate` | prueft, verschickt, vermerkt |

Der Hook wird im Core aufgerufen, nachdem die Charaktererstellung erfolgreich
festgeschrieben wurde. Der Charakter steht zu diesem Zeitpunkt in der
Datenbank, der Mailversand ist also sicher.

Ausgenommen sind Bot-Konten. Der Praefix wird aus
`AiPlayerbot.RandomBotAccountPrefix` gelesen, damit beide Seiten nicht
auseinanderlaufen; weitere Praefixe lassen sich konfigurieren.

## Einmaligkeit

Zweifach abgesichert: `OnPlayerCreate` feuert je Charakter genau einmal, und
zusaetzlich wird jeder belieferte Charakter in `mod_welcome_promos_sent`
vermerkt. Ein erneutes Anmelden erzeugt keine weitere Mail.

## Verhaeltnis zur Sammleredition

Die Belohnungen der Sammleredition laufen unveraendert ueber die
Kontokennzeichen des Cores. Es gibt keine Ueberschneidung: Jene verwenden
andere Gegenstandsnummern (13582 Zergling Leash, 13583 Panda Collar,
13584 Diablo Stone), dieses Modul verschickt sie nicht.

## Einstellungen

Siehe `conf/welcome_promos.conf.dist`. Gegenstaende, Absender, Betreff und
Text sind frei konfigurierbar. Nicht vorhandene Gegenstandsnummern werden
uebersprungen und protokolliert, statt die Mail stillschweigend unvollstaendig
zu verschicken.
