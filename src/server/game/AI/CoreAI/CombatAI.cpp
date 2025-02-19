/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CombatAI.h"
#include "ConditionMgr.h"
#include "Creature.h"
#include "CreatureAIImpl.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Vehicle.h"

/////////////////
// AggressorAI
/////////////////

int32 AggressorAI::Permissible(Creature const* creature)
{
    // have some hostile factions, it will be selected by IsHostileTo check at MoveInLineOfSight
    if (!creature->IsCivilian() && !creature->IsNeutralToAll())
        return PERMIT_BASE_REACTIVE;

    return PERMIT_BASE_NO;
}

void AggressorAI::UpdateAI(uint32 /*diff*/)
{
    if (!UpdateVictim())
        return;

    DoMeleeAttackIfReady();
}

/////////////////
// CombatAI
/////////////////

void CombatAI::InitializeAI()
{
    for (uint32 i = 0; i < MAX_CREATURE_SPELLS; ++i)
        if (me->m_spells[i] && sSpellMgr->GetSpellInfo(me->m_spells[i], me->GetMap()->GetDifficultyID()))
            spells.push_back(me->m_spells[i]);

    CreatureAI::InitializeAI();
}

void CombatAI::Reset()
{
    events.Reset();
}

void CombatAI::JustDied(Unit* killer)
{
    for (SpellVct::iterator i = spells.begin(); i != spells.end(); ++i)
        if (AISpellInfoType const* info = GetAISpellInfo(*i, me->GetMap()->GetDifficultyID()))
            if (info->condition == AICOND_DIE)
                me->CastSpell(killer, *i, true);
}

void CombatAI::JustEngagedWith(Unit* who)
{
    for (SpellVct::iterator i = spells.begin(); i != spells.end(); ++i)
    {
        if (AISpellInfoType const* info = GetAISpellInfo(*i, me->GetMap()->GetDifficultyID()))
        {
            if (info->condition == AICOND_AGGRO)
                me->CastSpell(who, *i, false);
            else if (info->condition == AICOND_COMBAT)
                events.ScheduleEvent(*i, info->cooldown + rand32() % info->cooldown);
        }
    }
}

void CombatAI::UpdateAI(uint32 diff)
{
    if (!UpdateVictim())
        return;

    events.Update(diff);

    if (me->HasUnitState(UNIT_STATE_CASTING))
        return;

    if (uint32 spellId = events.ExecuteEvent())
    {
        DoCast(spellId);
        if (AISpellInfoType const* info = GetAISpellInfo(spellId, me->GetMap()->GetDifficultyID()))
            events.ScheduleEvent(spellId, info->cooldown + rand32() % info->cooldown);
    }
    else
        DoMeleeAttackIfReady();
}

void CombatAI::SpellInterrupted(uint32 spellId, uint32 unTimeMs)
{
    events.RescheduleEvent(spellId, unTimeMs);
}

/////////////////
// CasterAI
/////////////////

void CasterAI::InitializeAI()
{
    CombatAI::InitializeAI();

    m_attackDist = 30.0f;
    for (SpellVct::iterator itr = spells.begin(); itr != spells.end(); ++itr)
        if (AISpellInfoType const* info = GetAISpellInfo(*itr, me->GetMap()->GetDifficultyID()))
            if (info->condition == AICOND_COMBAT && m_attackDist > info->maxRange)
                m_attackDist = info->maxRange;

    if (m_attackDist == 30.0f)
        m_attackDist = MELEE_RANGE;
}

void CasterAI::JustEngagedWith(Unit* who)
{
    if (spells.empty())
        return;

    uint32 spell = rand32() % spells.size();
    uint32 count = 0;
    for (SpellVct::iterator itr = spells.begin(); itr != spells.end(); ++itr, ++count)
    {
        if (AISpellInfoType const* info = GetAISpellInfo(*itr, me->GetMap()->GetDifficultyID()))
        {
            if (info->condition == AICOND_AGGRO)
                me->CastSpell(who, *itr, false);
            else if (info->condition == AICOND_COMBAT)
            {
                uint32 cooldown = info->realCooldown;
                if (count == spell)
                {
                    DoCast(spells[spell]);
                    cooldown += me->GetCurrentSpellCastTime(*itr);
                }
                events.ScheduleEvent(*itr, cooldown);
            }
        }
    }
}

void CasterAI::UpdateAI(uint32 diff)
{
    if (!UpdateVictim())
        return;

    events.Update(diff);

    if (me->GetVictim() && me->EnsureVictim()->HasBreakableByDamageCrowdControlAura(me))
    {
        me->InterruptNonMeleeSpells(false);
        return;
    }

    if (me->HasUnitState(UNIT_STATE_CASTING))
        return;

    if (uint32 spellId = events.ExecuteEvent())
    {
        DoCast(spellId);
        uint32 casttime = me->GetCurrentSpellCastTime(spellId);
        if (AISpellInfoType const* info = GetAISpellInfo(spellId, me->GetMap()->GetDifficultyID()))
            events.ScheduleEvent(spellId, (casttime ? casttime : 500) + info->realCooldown);
    }
}

//////////////
// ArcherAI
//////////////

ArcherAI::ArcherAI(Creature* c, uint32 scriptId) : CreatureAI(c, scriptId)
{
    if (!me->m_spells[0])
        TC_LOG_ERROR("misc", "ArcherAI set for creature (entry = %u) with spell1=0. AI will do nothing", me->GetEntry());

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(me->m_spells[0], me->GetMap()->GetDifficultyID());
    m_minRange = spellInfo ? spellInfo->GetMinRange(false) : 0;

    if (!m_minRange)
        m_minRange = MELEE_RANGE;
    me->m_CombatDistance = spellInfo ? spellInfo->GetMaxRange(false) : 0;
    me->m_SightDistance = me->m_CombatDistance;
}

void ArcherAI::AttackStart(Unit* who)
{
    if (!who)
        return;

    if (me->IsWithinCombatRange(who, m_minRange))
    {
        if (me->Attack(who, true) && !who->IsFlying())
            me->GetMotionMaster()->MoveChase(who);
    }
    else
    {
        if (me->Attack(who, false) && !who->IsFlying())
            me->GetMotionMaster()->MoveChase(who, me->m_CombatDistance);
    }

    if (who->IsFlying())
        me->GetMotionMaster()->MoveIdle();
}

void ArcherAI::UpdateAI(uint32 /*diff*/)
{
    if (!UpdateVictim())
        return;

    if (!me->IsWithinCombatRange(me->GetVictim(), m_minRange))
        DoSpellAttackIfReady(me->m_spells[0]);
    else
        DoMeleeAttackIfReady();
}

//////////////
// TurretAI
//////////////

TurretAI::TurretAI(Creature* c, uint32 scriptId) : CreatureAI(c, scriptId)
{
    if (!me->m_spells[0])
        TC_LOG_ERROR("misc", "TurretAI set for creature (entry = %u) with spell1=0. AI will do nothing", me->GetEntry());

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(me->m_spells[0], me->GetMap()->GetDifficultyID());
    m_minRange = spellInfo ? spellInfo->GetMinRange(false) : 0;
    me->m_CombatDistance = spellInfo ? spellInfo->GetMaxRange(false) : 0;
    me->m_SightDistance = me->m_CombatDistance;
}

bool TurretAI::CanAIAttack(Unit const* who) const
{
    /// @todo use one function to replace it
    if (!me->IsWithinCombatRange(who, me->m_CombatDistance)
        || (m_minRange && me->IsWithinCombatRange(who, m_minRange)))
        return false;
    return true;
}

void TurretAI::AttackStart(Unit* who)
{
    if (who)
        me->Attack(who, false);
}

void TurretAI::UpdateAI(uint32 /*diff*/)
{
    if (!UpdateVictim())
        return;

    DoSpellAttackIfReady(me->m_spells[0]);
}

//////////////
// VehicleAI
//////////////

VehicleAI::VehicleAI(Creature* creature, uint32 scriptId) : CreatureAI(creature, scriptId), m_HasConditions(false), m_ConditionsTimer(VEHICLE_CONDITION_CHECK_TIME)
{
    LoadConditions();
    m_DoDismiss = false;
    m_DismissTimer = VEHICLE_DISMISS_TIME;
}

// NOTE: VehicleAI::UpdateAI runs even while the vehicle is mounted
void VehicleAI::UpdateAI(uint32 diff)
{
    CheckConditions(diff);

    if (m_DoDismiss)
    {
        if (m_DismissTimer < diff)
        {
            m_DoDismiss = false;
            me->DespawnOrUnsummon();
        }
        else
            m_DismissTimer -= diff;
    }
}

void VehicleAI::OnCharmed(bool /*isNew*/)
{
    bool const charmed = me->IsCharmed();
    if (!me->GetVehicleKit()->IsVehicleInUse() && !charmed && m_HasConditions) // was used and has conditions
    {
        m_DoDismiss = true; // needs reset
    }
    else if (charmed)
        m_DoDismiss = false; // in use again

    m_DismissTimer = VEHICLE_DISMISS_TIME; // reset timer
}

void VehicleAI::LoadConditions()
{
    m_HasConditions = sConditionMgr->HasConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_CREATURE_TEMPLATE_VEHICLE, me->GetEntry());
}

void VehicleAI::CheckConditions(uint32 diff)
{
    if (!m_HasConditions)
        return;

    if (m_ConditionsTimer <= diff)
    {
        if (Vehicle * vehicleKit = me->GetVehicleKit())
        {
            for (SeatMap::iterator itr = vehicleKit->Seats.begin(); itr != vehicleKit->Seats.end(); ++itr)
                if (Unit * passenger = ObjectAccessor::GetUnit(*me, itr->second.Passenger.Guid))
                {
                    if (Player * player = passenger->ToPlayer())
                    {
                        if (!sConditionMgr->IsObjectMeetingNotGroupedConditions(CONDITION_SOURCE_TYPE_CREATURE_TEMPLATE_VEHICLE, me->GetEntry(), player, me))
                        {
                            player->ExitVehicle();
                            return; // check other pessanger in next tick
                        }
                    }
                }
        }

        m_ConditionsTimer = VEHICLE_CONDITION_CHECK_TIME;
    }
    else
        m_ConditionsTimer -= diff;
}

int32 VehicleAI::Permissible(Creature const* creature)
{
    if (creature->IsVehicle())
        return PERMIT_BASE_SPECIAL;

    return PERMIT_BASE_NO;
}
