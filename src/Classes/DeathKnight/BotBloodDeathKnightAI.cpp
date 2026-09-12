/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BloodDeathKnightSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "SpellAuras.h"
#include "Pet.h"
#include "Chat.h"
#include <algorithm>
#include <vector>

class BotBloodDeathKnightAI : public AdaptiveBotAI
{
public:
    explicit BotBloodDeathKnightAI(Creature* creature) : AdaptiveBotAI(creature) {}

    bool IsTankBot() const override { return true; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case BloodDeathKnightSpells::HEART_STRIKE:
                return BloodDeathKnightSpells::HEART_STRIKE_MIN_LEVEL;
            case BloodDeathKnightSpells::DEATH_STRIKE:
                return BloodDeathKnightSpells::DEATH_STRIKE_MIN_LEVEL;
            case BloodDeathKnightSpells::VAMPIRIC_BLOOD:
                return BloodDeathKnightSpells::VAMPIRIC_BLOOD_MIN_LEVEL;
            case BloodDeathKnightSpells::RUNE_TAP:
                return BloodDeathKnightSpells::RUNE_TAP_MIN_LEVEL;
            default:
                return 0;
        }
    }

    void Reset() override
    {
        // 符文能量通道必须先于基类重置完成配置，保证 SyncLevelWithMaster 走符文能量分支
        me->setPowerType(POWER_RUNIC_POWER);
        me->SetMaxPower(POWER_RUNIC_POWER, 100);
        me->SetPower(POWER_RUNIC_POWER, 0);

        AdaptiveBotAI::Reset();

        presenceCheckTimer = 0;
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 /*level*/) override
    {
        ApplyPassiveTalents();
    }

    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);

        // 巡检计时器：每 3 秒复核一次常驻姿态与增益
        if (presenceCheckTimer <= diff)
            presenceCheckTimer = 3000;
        else
            presenceCheckTimer -= diff;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);

            if (presenceCheckTimer == 3000)
            {
                MaintainFrostPresence();
                MaintainHornOfWinter();
            }
            return;
        }

        // =====================================================================
        // 2. 索敌仲裁
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (!victim || !victim->IsAlive())
            return;

        // =====================================================================
        // 3. 近战平砍与追击状态机接管
        // =====================================================================
        ManageMeleeCombat(victim);

        // =====================================================================
        // Action Priority List (APL) 核心决策循环
        // =====================================================================

        // P0: 姿态与开怪 (冰霜灵气 / 死亡之握)
        if (MaintainFrostPresence())
            return;

        if (TryDeathGrip(victim))
            return;

        // P1: 生存与减伤链 (绿罩 / 冰封之韧 / 吸血鬼之血 / 符文分流)
        if (MaintainDefensiveCooldowns(victim))
            return;

        // P2: 仇恨救急 (死握拉回 / 黑暗命令嘲讽)
        if (HandleTauntEmergency())
            return;

        // P3: 核心自疗与高仇恨泄能 (灵界打击 / 符文打击)
        if (MaintainDeathStrike(victim))
            return;

        if (TryRuneStrike(victim))
            return;

        // P4: 疾病链与群拉 (冰冷触摸 / 暗影打击 / 传染)
        if (MaintainDiseases(victim))
            return;

        // P5: AoE 与填充 (枯萎凋零 / 血液沸腾 / 心脏打击 / 灵界打击)
        if (PerformCombatFiller(victim))
            return;
    }

private:
    uint32 presenceCheckTimer{ 0 };

    // =========================================================================
    // 常驻姿态与增益维护器
    // =========================================================================
    bool MaintainFrostPresence()
    {
        if (me->HasAura(BloodDeathKnightSpells::FROST_PRESENCE))
            return false;

        if (!CanCast(me, BloodDeathKnightSpells::FROST_PRESENCE, true))
            return false;

        return ExecuteSpell(me, BloodDeathKnightSpells::FROST_PRESENCE, true);
    }

    bool MaintainHornOfWinter()
    {
        if (me->HasAura(BloodDeathKnightSpells::HORN_OF_WINTER))
            return false;

        if (!CanCast(me, BloodDeathKnightSpells::HORN_OF_WINTER, true))
            return false;

        return ExecuteSpell(me, BloodDeathKnightSpells::HORN_OF_WINTER, true);
    }

    // =========================================================================
    // 死亡之握：8 ~ 30 码区间将目标拉回近战位
    // =========================================================================
    bool TryDeathGrip(Unit* target)
    {
        if (!target || target == me)
            return false;

        float const dist = me->GetDistance(target);
        if (dist < 8.0f || dist > 30.0f)
            return false;

        if (!CanCast(target, BloodDeathKnightSpells::DEATH_GRIP, true))
            return false;

        return ExecuteSpell(target, BloodDeathKnightSpells::DEATH_GRIP, true);
    }

    // =========================================================================
    // 生存减伤链 (反魔法护盾 / 冰封之韧 / 吸血鬼之血 / 符文分流)
    // =========================================================================
    bool MaintainDefensiveCooldowns(Unit* victim)
    {
        float const hpPct = me->GetHealthPct();

        // 反魔法护盾 (绿罩)：目标正在施法，或自身血量 < 75% 面对法系目标
        bool const targetCasting = victim->IsNonMeleeSpellCast(false);
        bool const casterTarget = (victim->GetMaxPower(POWER_MANA) > 0);
        bool const lowHpVsCaster = (hpPct < 75.0f && casterTarget);

        if ((targetCasting || lowHpVsCaster) &&
            !me->HasAura(BloodDeathKnightSpells::ANTI_MAGIC_SHELL) &&
            CanCast(me, BloodDeathKnightSpells::ANTI_MAGIC_SHELL, true))
        {
            if (ExecuteSpell(me, BloodDeathKnightSpells::ANTI_MAGIC_SHELL, true))
                return true;
        }

        // 冰封之韧：自身血量 < 35%
        if (hpPct < 35.0f &&
            !me->HasAura(BloodDeathKnightSpells::ICEBOUND_FORTITUDE) &&
            CanCast(me, BloodDeathKnightSpells::ICEBOUND_FORTITUDE, true))
        {
            if (ExecuteSpell(me, BloodDeathKnightSpells::ICEBOUND_FORTITUDE, true))
                return true;
        }

        // 吸血鬼之血：自身血量 < 50%
        if (hpPct < 50.0f &&
            !me->HasAura(BloodDeathKnightSpells::VAMPIRIC_BLOOD) &&
            CanCast(me, BloodDeathKnightSpells::VAMPIRIC_BLOOD, true))
        {
            if (ExecuteSpell(me, BloodDeathKnightSpells::VAMPIRIC_BLOOD, true))
                return true;
        }

        // 符文分流：自身血量 < 60%
        uint32 const runeTap = GetAppropriateRank(BloodDeathKnightSpells::RUNE_TAP);
        if (runeTap && hpPct < 60.0f && CanCast(me, runeTap, true))
        {
            if (ExecuteSpell(me, runeTap, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // 仇恨救急：脱战目标死握拉回 + 近战位黑暗命令嘲讽
    // =========================================================================
    bool HandleTauntEmergency()
    {
        Unit* urgentTarget = GetUrgentThreatTarget();
        if (!urgentTarget)
            return false;

        if (TryDeathGrip(urgentTarget))
            return true;

        if (me->IsWithinMeleeRange(urgentTarget) &&
            CanCast(urgentTarget, BloodDeathKnightSpells::DARK_COMMAND, true))
        {
            if (ExecuteSpell(urgentTarget, BloodDeathKnightSpells::DARK_COMMAND, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // 核心自疗：近战位且自身血量 < 85% 时以灵界打击汲取生命
    // =========================================================================
    bool MaintainDeathStrike(Unit* victim)
    {
        if (!victim || me->GetHealthPct() >= 85.0f)
            return false;

        if (!me->IsWithinMeleeRange(victim))
            return false;

        uint32 const deathStrike = GetAppropriateRank(BloodDeathKnightSpells::DEATH_STRIKE);
        if (!deathStrike || !CanCast(victim, deathStrike, true))
            return false;

        return ExecuteSpell(victim, deathStrike, true);
    }

    // =========================================================================
    // 高仇恨泄能：符文打击 (下一次平砍强化，不占用 GCD)
    // =========================================================================
    bool TryRuneStrike(Unit* victim)
    {
        if (!victim || !me->IsWithinMeleeRange(victim))
            return false;

        if (!CanCast(victim, BloodDeathKnightSpells::RUNE_STRIKE, true))
            return false;

        return ExecuteSpell(victim, BloodDeathKnightSpells::RUNE_STRIKE, true);
    }

    // =========================================================================
    // 疾病链：冰冷触摸 / 暗影打击 / 传染扩散
    // =========================================================================
    bool MaintainDiseases(Unit* victim)
    {
        if (!victim)
            return false;

        // 冰霜疫病：远程即可补挂
        if (!victim->HasAura(BloodDeathKnightSpells::AURA_FROST_FEVER))
        {
            uint32 const icyTouch = GetAppropriateRank(BloodDeathKnightSpells::ICY_TOUCH);
            if (icyTouch && CanCast(victim, icyTouch, true))
            {
                if (ExecuteSpell(victim, icyTouch, true))
                    return true;
            }
        }

        // 暗影疫病：近战位补挂
        if (me->IsWithinMeleeRange(victim) &&
            !victim->HasAura(BloodDeathKnightSpells::AURA_BLOOD_PLAGUE))
        {
            uint32 const plagueStrike = GetAppropriateRank(BloodDeathKnightSpells::PLAGUE_STRIKE);
            if (plagueStrike && CanCast(victim, plagueStrike, true))
            {
                if (ExecuteSpell(victim, plagueStrike, true))
                    return true;
            }
        }

        // 传染：多目标且主目标双病齐备时向周围扩散
        if (CountNearbyEnemies(10.0f) >= 2 &&
            victim->HasAura(BloodDeathKnightSpells::AURA_FROST_FEVER) &&
            victim->HasAura(BloodDeathKnightSpells::AURA_BLOOD_PLAGUE))
        {
            if (CanCast(victim, BloodDeathKnightSpells::PESTILENCE, true))
            {
                if (ExecuteSpell(victim, BloodDeathKnightSpells::PESTILENCE, true))
                    return true;
            }
        }

        return false;
    }

    // =========================================================================
    // AoE 群拉与单体填充 (枯萎凋零 / 血液沸腾 / 心脏打击 / 灵界打击)
    // =========================================================================
    bool PerformCombatFiller(Unit* victim)
    {
        if (!victim)
            return false;

        bool const inMelee = me->IsWithinMeleeRange(victim);
        bool const needsAoE = (CountNearbyEnemies(10.0f) >= 2);

        // 枯萎凋零：多目标铺地仇恨
        if (needsAoE)
        {
            uint32 const deathAndDecay = GetAppropriateRank(BloodDeathKnightSpells::DEATH_AND_DECAY);
            if (deathAndDecay && CanCast(victim, deathAndDecay, true))
            {
                if (ExecuteSpell(victim, deathAndDecay, true, victim))
                    return true;
            }
        }

        // 血液沸腾：多目标自体 AoE 仇恨
        if (needsAoE && inMelee)
        {
            uint32 const bloodBoil = GetAppropriateRank(BloodDeathKnightSpells::BLOOD_BOIL);
            if (bloodBoil && CanCast(me, bloodBoil, true))
            {
                if (ExecuteSpell(me, bloodBoil, true, victim))
                    return true;
            }
        }

        // 心脏打击：单体核心填充
        if (inMelee)
        {
            uint32 const heartStrike = GetAppropriateRank(BloodDeathKnightSpells::HEART_STRIKE);
            if (heartStrike && CanCast(victim, heartStrike, true))
            {
                if (ExecuteSpell(victim, heartStrike, true))
                    return true;
            }
        }

        // 灵界打击：兜底填充
        if (inMelee)
        {
            uint32 const deathStrike = GetAppropriateRank(BloodDeathKnightSpells::DEATH_STRIKE);
            if (deathStrike && CanCast(victim, deathStrike, true))
            {
                if (ExecuteSpell(victim, deathStrike, true))
                    return true;
            }
        }

        return false;
    }

    // =========================================================================
    // 周围可攻击敌人计数器 (用于群体仇恨判定)
    // 跨来源去重采样：自身 + 主人/队友 + 随从集群的战斗攻击者，
    // 确保能识别正在攻击队友、而非仅攻击自身的怪物。
    // =========================================================================
    uint32 CountNearbyEnemies(float range)
    {
        std::vector<Unit*> enemies;

        auto Consider = [&](Unit* candidate)
        {
            if (!candidate || !candidate->IsAlive() || !candidate->IsInWorld())
                return;
            if (candidate == me || candidate->GetMap() != me->GetMap())
                return;
            if (!me->IsWithinDist(candidate, range) || !me->IsValidAttackTarget(candidate))
                return;
            if (std::find(enemies.begin(), enemies.end(), candidate) != enemies.end())
                return;

            enemies.push_back(candidate);
        };

        // 1. 自身战斗攻击者
        for (Unit* attacker : me->getAttackers())
            Consider(attacker);

        Player* master = GetMaster();

        if (master)
        {
            // 2. 指挥官与指挥官的宠物
            for (Unit* attacker : master->getAttackers())
                Consider(attacker);

            if (Unit* masterPet = master->GetPet())
            {
                for (Unit* attacker : masterPet->getAttackers())
                    Consider(attacker);
            }

            // 3. 小队成员的战斗攻击者
            if (Group* group = master->GetGroup())
            {
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->GetSource();
                    if (!member || member == master)
                        continue;

                    for (Unit* attacker : member->getAttackers())
                        Consider(attacker);

                    if (Unit* memberPet = member->GetPet())
                    {
                        for (Unit* attacker : memberPet->getAttackers())
                            Consider(attacker);
                    }
                }
            }

            // 4. 同指挥官随从集群
            {
                std::lock_guard<std::mutex> lock(s_botRegistryMutex);
                auto it = s_masterBotRegistry.find(master->GetGUID());
                if (it != s_masterBotRegistry.end())
                {
                    for (AdaptiveBotAI* allyBot : it->second)
                    {
                        if (allyBot && allyBot->me && allyBot->me != me)
                        {
                            for (Unit* attacker : allyBot->me->getAttackers())
                                Consider(attacker);
                        }
                    }
                }
            }
        }

        return static_cast<uint32>(enemies.size());
    }

    // =========================================================================
    // 鲜血天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // =========================================================================
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

        SyncPassive(BloodDeathKnightSpells::PASSIVE_TALENT_MIN_LEVEL, BloodDeathKnightSpells::WILL_OF_THE_NECROPOLIS);
        SyncPassive(BloodDeathKnightSpells::PASSIVE_TALENT_MIN_LEVEL, BloodDeathKnightSpells::VETERAN_OF_THE_THIRD_WAR);
        SyncPassive(BloodDeathKnightSpells::PASSIVE_TALENT_MIN_LEVEL, BloodDeathKnightSpells::TOUGHNESS);
        SyncPassive(BloodDeathKnightSpells::PASSIVE_TALENT_MIN_LEVEL, BloodDeathKnightSpells::ANTICIPATION);
    }
};

void AddSC_bot_blood_death_knight()
{
    new AdaptiveBotScript<BotBloodDeathKnightAI>("bot_blood_death_knight");
}
