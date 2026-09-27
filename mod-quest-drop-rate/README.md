# mod-quest-drop-rate

Erhöht die Dropchance echter Questgegenstände (`LootStoreItem::needs_quest`,
Spalte `QuestRequired = 1`) um `QuestDropRate.Multiplier`, höchstens auf 100 %.
Umgesetzt über den Hook `GlobalScript::OnItemRoll`, ohne Core-Patch.

Unverändert bleiben normales Loot, Referenzen, Gegenstände ohne
Quest-Anforderung, alle `Rate.Drop.Item.*`-Werte und Questgegenstände in
Loot-Gruppen, die auch normale Gegenstände enthalten. Betrieb und Wirkung
beschreibt `docs/einstellungen.md` im WoW-Server-Verzeichnis.

`src/QuestDropRateLogic.h` enthält die Rechenlogik ohne Core-Abhängigkeiten.
