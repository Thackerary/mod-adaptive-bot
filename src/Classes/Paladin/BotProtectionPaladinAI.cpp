/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "ProtectionPaladinSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "SpellAuras.h"
#include "Pet.h"
#include "Chat.h"
#include <algorithm>
#include <vector>

class BotProtectionPaladinAI : public AdaptiveBotAI
{
public:
    explicit BotProtectionPaladinAI(Creature* creature) : AdaptiveBotAI(creature) {}

    bool IsTankBot() const override { return true; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case ProtectionPaladinSpells::DIVINE_PROTECTION:
                return 6;
            case ProtectionPaladinSpells::HOLY_SHIELD:
                return 40;
            case ProtectionPaladinSpells::HAMMER_OF_THE_RIGHTEOUS:
                return 60;
            case ProtectionPaladinSpells::DIVINE_SACRIFICE:
                return 70;
            case ProtectionPaladinSpells::SHIELD_OF_RIGHTEOUSNESS:
                return 75;
            case ProtectionPaladinSpells::DIVINE_PLEA:
                return 71;
            default:
                return 0;
        }
    }

    void Reset() override
    {
        AdaptiveBotAI::Reset();
        blessingCheckTimer = 0;
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 /*level*/) override
    {
        ApplyPassiveTalents();
    }

    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);

        // 巡检计时器：每 3 秒复核一次常驻增益
        if (blessingCheckTimer <= diff)
            blessingCheckTimer = 3000;
        else
            blessingCheckTimer -= diff;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);

            if (blessingCheckTimer == 3000)
            {
                MaintainRighteousFury();
                MaintainSeal();
                MaintainBlessing();
                MaintainDivinePlea(50.0f);
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
        // Action Priority List (APL) 核心决策循环 (969 原则)
        // =====================================================================

        // ---------------------------------------------------------------------
        // P0: 核心常驻增益维护 (正义之怒 / 圣印)
        // ---------------------------------------------------------------------
        if (MaintainRighteousFury())
            return;

        if (MaintainSeal())
            return;

        // 战斗中法力吃紧时开启神圣祈求 (常驻回蓝闭环)
        if (MaintainDivinePlea(80.0f))
            return;

        // ---------------------------------------------------------------------
        // P1: 生存与减伤链
        // ---------------------------------------------------------------------
        float const hpPct = me->GetHealthPct();

        // 自身生命 < 30%，且未处于免疫/自律状态：圣佑术
        if (hpPct < 30.0f &&
            me->GetLevel() >= 6 &&
            !me->HasAura(ProtectionPaladinSpells::DIVINE_PROTECTION) &&
            !me->HasAura(ProtectionPaladinSpells::DIVINE_SHIELD) &&
            !me->HasAura(ProtectionPaladinSpells::FORBEARANCE))
        {
            if (CanCast(me, ProtectionPaladinSpells::DIVINE_PROTECTION, true))
            {
                if (ExecuteSpell(me, ProtectionPaladinSpells::DIVINE_PROTECTION, true))
                    return;
            }
        }

        // 团队平均血量 < 60%，且自身生命 > 50%：神圣牺牲 (团队转伤减伤)
        // 血量守卫：自身血量过低时开启大牺牲会被转伤直接打死
        if (me->GetLevel() >= 70 &&
            me->GetHealthPct() > 50.0f &&
            !me->HasAura(ProtectionPaladinSpells::DIVINE_SACRIFICE) &&
            ComputeGroupAverageHealthPct() < 60.0f)
        {
            if (CanCast(me, ProtectionPaladinSpells::DIVINE_SACRIFICE, true))
            {
                if (ExecuteSpell(me, ProtectionPaladinSpells::DIVINE_SACRIFICE, true))
                    return;
            }
        }

        // ---------------------------------------------------------------------
        // P2: 仇恨救急 (清算之手 / 正义防御)
        // ---------------------------------------------------------------------
        if (Unit* urgentTarget = GetUrgentThreatTarget())
        {
            uint32 const reckoning = GetAppropriateRank(ProtectionPaladinSpells::HAND_OF_RECKONING);
            if (reckoning && CanCast(urgentTarget, reckoning, true))
            {
                if (ExecuteSpell(urgentTarget, reckoning, true))
                    return;
            }
        }

        // 正义防御为对友方目标施放的多目标嘲讽，须锚定被攻击的队友
        if (Unit* ally = SelectAllyUnderAttack())
        {
            if (CanCast(ally, ProtectionPaladinSpells::RIGHTEOUS_DEFENSE, true))
            {
                if (ExecuteSpell(ally, ProtectionPaladinSpells::RIGHTEOUS_DEFENSE, true))
                    return;
            }
        }

        // ---------------------------------------------------------------------
        // P3: 神圣之盾常驻维护 (减伤 + 仇恨核心)
        // ---------------------------------------------------------------------
        uint32 const holyShield = GetAppropriateRank(ProtectionPaladinSpells::HOLY_SHIELD);
        if (holyShield && me->GetLevel() >= 40 && !me->HasAura(holyShield))
        {
            if (CanCast(me, holyShield, true))
            {
                if (ExecuteSpell(me, holyShield, true))
                    return;
            }
        }

        // ---------------------------------------------------------------------
        // P4: 核心仇恨循环 (奉献 + 正义之锤)
        //     969 标准循环组成部分：单体目标下同样施放，绝不因单目标而闲置；
        //     多目标时提升优先级抢先铺开群体仇恨。
        // ---------------------------------------------------------------------
        uint32 const nearbyEnemies = CountNearbyEnemies(10.0f);
        bool const needsAoEThreat = (nearbyEnemies >= 2);
        bool const inMelee = me->IsWithinMeleeRange(victim);

        // 1. 奉献铺地 (以身为中心的 AoE 仇恨核心)
        uint32 const consecration = GetAppropriateRank(ProtectionPaladinSpells::CONSECRATION);
        if (consecration && (needsAoEThreat || inMelee) && CanCast(me, consecration, true))
        {
            if (ExecuteSpell(me, consecration, true, victim))
                return;
        }

        // 2. 正义之锤 (群体打击 / 单体核心仇恨填充)
        uint32 const hammer = GetAppropriateRank(ProtectionPaladinSpells::HAMMER_OF_THE_RIGHTEOUS);
        if (hammer && me->GetLevel() >= 60 && (needsAoEThreat || inMelee) && CanCast(victim, hammer, true))
        {
            if (ExecuteSpell(victim, hammer, true))
                return;
        }

        // ---------------------------------------------------------------------
        // P5: 单体高仇恨打击 (正义之盾 + 审判)
        // ---------------------------------------------------------------------
        uint32 const shieldOfRighteousness = GetAppropriateRank(ProtectionPaladinSpells::SHIELD_OF_RIGHTEOUSNESS);
        if (shieldOfRighteousness && me->GetLevel() >= 75 && CanCast(victim, shieldOfRighteousness, true))
        {
            if (ExecuteSpell(victim, shieldOfRighteousness, true))
                return;
        }

        if (CanCast(victim, ProtectionPaladinSpells::JUDGEMENT_OF_LIGHT, true))
        {
            if (ExecuteSpell(victim, ProtectionPaladinSpells::JUDGEMENT_OF_LIGHT, true))
                return;
        }

        // ---------------------------------------------------------------------
        // P6: 填充循环 (奉献 / 正义之锤 兜底，严格近战范围内施放)
        // ---------------------------------------------------------------------
        if (inMelee)
        {
            uint32 const fillerConsecration = GetAppropriateRank(ProtectionPaladinSpells::CONSECRATION);
            if (fillerConsecration && CanCast(me, fillerConsecration, true))
            {
                if (ExecuteSpell(me, fillerConsecration, true, victim))
                    return;
            }

            uint32 const fillerHammer = GetAppropriateRank(ProtectionPaladinSpells::HAMMER_OF_THE_RIGHTEOUS);
            if (fillerHammer && me->GetLevel() >= 60 && CanCast(victim, fillerHammer, true))
            {
                if (ExecuteSpell(victim, fillerHammer, true))
                    return;
            }
        }
    }

private:
    uint32 blessingCheckTimer{ 0 };

    // =========================================================================
    // 团队平均血量采样器 (用于神圣牺牲的团队危机仲裁)
    // =========================================================================
    float ComputeGroupAverageHealthPct()
    {
        Player* master = GetMaster();
        if (!master)
            return 100.0f;

        uint32 total = 0;
        uint32 count = 0;

        auto Accumulate = [&](Unit* unit)
        {
            if (unit && unit->IsAlive() && unit->IsInWorld() && unit->GetMap() == me->GetMap())
            {
                total += static_cast<uint32>(unit->GetHealthPct());
                ++count;
            }
        };

        Accumulate(me);
        Accumulate(master);
        Accumulate(master->GetPet());

        if (Group* group = master->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                if (Player* member = itr->GetSource())
                {
                    Accumulate(member);
                    Accumulate(member->GetPet());
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(s_botRegistryMutex);
            auto it = s_masterBotRegistry.find(master->GetGUID());
            if (it != s_masterBotRegistry.end())
            {
                for (AdaptiveBotAI* allyBot : it->second)
                {
                    if (allyBot && allyBot->me && allyBot->me != me)
                        Accumulate(allyBot->me);
                }
            }
        }

        return count > 0 ? (static_cast<float>(total) / static_cast<float>(count)) : 100.0f;
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
    // 队友受击雷达 (返回正在被非当前坦克目标攻击的友方单位)
    // 正义防御为对友方施放的多目标嘲讽，需要友方锚点
    // =========================================================================
    Unit* SelectAllyUnderAttack()
    {
        Player* master = GetMaster();
        if (!master)
            return nullptr;

        Unit* groupTank = GetGroupTank();

        auto Check = [&](Unit* friendly) -> Unit*
        {
            if (!friendly || !friendly->IsAlive() || !friendly->IsInWorld())
                return nullptr;
            if (friendly->GetMap() != me->GetMap() || !friendly->IsFriendlyTo(me))
                return nullptr;
            if (groupTank && friendly == groupTank)
                return nullptr;

            for (Unit* attacker : friendly->getAttackers())
            {
                if (attacker && attacker->IsAlive() && attacker->GetMap() == me->GetMap() && attacker != me)
                {
                    if (attacker != me->GetVictim() && (!groupTank || attacker->GetVictim() != groupTank))
                        return friendly;
                }
            }
            return nullptr;
        };

        if (Unit* target = Check(master))
            return target;
        if (Unit* target = Check(master->GetPet()))
            return target;

        if (Group* group = master->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                if (Player* member = itr->GetSource())
                {
                    if (member != master)
                    {
                        if (Unit* target = Check(member))
                            return target;
                        if (Unit* target = Check(member->GetPet()))
                            return target;
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(s_botRegistryMutex);
            auto it = s_masterBotRegistry.find(master->GetGUID());
            if (it != s_masterBotRegistry.end())
            {
                for (AdaptiveBotAI* allyBot : it->second)
                {
                    if (allyBot && allyBot->me && allyBot->me != me)
                    {
                        if (Unit* target = Check(allyBot->me))
                            return target;
                    }
                }
            }
        }

        return nullptr;
    }

    // =========================================================================
    // 常驻增益维护器
    // =========================================================================
    bool MaintainRighteousFury()
    {
        if (me->HasAura(ProtectionPaladinSpells::RIGHTEOUS_FURY))
            return false;

        if (!CanCast(me, ProtectionPaladinSpells::RIGHTEOUS_FURY, true))
            return false;

        return ExecuteSpell(me, ProtectionPaladinSpells::RIGHTEOUS_FURY, true);
    }

    uint32 GetPreferredSealSpell() const
    {
        // 复仇圣印需 64 级解锁，低级段（含 < 24 级）回退至正义圣印兜底
        if (me->GetLevel() >= 64)
            return ProtectionPaladinSpells::SEAL_OF_VENGEANCE;

        return ProtectionPaladinSpells::SEAL_OF_RIGHTEOUSNESS;
    }

    bool MaintainSeal()
    {
        if (me->HasAura(ProtectionPaladinSpells::SEAL_OF_VENGEANCE) ||
            me->HasAura(ProtectionPaladinSpells::SEAL_OF_CORRUPTION) ||
            me->HasAura(ProtectionPaladinSpells::SEAL_OF_RIGHTEOUSNESS))
        {
            return false;
        }

        uint32 const preferredSeal = GetPreferredSealSpell();
        if (CanCast(me, preferredSeal, true))
        {
            if (ExecuteSpell(me, preferredSeal, true))
                return true;
        }

        // 高级圣印未解锁或施放失败时，回退至正义圣印
        if (preferredSeal != ProtectionPaladinSpells::SEAL_OF_RIGHTEOUSNESS &&
            CanCast(me, ProtectionPaladinSpells::SEAL_OF_RIGHTEOUSNESS, true))
        {
            if (ExecuteSpell(me, ProtectionPaladinSpells::SEAL_OF_RIGHTEOUSNESS, true))
                return true;
        }

        return false;
    }

    uint32 GetSanctuaryBlessingSpell() const
    {
        if (me->GetLevel() >= 60)
            return GetAppropriateRank(ProtectionPaladinSpells::GREATER_BLESSING_OF_SANCTUARY);

        return GetAppropriateRank(ProtectionPaladinSpells::BLESSING_OF_SANCTUARY);
    }

    bool MaintainBlessing()
    {
        uint32 const blessingSpell = GetSanctuaryBlessingSpell();
        if (!blessingSpell)
            return false;

        Player* master = GetMaster();

        auto NeedsBlessing = [blessingSpell](Unit* target) -> bool
        {
            return target && target->IsAlive() && !target->HasAura(blessingSpell);
        };

        if (NeedsBlessing(me) && CanCast(me, blessingSpell, true))
        {
            if (ExecuteSpell(me, blessingSpell, true))
                return true;
        }

        if (NeedsBlessing(master) && CanCast(master, blessingSpell, true))
        {
            if (ExecuteSpell(master, blessingSpell, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // 神圣祈求维护器 (法力恢复闭环)
    // =========================================================================
    bool MaintainDivinePlea(float manaPctThreshold)
    {
        if (me->getPowerType() != POWER_MANA)
            return false;

        if (me->GetLevel() < ProtectionPaladinSpells::DIVINE_PLEA_MIN_LEVEL)
            return false;

        if (me->HasAura(ProtectionPaladinSpells::DIVINE_PLEA))
            return false;

        if (me->GetPowerPct(POWER_MANA) >= manaPctThreshold)
            return false;

        if (!CanCast(me, ProtectionPaladinSpells::DIVINE_PLEA, true))
            return false;

        return ExecuteSpell(me, ProtectionPaladinSpells::DIVINE_PLEA, true);
    }

    // =========================================================================
    // 防护天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
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

        SyncPassive(10, ProtectionPaladinSpells::AURA_DIVINE_STRENGTH);
        SyncPassive(10, ProtectionPaladinSpells::AURA_TOUGHNESS);
        SyncPassive(10, ProtectionPaladinSpells::AURA_ANTICIPATION);
        SyncPassive(10, ProtectionPaladinSpells::AURA_REDOUBT);
        SyncPassive(50, ProtectionPaladinSpells::ARDENT_DEFENDER); // 50 级解锁被动免死 (春哥)
    }
};

void AddSC_bot_protection_paladin()
{
    new AdaptiveBotScript<BotProtectionPaladinAI>("bot_protection_paladin");
}
