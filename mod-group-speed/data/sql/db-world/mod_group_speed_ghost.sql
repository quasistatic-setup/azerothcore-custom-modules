-- Geist-Aura 8326 an das AuraScript für dreifache Geistgeschwindigkeit binden.
DELETE FROM `spell_script_names` WHERE `spell_id` = 8326 AND `ScriptName` = 'spell_mod_group_speed_ghost';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (8326, 'spell_mod_group_speed_ghost');
