-- Bind the ghost aura 8326 to the AuraScript for triple ghost speed.
DELETE FROM `spell_script_names` WHERE `spell_id` = 8326 AND `ScriptName` = 'spell_mod_group_speed_ghost';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (8326, 'spell_mod_group_speed_ghost');
