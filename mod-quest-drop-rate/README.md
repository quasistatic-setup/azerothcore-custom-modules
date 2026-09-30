# mod-quest-drop-rate

Erhöht die Dropchance echter Questgegenstände (`LootStoreItem::needs_quest`,
Spalte `QuestRequired = 1`) um `QuestDropRate.Multiplier`, höchstens auf 100 %.
Umgesetzt über den Hook `GlobalScript::OnItemRoll`, ohne Core-Patch.
Schalter und Faktor stehen in `quest_drop_rate.conf`.

## Wirkung

- Neue Chance ist `min(Tabellenchance × Faktor, 100)`, für jede Loot-Art,
  auch Angeln, Taschendiebstahl und Kürschnerei.
- Außerhalb von Loot-Gruppen wendet der Core danach noch
  `Rate.Drop.Item.<Qualität>` an. Beispiel mit Faktor 2 und
  `Rate.Drop.Item.Normal = 0.8`: aus 40 % wird bei einem weißen
  Questgegenstand 80 × 0.8 = 64 % statt 32 %. Erreicht die Chance 100, fällt
  der Gegenstand sicher.
- Unverändert bleiben normales Loot, Referenzen, alle `Rate.Drop.Item.*` und
  Questgegenstände in Loot-Gruppen, die auch normale Gegenstände enthalten.
  Einträge einer Gruppe teilen sich einen Wurf; mehr Chance für den
  Questgegenstand fehlte dort den übrigen. Diese Paare aus Gegenstand und
  Gruppe liest das Modul beim Start aus der World-Datenbank und meldet ihre
  Zahl im Serverlog.
- Gruppen nur aus Questgegenständen werden angehoben; mehrere
  Questgegenstände einer solchen Gruppe teilen sich höchstens 100 %.
- `.reload config` übernimmt Schalter und Faktor. Geänderte Loot-Tabellen
  wirken auf den Gruppenausschluss erst nach einem Neustart.

## Aufbau

`src/QuestDropRateLogic.h` enthält die Rechenlogik ohne Core-Abhängigkeiten;
`tests/run_logic_test.sh` prüft sie ohne Server.
