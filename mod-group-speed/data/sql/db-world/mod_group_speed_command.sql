-- Help text and permission for .group speed.
-- The level is copied from "modify speed all" so the group command is never
-- available at a lower level than the single-target command. If that row is
-- missing, 3 (administrator) applies, which is the stricter choice.
DELETE FROM `command` WHERE `name` = 'group speed';
INSERT INTO `command` (`name`, `security`, `help`)
SELECT 'group speed', IFNULL(MAX(`security`), 3),
       'Syntax: .group speed #rate\n\nSets walk, run, swim and flight speed of every online member of your party or raid (including playerbots) to #rate, like .modify speed all. 1 is normal speed; allowed range 0.1 to 50. Members in flight are skipped. Not persistent across logout.'
FROM `command` WHERE `name` = 'modify speed all';
