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

    bool IsTankBot() const override { return true; }

    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
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

    void Reset() override
    {
        AdaptiveBotAI::Reset();
        shoutCheckTimer = 0;
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 /*level*/) override
    {
        ApplyPassiveTalents();
    }

    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);

        // 维护团队怒吼计时器 (战斗内外通用)
        if (shoutCheckTimer <= diff)
            shoutCheckTimer = 3000;
        else
            shoutCheckTimer -= diff;

        // 1. 脱战业务维护
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);

            if (shoutCheckTimer == 3000)
                MaintainTeamShout(); 

            if (me->GetLevel() >= 10 && !me->HasAura(ProtectionWarriorSpells::DEFENSIVE_STANCE)) 
            {
                if (CanCast(me, ProtectionWarriorSpells::DEFENSIVE_STANCE, true)) 
                    ExecuteSpell(me, ProtectionWarriorSpells::DEFENSIVE_STANCE); 
            }
            return;
        }

        // 2. 索敌仲裁
        Unit* victim = SelectAssistTarget(); 
        if (!victim || !victim->IsAlive()) 
            return;

        // 3. 近战平砍与追击状态机接管
        ManageMeleeCombat(victim); 

        // =====================================================================
        // Action Priority List (APL) 核心决策循环
        // =====================================================================

        // ---------------------------------------------------------------------
        // P0: 核心姿态维护与怒气唤醒
        // ---------------------------------------------------------------------
        if (me->GetLevel() >= 10 && !me->HasAura(ProtectionWarriorSpells::DEFENSIVE_STANCE)) 
        {
            if (CanCast(me, ProtectionWarriorSpells::DEFENSIVE_STANCE, true)) 
            {
                ExecuteSpell(me, ProtectionWarriorSpells::DEFENSIVE_STANCE); 
                return;
            }
        }

        if (me->GetPower(POWER_RAGE) < 150) 
        {
            if (CanCast(me, ProtectionWarriorSpells::BLOODRAGE, false)) 
                ExecuteSpell(me, ProtectionWarriorSpells::BLOODRAGE, false); 
        }

        // ---------------------------------------------------------------------
        // P1: 硬核减伤链与生死急救
        // ---------------------------------------------------------------------
        float const hpPct = me->GetHealthPct(); 

        // 生命 < 25%：破釜沉舟
        if (hpPct < 25.0f) 
        {
            uint32 const lastStand = GetAppropriateRank(ProtectionWarriorSpells::LAST_STAND); 
            if (lastStand && CanCast(me, lastStand, false)) 
            {
                if (ExecuteSpell(me, lastStand, false)) 
                    return;
            }
        }

        // 生命 < 35%：盾墙
        if (hpPct < 35.0f) 
        {
            uint32 const shieldWall = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_WALL); 
            if (shieldWall && CanCast(me, shieldWall, false)) 
            {
                if (ExecuteSpell(me, shieldWall, false)) 
                    return;
            }
        }

        // 生命 < 50%：狂暴回复 (前置激怒激活保护)
        if (hpPct < 50.0f && !me->HasAura(ProtectionWarriorSpells::ENRAGED_REGENERATION)) 
        {
            bool hasEnrage = me->HasAura(ProtectionWarriorSpells::BLOODRAGE) ||
                             me->HasAura(ProtectionWarriorSpells::BERSERKER_RAGE); 

            if (!hasEnrage) 
            {
                if (CanCast(me, ProtectionWarriorSpells::BLOODRAGE, false)) 
                    hasEnrage = ExecuteSpell(me, ProtectionWarriorSpells::BLOODRAGE, false); 
                else if (CanCast(me, ProtectionWarriorSpells::BERSERKER_RAGE, false)) 
                    hasEnrage = ExecuteSpell(me, ProtectionWarriorSpells::BERSERKER_RAGE, false); 
            }

            if (hasEnrage) 
            {
                uint32 const enrRegen = GetAppropriateRank(ProtectionWarriorSpells::ENRAGED_REGENERATION); 
                if (enrRegen && CanCast(me, enrRegen, false)) 
                {
                    if (ExecuteSpell(me, enrRegen, false)) 
                        return;
                }
            }
        }

        // 生命 < 80% 或正在挨打：盾牌格挡
        if (hpPct < 80.0f || !me->getAttackers().empty()) 
        {
            uint32 const shieldBlock = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_BLOCK); 
            if (shieldBlock && CanCast(me, shieldBlock, false)) 
                ExecuteSpell(me, shieldBlock, false); 
        }

        // ---------------------------------------------------------------------
        // P2: 机动突进、战术打断、援护与换防救火
        // ---------------------------------------------------------------------
        float const distToVictim = me->GetDistance(victim); 

        // 1. 常规起手突进接怪
        if (distToVictim >= 8.0f && distToVictim <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT)) 
        {
            uint32 const charge = GetAppropriateRank(ProtectionWarriorSpells::CHARGE); 
            if (charge && CanCast(victim, charge, false)) 
            {
                if (ExecuteSpell(victim, charge, false)) 
                    return;
            }

            uint32 const intercept = GetAppropriateRank(ProtectionWarriorSpells::INTERCEPT); 
            if (intercept && CanCast(victim, intercept, false)) 
            {
                if (ExecuteSpell(victim, intercept, false)) 
                    return;
            }
        }

        // 2. 战术打断：盾击接入阶段三基类记忆化压秒打断仲裁引擎。
        // 交由基类 ShouldInterruptTarget 统一裁决：引导类即刻抢断，
        // 读条类严格按 learnedInterruptDelays 学到的压秒余量出手，
        // 彻底取代原先「只要在读条就砍」的盲目秒断。
        uint32 const shieldBash = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_BASH);
        if (shieldBash && TryInterrupt(victim, shieldBash))
            return;

        // 远距离突进打断：仅当目标确在施法且处于突进射程时才交冲锋/拦截，
        // 避免把机动技能浪费在无读条的常规拉怪上。
        if (victim->HasUnitState(UNIT_STATE_CASTING) && distToVictim >= 8.0f && distToVictim <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT))
        {
            uint32 const charge = GetAppropriateRank(ProtectionWarriorSpells::CHARGE); 
            if (charge && CanCast(victim, charge, false)) 
            {
                if (ExecuteSpell(victim, charge, false)) 
                    return;
            }
            uint32 const intercept = GetAppropriateRank(ProtectionWarriorSpells::INTERCEPT); 
            if (intercept && CanCast(victim, intercept, false)) 
            {
                if (ExecuteSpell(victim, intercept, false)) 
                    return;
            }
        }

        // 3. 战术机动：援护队友 (ID 3411)
        Player* master = GetMaster(); 
        if (master && master->IsAlive() && master->GetHealthPct() < 70.0f && !master->getAttackers().empty()) 
        {
            float const distToMaster = me->GetDistance(master); 
            if (distToMaster >= 8.0f && distToMaster <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT)) 
            {
                uint32 const intervene = GetAppropriateRank(ProtectionWarriorSpells::INTERVENE); 
                if (intervene && CanCast(master, intervene, true)) 
                {
                    if (ExecuteSpell(master, intervene, true)) 
                        return;
                }
            }
        }

        // 4. 仇恨失控剥离 (换防救火)
        if (Unit* urgentTarget = GetUrgentThreatTarget()) 
        {
            float const distToUrgent = me->GetDistance(urgentTarget); 

            // 战神远距离冲锋/拦截优先贴脸打断控怪
            if (distToUrgent >= 8.0f && distToUrgent <= 25.0f && !me->HasUnitState(UNIT_STATE_ROOT)) 
            {
                uint32 const charge = GetAppropriateRank(ProtectionWarriorSpells::CHARGE); 
                if (charge && CanCast(urgentTarget, charge, false)) 
                {
                    if (ExecuteSpell(urgentTarget, charge, false)) 
                        return;
                }
            }

            // 远程嘲讽 (30 码, 强拉仇恨)
            uint32 const taunt = GetAppropriateRank(ProtectionWarriorSpells::TAUNT); 
            if (taunt && CanCast(urgentTarget, taunt, false)) 
            {
                if (ExecuteSpell(urgentTarget, taunt, false)) 
                    return;
            }
        }

        // 5. 强力单体昏迷硬控：震荡猛击
        if (victim->HasUnitState(UNIT_STATE_CASTING) || victim->GetHealthPct() > 50.0f) 
        {
            uint32 const concBlow = GetAppropriateRank(ProtectionWarriorSpells::CONCUSSION_BLOW); 
            if (concBlow && CanCast(victim, concBlow, true)) 
            {
                if (ExecuteSpell(victim, concBlow, true)) 
                    return;
            }
        }

        // ---------------------------------------------------------------------
        // P3: 核心仇恨打击、减益维持与 AoE 控制
        // ---------------------------------------------------------------------
        uint8 meleeEnemies = 0; 
        for (Unit* attacker : me->getAttackers()) 
        {
            if (attacker && attacker->IsAlive() && me->IsWithinMeleeRange(attacker)) 
            {
                ++meleeEnemies; 
                if (meleeEnemies >= 3) 
                    break;
            }
        }

        // 1. 维持核心减速/减攻速 Debuff：雷霆一击
        if (me->IsWithinDist(victim, 8.0f) && !victim->HasAura(ProtectionWarriorSpells::THUNDER_CLAP)) 
        {
            uint32 const thunderClap = GetAppropriateRank(ProtectionWarriorSpells::THUNDER_CLAP); 
            if (thunderClap && CanCast(me, thunderClap, true)) 
            {
                if (ExecuteSpell(me, thunderClap, true)) 
                    return;
            }
        }

        // 2. 核心仇恨打击：盾牌猛击
        uint32 const shieldSlam = GetAppropriateRank(ProtectionWarriorSpells::SHIELD_SLAM); 
        if (shieldSlam && CanCast(victim, shieldSlam, true)) 
        {
            if (ExecuteSpell(victim, shieldSlam, true)) 
                return;
        }

        // 3. 高效反击：复仇 (严格检查格挡防御触发态)
        if (me->HasAuraState(AURA_STATE_DEFENSE)) 
        {
            uint32 const revenge = GetAppropriateRank(ProtectionWarriorSpells::REVENGE); 
            if (revenge && CanCast(victim, revenge, true)) 
            {
                if (ExecuteSpell(victim, revenge, true)) 
                    return;
            }
        }

        // 4. 正面锥形群拉与群体昏迷：震荡波
        if (me->IsWithinDist(victim, 10.0f)) 
        {
            uint32 const shockwave = GetAppropriateRank(ProtectionWarriorSpells::SHOCKWAVE); 
            if (shockwave && CanCast(me, shockwave, true)) 
            {
                if (ExecuteSpell(me, shockwave, true, victim)) 
                    return;
            }
        }

        // 5. 战斗中团队怒吼维持 (消除战斗中长达数分钟的 Buff 断档)
        if (shoutCheckTimer == 3000 && me->GetPower(POWER_RAGE) >= 200) 
        {
            if (MaintainTeamShout()) 
                return;
        }

        // 6. 降低近战攻强 Debuff：挫志怒吼
        if (me->IsWithinDist(victim, 10.0f) &&
            !victim->HasAura(ProtectionWarriorSpells::DEMORALIZING_SHOUT) &&
            !victim->HasAura(ProtectionWarriorSpells::AURA_DEMO_ROAR)) 
        {
            uint32 const demoShout = GetAppropriateRank(ProtectionWarriorSpells::DEMORALIZING_SHOUT); 
            if (demoShout && CanCast(me, demoShout, true)) 
            {
                if (ExecuteSpell(me, demoShout, true)) 
                    return;
            }
        }

        // 7. 核心破甲维持与填充打击：毁灭打击 / 破甲攻击
        bool const hasOtherArmorDebuff = victim->HasAura(ProtectionWarriorSpells::AURA_EXPOSE_ARMOR) ||
                                         victim->HasAura(ProtectionWarriorSpells::AURA_ACID_SPIT); 

        uint32 const devastate = GetAppropriateRank(ProtectionWarriorSpells::DEVASTATE); 
        uint32 const sunderSpell = devastate ? devastate : GetAppropriateRank(ProtectionWarriorSpells::SUNDER_ARMOR); 

        if (sunderSpell && !hasOtherArmorDebuff) 
        {
            uint32 const activeSunderRank = GetAppropriateRank(ProtectionWarriorSpells::SUNDER_ARMOR); 
            Aura* sunderAura = activeSunderRank ? victim->GetAura(activeSunderRank) : nullptr; 
            bool const needSunder = !sunderAura || sunderAura->GetStackAmount() < 5 || sunderAura->GetDuration() < 3000; 

            if (needSunder && CanCast(victim, sunderSpell, true)) 
            {
                if (ExecuteSpell(victim, sunderSpell, true)) 
                    return;
            }
            else if (devastate && CanCast(victim, devastate, true)) 
            {
                if (ExecuteSpell(victim, devastate, true)) 
                    return;
            }
        }

        // ---------------------------------------------------------------------
        // P4: 高怒泄怒平砍强化 (排队去重)
        // ---------------------------------------------------------------------
        if (me->GetPower(POWER_RAGE) >= 450 && !me->GetCurrentSpell(CURRENT_MELEE_SPELL)) 
        {
            if (meleeEnemies >= 2) 
            {
                uint32 const cleave = GetAppropriateRank(ProtectionWarriorSpells::CLEAVE); 
                if (cleave && CanCast(victim, cleave, false)) 
                    ExecuteSpell(victim, cleave, false); 
            }
            else
            {
                uint32 const heroicStrike = GetAppropriateRank(ProtectionWarriorSpells::HEROIC_STRIKE); 
                if (heroicStrike && CanCast(victim, heroicStrike, false)) 
                    ExecuteSpell(victim, heroicStrike, false); 
            }
        }
    }

private:
    uint32 shoutCheckTimer{ 0 };

    uint32 GetTeamShoutSpell() const
    {
        if (me->GetLevel() >= 68) 
            return GetAppropriateRank(ProtectionWarriorSpells::COMMANDING_SHOUT); 
        return GetAppropriateRank(ProtectionWarriorSpells::BATTLE_SHOUT); 
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

        SyncPassive(10, ProtectionWarriorSpells::AURA_TOUGHNESS); 
        SyncPassive(10, ProtectionWarriorSpells::AURA_WARBRINGER); 
        SyncPassive(20, ProtectionWarriorSpells::AURA_ONE_HANDED_SPEC); 
        SyncPassive(30, ProtectionWarriorSpells::AURA_CRITICAL_BLOCK); 
        SyncPassive(40, ProtectionWarriorSpells::AURA_VITALITY); 
        SyncPassive(50, ProtectionWarriorSpells::AURA_DAMAGE_SHIELD); 
    }

    // 全局通用的怒吼维持 (战斗内外皆可调用)
    bool MaintainTeamShout()
    {
        Player* master = GetMaster(); 
        uint32 const shoutSpell = GetTeamShoutSpell(); 
        if (!shoutSpell) 
            return false;

        auto NeedsTeamShout = [](Unit* target, uint32 spellId) -> bool
        {
            if (!target || !target->IsAlive())
                return false;
            return !target->HasAura(spellId);
        };

        bool const selfNeedsShout   = NeedsTeamShout(me, shoutSpell); 
        bool const masterNeedsShout = master && NeedsTeamShout(master, shoutSpell); 

        if (selfNeedsShout || masterNeedsShout) 
        {
            if (CanCast(me, shoutSpell, true)) 
            {
                if (ExecuteSpell(me, shoutSpell, true)) 
                    return true;
            }
        }
        return false;
    }
};

void AddSC_bot_protection_warrior()
{
    new AdaptiveBotScript<BotProtectionWarriorAI>("bot_protection_warrior");
}
