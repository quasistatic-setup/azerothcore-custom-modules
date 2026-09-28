# mod-group-speed

Setzt mit einem Befehl die Geschwindigkeit der ganzen eigenen Gruppe und
lässt Geister dreimal so schnell laufen.

```
.group speed 2     doppelte Geschwindigkeit
.group speed 1     normal
```

## Verhalten

Für jedes online befindliche Mitglied der Party oder des Raids des Aufrufers,
den Aufrufer und Playerbots eingeschlossen, gilt dasselbe wie bei
`.modify speed all` für einen ausgewählten Spieler:

| Punkt | Wert |
|---|---|
| Bewegungsarten | walk, run, swim, flight über `SetSpeed(..., true)` |
| Grenzen | 0.1 bis 50, sonst `Incorrect values.` |
| Übersprungen | Mitglieder im Taxiflug, Mitglieder mit höherer Kontostufe, Mitglieder im Ladebildschirm |
| Nicht betroffen | Begleiter, NPCs, Spieler außerhalb der Gruppe, Offline-Mitglieder |
| Dauer | bis zum Logout, wie `.modify speed all` |

Das WoW-Target spielt keine Rolle. Ohne Gruppe gibt es eine Fehlermeldung und
keine Änderung. Die Rückmeldung ist eine Zeile, etwa
`Group speed set to 2.00 for 4 members; 1 member skipped: Hunt (in flight).`

## Geistgeschwindigkeit

Ein AuraScript auf die Geist-Aura 8326 (`src/GhostSpeed.cpp`) hebt deren
Bonus von +50 % auf +200 % Lauf- und Schwimmgeschwindigkeit: Der Geist läuft
dreimal so schnell wie normal. Das gilt für alle Spieler und Playerbots, auch
nach Relog oder Teleport als Geist, weil der Wert Teil der Aura ist und nicht
wie `.group speed` beim nächsten Neuberechnen verloren geht. Die Nachtelfen-Aura
20584 (+75 %) stapelt nicht, es zählt der höhere Wert.
`data/sql/db-world/mod_group_speed_ghost.sql` bindet das Skript über
`spell_script_names` an die Aura.

## Berechtigung

Registriert mit `RBAC_PERM_COMMAND_MODIFY_SPEED_ALL`, wie `.modify speed all`.
Wirksam ist, wie bei jedem Befehl, die Stufe aus `acore_world.command`.
`data/sql/db-world/mod_group_speed_command.sql` legt die Zeile
`group speed` mit der Stufe von `modify speed all` an (derzeit 2).

## Warum ein Modul

AzerothCore führt die Befehlstabellen aller `CommandScript`s über den Namen
zusammen. `speed` hängt sich so unter das vorhandene `.group`, ohne
`cs_group.cpp` zu verändern; ein Core-Update überschreibt den Befehl nicht.
Ein neues Modul braucht einmalig einen CMake-Lauf, weil die Modulliste beim
Konfigurieren festgelegt wird.
