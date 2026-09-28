/*
 * mod-group-speed: Geistgeschwindigkeit
 *
 * Die Geist-Aura 8326 bringt laut Spell.dbc +50 % Lauf- und
 * Schwimmgeschwindigkeit (Effekt 1 SPELL_AURA_MOD_INCREASE_SPEED, Effekt 2
 * SPELL_AURA_MOD_INCREASE_SWIM_SPEED). Dieses AuraScript setzt beide Werte
 * auf +200 %, der Geist läuft also dreimal so schnell wie normal.
 *
 * Warum über die Aura: Unit::UpdateSpeed berechnet das Tempo bei jeder
 * Auraänderung neu aus den Auren. Ein direktes SetSpeed (wie .group speed)
 * ginge dabei verloren; der erhöhte Aurawert bleibt dagegen bei Relog,
 * Teleport und weiteren Auren wirksam. Die Nachtelfen-Aura 20584 (+75 %)
 * stapelt nicht mit dem Geist, es zählt der höhere Wert.
 *
 * Zuordnung zur Aura: data/sql/db-world/mod_group_speed_ghost.sql.
 */

#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

namespace
{
    constexpr int32 GHOST_SPEED_BONUS_PCT = 200;
}

class spell_mod_group_speed_ghost : public AuraScript
{
    PrepareAuraScript(spell_mod_group_speed_ghost);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = GHOST_SPEED_BONUS_PCT;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_mod_group_speed_ghost::CalculateAmount, EFFECT_1, SPELL_AURA_MOD_INCREASE_SPEED);
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_mod_group_speed_ghost::CalculateAmount, EFFECT_2, SPELL_AURA_MOD_INCREASE_SWIM_SPEED);
    }
};

void AddGhostSpeedScripts()
{
    RegisterSpellScript(spell_mod_group_speed_ghost);
}
