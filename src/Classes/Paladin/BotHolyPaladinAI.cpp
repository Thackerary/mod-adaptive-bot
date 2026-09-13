/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "HolyPaladinSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "SpellAuras.h"
#include "Pet.h"
#include "Chat.h"
#include <algorithm>
#include <cmath>
#include <vector>

class BotHolyPaladinAI : public AdaptiveBotAI
{
    // =========================================================================
    // 团队状态快照：单帧「打地鼠」雷达采样结果
    // =========================================================================
    struct GroupSnapshot
    {
        std::vector<Unit*> allies;          // 40 码内、视线可达、存活的友方单位
        Unit* lowestHpAlly{ nullptr };      // 全队生命百分比最低成员 (治疗阶梯锚点)
        float lowestHpPct{ 100.0f };
        Unit* mainTank{ nullptr };          // 道标/护盾锚点：主坦，退化时取指挥官
        float averageHpPct{ 100.0f };       // 全队平均生命百分比 (续航仲裁依据)
    };

    // 站位与巡检参数
    static constexpr uint32 BUFF_SWEEP_INTERVAL = 3000;   // ms
    static constexpr float  HEAL_RANGE          = 40.0f;  // 治疗射程上限
    static constexpr float  IDEAL_FOLLOW_DIST   = 18.0f;  // 理想站位 (主坦后方)
    static constexpr float  MIN_SAFE_DIST       = 8.0f;   // 低于此距离判定为被贴脸
    static constexpr float  MAX_SAFE_DIST       = 25.0f;  // 高于此距离判定为脱节

public:
    explicit BotHolyPaladinAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return true; }

    // 治疗随从按远程单位接管移动逻辑，禁止迈入怪物近战范围 (P5 硬约束)
    bool IsRangedBot() const override { return true; }

    // 治疗专精不承担输出职责：压低伤害，避免继承法系远程的高额输出乘数
    float GetDamageDealtMultiplier() const override { return 0.25f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case HolyPaladinSpells::DIVINE_FAVOR:        return 20; // 神恩术
            case HolyPaladinSpells::DIVINE_SHIELD:       return 34; // 圣盾术 Rank 2
            case HolyPaladinSpells::HOLY_SHOCK:          return 40; // 神圣震击
            case HolyPaladinSpells::DIVINE_ILLUMINATION: return 50; // 神启
            case HolyPaladinSpells::BEACON_OF_LIGHT:     return 60; // 圣光道标
            case HolyPaladinSpells::SACRED_SHIELD:       return 80; // 圣洁护盾
            case HolyPaladinSpells::AVENGING_WRATH:      return 70; // 复仇之怒
            case HolyPaladinSpells::DIVINE_PLEA:         return 71; // 神圣祈求
            default:                                     return 0;
        }
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 法力通道必须在基类 Reset 之前配置，以便等级同步时正确补满法力池
        me->setPowerType(POWER_MANA);
        AdaptiveBotAI::Reset();
        buffSweepTimer = 0;
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 level) override
    {
        AdaptiveBotAI::OnLevelSynced(level);
        me->setPowerType(POWER_MANA);

        if (me->GetMaxPower(POWER_MANA) > 0)
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));

        ApplyPassiveTalents();
    }

    // =========================================================================
    // 核心决策循环
    // =========================================================================
    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);

        if (buffSweepTimer <= diff)
            buffSweepTimer = BUFF_SWEEP_INTERVAL;
        else
            buffSweepTimer -= diff;

        bool const sweepDue = (buffSweepTimer == BUFF_SWEEP_INTERVAL);

        // 打地鼠雷达：每帧刷新队友状态快照
        RefreshGroupSnapshot();

        // 全局读条守卫：圣光术 / 圣光闪现等长读条期间冻结一切决策，
        // 杜绝脱战与战时被自身跟随移动指令 (UpdateFollowMaster / MoveFollow) 掐断读条。
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            if (sweepDue)
            {
                if (MaintainAura()) return;
                if (MaintainSealOfWisdom()) return;
                if (MaintainBlessing()) return;
            }

            if (MaintainBeaconOfLight()) return;
            if (MaintainSacredShield()) return;

            // 脱战预热：为受损队友补满血线
            if (TryEmergencyHeals()) return;
            if (TryLadderHeal()) return;

            UpdateFollowMaster(diff);
            return;
        }

        // =====================================================================
        // 2. 战斗内 APL
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (victim && victim->IsAlive() && victim->GetMap() == me->GetMap() && me->GetVictim() != victim)
            me->Attack(victim, false); // 仅锚定敌对目标供审判使用，绝不开启近战追击

        // ---- P0: 自保与死中求活 ----
        if (TrySelfPreservation()) return;
        if (TryEmergencyHeals()) return;

        // ---- P1: 核心 Buff 与道标维护 ----
        if (sweepDue)
        {
            if (MaintainAura()) return;
            if (MaintainSealOfWisdom()) return;
            if (MaintainBlessing()) return;
        }

        // 道标与圣洁护盾为治疗核心，每帧校验，缺失立即补齐
        if (MaintainBeaconOfLight()) return;
        if (MaintainSacredShield()) return;

        // ---- P2: 纯洁审判急速保持与续航维护 ----
        if (MaintainJudgementsOfThePure(victim)) return;
        if (MaintainManaCooldowns()) return;

        // ---- P3: 阶梯式治疗核心 ----
        if (TryLadderHeal()) return;

        // ---- P4: 团队辅助与驱散 ----
        if (TryCleanse()) return;

        // ---- P5: 跟随与站位控制 ----
        MaintainHealerPositioning();
    }

private:
    GroupSnapshot groupSnapshot;
    uint32 buffSweepTimer{ 0 };

    // =========================================================================
    // 队友状态巡检 (打地鼠雷达)
    // =========================================================================
    void RefreshGroupSnapshot()
    {
        GroupSnapshot& snap = groupSnapshot;
        snap.allies.clear();
        snap.lowestHpAlly = nullptr;
        snap.lowestHpPct = 101.0f;
        snap.averageHpPct = 100.0f;

        Player* master = GetMaster();

        // 道标锚点：优先主坦，无主坦时退化为指挥官；阵亡则回落指挥官
        Unit* groupTank = GetGroupTank();
        snap.mainTank = groupTank ? groupTank : static_cast<Unit*>(master);
        if (snap.mainTank && !snap.mainTank->IsAlive())
            snap.mainTank = master;

        if (!master)
        {
            ConsiderAlly(snap, me);
            FinalizeSnapshot(snap);
            return;
        }

        ConsiderAlly(snap, me);
        ConsiderAlly(snap, master);
        ConsiderAlly(snap, master->GetPet());

        if (Group* group = master->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || member == master || member->GetMap() != me->GetMap())
                    continue;

                ConsiderAlly(snap, member);
                ConsiderAlly(snap, member->GetPet());
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
                        ConsiderAlly(snap, allyBot->me);
                }
            }
        }

        FinalizeSnapshot(snap);
    }

    void ConsiderAlly(GroupSnapshot& snap, Unit* unit)
    {
        if (!unit || !unit->IsAlive() || !unit->IsInWorld())
            return;

        if (unit->GetMap() != me->GetMap() || !unit->IsFriendlyTo(me))
            return;

        // 排除图腾等无意义治疗目标
        if (unit->GetTypeId() == TYPEID_UNIT && unit->ToCreature()->IsTotem())
            return;

        // 硬性治疗条件：40 码内且视线可达
        if (!me->IsWithinDist(unit, HEAL_RANGE) || !me->IsWithinLOSInMap(unit))
            return;

        if (std::find(snap.allies.begin(), snap.allies.end(), unit) != snap.allies.end())
            return;

        snap.allies.push_back(unit);

        float const hp = unit->GetHealthPct();
        if (hp < snap.lowestHpPct)
        {
            snap.lowestHpPct = hp;
            snap.lowestHpAlly = unit;
        }
    }

    void FinalizeSnapshot(GroupSnapshot& snap)
    {
        if (snap.allies.empty())
        {
            snap.lowestHpAlly = nullptr;
            snap.lowestHpPct = 100.0f;
            snap.averageHpPct = 100.0f;
            return;
        }

        if (!snap.lowestHpAlly)
        {
            snap.lowestHpAlly = snap.allies.front();
            snap.lowestHpPct = snap.lowestHpAlly->GetHealthPct();
        }

        float total = 0.0f;
        for (Unit* ally : snap.allies)
            total += ally->GetHealthPct();

        snap.averageHpPct = total / static_cast<float>(snap.allies.size());
    }

    // =========================================================================
    // 治疗合法性校验 (距离 <= 40 码 + 视线 + 阵营)
    // =========================================================================
    bool IsValidHealTarget(Unit* target) const
    {
        if (!target || !target->IsAlive() || !target->IsInWorld())
            return false;

        if (target->GetMap() != me->GetMap() || !target->IsFriendlyTo(me))
            return false;

        return me->IsWithinDist(target, HEAL_RANGE) && me->IsWithinLOSInMap(target);
    }

    bool IsUnderPhysicalMelee(Unit* unit) const
    {
        if (!unit)
            return false;

        for (Unit* attacker : unit->getAttackers())
        {
            if (attacker && attacker->IsAlive() && attacker->GetMap() == me->GetMap() && attacker->IsWithinMeleeRange(unit))
                return true;
        }

        return false;
    }

    bool HasDispellableDebuff(Unit* unit, SpellInfo const* cleanseInfo) const
    {
        if (!unit || !cleanseInfo)
            return false;

        for (auto const& entry : unit->GetAppliedAuras())
        {
            AuraApplication* application = entry.second;
            if (!application)
                continue;

            Aura* aura = application->GetBase();
            if (!aura)
                continue;

            SpellInfo const* auraInfo = aura->GetSpellInfo();
            if (auraInfo && auraInfo->Dispel != DISPEL_NONE && cleanseInfo->CanDispelAura(auraInfo))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P0: 自保与死中求活
    // =========================================================================
    bool TrySelfPreservation()
    {
        if (me->GetHealthPct() >= 20.0f)
            return false;

        uint32 const divineShield = GetAppropriateRank(HolyPaladinSpells::DIVINE_SHIELD, false);
        if (!divineShield || me->HasAura(divineShield))
            return false;

        if (!CanCast(me, divineShield, true))
            return false;

        return ExecuteSpell(me, divineShield, true);
    }

    bool TryEmergencyHeals()
    {
        // 1) 主坦 (或任意队友) 生命 < 15%：圣疗术极限救急
        Unit* layOnHandsTarget = nullptr;
        if (groupSnapshot.mainTank && groupSnapshot.mainTank->IsAlive() &&
            groupSnapshot.mainTank->GetHealthPct() < 15.0f && IsValidHealTarget(groupSnapshot.mainTank))
        {
            layOnHandsTarget = groupSnapshot.mainTank;
        }
        else if (groupSnapshot.lowestHpAlly && groupSnapshot.lowestHpPct < 15.0f)
        {
            layOnHandsTarget = groupSnapshot.lowestHpAlly;
        }

        if (layOnHandsTarget)
        {
            uint32 const layOnHands = GetAppropriateRank(HolyPaladinSpells::LAY_ON_HANDS, false);
            // 圣疗术会为目标施加自律：已有自律者直接跳过，避免浪费这张超长 CD 底牌
            if (layOnHands && !layOnHandsTarget->HasAura(HolyPaladinSpells::FORBEARANCE) &&
                CanCast(layOnHandsTarget, layOnHands, true) &&
                ExecuteSpell(layOnHandsTarget, layOnHands, true))
            {
                return true;
            }
        }

        // 2) 非坦队友被物理围殴且生命 < 25%：保护之手 (物理免疫)
        if (groupSnapshot.lowestHpAlly && groupSnapshot.lowestHpPct < 25.0f)
        {
            Unit* protectTarget = groupSnapshot.lowestHpAlly;
            bool const isMainTank = (groupSnapshot.mainTank && protectTarget == groupSnapshot.mainTank);

            if (!isMainTank && !protectTarget->HasAura(HolyPaladinSpells::FORBEARANCE) && IsUnderPhysicalMelee(protectTarget))
            {
                uint32 const handOfProtection = GetAppropriateRank(HolyPaladinSpells::HAND_OF_PROTECTION, false);
                if (handOfProtection && CanCast(protectTarget, handOfProtection, true) &&
                    ExecuteSpell(protectTarget, handOfProtection, true))
                {
                    return true;
                }
            }
        }

        return false;
    }

    // =========================================================================
    // P1: 常驻光环、圣印与祝福
    // =========================================================================
    bool MaintainAura()
    {
        uint32 const concentration = GetAppropriateRank(HolyPaladinSpells::CONCENTRATION_AURA, false);

        // 专注光环是治疗主光环：已学会时无条件维持自身那一份，
        // 绝不因防骑队友的虔诚光环覆盖而误判为「已有光环」而放弃施放
        if (concentration)
        {
            if (me->HasAura(concentration))
                return false;

            return CanCast(me, concentration, true) && ExecuteSpell(me, concentration, true);
        }

        // 低等级过渡期回退虔诚光环：仅校验自身施加的那一份，避免干扰团队他人的光环配置
        uint32 const devotion = GetAppropriateRank(HolyPaladinSpells::DEVOTION_AURA, false);
        if (!devotion || me->GetAura(devotion, me->GetGUID()))
            return false;

        return CanCast(me, devotion, true) && ExecuteSpell(me, devotion, true);
    }

    bool MaintainSealOfWisdom()
    {
        uint32 const seal = GetAppropriateRank(HolyPaladinSpells::SEAL_OF_WISDOM, false);
        if (!seal || me->HasAura(seal))
            return false;

        if (!CanCast(me, seal, true))
            return false;

        return ExecuteSpell(me, seal, true);
    }

    bool MaintainBlessing()
    {
        if (groupSnapshot.allies.empty())
            return false;

        // 团队祝福分流：按目标能量通道选择祝福，每帧只补一个缺口，平滑铺满全队
        for (Unit* ally : groupSnapshot.allies)
        {
            if (!ally || !ally->IsAlive() || ally->GetMap() != me->GetMap())
                continue;

            uint32 const blessing = (ally->getPowerType() == POWER_MANA)
                ? GetAppropriateRank(HolyPaladinSpells::BLESSING_OF_WISDOM, false)
                : GetAppropriateRank(HolyPaladinSpells::BLESSING_OF_KINGS, false);

            if (!blessing || ally->HasAura(blessing))
                continue;

            // 施法校验失败（如宠物等非法祝福目标）时继续扫描，避免阻塞其它队友
            if (!CanCast(ally, blessing, true))
                continue;

            if (ExecuteSpell(ally, blessing, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P1: 圣光道标与圣洁护盾 (主坦锚点)
    // =========================================================================
    bool MaintainBeaconOfLight()
    {
        uint32 const beacon = GetAppropriateRank(HolyPaladinSpells::BEACON_OF_LIGHT, false);
        if (!beacon)
            return false;

        Unit* tank = groupSnapshot.mainTank;
        if (!tank || !tank->IsAlive() || tank->GetMap() != me->GetMap())
            return false;

        // 剩余 < 4 秒即提前补挂：道标一旦断档，主坦折射治疗会瞬间归零
        if (Aura* existing = tank->GetAura(beacon, me->GetGUID()))
        {
            if (existing->GetDuration() > 4000)
                return false;
        }

        if (!CanCast(tank, beacon, true))
            return false;

        return ExecuteSpell(tank, beacon, true);
    }

    bool MaintainSacredShield()
    {
        uint32 const sacredShield = GetAppropriateRank(HolyPaladinSpells::SACRED_SHIELD, false);
        if (!sacredShield)
            return false;

        Unit* tank = groupSnapshot.mainTank;
        if (!tank || !tank->IsAlive() || tank->GetMap() != me->GetMap())
            return false;

        // 剩余 < 4 秒即提前补挂：吸收盾断档会导致主坦承受尖刺伤害
        if (Aura* existing = tank->GetAura(sacredShield, me->GetGUID()))
        {
            if (existing->GetDuration() > 4000)
                return false;
        }

        if (!CanCast(tank, sacredShield, true))
            return false;

        return ExecuteSpell(tank, sacredShield, true);
    }

    // =========================================================================
    // P2: 纯洁审判急速保持
    // 注意：光明审判基础射程较短，CanCast 会自行裁决；不在射程内时自然跳过，
    //       待站位进入有效距离后由下一次决策补齐急速 Buff。
    // =========================================================================
    bool MaintainJudgementsOfThePure(Unit* victim)
    {
        // 仅以「急速 Buff 本体」作为判定依据；剩余 > 3 秒不重复审判，
        // 缺失或即将断档时立即补打，彻底解除对被动光环自检导致的死锁
        if (Aura* hasteAura = me->GetAura(HolyPaladinSpells::BUFF_JUDGEMENTS_OF_THE_PURE))
        {
            if (hasteAura->GetDuration() > 3000)
                return false;
        }

        uint32 const judgement = GetAppropriateRank(HolyPaladinSpells::JUDGEMENT_OF_LIGHT, false);
        if (!judgement)
            return false;

        if (!victim || !victim->IsAlive() || victim->GetMap() != me->GetMap() || victim->IsFriendlyTo(me))
            return false;

        if (!CanCast(victim, judgement, true))
            return false;

        return ExecuteSpell(victim, judgement, true);
    }

    // =========================================================================
    // P2: 续航仲裁 (神圣祈求 / 复仇之怒对冲 / 神启)
    // =========================================================================
    bool MaintainManaCooldowns()
    {
        if (me->getPowerType() != POWER_MANA || !me->IsInCombat())
            return false;

        float const manaPct = me->GetPowerPct(POWER_MANA);

        // 神启：耗蓝减半，法力 < 50% 即卡 CD 开启 (无血线门槛)，
        // 提前覆盖后续高压刷血窗口，提升整场续航总效率
        uint32 const divineIllumination = GetAppropriateRank(HolyPaladinSpells::DIVINE_ILLUMINATION, false);
        if (divineIllumination && manaPct < 50.0f && !me->HasAura(divineIllumination))
        {
            if (CanCast(me, divineIllumination, true) && ExecuteSpell(me, divineIllumination, true))
                return true;
        }

        // 神圣祈求会降低 50% 治疗量，仅在法力 < 40% 时才进入其仲裁流程
        if (manaPct >= 40.0f)
            return false;

        float const avgHp = groupSnapshot.averageHpPct;

        // 全队血线 < 60%：治疗压力过大，禁止开启降低 50% 治疗量的神圣祈求
        if (avgHp < 60.0f)
            return false;

        // 60% ~ 80%：必须先以复仇之怒 (+20% 治疗) 对冲神圣祈求的 50% 治疗惩罚
        if (avgHp < 80.0f)
        {
            uint32 const avengingWrath = GetAppropriateRank(HolyPaladinSpells::AVENGING_WRATH, false);
            if (!avengingWrath)
                return false; // 未解锁对冲手段，禁止启用神圣祈求

            if (!me->HasAura(avengingWrath))
            {
                if (CanCast(me, avengingWrath, true) && ExecuteSpell(me, avengingWrath, true))
                    return true;

                return false; // 等待翅膀生效后再开祈求
            }
        }

        uint32 const divinePlea = GetAppropriateRank(HolyPaladinSpells::DIVINE_PLEA, false);
        if (divinePlea && !me->HasAura(divinePlea))
        {
            if (CanCast(me, divinePlea, true) && ExecuteSpell(me, divinePlea, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P3: 阶梯式治疗核心 (目标：LowestHpAlly，道标自动镜像回坦克)
    // =========================================================================
    bool TryLadderHeal()
    {
        Unit* target = groupSnapshot.lowestHpAlly;
        if (!target || groupSnapshot.lowestHpPct >= 95.0f)
            return false;

        if (!IsValidHealTarget(target))
            return false;

        float const hpPct = groupSnapshot.lowestHpPct;

        // ---------------------------------------------------------------------
        // 高危重伤 (< 60%)：神恩术 + 圣光术大加
        // 神恩术为自身增益，需跨帧与圣光术分离施放，避免同帧 GCD 互斥
        // ---------------------------------------------------------------------
        if (hpPct < 60.0f)
        {
            uint32 const divineFavor = GetAppropriateRank(HolyPaladinSpells::DIVINE_FAVOR, false);
            if (divineFavor && !me->HasAura(divineFavor))
            {
                if (CanCast(me, divineFavor, true) && ExecuteSpell(me, divineFavor, true))
                    return true;
            }

            uint32 const holyLight = GetAppropriateRank(HolyPaladinSpells::HOLY_LIGHT, false);
            if (holyLight && CanCast(target, holyLight, true) && ExecuteSpell(target, holyLight, true))
                return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 中度掉血 (< 80%)：优先瞬发神圣震击，冷却中回退圣光术
        // ---------------------------------------------------------------------
        if (hpPct < 80.0f)
        {
            uint32 const holyShock = GetAppropriateRank(HolyPaladinSpells::HOLY_SHOCK, false);
            if (holyShock && CanCast(target, holyShock, true) && ExecuteSpell(target, holyShock, true))
                return true;

            uint32 const holyLight = GetAppropriateRank(HolyPaladinSpells::HOLY_LIGHT, false);
            if (holyLight && CanCast(target, holyLight, true) && ExecuteSpell(target, holyLight, true))
                return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 平稳掉血 (80% ~ 95%)：圣光闪现效率填充
        // ---------------------------------------------------------------------
        uint32 const flashOfLight = GetAppropriateRank(HolyPaladinSpells::FLASH_OF_LIGHT, false);
        if (flashOfLight && CanCast(target, flashOfLight, true) && ExecuteSpell(target, flashOfLight, true))
            return true;

        return false;
    }

    // =========================================================================
    // P4: 团队辅助与驱散 (仅在血线健康时介入，避免挤占治疗窗口)
    // =========================================================================
    bool TryCleanse()
    {
        if (!groupSnapshot.lowestHpAlly || groupSnapshot.lowestHpPct < 90.0f)
            return false;

        uint32 const cleanse = GetAppropriateRank(HolyPaladinSpells::CLEANSE, false);
        if (!cleanse)
            return false;

        SpellInfo const* cleanseInfo = sSpellMgr->GetSpellInfo(cleanse);
        if (!cleanseInfo)
            return false;

        for (Unit* ally : groupSnapshot.allies)
        {
            if (!IsValidHealTarget(ally) || !HasDispellableDebuff(ally, cleanseInfo))
                continue;

            if (!CanCast(ally, cleanse, true))
                return false;

            return ExecuteSpell(ally, cleanse, true);
        }

        return false;
    }

    // =========================================================================
    // P5: 站位控制 (主坦后方 15 ~ 25 码，禁止进入怪物近战范围)
    // =========================================================================
    void MaintainHealerPositioning()
    {
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        Unit* anchor = groupSnapshot.mainTank;
        if (!anchor || !anchor->IsAlive())
            anchor = GetMaster();

        if (!anchor || !anchor->IsInWorld() || anchor->GetMap() != me->GetMap())
            return;

        bool const underMeleePressure = IsUnderPhysicalMelee(me);
        float const dist = me->GetDistance(anchor);
        bool const needsReposition = underMeleePressure || dist > MAX_SAFE_DIST || dist < MIN_SAFE_DIST;

        if (!needsReposition)
            return;

        // 站位于锚点正后方，避免与坦克抢正面仇恨面
        float const behindAngle = anchor->GetOrientation() + static_cast<float>(M_PI);
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();

        if (underMeleePressure)
        {
            // 被贴脸时禁止原地后撤：后撤会被持续追打并拉开与坦克的距离，
            // 必须立刻贴到坦克身侧，借坦克的 AoE 仇恨把小怪拉走。
            // 加双闸门 (距离 > 3 码 且 未处于跟随态) 防止每帧重建移动生成器造成路径抖动。
            if (me->GetDistance(anchor) > 3.0f && moveType != FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveFollow(anchor, 2.0f, 0.0f);

            return;
        }

        if (moveType != FOLLOW_MOTION_TYPE)
            me->GetMotionMaster()->MoveFollow(anchor, IDEAL_FOLLOW_DIST, behindAngle);
    }

    // =========================================================================
    // 神圣天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
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

        SyncPassive(20, HolyPaladinSpells::ILLUMINATION);        // 启发：暴击治疗回蓝
        SyncPassive(40, HolyPaladinSpells::IMPROVED_JUDGEMENTS); // 强化审判：审判射程 +20 码 (10 -> 30)
        SyncPassive(50, HolyPaladinSpells::HOLY_GUIDANCE);       // 神圣指引：智力转化法强
        SyncPassive(50, HolyPaladinSpells::INFUSION_OF_LIGHT);   // 圣光灌注：圣光闪现/神圣震击强化

        // 纯洁审判天赋被动：Creature 没有天赋树，必须注入被动触发器 (54155)，
        // 否则施放审判后引擎不会派生急速 Buff (53657)；
        // 急速 Buff 本体仍由 MaintainJudgementsOfThePure() 依剩余时间动态刷新。
        SyncPassive(50, HolyPaladinSpells::TALENT_JUDGEMENTS_OF_THE_PURE);
    }
};

void AddSC_bot_holy_paladin()
{
    new AdaptiveBotScript<BotHolyPaladinAI>("bot_holy_paladin");
}
