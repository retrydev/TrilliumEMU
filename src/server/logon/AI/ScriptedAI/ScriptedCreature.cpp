/*
 * Copyright (C) 2005 - 2011 MaNGOS <http://www.getmangos.org/>
 *
 * Copyright (C) 2008 - 2011 TrinityCore <http://www.trinitycore.org/>
 *
 * Copyright (C) 2011 TrilliumEMU <http://www.trilliumemu.org/>
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

#include "ScriptPCH.h"
#include "Item.h"
#include "Spell.h"
#include "ObjectMgr.h"
#include "TemporarySummon.h"

// Spell summary for ScriptedAI::SelectSpell
struct TSpellSummary
{
    uint8 Targets;                                          // set of enum SelectTarget
    uint8 Effects;                                          // set of enum SelectEffect
} extern *SpellSummary;

void SummonList::DoZoneInCombat(uint32 entry)
{
    for (iterator i = begin(); i != end();)
    {
        Creature *summon = Unit::GetCreature(*me, *i);
        ++i;
        if (summon && summon->IsAIEnabled
            && (!entry || summon->GetEntry() == entry))
            summon->AI()->DoZoneInCombat();
    }
}

void SummonList::DoAction(uint32 entry, int32 info)
{
    for (iterator i = begin(); i != end();)
    {
        Creature *summon = Unit::GetCreature(*me, *i);
        ++i;
        if (summon && summon->IsAIEnabled
            && (!entry || summon->GetEntry() == entry))
            summon->AI()->DoAction(info);
    }
}

void SummonList::DespawnEntry(uint32 entry)
{
    for (iterator i = begin(); i != end();)
    {
        Creature *summon = Unit::GetCreature(*me, *i);
        if (!summon)
            erase(i++);
        else if (summon->GetEntry() == entry)
        {
            erase(i++);
            summon->setDeathState(JUST_DIED);
            summon->RemoveCorpse();
        }
        else
            ++i;
    }
}

void SummonList::DespawnAll()
{
    while (!empty())
    {
        Creature *summon = Unit::GetCreature(*me, *begin());
        if (!summon)
            erase(begin());
        else
        {
            erase(begin());
            if (TempSummon* summ = summon->ToTempSummon())
            {
                summon->DestroyForNearbyPlayers();
                summ->UnSummon();
            }
            else
                summon->DisappearAndDie();
        }
    }
}

void SummonList::RemoveNotExisting()
{
    for (iterator i = begin(); i != end();)
    {
        if (Unit::GetCreature(*me, *i))
            ++i;
        else
            erase(i++);
    }
}

bool SummonList::HasEntry(uint32 entry)
{
    for (iterator i = begin(); i != end();)
    {
        Creature* summon = Unit::GetCreature(*me, *i);
        if (!summon)
            erase(i++);
        else if (summon->GetEntry() == entry)
            return true;
        else
            ++i;
    }

    return false;
}

ScriptedAI::ScriptedAI(Creature* pCreature) : CreatureAI(pCreature),
    me(pCreature),
    IsFleeing(false),
    _evadeCheckCooldown(2500),
    _isCombatMovementAllowed(true)
{
    _isHeroic = me->GetMap()->IsHeroic();
    _difficulty = Difficulty(me->GetMap()->GetSpawnMode());
}

void ScriptedAI::AttackStartNoMove(Unit* pWho)
{}

void ScriptedAI::UpdateAI(uint32 const /*diff*/)
{}

void ScriptedAI::DoStartMovement(Unit* pVictim, float fDistance, float fAngle)
{
    if (pVictim)
        me->GetMotionMaster()->MoveChase(pVictim, fDistance, fAngle);
}

void ScriptedAI::DoStartNoMovement(Unit* pVictim)
{
    if (!pVictim)
        return;

    me->GetMotionMaster()->MoveIdle();
}

void ScriptedAI::DoStopAttack()
{}

void ScriptedAI::DoCastSpell(Unit* pTarget, SpellInfo const* pSpellInfo, bool bTriggered)
{
    if (!pTarget || me->IsNonMeleeSpellCasted(false))
        return;

    me->StopMoving();
    me->CastSpell(pTarget, pSpellInfo, bTriggered ? TRIGGERED_FULL_MASK : TRIGGERED_NONE);
}

void ScriptedAI::DoPlaySoundToSet(WorldObject* pSource, uint32 uiSoundId)
{
    if (!pSource)
        return;

    if (!GetSoundEntriesStore()->LookupEntry(uiSoundId))
    {
        sLog->outError("TSCR: Invalid soundId %u used in DoPlaySoundToSet (Source: TypeId %u, GUID %u)", uiSoundId, pSource->GetTypeId(), pSource->GetGUIDLow());
        return;
    }

    pSource->PlayDirectSound(uiSoundId);
}

Creature* ScriptedAI::DoSpawnCreature(uint32 entry, float offsetX, float offsetY, float offsetZ, float angle, uint32 type, uint32 despawntime)
{
    return me->SummonCreature(entry, me->GetPositionX() + offsetX, me->GetPositionY() + offsetY, me->GetPositionZ() + offsetZ, angle, TempSummonType(type), despawntime);
}

SpellInfo const* ScriptedAI::SelectSpell(Unit* target, uint32 school, uint32 mechanic, SelectTargetType targets, uint32 powerCostMin, uint32 powerCostMax, float rangeMin, float rangeMax, SelectEffect effects)
{
    //No target so we can't cast
    if (!target)
        return false;

    //Silenced so we can't cast
    if (me->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SILENCED))
        return false;

    //Using the extended script system we first create a list of viable spells
    SpellInfo const* apSpell[CREATURE_MAX_SPELLS];
    memset(apSpell, 0, CREATURE_MAX_SPELLS * sizeof(SpellInfo*));

    uint32 spellCount = 0;

    SpellInfo const* tempSpell = NULL;

    //Check if each spell is viable(set it to null if not)
    for (uint32 i = 0; i < CREATURE_MAX_SPELLS; i++)
    {
        tempSpell = sSpellMgr->GetSpellInfo(me->m_spells[i]);

        //This spell doesn't exist
        if (!tempSpell)
            continue;

        // Targets and Effects checked first as most used restrictions
        //Check the spell targets if specified
        if (targets && !(SpellSummary[me->m_spells[i]].Targets & (1 << (targets-1))))
            continue;

        //Check the type of spell if we are looking for a specific spell type
        if (effects && !(SpellSummary[me->m_spells[i]].Effects & (1 << (effects-1))))
            continue;

        //Check for school if specified
        if (school && (tempSpell->SchoolMask & school) == 0)
            continue;

        //Check for spell mechanic if specified
        if (mechanic && tempSpell->Mechanic != mechanic)
            continue;

        //Make sure that the spell uses the requested amount of power
        if (powerCostMin && tempSpell->ManaCost < powerCostMin)
            continue;

        if (powerCostMax && tempSpell->ManaCost > powerCostMax)
            continue;

        //Continue if we don't have the mana to actually cast this spell
        if (tempSpell->ManaCost > me->GetPower(Powers(tempSpell->PowerType)))
            continue;

        //Check if the spell meets our range requirements
        if (rangeMin && me->GetSpellMinRangeForTarget(target, tempSpell) < rangeMin)
            continue;
        if (rangeMax && me->GetSpellMaxRangeForTarget(target, tempSpell) > rangeMax)
            continue;

        //Check if our target is in range
        if (me->IsWithinDistInMap(target, float(me->GetSpellMinRangeForTarget(target, tempSpell))) || !me->IsWithinDistInMap(target, float(me->GetSpellMaxRangeForTarget(target, tempSpell))))
            continue;

        //All good so lets add it to the spell list
        apSpell[spellCount] = tempSpell;
        ++spellCount;
    }

    //We got our usable spells so now lets randomly pick one
    if (!spellCount)
        return NULL;

    return apSpell[urand(0, spellCount - 1)];
}

bool ScriptedAI::CanCast(Unit* target, SpellInfo const* spell, bool triggered /*= false*/)
{
    //No target so we can't cast
    if (!target || !spell)
        return false;

    //Silenced so we can't cast
    if (!triggered && me->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SILENCED))
        return false;

    //Check for power
    if (!triggered && me->GetPower(Powers(spell->PowerType)) < spell->ManaCost)
        return false;

    //Unit is out of range of this spell
    if (me->IsInRange(target, float(me->GetSpellMinRangeForTarget(target, spell)), float(me->GetSpellMaxRangeForTarget(target, spell))))
        return false;

    return true;
}

void ScriptedAI::DoResetThreat()
{
    if (!me->CanHaveThreatList() || me->getThreatManager().isThreatListEmpty())
    {
        sLog->outError("TSCR: DoResetThreat called for creature that either cannot have threat list or has empty threat list (me entry = %d)", me->GetEntry());
        return;
    }

    std::list<HostileReference*>& threatlist = me->getThreatManager().getThreatList();

    for (std::list<HostileReference*>::iterator itr = threatlist.begin(); itr != threatlist.end(); ++itr)
    {
        Unit* pUnit = Unit::GetUnit((*me), (*itr)->getUnitGuid());

        if (pUnit && DoGetThreat(pUnit))
            DoModifyThreatPercent(pUnit, -100);
    }
}

float ScriptedAI::DoGetThreat(Unit* pUnit)
{
    if (!pUnit) return 0.0f;
    return me->getThreatManager().getThreat(pUnit);
}

void ScriptedAI::DoModifyThreatPercent(Unit* pUnit, int32 pct)
{
    if (!pUnit) return;
    me->getThreatManager().modifyThreatPercent(pUnit, pct);
}

void ScriptedAI::DoTeleportTo(float fX, float fY, float fZ, uint32 uiTime)
{
    me->Relocate(fX, fY, fZ);
    me->SendMonsterMove(fX, fY, fZ, uiTime);
}

void ScriptedAI::DoTeleportTo(const float fPos[4])
{
    me->NearTeleportTo(fPos[0], fPos[1], fPos[2], fPos[3]);
}

void ScriptedAI::DoTeleportPlayer(Unit* pUnit, float fX, float fY, float fZ, float fO)
{
    if (!pUnit || pUnit->GetTypeId() != TYPEID_PLAYER)
    {
        if (pUnit)
            sLog->outError("TSCR: Creature " UI64FMTD " (Entry: %u) Tried to teleport non-player unit (Type: %u GUID: " UI64FMTD ") to x: %f y:%f z: %f o: %f. Aborted.", me->GetGUID(), me->GetEntry(), pUnit->GetTypeId(), pUnit->GetGUID(), fX, fY, fZ, fO);
        return;
    }

    CAST_PLR(pUnit)->TeleportTo(pUnit->GetMapId(), fX, fY, fZ, fO, TELE_TO_NOT_LEAVE_COMBAT);
}

void ScriptedAI::DoTeleportAll(float fX, float fY, float fZ, float fO)
{
    Map *map = me->GetMap();
    if (!map->IsDungeon())
        return;

    Map::PlayerList const &PlayerList = map->GetPlayers();
    for (Map::PlayerList::const_iterator i = PlayerList.begin(); i != PlayerList.end(); ++i)
        if (Player* i_pl = i->getSource())
            if (i_pl->isAlive())
                i_pl->TeleportTo(me->GetMapId(), fX, fY, fZ, fO, TELE_TO_NOT_LEAVE_COMBAT);
}

Unit* ScriptedAI::DoSelectLowestHpFriendly(float fRange, uint32 uiMinHPDiff)
{
    Unit* pUnit = NULL;
    Trillium::MostHPMissingInRange u_check(me, fRange, uiMinHPDiff);
    Trillium::UnitLastSearcher<Trillium::MostHPMissingInRange> searcher(me, pUnit, u_check);
    me->VisitNearbyObject(fRange, searcher);

    return pUnit;
}

std::list<Creature*> ScriptedAI::DoFindFriendlyCC(float fRange)
{
    std::list<Creature*> pList;
    Trillium::FriendlyCCedInRange u_check(me, fRange);
    Trillium::CreatureListSearcher<Trillium::FriendlyCCedInRange> searcher(me, pList, u_check);
    me->VisitNearbyObject(fRange, searcher);
    return pList;
}

std::list<Creature*> ScriptedAI::DoFindFriendlyMissingBuff(float fRange, uint32 uiSpellid)
{
    std::list<Creature*> pList;
    Trillium::FriendlyMissingBuffInRange u_check(me, fRange, uiSpellid);
    Trillium::CreatureListSearcher<Trillium::FriendlyMissingBuffInRange> searcher(me, pList, u_check);
    me->VisitNearbyObject(fRange, searcher);
    return pList;
}

Player* ScriptedAI::GetPlayerAtMinimumRange(float fMinimumRange)
{
    Player* pPlayer = NULL;

    CellPair pair(Trillium::ComputeCellPair(me->GetPositionX(), me->GetPositionY()));
    Cell cell(pair);
    cell.data.Part.reserved = ALL_DISTRICT;
    cell.SetNoCreate();

    Trillium::PlayerAtMinimumRangeAway check(me, fMinimumRange);
    Trillium::PlayerSearcher<Trillium::PlayerAtMinimumRangeAway> searcher(me, pPlayer, check);
    TypeContainerVisitor<Trillium::PlayerSearcher<Trillium::PlayerAtMinimumRangeAway>, GridTypeMapContainer> visitor(searcher);

    cell.Visit(pair, visitor, *(me->GetMap()));

    return pPlayer;
}

void ScriptedAI::SetEquipmentSlots(bool loadDefault, int32 mainHand /*= EQUIP_NO_CHANGE*/, int32 offHand /*= EQUIP_NO_CHANGE*/, int32 ranged /*= EQUIP_NO_CHANGE*/)
{
    if (loadDefault)
    {
        if (CreatureTemplate const* creatureInfo = sObjectMgr->GetCreatureTemplate(me->GetEntry()))
            me->LoadEquipment(creatureInfo->equipmentId, true);

        return;
    }

    if (mainHand >= 0)
        me->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + 0, uint32(mainHand));

    if (offHand >= 0)
        me->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + 1, uint32(offHand));

    if (ranged >= 0)
        me->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + 2, uint32(ranged));
}

void ScriptedAI::SetCombatMovement(bool allowMovement)
{
    _isCombatMovementAllowed = allowMovement;
}

enum eNPCs
{
    NPC_BROODLORD   = 12017,
    NPC_VOID_REAVER = 19516,
    NPC_JAN_ALAI    = 23578,
    NPC_SARTHARION  = 28860
};

// Hacklike storage used for misc creatures that are expected to evade of outside of a certain area.
// It is assumed the information is found elswehere and can be handled by the core. So far no luck finding such information/way to extract it.
bool ScriptedAI::EnterEvadeIfOutOfCombatArea(uint32 const diff)
{
    if (_evadeCheckCooldown <= diff)
        _evadeCheckCooldown = 2500;
    else
    {
        _evadeCheckCooldown -= diff;
        return false;
    }

    if (me->IsInEvadeMode() || !me->getVictim())
        return false;

    float fX = me->GetPositionX();
    float fY = me->GetPositionY();
    float fZ = me->GetPositionZ();

    switch(me->GetEntry())
    {
        case NPC_BROODLORD:                                         // broodlord (not move down stairs)
            if (fZ > 448.60f)
                return false;
            break;
        case NPC_VOID_REAVER:                                         // void reaver (calculate from center of room)
            if (me->GetDistance2d(432.59f, 371.93f) < 105.0f)
                return false;
            break;
        case NPC_JAN_ALAI:                                         // jan'alai (calculate by Z)
            if (fZ > 12.0f)
                return false;
            break;
        case NPC_SARTHARION:                                         // sartharion (calculate box)
            if (fX > 3218.86f && fX < 3275.69f && fY < 572.40f && fY > 484.68f)
                return false;
            break;
        default:
            sLog->outError("TSCR: EnterEvadeIfOutOfCombatArea used for creature entry %u, but does not have any definition.", me->GetEntry());
            return false;
    }

    EnterEvadeMode();
    return true;
}

void Scripted_NoMovementAI::AttackStart(Unit* target)
{}

// BossAI - for instanced bosses

BossAI::BossAI(Creature* creature, uint32 bossId) : ScriptedAI(creature),
    instance(creature->GetInstanceScript()),
    summons(creature),
    _boundary(instance ? instance->GetBossBoundary(bossId) : NULL),
    _bossId(bossId)
{
}

void BossAI::_Reset()
{
    if (!me->isAlive())
        return;

    me->ResetLootMode();
    events.Reset();
    summons.DespawnAll();
    if (instance)
        instance->SetBossState(_bossId, NOT_STARTED);
}

void BossAI::_JustDied()
{
    events.Reset();
    summons.DespawnAll();
    if (instance)
    {
        instance->SetBossState(_bossId, DONE);
        instance->SaveToDB();
    }
}

void BossAI::_EnterCombat()
{
    me->setActive(true);
    DoZoneInCombat();
    if (instance)
    {
        // bosses do not respawn, check only on enter combat
        if (!instance->CheckRequiredBosses(_bossId))
        {
            EnterEvadeMode();
            return;
        }
        instance->SetBossState(_bossId, IN_PROGRESS);
    }
}

void BossAI::TeleportCheaters()
{
    float x, y, z;
    me->GetPosition(x, y, z);
    std::list<HostileReference*>& threatList = me->getThreatManager().getThreatList();
    for (std::list<HostileReference*>::iterator itr = threatList.begin(); itr != threatList.end(); ++itr)
        if (Unit* target = (*itr)->getTarget())
            if (target->GetTypeId() == TYPEID_PLAYER && !CheckBoundary(target))
                target->NearTeleportTo(x, y, z, 0);
}

bool BossAI::CheckBoundary(Unit* who)
{
    if (!GetBoundary() || !who)
        return true;

    for (BossBoundaryMap::const_iterator itr = GetBoundary()->begin(); itr != GetBoundary()->end(); ++itr)
    {
        switch (itr->first)
        {
            case BOUNDARY_N:
                if (me->GetPositionX() > itr->second)
                    return false;
                break;
            case BOUNDARY_S:
                if (me->GetPositionX() < itr->second)
                    return false;
                break;
            case BOUNDARY_E:
                if (me->GetPositionY() < itr->second)
                    return false;
                break;
            case BOUNDARY_W:
                if (me->GetPositionY() > itr->second)
                    return false;
                break;
            case BOUNDARY_NW:
                if (me->GetPositionX() + me->GetPositionY() > itr->second)
                    return false;
                break;
            case BOUNDARY_SE:
                if (me->GetPositionX() + me->GetPositionY() < itr->second)
                    return false;
                break;
            case BOUNDARY_NE:
                if (me->GetPositionX() - me->GetPositionY() > itr->second)
                    return false;
                break;
            case BOUNDARY_SW:
                if (me->GetPositionX() - me->GetPositionY() < itr->second)
                    return false;
                break;
            default:
                break;
        }
    }

    return true;
}

void BossAI::JustSummoned(Creature* summon)
{
    summons.Summon(summon);
    if (me->isInCombat())
        DoZoneInCombat(summon);
}

void BossAI::SummonedCreatureDespawn(Creature* summon)
{
    summons.Despawn(summon);
}

void BossAI::UpdateAI(uint32 const diff)
{
    if (!UpdateVictim())
        return;

    events.Update(diff);

    if (me->HasUnitState(UNIT_STAT_CASTING))
        return;

    while (uint32 eventId = events.ExecuteEvent())
        ExecuteEvent(eventId);
}

// WorldBossAI - for non-instanced bosses

WorldBossAI::WorldBossAI(Creature* creature) :
    ScriptedAI(creature),
    summons(creature)
{
}

void WorldBossAI::_Reset()
{
    if (!me->isAlive())
        return;

    events.Reset();
    summons.DespawnAll();
}

void WorldBossAI::_JustDied()
{
    events.Reset();
    summons.DespawnAll();
}

void WorldBossAI::_EnterCombat()
{
    Unit* target = SelectTarget(SELECT_TARGET_RANDOM, 0, 0.0f, true);
    if (target)
        AttackStart(target);
}

void WorldBossAI::JustSummoned(Creature* summon)
{
    summons.Summon(summon);
    Unit* target = SelectTarget(SELECT_TARGET_RANDOM, 0, 0.0f, true);
    if (target)
        summon->AI()->AttackStart(target);
}

void WorldBossAI::SummonedCreatureDespawn(Creature* summon)
{
    summons.Despawn(summon);
}

void WorldBossAI::UpdateAI(uint32 const diff)
{
    if (!UpdateVictim())
        return;

    events.Update(diff);

    if (me->HasUnitState(UNIT_STAT_CASTING))
        return;

    while (uint32 eventId = events.ExecuteEvent())
        ExecuteEvent(eventId);
}

// SD2 grid searchers.
Creature* GetClosestCreatureWithEntry(WorldObject* source, uint32 entry, float maxSearchRange, bool alive /*= true*/)
{
    return source->FindNearestCreature(entry, maxSearchRange, alive);
}

GameObject* GetClosestGameObjectWithEntry(WorldObject* source, uint32 entry, float maxSearchRange)
{
    return source->FindNearestGameObject(entry, maxSearchRange);
}

void GetCreatureListWithEntryInGrid(std::list<Creature*>& list, WorldObject* source, uint32 entry, float maxSearchRange)
{
    source->GetCreatureListWithEntryInGrid(list, entry, maxSearchRange);
}

void GetGameObjectListWithEntryInGrid(std::list<GameObject*>& list, WorldObject* source, uint32 entry, float maxSearchRange)
{
    source->GetGameObjectListWithEntryInGrid(list, entry, maxSearchRange);
}
