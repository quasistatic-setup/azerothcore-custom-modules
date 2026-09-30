# mod-out-of-combat-mana

Vervielfacht die normale Mana-Regeneration von Spielern außerhalb des Kampfes
um `OutOfCombatMana.Multiplier`. Gilt für jedes `Player`-Objekt, also auch für
Playerbots. Umgesetzt über den Hook `PlayerScript::OnPlayerUpdate`, ohne
Core-Patch. Schalter und Faktor stehen in `out_of_combat_mana.conf`.

## Wirkung

- Faktor auf das, was der Core ohnehin gibt, einschließlich `Rate.Mana` und
  der optionalen Anhebung unter Stufe 15. Beispiel mit `Rate.Mana = 1.5` und
  Faktor 2: außerhalb des Kampfes das Doppelte der normalen Regeneration,
  im Kampf unverändert. Faktor 1 entspricht genau dem Verhalten ohne Modul.
- Die Fünf-Sekunden-Regel bleibt: Nach einem Manaverbrauch vervielfacht das
  Modul den unterbrochenen Regenerationswert, wie ihn der Core gerade nutzt.
  Ist er 0, kommt auch nichts hinzu. Auren, die Mana-Regeneration
  verhindern, und `.cheat power` wirken wie im Core.
- Unverändert bleiben `Rate.Mana`, Wut, Energie, Runenmacht sowie Begleiter,
  Wächter und Kreaturen.
- `.reload config` übernimmt Schalter und Faktor; das Serverlog meldet den
  Zustand beim Start und nach jedem Reload.

## Umsetzung

Der Core ruft `OnPlayerUpdate` in `Player::Update` unmittelbar vor der
Regeneration auf und rechnet Mana dort in jedem Schritt mit genau dessen
Dauer. Das Modul rechnet im selben Schritt mit derselben Formel den Anteil
über 1 und führt einen eigenen Bruchteil je Spieler in dessen `CustomData`.
Ganze Punkte schreibt es wie der Core nur als Feldänderung; ein eigenes
`SMSG_POWER_UPDATE` sendet es nur beim Erreichen des Maximums.

`src/OutOfCombatManaLogic.h` enthält die Rechenlogik ohne
Core-Abhängigkeiten; `tests/run_logic_test.sh` prüft sie ohne Server.
