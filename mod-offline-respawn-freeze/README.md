# mod-offline-respawn-freeze

Haelt die Respawn-Zeit von Kreaturen an, solange der Server nicht laeuft.

## Zweck

`creature_respawn.respawnTime` ist in AzerothCore eine absolute Unix-Zeit. Ein
Mob, der zehn Minuten bis zum Respawn braucht, erscheint daher auch dann
wieder, wenn der Server in der Zwischenzeit ausgeschaltet war. Fuer einen
Server, der nur zeitweise laeuft, ist das unerwuenscht: Die Welt wirkt bei
jedem Start vollstaendig zurueckgesetzt.

Dieses Modul verschiebt beim Start die noch offenen Respawn-Zeitpunkte um
genau die Ausfallzeit nach hinten. Ein Mob mit sieben verbleibenden Minuten
hat nach zwanzig Stunden Ausfallzeit weiterhin sieben Minuten vor sich.

## Was nicht betroffen ist

GameObjects, also Kraeuter, Erz und Truhen, bleiben unberuehrt; sie stehen in
einer eigenen Tabelle, die das Modul nicht anfasst. Ebenso unberuehrt bleiben
Auktionen, Post, Kalender, saisonale Ereignisse und die Weltzeit. Es wird keine
virtuelle Uhr eingefuehrt.

Instanzbindungen (`instance.resettime`) werden bewusst nicht verschoben. Sie
laufen nach echter Kalenderzeit ab, damit die woechentlichen Resets mit der
echten Woche synchron bleiben.

## Arbeitsweise

| Zeitpunkt | Hook | Vorgang |
|---|---|---|
| Betrieb | `OnUpdate` | schreibt alle 60 Sekunden ein Lebenszeichen |
| Herunterfahren | `OnAfterUnloadAllMaps` | vermerkt den Zeitpunkt als sauberes Ende |
| Start | `OnBeforeWorldInitialized` | verschiebt die offenen Respawn-Zeiten |

Das Lebenszeichen deckt den Absturzfall ab: Ohne sauberes Herunterfahren gibt
es sonst keinen Anhaltspunkt, ab wann der Server nicht mehr lief. Die
Abweichung betraegt hoechstens ein Heartbeat-Intervall.

Der Eingriff erfolgt in `OnBeforeWorldInitialized`, weil Karten ihre
Respawn-Zeiten beim Erzeugen lesen (`Map::LoadRespawnTimes`). Zu diesem
Zeitpunkt existiert noch keine Karte.

## Sonderfaelle

Bereits faellige Respawn-Zeiten werden nicht verschoben. Sie wuerden sonst
nachtraeglich in die Zukunft wandern und spaeter erscheinen als ohne das Modul.

AzerothCore setzt `respawnTime` in Instanzen mit Reset-Periode auf "jetzt plus
ein Jahr", um zu kennzeichnen, dass eine Kreatur vor dem naechsten Reset nicht
wiederkehrt. Das ist eine Markierung, kein Zeitpunkt. `MaxFutureDays` haelt
solche Werte aus der Verschiebung heraus.

## Einstellungen

Siehe `conf/offline_respawn_freeze.conf.dist`. Fuer den ersten Lauf empfiehlt
sich `OfflineRespawnFreeze.DryRun = 1`: Das Modul rechnet und protokolliert
dann vollstaendig, ohne etwas zu schreiben.

## Zustandstabelle

Das Modul legt `mod_offline_respawn_freeze` in `acore_characters` selbst an.
Sie enthaelt eine einzige Zeile mit dem letzten bekannten Serverzeitpunkt, der
Angabe, ob sauber beendet wurde, sowie Umfang und Zeitpunkt der letzten
Verschiebung.
