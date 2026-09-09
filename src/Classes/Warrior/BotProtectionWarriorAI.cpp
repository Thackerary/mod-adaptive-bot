/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "ProtectionWarriorSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "SpellAuras.h"

class BotProtectionWarriorAI : public AdaptiveBotAI
{
public:
    explicit BotProtectionWarriorAI(Creature* creature) : AdaptiveBotAI(creature) {}

    bool IsTankBot() const override { return true; }[cite: 6]

    uint8 GetTalentSpellMinLevel(uint32 spellId) const override[cite: 6]
    {
        switch (spellId)
        {
            case ProtectionWarriorSpells::LAST_STAND:
                return 20;
            case ProtectionWarriorSpells::CONCUSSION_BLOW:
                return 30;
            case ProtectionWarriorSpells::SHIELD_SLAM:
                return 40;
            case ProtectionWarriorSpells::DEVASTATE:
                return 50;
            case ProtectionWarriorSpells::SHOCKWAVE:
                return 60;
            default:
                return 0;
        }
    }

    void Reset() override[cite: 6]
    {
        AdaptiveBotAI::Reset();
        shoutCheckTimer = 0;
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 /*level*/) override[cite: 6]
    {
        ApplyPassiveTalents();
    }

    void UpdateAI(uint32 diff) override[cite: 6]
    {
        UpdateTimers(diff);[cite: 6]

        // 维护团队怒吼计时器 (战斗内外通用)
        if (shoutCheckTimer <= diff)
            shoutCheckTimer = 3000;
        else
            shoutCheckTimer -= diff;

        // 1. 脱战业务维护
        if (!me->IsInCombat())[cite: 6]
        {
            if (TryEngageCombat())[cite: 6]
                return;

            UpdateFollowMaster(diff);[cite: 6]

            if (shoutCheckTimer == 3000)
                MaintainTeamShout();[cite: 6]

            if (me->GetLevel() >= 10 && !me->HasAura(ProtectionWarriorSpells::DEFENSIVE_STANCE))[cite: 6]
            {
                if (CanCast(me, ProtectionWarriorSpells::DEFENSIVE_STANCE, true))[cite: 6]
                    ExecuteSpell(me, ProtectionWarriorSpells::DEFENSIVE_STANCE);[cite: 6]
            }
            return;
        }

        // 2. 索敌仲裁
        Unit* victim = SelectAssistTarget();[cite: 6]
        if (!victim || !victim->IsAlive())[cite: 6]
            return;

        // 3. 近战平砍与追击状态机接管
        ManageMeleeCombat(victim);[cite: 6]

        // =====================================================================
        // Action Priority List (APL) 核心决策循环
        // =====================================================================

        // ---------------------------------------------------------------------
        // P0: 核心姿态维护与怒气唤醒
        // ---------------------------------------------------------------------
        if (me->GetLevel() >= 10 && !me->HasAura(ProtectionWarriorSpells::DEFENSIVE_STANCE))[cite: 6]
        {
            if (CanCast(me, ProtectionWarriorSpells::DEFENSIVE_STANCE, true))[cite: 6]
            {
                ExecuteSpell(me, ProtectionWarriorSpells::DEFENSIVE_STANCE);[cite: 6]
                return;
            }
        }

        if (me->GetPower(POWER_RAGE) < 150)[cite: 6]
        {
            if (CanCast(me, ProtectionWarriorSpells::BLOODRAGE, false))[cite: 6]
                ExecuteSpell(me, ProtectionWarriorSpells::BLOODRAGE, false);[cite: 6]
        }

        // ---------------------------------------------------------------------
        // P1: 硬核减伤链与生死急救
        // ---------------------------------------------------------------------
        float const hpPct = me->GetHealthPct();[cite: 6]

        // 生命 < 25%：破釜沉舟
        if (hpPct < 25.0f)[cite: 6]
        {
            uint32 const lastStand = GetAppropriateRank(ProtectionWarriorSpells::LAST_STAND);[cite: 6]
            if (lastStand && CanCast(me, lastStand, false))[cite: 6]
            {
                if (ExecuteSpell(me, lastStand, false))[cite: 6]
                    return;
            }
        }

        // 生命 < 35%：盾墙
        if (hpPct < 35.0f)[cite: 6]
        {
            uint32 const shieldWall = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_WALL);[cite: 6]
            if (shieldWall && CanCast(me, shieldWall, false))[cite: 6]
            {
                if (ExecuteSpell(me, shieldWall, false))[cite: 6]
                    return;
            }
        }

        // 生命 < 50%：狂暴回复 (前置激怒激活保护)
        if (hpPct < 50.0f && !me->HasAura(ProtectionWarriorSpells::ENRAGED_REGENERATION))[cite: 6]
        {
            bool hasEnrage = me->HasAura(ProtectionWarriorSpells::BLOODRAGE) ||
                             me->HasAura(ProtectionWarriorSpells::BERSERKER_RAGE);[cite: 6]

            if (!hasEnrage)[cite: 6]
            {
                if (CanCast(me, ProtectionWarriorSpells::BLOODRAGE, false))[cite: 6]
                    hasEnrage = ExecuteSpell(me, ProtectionWarriorSpells::BLOODRAGE, false);[cite: 6]
                else if (CanCast(me, ProtectionWarriorSpells::BERSERKER_RAGE, false))[cite: 6]
                    hasEnrage = ExecuteSpell(me, ProtectionWarriorSpells::BERSERKER_RAGE, false);[cite: 6]
            }

            if (hasEnrage)[cite: 6]
            {
                uint32 const enrRegen = GetAppropriateRank(ProtectionWarriorSpells::ENRAGED_REGENERATION);[cite: 6]
                if (enrRegen && CanCast(me, enrRegen, false))[cite: 6]
                {
                    if (ExecuteSpell(me, enrRegen, false))[cite: 6]
                        return;
                }
            }
        }

        // 生命 < 80% 或正在挨打：盾牌格挡
        if (hpPct < 80.0f || !me->getAttackers().empty())[cite: 6]
        {
            uint32 const shieldBlock = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_BLOCK);[cite: 6]
            if (shieldBlock && CanCast(me, shieldBlock, false))[cite: 6]
                ExecuteSpell(me, shieldBlock, false);[cite: 6]
        }

        // ---------------------------------------------------------------------
        // P2: 机动突进、战术打断、援护与换防救火
        // ---------------------------------------------------------------------
        float const distToVictim = me->GetDistance(victim);[cite: 6]

        // 1. 常规起手突进接怪
        if (distToVictim >= 8.0f && distToVictim <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT))[cite: 6]
        {
            uint32 const charge = GetAppropriateRank(ProtectionWarriorSpells::CHARGE);[cite: 6]
            if (charge && CanCast(victim, charge, false))[cite: 6]
            {
                if (ExecuteSpell(victim, charge, false))[cite: 6]
                    return;
            }

            uint32 const intercept = GetAppropriateRank(ProtectionWarriorSpells::INTERCEPT);[cite: 6]
            if (intercept && CanCast(victim, intercept, false))[cite: 6]
            {
                if (ExecuteSpell(victim, intercept, false))[cite: 6]
                    return;
            }
        }

        // 2. 战术打断：目标正在读条施法
        if (victim->HasUnitState(UNIT_STATE_CASTING))[cite: 6]
        {
            if (me->IsWithinMeleeRange(victim))[cite: 6]
            {
                uint32 const shieldBash = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_BASH);[cite: 6]
                if (shieldBash && CanCast(victim, shieldBash, true))[cite: 6]
                {
                    if (ExecuteSpell(victim, shieldBash, true))[cite: 6]
                        return;
                }
            }
            else if (distToVictim >= 8.0f && distToVictim <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT))[cite: 6]
            {
                uint32 const charge = GetAppropriateRank(ProtectionWarriorSpells::CHARGE);[cite: 6]
                if (charge && CanCast(victim, charge, false))[cite: 6]
                {
                    if (ExecuteSpell(victim, charge, false))[cite: 6]
                        return;
                }
                uint32 const intercept = GetAppropriateRank(ProtectionWarriorSpells::INTERCEPT);[cite: 6]
                if (intercept && CanCast(victim, intercept, false))[cite: 6]
                {
                    if (ExecuteSpell(victim, intercept, false))[cite: 6]
                        return;
                }
            }
        }

        // 3. 战术机动：援护队友 (ID 3411)
        Player* master = GetMaster();[cite: 6]
        if (master && master->IsAlive() && master->GetHealthPct() < 70.0f && !master->getAttackers().empty())[cite: 6]
        {
            float const distToMaster = me->GetDistance(master);[cite: 6]
            if (distToMaster >= 8.0f && distToMaster <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT))[cite: 6]
            {
                uint32 const intervene = GetAppropriateRank(ProtectionWarriorSpells::INTERVENE);[cite: 6]
                if (intervene && CanCast(master, intervene, true))[cite: 6]
                {
                    if (ExecuteSpell(master, intervene, true))[cite: 6]
                        return;
                }
            }
        }

        // 4. 仇恨失控剥离 (换防救火)
        if (Unit* urgentTarget = GetUrgentThreatTarget())[cite: 6]
        {
            float const distToUrgent = me->GetDistance(urgentTarget);[cite: 6]

            // 战神远距离冲锋/拦截优先贴脸打断控怪
            if (distToUrgent >= 8.0f && distToUrgent <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT))[cite: 6]
            {
                uint32 const charge = GetAppropriateRank(ProtectionWarriorSpells::CHARGE);[cite: 6]
                if (charge && CanCast(urgentTarget, charge, false))[cite: 6]
                {
                    if (ExecuteSpell(urgentTarget, charge, false))[cite: 6]
                        return;
                }
            }

            // 远程嘲讽 (30 码, 强拉仇恨)
            uint32 const taunt = GetAppropriateRank(ProtectionWarriorSpells::TAUNT);[cite: 6]
            if (taunt && CanCast(urgentTarget, taunt, false))[cite: 6]
            {
                if (ExecuteSpell(urgentTarget, taunt, false))[cite: 6]
                    return;
            }
        }

        // 5. 强力单体昏迷硬控：震荡猛击
        if (victim->HasUnitState(UNIT_STATE_CASTING) || victim->GetHealthPct() > 50.0f)[cite: 6]
        {
            uint32 const concBlow = GetAppropriateRank(ProtectionWarriorSpells::CONCUSSION_BLOW);[cite: 6]
            if (concBlow && CanCast(victim, concBlow, true))[cite: 6]
            {
                if (ExecuteSpell(victim, concBlow, true))[cite: 6]
                    return;
            }
        }

        // ---------------------------------------------------------------------
        // P3: 核心仇恨打击、减益维持与 AoE 控制
        // ---------------------------------------------------------------------
        uint8 meleeEnemies = 0;[cite: 6]
        for (Unit* attacker : me->getAttackers())[cite: 6]
        {
            if (attacker && attacker->IsAlive() && me->IsWithinMeleeRange(attacker))[cite: 6]
            {
                ++meleeEnemies;[cite: 6]
                if (meleeEnemies >= 3)[cite: 6]
                    break;
            }
        }

        // 1. 维持核心减速/减攻速 Debuff：雷霆一击
        if (me->IsWithinDist(victim, 8.0f) && !victim->HasAura(ProtectionWarriorSpells::THUNDER_CLAP))[cite: 6]
        {
            uint32 const thunderClap = GetAppropriateRank(ProtectionWarriorSpells::THUNDER_CLAP);[cite: 6]
            if (thunderClap && CanCast(me, thunderClap, true))[cite: 6]
            {
                if (ExecuteSpell(me, thunderClap, true))[cite: 6]
                    return;
            }
        }

        // 2. 核心仇恨打击：盾牌猛击
        uint32 const shieldSlam = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_SLAM);[cite: 6]
        if (shieldSlam && CanCast(victim, shieldSlam, true))[cite: 6]
        {
            if (ExecuteSpell(victim, shieldSlam, true))[cite: 6]
                return;
        }

        // 3. 高效反击：复仇 (严格检查格挡防御触发态)
        if (me->HasAuraState(AURA_STATE_DEFENSE))[cite: 6]
        {
            uint32 const revenge = GetAppropriateRank(ProtectionWarriorSpells::REVENGE);[cite: 6]
            if (revenge && CanCast(victim, revenge, true))[cite: 6]
            {
                if (ExecuteSpell(victim, revenge, true))[cite: 6]
                    return;
            }
        }

        // 4. 正面锥形群拉与群体昏迷：震荡波
        if (me->IsWithinDist(victim, 10.0f))[cite: 6]
        {
            uint32 const shockwave = GetAppropriateRank(ProtectionWarriorSpells::SHOCKWAVE);[cite: 6]
            if (shockwave && CanCast(me, shockwave, true))[cite: 6]
            {
                if (ExecuteSpell(me, shockwave, true, victim))[cite: 6]
                    return;
            }
        }

        // 5. 战斗中团队怒吼维持 (消除战斗中长达数分钟的 Buff 断档)
        if (shoutCheckTimer == 3000 && me->GetPower(POWER_RAGE) >= 200)[cite: 6]
        {
            if (MaintainTeamShout())[cite: 6]
                return;
        }

        // 6. 降低近战攻强 Debuff：挫志怒吼
        if (me->IsWithinDist(victim, 10.0f) &&
            !victim->HasAura(ProtectionWarriorSpells::DEMORALIZING_SHOUT) &&
            !victim->HasAura(ProtectionWarriorSpells::AURA_DEMO_ROAR))[cite: 6]
        {
            uint32 const demoShout = GetAppropriateRank(ProtectionWarriorSpells::DEMORALIZING_SHOUT);[cite: 6]
            if (demoShout && CanCast(me, demoShout, true))[cite: 6]
            {
                if (ExecuteSpell(me, demoShout, true))[cite: 6]
                    return;
            }
        }

        // 7. 核心破甲维持与填充打击：毁灭打击 / 破甲攻击
        bool const hasOtherArmorDebuff = victim->HasAura(ProtectionWarriorSpells::AURA_EXPOSE_ARMOR) ||
                                         victim->HasAura(ProtectionWarriorSpells::AURA_ACID_SPIT);[cite: 6]

        uint32 const devastate = GetAppropriateRank(ProtectionWarriorSpells::DEVASTATE);[cite: 6]
        uint32 const sunderSpell = devastate ? devastate : GetAppropriateRank(ProtectionWarriorSpells::SUNDER_ARMOR);[cite: 6]

        if (sunderSpell && !hasOtherArmorDebuff)[cite: 6]
        {
            uint32 const activeSunderRank = GetAppropriateRank(ProtectionWarriorSpells::SUNDER_ARMOR);[cite: 6]
            Aura* sunderAura = activeSunderRank ? victim->GetAura(activeSunderRank) : nullptr;[cite: 6]
            bool const needSunder = !sunderAura || sunderAura->GetStackAmount() < 5 || sunderAura->GetDuration() < 3000;[cite: 6]

            if (needSunder && CanCast(victim, sunderSpell, true))[cite: 6]
            {
                if (ExecuteSpell(victim, sunderSpell, true))[cite: 6]
                    return;
            }
            else if (devastate && CanCast(victim, devastate, true))[cite: 6]
            {
                if (ExecuteSpell(victim, devastate, true))[cite: 6]
                    return;
            }
        }

        // ---------------------------------------------------------------------
        // P4: 高怒泄怒平砍强化 (排队去重)
        // ---------------------------------------------------------------------
        if (me->GetPower(POWER_RAGE) >= 450 && !me->GetCurrentSpell(CURRENT_MELEE_SPELL))[cite: 6]
        {
            if (meleeEnemies >= 2)[cite: 6]
            {
                uint32 const cleave = GetAppropriateRank(ProtectionWarriorSpells::CLEAVE);[cite: 6]
                if (cleave && CanCast(victim, cleave, false))[cite: 6]
                    ExecuteSpell(victim, cleave, false);[cite: 6]
            }
            else
            {
                uint32 const heroicStrike = GetAppropriateRank(ProtectionWarriorSpells::HEROIC_STRIKE);[cite: 6]
                if (heroicStrike && CanCast(victim, heroicStrike, false))[cite: 6]
                    ExecuteSpell(victim, heroicStrike, false);[cite: 6]
            }
        }
    }

private:
    uint32 shoutCheckTimer{ 0 };

    uint32 GetTeamShoutSpell() const
    {
        if (me->GetLevel() >= 68)[cite: 6]
            return GetAppropriateRank(ProtectionWarriorSpells::COMMANDING_SHOUT);[cite: 6]
        return GetAppropriateRank(ProtectionWarriorSpells::BATTLE_SHOUT);[cite: 6]
    }

    void ApplyPassiveTalents()
    {
        uint8 const level = me->GetLevel();

        auto SyncPassive = [this, level](uint8 minLvl, uint32 spellId)
        {
            if (level >= minLvl)
            {
                if (!me->HasAura(spellId))
                    me->AddAura(spellId, me);
            }
            else
            {
                me->RemoveAurasDueToSpell(spellId);
            }
        };

        SyncPassive(10, ProtectionWarriorSpells::AURA_TOUGHNESS);[cite: 6]
        SyncPassive(10, ProtectionWarriorSpells::AURA_WARBRINGER);[cite: 6]
        SyncPassive(20, ProtectionWarriorSpells::AURA_ONE_HANDED_SPEC);[cite: 6]
        SyncPassive(30, ProtectionWarriorSpells::AURA_CRITICAL_BLOCK);[cite: 6]
        SyncPassive(40, ProtectionWarriorSpells::AURA_VITALITY);[cite: 6]
        SyncPassive(50, ProtectionWarriorSpells::AURA_DAMAGE_SHIELD);[cite: 6]
    }

    // 全局通用的怒吼维持 (战斗内外皆可调用)
    bool MaintainTeamShout()
    {
        Player* master = GetMaster();[cite: 6]
        uint32 const shoutSpell = GetTeamShoutSpell();[cite: 6]
        if (!shoutSpell)[cite: 6]
            return false;

        auto NeedsTeamShout = [](Unit* target, uint32 spellId) -> bool
        {
            if (!target || !target->IsAlive())
                return false;
            return !target->HasAura(spellId);
        };

        bool const selfNeedsShout   = NeedsTeamShout(me, shoutSpell);[cite: 6]
        bool const masterNeedsShout = master && NeedsTeamShout(master, shoutSpell);[cite: 6]

        if (selfNeedsShout || masterNeedsShout)[cite: 6]
        {
            if (CanCast(me, shoutSpell, true))[cite: 6]
            {
                if (ExecuteSpell(me, shoutSpell, true))[cite: 6]
                    return true;
            }
        }
        return false;
    }
};

void AddSC_bot_protection_warrior()
{
    new AdaptiveBotScript<BotProtectionWarriorAI>("bot_protection_warrior");[cite: 6]
}