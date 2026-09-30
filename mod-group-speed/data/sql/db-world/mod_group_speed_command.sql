-- Hilfetext und Berechtigung für .group speed.
-- Die Stufe wird von "modify speed all" übernommen, damit der Gruppenbefehl nie
-- niedriger freigegeben ist als der Einzelbefehl. Fehlt jene Zeile, gilt 3
-- (Administrator), also eher strenger.
DELETE FROM `command` WHERE `name` = 'group speed';
INSERT INTO `command` (`name`, `security`, `help`)
SELECT 'group speed', IFNULL(MAX(`security`), 3),
       'Syntax: .group speed #rate\n\nSets walk, run, swim and flight speed of every online member of your party or raid (including playerbots) to #rate, like .modify speed all. 1 is normal speed; allowed range 0.1 to 50. Members in flight are skipped. Not persistent across logout.'
FROM `command` WHERE `name` = 'modify speed all';
