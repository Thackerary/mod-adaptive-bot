/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BearDruidSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "SpellAuras.h"
#include "Pet.h"
#include "Chat.h"
#include <algorithm>
#include <vector>

class BotBearDruidAI : public AdaptiveBotAI
{
public:
    explicit BotBearDruidAI(Creature* creature) : AdaptiveBotAI(creature) {}

    bool IsTankBot() const override { return true; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case BearDruidSpells::FERAL_CHARGE_BEAR:
                return BearDruidSpells::FERAL_CHARGE_MIN_LEVEL;
            case BearDruidSpells::ENRAGE:
                return BearDruidSpells::ENRAGE_MIN_LEVEL;
            case BearDruidSpells::CHALLENGING_ROAR:
                return BearDruidSpells::CHALLENGING_ROAR_MIN_LEVEL;
            case BearDruidSpells::MANGLE_BEAR:
                return BearDruidSpells::MANGLE_MIN_LEVEL;
            case BearDruidSpells::LACERATE:
                return BearDruidSpells::LACERATE_MIN_LEVEL;
            case BearDruidSpells::SURVIVAL_INSTINCTS:
                return BearDruidSpells::SURVIVAL_INSTINCTS_MIN_LEVEL;
            case BearDruidSpells::FRENZIED_REGENERATION:
                return BearDruidSpells::FRENZIED_REGENERATION_MIN_LEVEL;
            case BearDruidSpells::BARKSKIN:
                return BearDruidSpells::BARKSKIN_MIN_LEVEL;
            default:
                return 0;
        }
    }

    // =========================================================================
    // 形态契约豁免：允许从任意形态直接切入熊 / 巨熊形态
    // =========================================================================
    bool CheckShapeshiftExemption(SpellInfo const* spellInfo) const override
    {
        if (!spellInfo)
            return false;

        if (spellInfo->Id == BearDruidSpells::BEAR_FORM || spellInfo->Id == BearDruidSpells::DIRE_BEAR_FORM)
            return true;

        return AdaptiveBotAI::CheckShapeshiftExemption(spellInfo);
    }

    void Reset() override
    {
        // 配置怒气通道 (熊坦核心资源)
        me->setPowerType(POWER_RAGE);
        me->SetMaxPower(POWER_RAGE, 100);
        me->SetPower(POWER_RAGE, 20);

        AdaptiveBotAI::Reset();

        MaintainResourcePools();
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 level) override
    {
        AdaptiveBotAI::OnLevelSynced(level);

        MaintainResourcePools();
        ApplyPassiveTalents();
    }

    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);

        // =====================================================================
        // P0: 形态维护 (脱战与战斗均常驻熊 / 巨熊形态)
        // 变身占用 GCD，成功施放后本帧让出，避免后续技能被 GCD 阻断
        // =====================================================================
        if (MaintainBearForm())
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);

            MaintainResourcePools();
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

        // ---------------------------------------------------------------------
        // P0: 战时怒气保底 (随从生物默认不触发玩家受击回怒通道)
        // ---------------------------------------------------------------------
        SupplementRage();

        // ---------------------------------------------------------------------
        // P0: 远距突进、远程开怪与怒气补充
        // ---------------------------------------------------------------------
        if (TryFeralCharge(victim))
            return;

        if (MaintainFaerieFireFeral(victim))
            return;

        if (TryEnrage())
            return;

        // ---------------------------------------------------------------------
        // P1: 生存与减伤链
        // ---------------------------------------------------------------------
        if (MaintainSurvivalInstincts())
            return;

        if (MaintainFrenziedRegeneration())
            return;

        if (MaintainBarkskin())
            return;

        // ---------------------------------------------------------------------
        // P2: 嘲讽救急
        // ---------------------------------------------------------------------
        if (HandleTauntEmergency())
            return;

        // ---------------------------------------------------------------------
        // P3: 重殴泄怒 (on-next-swing，无 GCD，绝对禁止 return)
        // ---------------------------------------------------------------------
        TryMaul(victim);

        // ---------------------------------------------------------------------
        // P4: 核心群拉与单体打击
        // ---------------------------------------------------------------------
        PerformBearCombat(victim);
    }

private:
    // =========================================================================
    // 形态与资源维护
    // =========================================================================
    bool MaintainBearForm()
    {
        uint32 const desiredForm = (me->GetLevel() >= BearDruidSpells::DIRE_BEAR_FORM_MIN_LEVEL)
            ? BearDruidSpells::DIRE_BEAR_FORM
            : BearDruidSpells::BEAR_FORM;

        ShapeshiftForm const currentForm = me->GetShapeshiftForm();

        if (desiredForm == BearDruidSpells::DIRE_BEAR_FORM)
        {
            if (currentForm == FORM_DIREBEAR)
                return false;
        }
        else if (currentForm == FORM_BEAR)
        {
            return false;
        }

        if (!CanCast(me, desiredForm, true))
            return false;

        return ExecuteSpell(me, desiredForm, true);
    }

    void MaintainResourcePools()
    {
        // 怒气上限契约 (引擎若按 10 倍存储自动上调，此处仅做下限保障)
        if (me->GetMaxPower(POWER_RAGE) < 100)
            me->SetMaxPower(POWER_RAGE, 100);

        // 形态切换 / 野性赐福 / 精灵之火仍属法力消耗技能，需保留法力池
        if (me->GetMaxPower(POWER_MANA) < 5000)
            me->SetMaxPower(POWER_MANA, 5000);

        if (me->GetPower(POWER_MANA) < me->GetMaxPower(POWER_MANA))
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));
    }

    // =========================================================================
    // 技能降阶仲裁 (等级门槛 + Rank 1 保底)
    // =========================================================================
    uint32 GetUsableBearSpell(uint32 maxRankSpellId, uint8 minLevel) const
    {
        if (me->GetLevel() < minLevel)
            return 0;

        return GetAppropriateRank(maxRankSpellId);
    }

    // =========================================================================
    // 战时怒气保底 (随从生物默认不触发玩家受击回怒通道，杜绝 0 怒气瘫痪)
    // =========================================================================
    void SupplementRage()
    {
        if (me->GetPower(POWER_RAGE) >= 25)
            return;

        me->ModifyPower(POWER_RAGE, 15);
    }

    // =========================================================================
    // P0: 野性冲锋 - 巨熊 (8~25 码正前方突进)
    // =========================================================================
    bool TryFeralCharge(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return false;

        uint32 const chargeSpell = GetUsableBearSpell(BearDruidSpells::FERAL_CHARGE_BEAR, BearDruidSpells::FERAL_CHARGE_MIN_LEVEL);
        if (!chargeSpell)
            return false;

        if (me->HasUnitState(UNIT_STATE_CHARGING))
            return false;

        float const dist = me->GetDistance(victim);
        if (dist < 8.0f || dist > 25.0f)
            return false;

        if (!me->isInFrontInMap(victim, 25.0f))
            return false;

        if (!CanCast(victim, chargeSpell, true))
            return false;

        return ExecuteSpell(victim, chargeSpell, true);
    }

    // =========================================================================
    // P0: 精灵之火 (野性) 远程开怪 / 破甲补挂
    // =========================================================================
    bool MaintainFaerieFireFeral(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return false;

        uint32 const faerieFire = GetUsableBearSpell(BearDruidSpells::FAERIE_FIRE_FERAL, BearDruidSpells::FAERIE_FIRE_MIN_LEVEL);
        if (!faerieFire)
            return false;

        if (victim->HasAura(faerieFire) || victim->HasAura(BearDruidSpells::FAERIE_FIRE_FERAL))
            return false;

        if (!me->IsWithinDist(victim, 30.0f))
            return false;

        if (!CanCast(victim, faerieFire, true))
            return false;

        return ExecuteSpell(victim, faerieFire, true);
    }

    // =========================================================================
    // P0: 激怒 (起手 / 怒气枯竭时补怒)
    // =========================================================================
    bool TryEnrage()
    {
        uint32 const enrage = GetUsableBearSpell(BearDruidSpells::ENRAGE, BearDruidSpells::ENRAGE_MIN_LEVEL);
        if (!enrage)
            return false;

        if (me->HasAura(enrage))
            return false;

        // 仅在怒气枯竭时使用，避免无谓的护甲惩罚
        if (me->GetPowerPct(POWER_RAGE) > 20.0f)
            return false;

        if (!CanCast(me, enrage, true))
            return false;

        return ExecuteSpell(me, enrage, true);
    }

    // =========================================================================
    // P1: 生存本能 (生命 < 30%)
    // =========================================================================
    bool MaintainSurvivalInstincts()
    {
        if (me->GetHealthPct() >= 30.0f)
            return false;

        uint32 const survivalInstincts = GetUsableBearSpell(BearDruidSpells::SURVIVAL_INSTINCTS, BearDruidSpells::SURVIVAL_INSTINCTS_MIN_LEVEL);
        if (!survivalInstincts || me->HasAura(survivalInstincts))
            return false;

        if (!CanCast(me, survivalInstincts, true))
            return false;

        return ExecuteSpell(me, survivalInstincts, true);
    }

    // =========================================================================
    // P1: 狂暴回复 (生命 < 50% 且怒气 >= 15%)
    // =========================================================================
    bool MaintainFrenziedRegeneration()
    {
        if (me->GetHealthPct() >= 50.0f)
            return false;

        if (me->GetPowerPct(POWER_RAGE) < 15.0f)
            return false;

        uint32 const frenziedRegeneration = GetUsableBearSpell(BearDruidSpells::FRENZIED_REGENERATION, BearDruidSpells::FRENZIED_REGENERATION_MIN_LEVEL);
        if (!frenziedRegeneration || me->HasAura(frenziedRegeneration))
            return false;

        if (!CanCast(me, frenziedRegeneration, true))
            return false;

        return ExecuteSpell(me, frenziedRegeneration, true);
    }

    // =========================================================================
    // P1: 树皮术 (生命 < 70%)
    // =========================================================================
    bool MaintainBarkskin()
    {
        if (me->GetHealthPct() >= 70.0f)
            return false;

        uint32 const barkskin = GetUsableBearSpell(BearDruidSpells::BARKSKIN, BearDruidSpells::BARKSKIN_MIN_LEVEL);
        if (!barkskin || me->HasAura(barkskin))
            return false;

        if (!CanCast(me, barkskin, true))
            return false;

        return ExecuteSpell(me, barkskin, true);
    }

    // =========================================================================
    // P2: 嘲讽救急 (低吼 / 挑战咆哮)
    // =========================================================================
    bool HandleTauntEmergency()
    {
        // 单体救急：拉住正在攻击队友的漏怪
        if (Unit* urgentTarget = GetUrgentThreatTarget())
        {
            uint32 const growl = GetUsableBearSpell(BearDruidSpells::GROWL, BearDruidSpells::GROWL_MIN_LEVEL);
            if (growl && CanCast(urgentTarget, growl, true))
            {
                if (ExecuteSpell(urgentTarget, growl, true))
                    return true;
            }
        }

        // 群体救急：近战范围内超过 2 只漏怪
        if (CountLooseEnemiesInMelee() > 2)
        {
            uint32 const challengingRoar = GetUsableBearSpell(BearDruidSpells::CHALLENGING_ROAR, BearDruidSpells::CHALLENGING_ROAR_MIN_LEVEL);
            if (challengingRoar && CanCast(me, challengingRoar, true))
            {
                if (ExecuteSpell(me, challengingRoar, true))
                    return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P3: 重殴泄怒 (怒气 > 35% 且处于近战位)
    // 注意：重殴为下一次平砍强化，不占 GCD，此处严禁向上层返回！
    // =========================================================================
    void TryMaul(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return;

        if (me->GetPowerPct(POWER_RAGE) <= 35.0f)
            return;

        if (!me->IsWithinMeleeRange(victim))
            return;

        uint32 const maul = GetUsableBearSpell(BearDruidSpells::MAUL, BearDruidSpells::MAUL_MIN_LEVEL);
        if (!maul)
            return;

        if (!CanCast(victim, maul, true))
            return;

        ExecuteSpell(victim, maul, true);
    }

    // =========================================================================
    // P4: 核心群拉与单体打击
    // =========================================================================
    void PerformBearCombat(Unit* victim)
    {
        uint32 const nearbyEnemies = CountNearbyEnemies(8.0f);
        bool const needsGroupThreat = (nearbyEnemies >= 2);

        uint32 const swipe = GetUsableBearSpell(BearDruidSpells::SWIPE_BEAR, BearDruidSpells::SWIPE_MIN_LEVEL);
        bool const inMelee = me->IsWithinMeleeRange(victim);

        // 多目标：横扫优先铺垫群体仇恨
        // 横扫为自身圆心正面锥形 AoE (DBC 范围为 Self only)，必须以自身为施法目标
        if (needsGroupThreat && inMelee && swipe && CanCast(me, swipe, true))
        {
            if (ExecuteSpell(me, swipe, true, victim))
                return;
        }

        // 单体核心仇恨：裂伤 - 熊 (6 秒 CD)
        uint32 const mangle = GetUsableBearSpell(BearDruidSpells::MANGLE_BEAR, BearDruidSpells::MANGLE_MIN_LEVEL);
        if (mangle && CanCast(victim, mangle, true))
        {
            if (ExecuteSpell(victim, mangle, true))
                return;
        }

        // 流血 DoT：割伤 (未满 5 层继续叠层；已满 5 层且即将断档时刷新持续时间)
        uint32 const lacerate = GetUsableBearSpell(BearDruidSpells::LACERATE, BearDruidSpells::LACERATE_MIN_LEVEL);
        if (lacerate && NeedsLacerateRefresh(victim, lacerate) && CanCast(victim, lacerate, true))
        {
            if (ExecuteSpell(victim, lacerate, true))
                return;
        }

        // 填充技能：横扫 - 熊 (同样以自身为圆心施放)
        if (inMelee && swipe && CanCast(me, swipe, true))
        {
            if (ExecuteSpell(me, swipe, true, victim))
                return;
        }
    }

    // 割伤刷新判定：未满 5 层需继续叠层；满 5 层后剩余时间 <= 4500ms 必须刷新，杜绝断档掉层
    bool NeedsLacerateRefresh(Unit* victim, uint32 lacerateSpell) const
    {
        if (!victim || !lacerateSpell)
            return false;

        Aura* lacerateAura = victim->GetAura(lacerateSpell);
        if (!lacerateAura)
            return true;

        if (lacerateAura->GetStackAmount() < 5)
            return true;

        return lacerateAura->GetDuration() <= 4500;
    }

    // =========================================================================
    // 全队雷达：汇总与小队交战的敌人 (去重)
    // =========================================================================
    void CollectGroupEngagedEnemies(std::vector<Unit*>& enemies)
    {
        auto Consider = [this, &enemies](Unit* candidate)
        {
            if (!candidate || candidate == me)
                return;

            if (!candidate->IsAlive() || !candidate->IsInWorld())
                return;

            if (candidate->GetMap() != me->GetMap())
                return;

            if (!me->IsValidAttackTarget(candidate))
                return;

            if (std::find(enemies.begin(), enemies.end(), candidate) != enemies.end())
                return;

            enemies.push_back(candidate);
        };

        // 1. 自身战斗攻击者
        for (Unit* attacker : me->getAttackers())
            Consider(attacker);

        Player* master = GetMaster();
        if (!master)
            return;

        // 2. 指挥官与指挥官宠物
        for (Unit* attacker : master->getAttackers())
            Consider(attacker);

        if (Unit* masterPet = master->GetPet())
        {
            for (Unit* attacker : masterPet->getAttackers())
                Consider(attacker);
        }

        // 3. 小队成员与其宠物
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

    // 周围可攻击敌人计数器 (用于群体仇恨判定)
    uint32 CountNearbyEnemies(float range)
    {
        std::vector<Unit*> enemies;
        CollectGroupEngagedEnemies(enemies);

        uint32 count = 0;
        for (Unit* enemy : enemies)
        {
            if (me->IsWithinDist(enemy, range))
                ++count;
        }

        return count;
    }

    // 近战范围内被 OT 的漏怪数量 (排除已被主坦锁定的目标)
    uint32 CountLooseEnemiesInMelee()
    {
        std::vector<Unit*> enemies;
        CollectGroupEngagedEnemies(enemies);

        Unit* groupTank = GetGroupTank();

        uint32 count = 0;
        for (Unit* enemy : enemies)
        {
            if (groupTank && enemy->GetVictim() == groupTank)
                continue;

            if (!me->IsWithinMeleeRange(enemy))
                continue;

            ++count;
        }

        return count;
    }

    // =========================================================================
    // 天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // =========================================================================
    void ApplyPassiveTalents()
    {
        uint8 const level = me->GetLevel();

        auto SyncPassive = [this, level](uint8 minLevel, uint32 spellId)
        {
            if (level >= minLevel)
            {
                if (!me->HasAura(spellId))
                    me->AddAura(spellId, me);
            }
            else
            {
                me->RemoveAurasDueToSpell(spellId);
            }
        };

        SyncPassive(BearDruidSpells::PASSIVE_TALENT_MIN_LEVEL, BearDruidSpells::SURVIVAL_OF_THE_FITTEST);
        SyncPassive(BearDruidSpells::PASSIVE_TALENT_MIN_LEVEL, BearDruidSpells::NATURAL_REACTION);
        SyncPassive(BearDruidSpells::PASSIVE_TALENT_MIN_LEVEL, BearDruidSpells::THICK_HIDE);
        SyncPassive(BearDruidSpells::PASSIVE_TALENT_MIN_LEVEL, BearDruidSpells::PROTECTOR_OF_THE_PACK);

        // 野性赐福：熊 / 巨熊形态下无法施放自然系法术，直接以光环形式为自身与指挥官挂载
        if (level >= BearDruidSpells::PASSIVE_TALENT_MIN_LEVEL)
        {
            uint32 const giftSpell = BearDruidSpells::GIFT_OF_THE_WILD;

            if (!me->HasAura(giftSpell))
                me->AddAura(giftSpell, me);

            if (Player* master = GetMaster())
            {
                if (!master->HasAura(giftSpell))
                    me->AddAura(giftSpell, master);
            }
        }
        else
        {
            me->RemoveAurasDueToSpell(BearDruidSpells::GIFT_OF_THE_WILD);
        }
    }
};

void AddSC_bot_bear_druid()
{
    new AdaptiveBotScript<BotBearDruidAI>("bot_bear_druid");
}
