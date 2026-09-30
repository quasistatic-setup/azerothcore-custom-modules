/*
 * mod-group-speed: ghost speed
 *
 * According to Spell.dbc the ghost aura 8326 grants +50 % run and swim speed
 * (effect 1 SPELL_AURA_MOD_INCREASE_SPEED, effect 2
 * SPELL_AURA_MOD_INCREASE_SWIM_SPEED). This AuraScript sets both values to
 * +200 %, so a ghost runs three times as fast as normal.
 *
 * Why through the aura: Unit::UpdateSpeed recalculates the speed from the
 * auras on every aura change. A direct SetSpeed (like .group speed) would be
 * lost there; the raised aura value stays in effect across relogs, teleports
 * and further auras. The Night Elf aura 20584 (+75 %) does not stack with the
 * ghost aura; the higher value wins.
 *
 * Binding to the aura: data/sql/db-world/mod_group_speed_ghost.sql.
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
