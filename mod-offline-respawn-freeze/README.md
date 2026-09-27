# mod-offline-respawn-freeze

Haelt die Respawn-Zeit von Kreaturen an, solange niemand spielt.

## Zweck

`creature_respawn.respawnTime` ist in AzerothCore eine absolute Unix-Zeit. Ein
Mob, der zehn Minuten bis zum Respawn braucht, erscheint daher auch dann
wieder, wenn in der Zwischenzeit niemand gespielt hat. Fuer einen Server, der
nur zeitweise laeuft, ist das unerwuenscht: Die Welt wirkt bei jedem Start
vollstaendig zurueckgesetzt.

Das Modul gleicht zwei Spannen aus:

1. **Server aus.** Beim Start werden die noch offenen Respawn-Zeitpunkte um
   genau die Ausfallzeit nach hinten verschoben. Ein Mob mit sieben
   verbleibenden Minuten hat nach zwanzig Stunden Ausfallzeit weiterhin sieben
   Minuten vor sich.
2. **Server laeuft, niemand angemeldet.** Beim Logout des letzten menschlichen
   Spielers werden die offenen Respawns geparkt und ihre Restzeit vermerkt,
   beim Login des ersten Menschen erhalten sie genau diese Restzeit zurueck.
   Diese Spanne deckt ab, dass der Server typischerweise vor dem Einloggen und
   nach dem Ausloggen noch eine Weile laeuft.

Der zweite Punkt ist ueber `OfflineRespawnFreeze.FreezeWhileNoPlayerOnline`
abschaltbar.

## Warum die Bots dabei nicht stoeren

Mit `AiPlayerbot.RandomBotAutologin = 1` spielen die Randombots, sobald der
Worldserver laeuft, also auch waehrend niemand am Rechner sitzt. Ein pauschaler
Freeze wuerde deren Kills mit einfrieren und die Welt entvoelkern.

Deshalb wird nicht global eingefroren, sondern nur der Bestand, den der Mensch
hinterlassen hat. Beim Logout werden die offenen Respawns auf einen Zeitpunkt
ein Jahr in der Zukunft gesetzt ("geparkt"); ihre Restzeit steht in einer
eigenen Tabelle. Beim Login erhaelt jeder Eintrag, der noch den Parkwert
traegt, seine Restzeit ab jetzt zurueck. Alles, was die Bots waehrend der
Abwesenheit erlegen, wird nicht geparkt und laeuft voellig normal weiter.

Eine Zuordnung nach Killer ist dafuer nicht noetig. Botsitzungen zaehlen ueber
`WorldSession::IsBot()` nicht als Spieler; es zaehlen der letzte Logout und der
erste Login eines Menschen. Sinnvoll ist das Verfahren daher bei einem einzigen
Spieler beziehungsweise einer einzigen Spielergruppe.

## Was nicht betroffen ist

GameObjects, also Kraeuter, Erz und Truhen, bleiben unberuehrt; sie stehen in
einer eigenen Tabelle, die das Modul nicht anfasst. Ebenso unberuehrt bleiben
Auktionen, Post, Kalender, saisonale Ereignisse und die Weltzeit. Es wird keine
virtuelle Uhr eingefuehrt.

Instanzbindungen (`instance.resettime`) werden bewusst nicht verschoben. Sie
laufen nach echter Kalenderzeit ab, damit die woechentlichen Resets mit der
echten Woche synchron bleiben.

Langlaeufer bleiben ebenfalls aussen vor, siehe `MaxSpawnTimeSecs`.

## Arbeitsweise

| Zeitpunkt | Hook | Vorgang |
|---|---|---|
| Betrieb | `OnUpdate` | schreibt alle 60 Sekunden ein Lebenszeichen |
| Logout des letzten Menschen | `OnPlayerLogout` | parkt offene Respawns, vermerkt Restzeiten |
| Herunterfahren | `OnAfterUnloadAllMaps` | vermerkt den Zeitpunkt als sauberes Ende |
| Start | `OnBeforeWorldInitialized` | verschiebt um die Ausfallzeit |
| Login des ersten Menschen | `OnPlayerLogin` | setzt jeden geparkten Eintrag auf jetzt plus Restzeit |

Das Lebenszeichen deckt den Absturzfall ab: Ohne sauberes Herunterfahren gibt
es sonst keinen Anhaltspunkt, ab wann der Server nicht mehr lief. Die
Abweichung betraegt hoechstens ein Heartbeat-Intervall.

Der Eingriff beim Start erfolgt in `OnBeforeWorldInitialized`, weil Karten ihre
Respawn-Zeiten beim Erzeugen lesen (`Map::LoadRespawnTimes`). Zu diesem
Zeitpunkt existiert noch keine Karte.

Beim Logout und Login ist das anders: Die Karten sind geladen und arbeiten mit
ihrem Speicherabbild. Das Modul schreibt deshalb ueber
`Map::SaveCreatureRespawnTime`, das Karte, Respawn-Queue und Datenbank gemeinsam
pflegt, und setzt zusaetzlich den Timer tot liegender Kreaturobjekte
(`Creature::SetRespawnTime`). Kreaturen, die beim Logout noch als Leiche liegen,
haben noch keinen Eintrag; sie werden ueber das Kreaturobjekt erfasst. Nicht
geladene Karten werden direkt in der Datenbank geparkt.

Parken statt nur Vermerken ist noetig, weil die Respawn-Queue der Karte
(`Map::ProcessRespawns`) faellige Kreaturen erscheinen laesst, sobald ihr Grid
geladen ist, und dabei den Eintrag loescht. Grids bleiben nach dem Logout noch
Minuten geladen, in der Naehe von Bots dauerhaft. Ein Verschieben erst beim
Login fand deshalb nichts mehr vor.

Beim Start bleiben geparkte Eintraege unberuehrt. Die pauschale Karenz entfaellt
dann, weil der Login die Restzeit exakt zurueckgibt.

## Sonderfaelle

Bereits faellige Respawn-Zeiten werden nicht verschoben. Sie wuerden sonst
nachtraeglich in die Zukunft wandern und spaeter erscheinen als ohne das Modul.

AzerothCore setzt `respawnTime` in Instanzen mit Reset-Periode auf "jetzt plus
ein Jahr", um zu kennzeichnen, dass eine Kreatur vor dem naechsten Reset nicht
wiederkehrt. Das ist eine Markierung, kein Zeitpunkt. `MaxFutureDays` haelt
solche Werte aus der Verschiebung heraus.

Stuerzt der Server ab, waehrend jemand angemeldet ist, bleibt der Anker leer.
Dann greift wie zuvor der Startpfad mit der pauschalen Karenz.

Wer das Modul abschaltet, waehrend Respawns geparkt sind, sollte sich vorher
einmal anmelden. Sonst bleiben die geparkten Mobs bis zu einem Jahr aus.
`FreezeWhileNoPlayerOnline = 0` ist dagegen unkritisch: Der naechste Start gibt
den geparkten Eintraegen ihre Restzeit zurueck.

Wird eine geparkte Leiche noch gepluendert, kuerzt der Core ihren Zeitpunkt um
einige Sekunden (`Creature::AllLootRemovedFromCorpse`). Als geparkt gilt daher
alles, was weiter als `MaxFutureDays` in der Zukunft liegt; die Kuerzung geht
von der Restzeit ab, genau wie ohne Parken.

Liegt der Logout laenger als `MaxDowntimeDays` zurueck, erscheint beim Login
alles Geparkte sofort, als waere die Zeit normal gelaufen.

## Langlaeufer

Der Freeze macht aus Kalenderzeit Spielzeit. Bei gewoehnlichen Mobs faellt das
nicht auf: In einer Messung am eigenen Bestand hatten 817 von 889 offenen
Eintraegen eine eigene Respawnzeit von hoechstens fuenf Minuten, sie stehen also
kurz nach dem Login ohnehin wieder da.

Bei seltenen Elite-Spawns kehrt sich die Wirkung um. Grunter mit 42 Stunden
oder Kurmokk mit 35 Stunden waeren bei zwei Stunden Spiel am Abend nicht mehr
nach zwei Tagen zurueck, sondern nach Wochen. Dasselbe gilt fuer Event-NPCs,
deren Ereignis nach echtem Kalender laeuft. `MaxSpawnTimeSecs` nimmt solche
Spawns von der Verschiebung aus; mit der Vorgabe von 1800 Sekunden betraf das
in derselben Messung 11 von 889 Eintraegen.

Ein Radius um den Spieler waere die naheliegende, aber schwaechere Alternative:
Er wuerde nur die ersten Minuten nach dem Login anders machen und braeuchte
einen Mittelpunkt, den es bei Gruppenbots, Instanzen und Fluegen quer ueber den
Kontinent nicht eindeutig gibt.

## Einstellungen

Siehe `conf/offline_respawn_freeze.conf.dist`. Fuer den ersten Lauf empfiehlt
sich `OfflineRespawnFreeze.DryRun = 1`: Das Modul rechnet und protokolliert
dann vollstaendig, ohne etwas zu schreiben.

## Eigene Tabellen

Das Modul legt beide Tabellen in `acore_characters` selbst an.

`mod_offline_respawn_freeze` enthaelt eine einzige Zeile mit dem letzten
bekannten Serverzeitpunkt, der Angabe, ob sauber beendet wurde, dem Anker des
letzten Logouts sowie Umfang und Zeitpunkt der letzten Verschiebung.

`mod_offline_respawn_freeze_snapshot` haelt zwischen Logout und Login den
geparkten Bestand: urspruenglicher Zeitpunkt, Restzeit und Parkwert. Ausserhalb
dieser Spanne ist die Tabelle leer.
