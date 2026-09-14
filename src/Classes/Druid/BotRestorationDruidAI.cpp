/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "RestorationDruidSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "Pet.h"
#include "Chat.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

class BotRestorationDruidAI : public AdaptiveBotAI
{
    // =========================================================================
    // 团队状态快照：单帧「打地鼠」雷达采样结果 (与 BotRestorationShamanAI 完全同构)
    // =========================================================================
    struct GroupSnapshot
    {
        std::vector<Unit*> allies;          // 40 码内、视线可达、存活的友方单位
        Unit* lowestHpAlly{ nullptr };      // 全队生命百分比最低成员 (治疗阶梯锚点，严格排除宠物)
        float lowestHpPct{ 100.0f };
        Unit* mainTank{ nullptr };          // 三花 / 回春锚点：主坦，退化时取指挥官
        float averageHpPct{ 100.0f };       // 全队平均生命百分比 (严格排除宠物)
    };

    // 站位与巡检参数
    static constexpr uint32 BUFF_SWEEP_INTERVAL = 3000;   // ms
    static constexpr float  HEAL_RANGE          = 40.0f;  // 治疗射程上限
    static constexpr float  IDEAL_FOLLOW_DIST   = 18.0f;  // 理想站位 (主坦后方)
    static constexpr float  MIN_SAFE_DIST       = 8.0f;   // 低于此距离判定为被贴脸
    static constexpr float  MAX_SAFE_DIST       = 25.0f;  // 高于此距离判定为脱节
    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 anchor->GetOrientation()，否则站位会随坦克转向持续漂移。
    // M_PI 即锚点正后方：坦克背身位，可规避顺劈斩与正面吐息。
    static constexpr float  BEHIND_ANGLE        = static_cast<float>(M_PI);

    // 自管冷却登记 (Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的
    // 瞬发 CD 技能必须由专精自行计时，否则会因 HasSpellCooldown 恒 false 而空转)
    static constexpr uint32 CD_SWIFTMEND         = 15000;
    static constexpr uint32 CD_WILD_GROWTH       = 6000;
    static constexpr uint32 CD_INNERVATE         = 180000;
    static constexpr uint32 CD_NATURES_SWIFTNESS = 120000;
    static constexpr uint32 CD_BARKSPIN          = 60000;

    // 生命之绽「三花不落」补刷窗口：叠满 3 层且剩余时间低于此值时提前续花
    static constexpr int32  LIFEBLOOM_REFRESH_WINDOW = 3000;

public:
    explicit BotRestorationDruidAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return true; }

    // 治疗随从按远程单位接管移动逻辑，禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 治疗专精不承担输出职责：压低伤害，避免继承法系远程的高额输出乘数
    float GetDamageDealtMultiplier() const override { return 0.25f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (生命之绽 / 回春术 / 愈合 / 滋养 / 治疗之触 / 激活 / 驱散)
    //     均为非天赋法术，严禁登记于此，其等级门槛由 GetAppropriateRank
    //     依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case RestorationDruidSpells::NATURES_SWIFTNESS: return 30; // 自然迅捷
            case RestorationDruidSpells::SWIFTMEND:         return 40; // 迅捷治愈
            case RestorationDruidSpells::TREE_OF_LIFE:      return 50; // 生命之树形态
            case RestorationDruidSpells::WILD_GROWTH:       return 60; // 野性成长 (恢复终极天赋)
            default:                                        return 0;
        }
    }

    // =========================================================================
    // 生命之树形态施法豁免
    // -------------------------------------------------------------------------
    // 树形态在 3.3.5a 属形态锁定状态：若不做豁免，一旦变形，全部治疗与驱散
    // 通道会被引擎的形态校验 (SpellInfo::CheckShapeshift) 拦截，
    // 随从将退化为原地发呆的木桩。
    // 仅当确实处于树形态时对正向法术 (治疗 / 驱散 / 自身增益) 放行，
    // 非形态状态与敌对法术依旧走引擎原生校验，零副作用。
    // =========================================================================
    bool CheckShapeshiftExemption(SpellInfo const* spellInfo) const override
    {
        if (AdaptiveBotAI::CheckShapeshiftExemption(spellInfo))
            return true;

        if (!spellInfo || me->GetShapeshiftForm() != FORM_TREE)
            return false;

        return spellInfo->IsPositive();
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
        isHuggingTank = false;
        ResetDruidTimers();
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
        UpdateDruidTimers(diff);

        if (buffSweepTimer <= diff)
            buffSweepTimer = BUFF_SWEEP_INTERVAL;
        else
            buffSweepTimer -= diff;

        bool const sweepDue = (buffSweepTimer == BUFF_SWEEP_INTERVAL);

        // 打地鼠雷达：每帧刷新队友状态快照
        RefreshGroupSnapshot();

        // 全局读条/通道双保险守卫：治疗之触 / 滋养 / 愈合 (读条) 期间引擎会置位
        // UNIT_STATE_CASTING，但引导类法术在部分状态下并不置位该标记，
        // 因此追加 CURRENT_CHANNELED_SPELL 显式判定，
        // 杜绝自身上层法术被跟随移动指令 (UpdateFollowMaster / MoveFollow) 掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            // 脱战残血优先救命：绝不允许形态与长效 HoT 抢占急救 GCD
            if (groupSnapshot.lowestHpPct < 90.0f)
            {
                if (TryLadderHeal()) return;
            }

            if (sweepDue)
            {
                if (MaintainTreeOfLife()) return;
                if (MaintainTankHoTs()) return;
            }

            if (TryLadderHeal()) return;

            UpdateFollowMaster(diff);
            return;
        }

        // =====================================================================
        // 2. 战斗内 APL
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (victim && victim->IsAlive() && victim->GetMap() == me->GetMap() && me->GetVictim() != victim)
            me->Attack(victim, false); // 仅锚定敌对目标维持进战姿态，绝不开启近战追击

        // ---- P0: 极限急救 (树皮自保 + 自然迅捷瞬发治疗之触) ----
        if (TryEmergencyHeals()) return;

        // ---- P1: 生命之树形态维持与主坦「三花聚顶」滚动 ----
        if (sweepDue)
        {
            if (MaintainTreeOfLife()) return;
            if (MaintainTankHoTs()) return;
        }

        // ---- P2: 团队群抬 (野性成长) 与续航 (激活) ----
        if (TryWildGrowth()) return;
        if (MaintainInnervate()) return;

        // ---- P3: 阶梯式直接治疗与迅捷治愈爆发 ----
        if (TryLadderHeal()) return;

        // ---- P4: 驱散 ----
        if (TryCleanse()) return;

        // ---- P5: 跟随与站位控制 ----
        MaintainHealerPositioning();
    }

private:
    GroupSnapshot groupSnapshot;
    uint32 buffSweepTimer{ 0 };

    // 贴脸避难状态标记：记录当前是否处于「紧抱坦克身侧」的应急姿态。
    // 仇恨解除后必须凭此标记主动重发跟随指令，否则会永久粘在坦克身后吃顺劈与吐息。
    bool isHuggingTank{ false };

    // 自管冷却计时
    uint32 swiftmendCooldown{ 0 };
    uint32 wildGrowthCooldown{ 0 };
    uint32 innervateCooldown{ 0 };
    uint32 naturesSwiftnessCooldown{ 0 };
    uint32 barkspinCooldown{ 0 };

    // =========================================================================
    // 天赋法术统一取用闸门
    // -------------------------------------------------------------------------
    // 基类 GetAppropriateRank(id, true) 为「Rank 1 保底」语义，会跳过
    // GetTalentSpellMinLevel 的等级校验；且 3.3.5a 天赋法术 DBC SpellLevel
    // 多为 0，降阶链无从回落，低等级随从会把高阶天赋 (如 60 级野性成长)
    // 当基础技能空放。
    // 故所有天赋法术统一经此闸门取用：未达登记解锁等级一律返回 0，
    // 由 APL 自然降级到基础治疗通道 (与基类契约语义完全一致)。
    // =========================================================================
    uint32 GetTalentSpell(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        if (minLevel > 0 && me->GetLevel() < minLevel)
            return 0;

        return GetAppropriateRank(spellId, true);
    }

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateDruidTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(swiftmendCooldown);
        Tick(wildGrowthCooldown);
        Tick(innervateCooldown);
        Tick(naturesSwiftnessCooldown);
        Tick(barkspinCooldown);
    }

    void ResetDruidTimers()
    {
        swiftmendCooldown = 0;
        wildGrowthCooldown = 0;
        innervateCooldown = 0;
        naturesSwiftnessCooldown = 0;
        barkspinCooldown = 0;
    }

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

        // HoT 锚点：优先主坦，无主坦时退化为指挥官；阵亡则回落指挥官
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

        // 宠物保留在 allies 名单内 (以便享受野性成长等溅射辅助)，
        // 但严禁用宠物血线触发全队恐慌：否则猎人宠物掉血会把 lowestHpAlly
        // 绑架到宠物身上，导致三花、回春、滋养全部错失真正的玩家与坦克。
        bool const isPet = (unit->ToPet() != nullptr);
        if (!isPet)
        {
            float const hp = unit->GetHealthPct();
            if (hp < snap.lowestHpPct)
            {
                snap.lowestHpPct = hp;
                snap.lowestHpAlly = unit;
            }
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

        // 兜底锚点同样必须过滤宠物：唯一可见目标恰为宠物时，
        // 直接取 front() 会把治疗锚点重新交还给宠物，抵消最低血线的过滤。
        if (!snap.lowestHpAlly)
        {
            for (Unit* ally : snap.allies)
            {
                if (ally && !ally->ToPet())
                {
                    snap.lowestHpAlly = ally;
                    snap.lowestHpPct = ally->GetHealthPct();
                    break;
                }
            }
        }

        // 平均血线剔除宠物：宠物残血会把平均值拉低，
        // 误触发「团队崩溃保护」从而永久锁死野性成长与激活的判定。
        float total = 0.0f;
        uint32 validCount = 0;
        for (Unit* ally : snap.allies)
        {
            if (ally && !ally->ToPet())
            {
                total += ally->GetHealthPct();
                ++validCount;
            }
        }

        snap.averageHpPct = (validCount > 0) ? (total / static_cast<float>(validCount)) : 100.0f;
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

    // 目标是否已挂可驱动迅捷治愈 / 滋养加成的核心 HoT (回春 / 愈合)
    bool HasSwiftmendFuel(Unit* target) const
    {
        if (!target)
            return false;

        uint32 const rejuvenation = GetAppropriateRank(RestorationDruidSpells::REJUVENATION, false);
        if (rejuvenation && target->HasAura(rejuvenation))
            return true;

        uint32 const regrowth = GetAppropriateRank(RestorationDruidSpells::REGROWTH, false);
        if (regrowth && target->HasAura(regrowth))
            return true;

        return false;
    }

    // =========================================================================
    // P0: 极限急救 (树皮自保 + 自然迅捷瞬发治疗之触)
    // =========================================================================
    Unit* SelectEmergencyTarget() const
    {
        // 优先自身濒死：治疗随从阵亡等于全队断奶
        if (me->GetHealthPct() < 35.0f && IsValidHealTarget(me))
            return me;

        // 主坦濒死：承伤核心，倒地即团灭
        if (groupSnapshot.mainTank && groupSnapshot.mainTank->IsAlive() &&
            !groupSnapshot.mainTank->ToPet() &&
            groupSnapshot.mainTank->GetHealthPct() < 30.0f &&
            IsValidHealTarget(groupSnapshot.mainTank))
        {
            return groupSnapshot.mainTank;
        }

        // 全队最低血线濒死
        if (groupSnapshot.lowestHpAlly && groupSnapshot.lowestHpAlly->IsAlive() &&
            !groupSnapshot.lowestHpAlly->ToPet() &&
            groupSnapshot.lowestHpPct < 30.0f &&
            IsValidHealTarget(groupSnapshot.lowestHpAlly))
        {
            return groupSnapshot.lowestHpAlly;
        }

        return nullptr;
    }

    bool TryEmergencyHeals()
    {
        // ---------------------------------------------------------------------
        // 情形零：树皮术自保 (瞬发 20% 减伤)
        // 自身承压且被物理贴脸时优先解锁减伤，避免治疗随从被近战秒杀导致断奶。
        // 自保增益不得吞掉当帧急救：施放后不 return，继续评估自然迅捷大加。
        // ---------------------------------------------------------------------
        if (me->GetHealthPct() < 40.0f && IsUnderPhysicalMelee(me))
        {
            uint32 const barkskin = GetAppropriateRank(RestorationDruidSpells::BARKSPIN, false);
            if (barkskin && barkspinCooldown == 0 && !me->HasAura(barkskin))
            {
                if (CanCast(me, barkskin, true) && ExecuteSpell(me, barkskin, true))
                    barkspinCooldown = CD_BARKSPIN;
            }
        }

        // ---------------------------------------------------------------------
        // 情形一 (最高优先级)：自然迅捷光环已就绪 —— 必须当帧兑现，
        // 严禁把这张瞬发大加留到 P3 被低级治疗法术误吞。
        // 自然迅捷光环仅短时存在 (10s 窗口)，若仍先走 SelectEmergencyTarget()
        // 的严格濒死门禁 (< 30%)，目标血线恰在 P3 前被抬离阈值就会失去目标；
        // 光环随即被 P3 中任何一发自然系法术 (回春 / 愈合) 消耗掉，
        // 2 分钟底牌被 1.5 秒小加白白吞没。
        // 因此只要光环存在且仍有任何受损目标，就当帧施放瞬发治疗之触兑现。
        // ---------------------------------------------------------------------
        if (me->HasAura(RestorationDruidSpells::NATURES_SWIFTNESS))
        {
            Unit* swiftTarget = SelectEmergencyTarget();

            // 降级锚点：无严格濒死目标时，只要全队仍有受损成员即兑现，
            // 避免光环在 10 秒窗口内白白过期。
            if (!swiftTarget &&
                groupSnapshot.lowestHpAlly && !groupSnapshot.lowestHpAlly->ToPet() &&
                groupSnapshot.lowestHpPct < 90.0f &&
                IsValidHealTarget(groupSnapshot.lowestHpAlly))
            {
                swiftTarget = groupSnapshot.lowestHpAlly;
            }

            if (swiftTarget)
                return TryHealingTouch(swiftTarget, true);
        }

        // 情形二：光环未激活，尝试主动开启自然迅捷 (Off-GCD，2 分钟自家冷却)
        Unit* target = SelectEmergencyTarget();
        if (!target)
            return false;

        uint32 const naturesSwiftness = GetTalentSpell(RestorationDruidSpells::NATURES_SWIFTNESS);
        if (!naturesSwiftness || naturesSwiftnessCooldown > 0)
            return false;

        if (!CanCast(me, naturesSwiftness, true) || !ExecuteSpell(me, naturesSwiftness, true))
            return false;

        naturesSwiftnessCooldown = CD_NATURES_SWIFTNESS;

        // Off-GCD 铁律：自然迅捷不占公共冷却，严禁在此无条件 return true。
        // 必须允许当帧决策流顺下，让自然迅捷光环当帧立即被治疗之触吃下；
        // 若光环尚未可见 (极端情形)，则返回 false 交由下一帧「情形一」兜底。
        if (me->HasAura(RestorationDruidSpells::NATURES_SWIFTNESS))
            return TryHealingTouch(target, true);

        return false;
    }

    // =========================================================================
    // P1-a: 生命之树形态维持 (50 级天赋)
    // -------------------------------------------------------------------------
    // 树形态提升治疗受疗与护甲，是恢复德长期收益最高的形态开关。
    // 但变形占用一次 GCD，战时全队承压时必须无条件让渡给治疗通道。
    // =========================================================================
    bool MaintainTreeOfLife()
    {
        uint32 const treeForm = GetTalentSpell(RestorationDruidSpells::TREE_OF_LIFE);
        if (!treeForm)
            return false;

        // 已处于树形态 (或光环生效中)：无需重复变形
        if (me->GetShapeshiftForm() == FORM_TREE || me->HasAura(treeForm))
            return false;

        // 战时节流门禁：全队脱离承压线后才允许变形维持
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 80.0f)
            return false;

        if (!CanCast(me, treeForm, true))
            return false;

        return ExecuteSpell(me, treeForm, true);
    }

    // =========================================================================
    // P1-b: 主坦「三花聚顶」与常驻回春滚动
    // -------------------------------------------------------------------------
    // 生命之绽按层叠加，满 3 层收益最高；提前续花可保住层数不掉。
    // 回春术作为迅捷治愈的跳板与常驻减伤，必须时刻在手。
    // =========================================================================
    bool MaintainTankHoTs()
    {
        Unit* tank = groupSnapshot.mainTank;

        // 排己判定 (致命)：无独立主坦时锚点会退化为指挥官或自身，
        // 随从不承担坦克职责，严禁对自身滚动三花白白烧蓝。
        if (!tank || tank == me || !tank->IsAlive() || tank->ToPet() || !IsValidHealTarget(tank))
            return false;

        // 战时节流门禁：全队重伤时补长效 HoT 的 GCD 必须让渡给急救通道
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 65.0f)
            return false;

        // ---- 生命之绽「三花不落」滚动维持 ----
        uint32 const lifebloom = GetAppropriateRank(RestorationDruidSpells::LIFEBLOOM, false);
        if (lifebloom)
        {
            Aura* bloom = tank->GetAura(lifebloom);

            // 层数检测必须走 Aura::GetStackAmount()：可叠加光环在底层仅有一个
            // AuraApplication 实例，GetAuraCount() 恒返回 1，用其判层会使本分支
            // 彻底沦为死代码，三花永远只挂 1 层。
            // 剩余时间检测排除 <= 0 的永久光环 (-1)，避免无意义刷新。
            bool const needRefresh = !bloom
                                  || bloom->GetStackAmount() < 3
                                  || (bloom->GetDuration() > 0 && bloom->GetDuration() < LIFEBLOOM_REFRESH_WINDOW);

            if (needRefresh)
            {
                // 瞬发 HoT：允许在跑位途中直接施放，无需 StopMoving
                if (CanCast(tank, lifebloom, true) && ExecuteSpell(tank, lifebloom, true))
                    return true;
            }
        }

        // ---- 主坦常驻回春术 ----
        uint32 const rejuvenation = GetAppropriateRank(RestorationDruidSpells::REJUVENATION, false);
        if (rejuvenation && !tank->HasAura(rejuvenation))
        {
            if (CanCast(tank, rejuvenation, true) && ExecuteSpell(tank, rejuvenation, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P2-a: 团队智能群抬 (野性成长)
    // -------------------------------------------------------------------------
    // 以 lowestHpAlly 为锚点：野性成长会自动向附近最需要治疗的队友逐跳扩散，
    // 在 2 人以上同时掉血时单发瞬发即可替代多次单体治疗，
    // 且享受雕文加成 (目标数量 +1，达 6 目标)。
    // 严禁在仅 1 人掉血时施放：收益低于单体直疗，纯粹浪费 6 秒 CD。
    // =========================================================================
    bool TryWildGrowth()
    {
        // 门禁一：无人处于濒死猝死线 (< 55%)
        // 群抬虽为瞬发，但优先级低于单体急救，重伤期必须让渡给 P0/P3 单体通道。
        if (groupSnapshot.lowestHpPct < 55.0f)
            return false;

        uint32 const wildGrowth = GetTalentSpell(RestorationDruidSpells::WILD_GROWTH);
        if (!wildGrowth || wildGrowthCooldown > 0)
            return false;

        // 门禁二：群抬收益判定，至少 2 名非宠物成员低于 85% 血线
        uint32 injuredCount = 0;
        for (Unit* ally : groupSnapshot.allies)
        {
            if (!ally || ally->ToPet())
                continue;

            if (!IsValidHealTarget(ally))
                continue;

            if (ally->GetHealthPct() < 85.0f)
                ++injuredCount;
        }

        if (injuredCount < 2)
            return false;

        Unit* anchor = groupSnapshot.lowestHpAlly;
        if (!IsValidHealTarget(anchor) || anchor->ToPet())
            return false;

        if (!CanCast(anchor, wildGrowth, true))
            return false;

        // 瞬发群抬：允许在跑位与抱坦避难途中直接施放，无需 StopMoving
        if (ExecuteSpell(anchor, wildGrowth, true))
        {
            wildGrowthCooldown = CD_WILD_GROWTH;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P2-b: 续航仲裁 (激活)
    // -------------------------------------------------------------------------
    // 激活为自身高额回蓝底牌 (3 分钟 CD)。
    // 血线安全门禁必不可少：治疗随从残蓝但队友正在挨打时，
    // 回蓝的 GCD 必须无条件让渡给救命通道，回蓝留待平稳期补足。
    // =========================================================================
    bool MaintainInnervate()
    {
        if (!me->IsInCombat())
            return false;

        if (me->getPowerType() != POWER_MANA)
            return false;

        if (me->GetPowerPct(POWER_MANA) >= 40.0f)
            return false;

        if (groupSnapshot.lowestHpPct < 60.0f)
            return false;

        uint32 const innervate = GetAppropriateRank(RestorationDruidSpells::INNERVATE, false);
        if (!innervate || innervateCooldown > 0 || me->HasAura(innervate))
            return false;

        if (!CanCast(me, innervate, true))
            return false;

        if (ExecuteSpell(me, innervate, true))
        {
            innervateCooldown = CD_INNERVATE;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P3-a: 迅捷治愈 (瞬发爆发，吞噬 HoT 换取大额直疗)
    // -------------------------------------------------------------------------
    // 雕文保证不消耗回春/愈合光环，是重伤目标最高效的瞬发救命手段。
    // 引擎侧要求目标持有回春或愈合，无跳板时施放会被底层直接拒绝。
    // =========================================================================
    bool TrySwiftmend(Unit* target)
    {
        uint32 const swiftmend = GetTalentSpell(RestorationDruidSpells::SWIFTMEND);
        if (!swiftmend || swiftmendCooldown > 0)
            return false;

        if (!target || target->ToPet() || target->GetHealthPct() >= 60.0f)
            return false;

        if (!IsValidHealTarget(target) || !HasSwiftmendFuel(target))
            return false;

        if (!CanCast(target, swiftmend, true))
            return false;

        // 瞬发爆发：允许在跑位与抱坦避难途中直接施放，无需 StopMoving
        if (ExecuteSpell(target, swiftmend, true))
        {
            swiftmendCooldown = CD_SWIFTMEND;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P3-b: 单体治疗技能执行器
    // =========================================================================
    bool TryRejuvenation(Unit* target)
    {
        uint32 const rejuvenation = GetAppropriateRank(RestorationDruidSpells::REJUVENATION, false);
        if (!rejuvenation || !target || target->ToPet() || !IsValidHealTarget(target))
            return false;

        // 光环防顶守卫：已挂回春时不再刷新，收益低于占用一个 GCD
        if (target->HasAura(rejuvenation))
            return false;

        if (!CanCast(target, rejuvenation, true))
            return false;

        return ExecuteSpell(target, rejuvenation, true);
    }

    bool TryRegrowth(Unit* target)
    {
        uint32 const regrowth = GetAppropriateRank(RestorationDruidSpells::REGROWTH, false);
        if (!regrowth || !target || target->ToPet() || !IsValidHealTarget(target))
            return false;

        // 光环防顶守卫：愈合自带 HoT，重复施放只取直接治疗部分，收益被浪费
        if (target->HasAura(regrowth))
            return false;

        // 施法资格必须先通过校验再刹停：CanCast 失败 (GCD/被控/超距) 时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(target, regrowth, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, regrowth, true);
    }

    bool TryNourish(Unit* target)
    {
        uint32 const nourish = GetAppropriateRank(RestorationDruidSpells::NOURISH, false);
        if (!nourish || !target || target->ToPet() || !IsValidHealTarget(target))
            return false;

        if (!CanCast(target, nourish, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, nourish, true);
    }

    bool TryHealingTouch(Unit* target, bool isInstant = false)
    {
        uint32 const healingTouch = GetAppropriateRank(RestorationDruidSpells::HEALING_TOUCH, false);
        if (!healingTouch || !target)
            return false;

        if (!CanCast(target, healingTouch, true))
            return false;

        // 自然迅捷使治疗之触瞬发：严禁 StopMoving，保障极限救急期的机动性。
        // (ExecuteSpell 内部亦会因 CalcCastTime 归零而跳过刹停，此处显式跳过形成双保险)
        if (!isInstant && me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, healingTouch, true);
    }

    // =========================================================================
    // P3-c: 阶梯式治疗核心 (目标：lowestHpAlly)
    // =========================================================================
    bool TryLadderHeal()
    {
        Unit* target = groupSnapshot.lowestHpAlly;
        if (!target || groupSnapshot.lowestHpPct >= 95.0f)
            return false;

        if (target->ToPet() || !IsValidHealTarget(target))
            return false;

        float const hpPct = groupSnapshot.lowestHpPct;

        // ---------------------------------------------------------------------
        // 高危重伤 (< 60%)：瞬发底牌优先，最高优先级。
        // 迅捷治愈 (瞬发大抬) -> 回春术 (瞬发止血 + 铺迅捷跳板)
        // -> 滋养 (吃 HoT 加成 1.5 秒快速抬血) -> 愈合兜底。
        // 群抬通道已被 P2 独立接管，绝不允许在单体濒死期抢走救命 GCD。
        // ---------------------------------------------------------------------
        if (hpPct < 60.0f)
        {
            // 1. 迅捷治愈：目标已有回春/愈合跳板时最高效的瞬发爆发
            if (TrySwiftmend(target)) return true;

            // 2. 缺失回春术：瞬发补挂，兼作后续迅捷治愈的跳板
            if (TryRejuvenation(target)) return true;

            // 3. 已挂 HoT：读条滋养吃满加成，是单体重伤期最高 HPS 的选择
            if (HasSwiftmendFuel(target) && TryNourish(target)) return true;

            // 4. 快速读条愈合兜底 (低等级尚无滋养时的唯一大加通道)
            if (TryRegrowth(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 中度掉血 (60% ~ 80%)：补齐双 HoT 缺口 + 滋养填充。
        // 此区间无生命危险，不占用迅捷治愈等长 CD 底牌。
        // ---------------------------------------------------------------------
        if (hpPct < 80.0f)
        {
            if (TryRejuvenation(target)) return true;
            if (TryRegrowth(target)) return true;
            if (TryNourish(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 平稳修血 (80% ~ 90%)：以回春术 HoT 覆盖受损目标，滋养收尾。
        // 此区间严禁动用高耗蓝读条群刷：对 85% 血线目标几乎全部过量。
        // ---------------------------------------------------------------------
        if (hpPct < 90.0f)
        {
            if (TryRejuvenation(target)) return true;

            // 法力节流门禁：88% 血线目标并无生命危险，在此区间投入高蓝读条
            // 会把残蓝高危期最后的法力余量烧光。残蓝 (< 35%) 时直接放行，
            // 让随从停手进入五秒规则精神回蓝通道，
            // 把法力留给真正的高压救命窗口。
            if (me->GetPowerPct(POWER_MANA) >= 35.0f && TryNourish(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 极其安全 (>= 90%)：不打高耗蓝读条，依靠已有 HoT 自行跳满，
        // 让法力进入五秒规则精神回蓝通道。
        // ---------------------------------------------------------------------
        return false;
    }

    // =========================================================================
    // P4: 驱散 (驱除诅咒 / 驱毒术)
    // -------------------------------------------------------------------------
    // 德鲁伊只能驱散诅咒与中毒两类，绝不能驱散魔法与疾病：
    // 掩码由 SPELL_EFFECT_DISPEL 效果解析得出，并在末尾与
    // (DISPEL_CURSE | DISPEL_POISON) 强制收敛，
    // 从底层杜绝任何路径引入 DISPEL_MAGIC / DISPEL_DISEASE。
    // =========================================================================
    uint32 GetDruidDispelMask(SpellInfo const* dispelInfo) const
    {
        if (!dispelInfo)
            return 0;

        uint32 mask = 0;
        for (SpellEffectInfo const& effect : dispelInfo->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_DISPEL)
                mask |= SpellInfo::GetDispelMask(static_cast<DispelType>(effect.MiscValue));
        }

        // 兜底：未显式声明 DISPEL 效果时回落到 DBC Dispel 字段
        if (!mask)
            mask = dispelInfo->GetDispelMask();

        // 硬性收敛：德鲁伊的驱散权限仅限诅咒与中毒
        mask &= SpellInfo::GetDispelMask(DISPEL_CURSE) | SpellInfo::GetDispelMask(DISPEL_POISON);

        return mask;
    }

    bool HasDispellableDebuff(Unit* unit, uint32 dispelMask) const
    {
        if (!unit || !dispelMask)
            return false;

        for (auto const& entry : unit->GetAppliedAuras())
        {
            AuraApplication* application = entry.second;
            if (!application)
                continue;

            // 严格排除正面增益：回春、愈合、兽群领袖等自身即为可驱散类型，
            // 若不过滤会陷入「驱散 -> 补 HoT -> 再驱散」的空蓝死循环。
            if (application->IsPositive())
                continue;

            Aura* aura = application->GetBase();
            if (!aura)
                continue;

            SpellInfo const* auraInfo = aura->GetSpellInfo();
            if (!auraInfo || auraInfo->Dispel == DISPEL_NONE)
                continue;

            if (dispelMask & SpellInfo::GetDispelMask(static_cast<DispelType>(auraInfo->Dispel)))
                return true;
        }

        return false;
    }

    bool TryCleanse()
    {
        if (!groupSnapshot.lowestHpAlly || groupSnapshot.lowestHpPct < 90.0f)
            return false;

        // 两个驱散法术必须独立获取。
        // 严禁使用 !dispel 级联赋值：那会在取得第一个法术 ID 后直接短路，
        // 另一个永远得不到赋值，导致只学其一等级段的随从终生无法驱散。
        uint32 const removeCurse   = GetAppropriateRank(RestorationDruidSpells::REMOVE_CURSE, false);
        uint32 const abolishPoison = GetAppropriateRank(RestorationDruidSpells::ABOLISH_POISON, false);

        if (!removeCurse && !abolishPoison)
            return false;

        SpellInfo const* removeCurseInfo   = removeCurse   ? sSpellMgr->GetSpellInfo(removeCurse)   : nullptr;
        SpellInfo const* abolishPoisonInfo = abolishPoison ? sSpellMgr->GetSpellInfo(abolishPoison) : nullptr;

        if (!removeCurseInfo && !abolishPoisonInfo)
            return false;

        uint32 const removeCurseMask   = GetDruidDispelMask(removeCurseInfo);
        uint32 const abolishPoisonMask = GetDruidDispelMask(abolishPoisonInfo);

        for (Unit* ally : groupSnapshot.allies)
        {
            if (!IsValidHealTarget(ally) || ally->ToPet())
                continue;

            // 逐目标择法：驱除诅咒 (仅诅咒) 与驱毒术 (仅中毒) 掩码互不覆盖，
            // 目标仅中诅咒而当前仅有驱毒术时 chosenDispel 保持 0 -> continue 跳过，
            // 严禁空放驱毒术浪费 GCD。
            uint32 chosenDispel = 0;
            if (removeCurse && HasDispellableDebuff(ally, removeCurseMask))
                chosenDispel = removeCurse;
            else if (abolishPoison && HasDispellableDebuff(ally, abolishPoisonMask))
                chosenDispel = abolishPoison;

            if (!chosenDispel)
                continue;

            // 单个目标施法校验失败 (超距/被卡视线) 时继续扫描其余队友，
            // 避免个别目标直接中断整轮驱散巡检。
            if (!CanCast(ally, chosenDispel, true))
                continue;

            return ExecuteSpell(ally, chosenDispel, true);
        }

        return false;
    }

    // =========================================================================
    // P5: 站位控制 (主坦后方 8 ~ 25 码，禁止进入怪物近战范围)
    // =========================================================================
    void MaintainHealerPositioning()
    {
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        Unit* anchor = groupSnapshot.mainTank;
        if (!anchor || !anchor->IsAlive())
            anchor = GetMaster();

        // 存活校验必不可少：坦克与指挥官同时阵亡时，锚点会退化为尸体，
        // 缺此校验会导致随从盲目跟随尸体移动，放弃原地自保与救援。
        if (!anchor || !anchor->IsAlive() || !anchor->IsInWorld() || anchor->GetMap() != me->GetMap())
            return;

        bool const underMeleePressure = IsUnderPhysicalMelee(me);
        float const dist = me->GetDistance(anchor);
        bool const needsReposition = underMeleePressure || dist > MAX_SAFE_DIST || dist < MIN_SAFE_DIST;

        if (!needsReposition)
            return;

        // angle 严禁自行叠加 anchor->GetOrientation()：FollowMovementGenerator 内部已自行
        // 以目标朝向为基准加算，二次叠加会让最终站位随坦克转向不断漂移。
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();

        if (underMeleePressure)
        {
            // 被贴脸时禁止原地后撤：后撤会被持续追打并拉开与坦克的距离，
            // 必须立刻贴到坦克身侧，借坦克的 AoE 仇恨把小怪拉走。
            // angle 必须传正后方：传 0.0f 会贴在坦克脸前，被顺劈斩与正面吐息一并打死。
            // 条件必须包含 !isHuggingTank：常态远程跟随同样是 FOLLOW_MOTION_TYPE，
            // 若只判 moveType 则 18 码跟随态下抱坦分支永远无法进入，随从将被贴脸至死。
            if ((!isHuggingTank || moveType != FOLLOW_MOTION_TYPE) && dist > 3.0f)
            {
                me->GetMotionMaster()->MoveFollow(anchor, 2.0f, BEHIND_ANGLE);
                isHuggingTank = true;
            }

            return;
        }

        // 仇恨威胁解除：若此前处于贴脸避难姿态，或因击退等意外落入 8 码内，
        // 必须主动重发跟随指令强制退回 18 码远程位。
        // FollowMovementGenerator 不会随距离变化自动重建，缺此步将永久停在 2 码处吃顺劈与吐息。
        // 但仅在「握手状态跃迁」或「尚未处于跟随态」时下发，否则 2 码走回 8 码途中
        // 会每帧重建路径造成抽搐。
        if (isHuggingTank || (dist < MIN_SAFE_DIST && moveType != FOLLOW_MOTION_TYPE))
        {
            isHuggingTank = false;
            me->GetMotionGroup... // placeholder
        }
    }
};
/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "RestorationDruidSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "Pet.h"
#include "Chat.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

class BotRestorationDruidAI : public AdaptiveBotAI
{
    // =========================================================================
    // 团队状态快照：单帧「打地鼠」雷达采样结果 (与 BotRestorationShamanAI 完全同构)
    // =========================================================================
    struct GroupSnapshot
    {
        std::vector<Unit*> allies;          // 40 码内、视线可达、存活的友方单位
        Unit* lowestHpAlly{ nullptr };      // 全队生命百分比最低成员 (治疗阶梯锚点，严格排除宠物)
        float lowestHpPct{ 100.0f };
        Unit* mainTank{ nullptr };          // 三花 / 回春锚点：主坦，退化时取指挥官
        float averageHpPct{ 100.0f };       // 全队平均生命百分比 (严格排除宠物)
    };

    // 站位与巡检参数
    static constexpr uint32 BUFF_SWEEP_INTERVAL = 3000;   // ms
    static constexpr float  HEAL_RANGE          = 40.0f;  // 治疗射程上限
    static constexpr float  IDEAL_FOLLOW_DIST   = 18.0f;  // 理想站位 (主坦后方)
    static constexpr float  MIN_SAFE_DIST       = 8.0f;   // 低于此距离判定为被贴脸
    static constexpr float  MAX_SAFE_DIST       = 25.0f;  // 高于此距离判定为脱节
    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 anchor->GetOrientation()，否则站位会随坦克转向持续漂移。
    // M_PI 即锚点正后方：坦克背身位，可规避顺劈斩与正面吐息。
    static constexpr float  BEHIND_ANGLE        = static_cast<float>(M_PI);

    // 自管冷却登记 (Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的
    // 瞬发 CD 技能必须由专精自行计时，否则会因 HasSpellCooldown 恒 false 而空转)
    static constexpr uint32 CD_SWIFTMEND         = 15000;
    static constexpr uint32 CD_WILD_GROWTH       = 6000;
    static constexpr uint32 CD_INNERVATE         = 180000;
    static constexpr uint32 CD_NATURES_SWIFTNESS = 120000;
    static constexpr uint32 CD_BARKSPIN          = 60000;

    // 生命之绽「三花不落」补刷窗口：叠满 3 层且剩余时间低于此值时提前续花
    static constexpr int32  LIFEBLOOM_REFRESH_WINDOW = 3000;

public:
    explicit BotRestorationDruidAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return true; }

    // 治疗随从按远程单位接管移动逻辑，禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 治疗专精不承担输出职责：压低伤害，避免继承法系远程的高额输出乘数
    float GetDamageDealtMultiplier() const override { return 0.25f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (生命之绽 / 回春术 / 愈合 / 滋养 / 治疗之触 / 激活 / 驱散)
    //     均为非天赋法术，严禁登记于此，其等级门槛由 GetAppropriateRank
    //     依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case RestorationDruidSpells::NATURES_SWIFTNESS: return 30; // 自然迅捷
            case RestorationDruidSpells::SWIFTMEND:         return 40; // 迅捷治愈
            case RestorationDruidSpells::TREE_OF_LIFE:      return 50; // 生命之树形态
            case RestorationDruidSpells::WILD_GROWTH:       return 60; // 野性成长 (恢复终极天赋)
            default:                                        return 0;
        }
    }

    // =========================================================================
    // 生命之树形态施法豁免
    // -------------------------------------------------------------------------
    // 树形态在 3.3.5a 属形态锁定状态：若不做豁免，一旦变形，全部治疗与驱散
    // 通道会被引擎的形态校验 (SpellInfo::CheckShapeshift) 拦截，
    // 随从将退化为原地发呆的木桩。
    // 仅当确实处于树形态时对正向法术 (治疗 / 驱散 / 自身增益) 放行，
    // 非形态状态与敌对法术依旧走引擎原生校验，零副作用。
    // =========================================================================
    bool CheckShapeshiftExemption(SpellInfo const* spellInfo) const override
    {
        if (AdaptiveBotAI::CheckShapeshiftExemption(spellInfo))
            return true;

        if (!spellInfo || me->GetShapeshiftForm() != FORM_TREE)
            return false;

        return spellInfo->IsPositive();
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
        isHuggingTank = false;
        ResetDruidTimers();
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
        UpdateDruidTimers(diff);

        if (buffSweepTimer <= diff)
            buffSweepTimer = BUFF_SWEEP_INTERVAL;
        else
            buffSweepTimer -= diff;

        bool const sweepDue = (buffSweepTimer == BUFF_SWEEP_INTERVAL);

        // 打地鼠雷达：每帧刷新队友状态快照
        RefreshGroupSnapshot();

        // 全局读条/通道双保险守卫：治疗之触 / 滋养 / 愈合 (读条) 期间引擎会置位
        // UNIT_STATE_CASTING，但引导类法术在部分状态下并不置位该标记，
        // 因此追加 CURRENT_CHANNELED_SPELL 显式判定，
        // 杜绝自身上层法术被跟随移动指令 (UpdateFollowMaster / MoveFollow) 掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            // 脱战残血优先救命：绝不允许形态与长效 HoT 抢占急救 GCD
            if (groupSnapshot.lowestHpPct < 90.0f)
            {
                if (TryLadderHeal()) return;
            }

            if (sweepDue)
            {
                if (MaintainTreeOfLife()) return;
                if (MaintainTankHoTs()) return;
            }

            if (TryLadderHeal()) return;

            UpdateFollowMaster(diff);
            return;
        }

        // =====================================================================
        // 2. 战斗内 APL
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (victim && victim->IsAlive() && victim->GetMap() == me->GetMap() && me->GetVictim() != victim)
            me->Attack(victim, false); // 仅锚定敌对目标维持进战姿态，绝不开启近战追击

        // ---- P0: 极限急救 (树皮自保 + 自然迅捷瞬发治疗之触) ----
        if (TryEmergencyHeals()) return;

        // ---- P1: 生命之树形态维持与主坦「三花聚顶」滚动 ----
        if (sweepDue)
        {
            if (MaintainTreeOfLife()) return;
            if (MaintainTankHoTs()) return;
        }

        // ---- P2: 团队群抬 (野性成长) 与续航 (激活) ----
        if (TryWildGrowth()) return;
        if (MaintainInnervate()) return;

        // ---- P3: 阶梯式直接治疗与迅捷治愈爆发 ----
        if (TryLadderHeal()) return;

        // ---- P4: 驱散 ----
        if (TryCleanse()) return;

        // ---- P5: 跟随与站位控制 ----
        MaintainHealerPositioning();
    }

private:
    GroupSnapshot groupSnapshot;
    uint32 buffSweepTimer{ 0 };

    // 贴脸避难状态标记：记录当前是否处于「紧抱坦克身侧」的应急姿态。
    // 仇恨解除后必须凭此标记主动重发跟随指令，否则会永久粘在坦克身后吃顺劈与吐息。
    bool isHuggingTank{ false };

    // 自管冷却计时
    uint32 swiftmendCooldown{ 0 };
    uint32 wildGrowthCooldown{ 0 };
    uint32 innervateCooldown{ 0 };
    uint32 naturesSwiftnessCooldown{ 0 };
    uint32 barkspinCooldown{ 0 };

    // =========================================================================
    // 天赋法术统一取用闸门
    // -------------------------------------------------------------------------
    // 基类 GetAppropriateRank(id, true) 为「Rank 1 保底」语义，会跳过
    // GetTalentSpellMinLevel 的等级校验；且 3.3.5a 天赋法术 DBC SpellLevel
    // 多为 0，降阶链无从回落，低等级随从会把高阶天赋 (如 60 级野性成长)
    // 当基础技能空放。
    // 故所有天赋法术统一经此闸门取用：未达登记解锁等级一律返回 0，
    // 由 APL 自然降级到基础治疗通道 (与基类契约语义完全一致)。
    // =========================================================================
    uint32 GetTalentSpell(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        if (minLevel > 0 && me->GetLevel() < minLevel)
            return 0;

        return GetAppropriateRank(spellId, true);
    }

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateDruidTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(swiftmendCooldown);
        Tick(wildGrowthCooldown);
        Tick(innervateCooldown);
        Tick(naturesSwiftnessCooldown);
        Tick(barkspinCooldown);
    }

    void ResetDruidTimers()
    {
        swiftmendCooldown = 0;
        wildGrowthCooldown = 0;
        innervateCooldown = 0;
        naturesSwiftnessCooldown = 0;
        barkspinCooldown = 0;
    }

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

        // HoT 锚点：优先主坦，无主坦时退化为指挥官；阵亡则回落指挥官
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

        // 宠物保留在 allies 名单内 (以便享受野性成长等溅射辅助)，
        // 但严禁用宠物血线触发全队恐慌：否则猎人宠物掉血会把 lowestHpAlly
        // 绑架到宠物身上，导致三花、回春、滋养全部错失真正的玩家与坦克。
        bool const isPet = (unit->ToPet() != nullptr);
        if (!isPet)
        {
            float const hp = unit->GetHealthPct();
            if (hp < snap.lowestHpPct)
            {
                snap.lowestHpPct = hp;
                snap.lowestHpAlly = unit;
            }
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

        // 兜底锚点同样必须过滤宠物：唯一可见目标恰为宠物时，
        // 直接取 front() 会把治疗锚点重新交还给宠物，抵消最低血线的过滤。
        if (!snap.lowestHpAlly)
        {
            for (Unit* ally : snap.allies)
            {
                if (ally && !ally->ToPet())
                {
                    snap.lowestHpAlly = ally;
                    snap.lowestHpPct = ally->GetHealthPct();
                    break;
                }
            }
        }

        // 平均血线剔除宠物：宠物残血会把平均值拉低，
        // 误触发「团队崩溃保护」从而永久锁死野性成长与激活的判定。
        float total = 0.0f;
        uint32 validCount = 0;
        for (Unit* ally : snap.allies)
        {
            if (ally && !ally->ToPet())
            {
                total += ally->GetHealthPct();
                ++validCount;
            }
        }

        snap.averageHpPct = (validCount > 0) ? (total / static_cast<float>(validCount)) : 100.0f;
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

    // 目标是否已挂可驱动迅捷治愈 / 滋养加成的核心 HoT (回春 / 愈合)
    bool HasSwiftmendFuel(Unit* target) const
    {
        if (!target)
            return false;

        uint32 const rejuvenation = GetAppropriateRank(RestorationDruidSpells::REJUVENATION, false);
        if (rejuvenation && target->HasAura(rejuvenation))
            return true;

        uint32 const regrowth = GetAppropriateRank(RestorationDruidSpells::REGROWTH, false);
        if (regrowth && target->HasAura(regrowth))
            return true;

        return false;
    }

    // =========================================================================
    // P0: 极限急救 (树皮自保 + 自然迅捷瞬发治疗之触)
    // =========================================================================
    Unit* SelectEmergencyTarget() const
    {
        // 优先自身濒死：治疗随从阵亡等于全队断奶
        if (me->GetHealthPct() < 35.0f && IsValidHealTarget(me))
            return me;

        // 主坦濒死：承伤核心，倒地即团灭
        if (groupSnapshot.mainTank && groupSnapshot.mainTank->IsAlive() &&
            !groupSnapshot.mainTank->ToPet() &&
            groupSnapshot.mainTank->GetHealthPct() < 30.0f &&
            IsValidHealTarget(groupSnapshot.mainTank))
        {
            return groupSnapshot.mainTank;
        }

        // 全队最低血线濒死
        if (groupSnapshot.lowestHpAlly && groupSnapshot.lowestHpAlly->IsAlive() &&
            !groupSnapshot.lowestHpAlly->ToPet() &&
            groupSnapshot.lowestHpPct < 30.0f &&
            IsValidHealTarget(groupSnapshot.lowestHpAlly))
        {
            return groupSnapshot.lowestHpAlly;
        }

        return nullptr;
    }

    bool TryEmergencyHeals()
    {
        // ---------------------------------------------------------------------
        // 情形零：树皮术自保 (瞬发 20% 减伤)
        // 自身承压且被物理贴脸时优先解锁减伤，避免治疗随从被近战秒杀导致断奶。
        // 自保增益不得吞掉当帧急救：施放后不 return，继续评估自然迅捷大加。
        // ---------------------------------------------------------------------
        if (me->GetHealthPct() < 40.0f && IsUnderPhysicalMelee(me))
        {
            uint32 const barkskin = GetAppropriateRank(RestorationDruidSpells::BARKSPIN, false);
            if (barkskin && barkspinCooldown == 0 && !me->HasAura(barkskin))
            {
                if (CanCast(me, barkskin, true) && ExecuteSpell(me, barkskin, true))
                    barkspinCooldown = CD_BARKSPIN;
            }
        }

        // ---------------------------------------------------------------------
        // 情形一 (最高优先级)：自然迅捷光环已就绪 —— 必须当帧兑现，
        // 严禁把这张瞬发大加留到 P3 被低级治疗法术误吞。
        // 自然迅捷光环仅短时存在 (10s 窗口)，若仍先走 SelectEmergencyTarget()
        // 的严格濒死门禁 (< 30%)，目标血线恰在 P3 前被抬离阈值就会失去目标；
        // 光环随即被 P3 中任何一发自然系法术 (回春 / 愈合) 消耗掉，
        // 2 分钟底牌被 1.5 秒小加白白吞没。
        // 因此只要光环存在且仍有任何受损目标，就当帧施放瞬发治疗之触兑现。
        // ---------------------------------------------------------------------
        if (me->HasAura(RestorationDruidSpells::NATURES_SWIFTNESS))
        {
            Unit* swiftTarget = SelectEmergencyTarget();

            // 降级锚点：无严格濒死目标时，只要全队仍有受损成员即兑现，
            // 避免光环在 10 秒窗口内白白过期。
            if (!swiftTarget &&
                groupSnapshot.lowestHpAlly && !groupSnapshot.lowestHpAlly->ToPet() &&
                groupSnapshot.lowestHpPct < 90.0f &&
                IsValidHealTarget(groupSnapshot.lowestHpAlly))
            {
                swiftTarget = groupSnapshot.lowestHpAlly;
            }

            if (swiftTarget)
                return TryHealingTouch(swiftTarget, true);
        }

        // 情形二：光环未激活，尝试主动开启自然迅捷 (Off-GCD，2 分钟自家冷却)
        Unit* target = SelectEmergencyTarget();
        if (!target)
            return false;

        uint32 const naturesSwiftness = GetTalentSpell(RestorationDruidSpells::NATURES_SWIFTNESS);
        if (!naturesSwiftness || naturesSwiftnessCooldown > 0)
            return false;

        if (!CanCast(me, naturesSwiftness, true) || !ExecuteSpell(me, naturesSwiftness, true))
            return false;

        naturesSwiftnessCooldown = CD_NATURES_SWIFTNESS;

        // Off-GCD 铁律：自然迅捷不占公共冷却，严禁在此无条件 return true。
        // 必须允许当帧决策流顺下，让自然迅捷光环当帧立即被治疗之触吃下；
        // 若光环尚未可见 (极端情形)，则返回 false 交由下一帧「情形一」兜底。
        if (me->HasAura(RestorationDruidSpells::NATURES_SWIFTNESS))
            return TryHealingTouch(target, true);

        return false;
    }

    // =========================================================================
    // P1-a: 生命之树形态维持 (50 级天赋)
    // -------------------------------------------------------------------------
    // 树形态提升治疗受疗与护甲，是恢复德长期收益最高的形态开关。
    // 但变形占用一次 GCD，战时全队承压时必须无条件让渡给治疗通道。
    // =========================================================================
    bool MaintainTreeOfLife()
    {
        uint32 const treeForm = GetTalentSpell(RestorationDruidSpells::TREE_OF_LIFE);
        if (!treeForm)
            return false;

        // 已处于树形态 (或光环生效中)：无需重复变形
        if (me->GetShapeshiftForm() == FORM_TREE || me->HasAura(treeForm))
            return false;

        // 战时节流门禁：全队脱离承压线后才允许变形维持
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 80.0f)
            return false;

        if (!CanCast(me, treeForm, true))
            return false;

        return ExecuteSpell(me, treeForm, true);
    }

    // =========================================================================
    // P1-b: 主坦「三花聚顶」与常驻回春滚动
    // -------------------------------------------------------------------------
    // 生命之绽按层叠加，满 3 层收益最高；提前续花可保住层数不掉。
    // 回春术作为迅捷治愈的跳板与常驻减伤，必须时刻在手。
    // =========================================================================
    bool MaintainTankHoTs()
    {
        Unit* tank = groupSnapshot.mainTank;

        // 排己判定 (致命)：无独立主坦时锚点会退化为指挥官或自身，
        // 随从不承担坦克职责，严禁对自身滚动三花白白烧蓝。
        if (!tank || tank == me || !tank->IsAlive() || tank->ToPet() || !IsValidHealTarget(tank))
            return false;

        // 战时节流门禁：全队重伤时补长效 HoT 的 GCD 必须让渡给急救通道
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 65.0f)
            return false;

        // ---- 生命之绽「三花不落」滚动维持 ----
        uint32 const lifebloom = GetAppropriateRank(RestorationDruidSpells::LIFEBLOOM, false);
        if (lifebloom)
        {
            Aura* bloom = tank->GetAura(lifebloom);

            // 层数检测必须走 Aura::GetStackAmount()：可叠加光环在底层仅有一个
            // AuraApplication 实例，GetAuraCount() 恒返回 1，用其判层会使本分支
            // 彻底沦为死代码，三花永远只挂 1 层。
            // 剩余时间检测排除 <= 0 的永久光环 (-1)，避免无意义刷新。
            bool const needRefresh = !bloom
                                  || bloom->GetStackAmount() < 3
                                  || (bloom->GetDuration() > 0 && bloom->GetDuration() < LIFEBLOOM_REFRESH_WINDOW);

            if (needRefresh)
            {
                // 瞬发 HoT：允许在跑位途中直接施放，无需 StopMoving
                if (CanCast(tank, lifebloom, true) && ExecuteSpell(tank, lifebloom, true))
                    return true;
            }
        }

        // ---- 主坦常驻回春术 ----
        uint32 const rejuvenation = GetAppropriateRank(RestorationDruidSpells::REJUVENATION, false);
        if (rejuvenation && !tank->HasAura(rejuvenation))
        {
            if (CanCast(tank, rejuvenation, true) && ExecuteSpell(tank, rejuvenation, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P2-a: 团队智能群抬 (野性成长)
    // -------------------------------------------------------------------------
    // 以 lowestHpAlly 为锚点：野性成长会自动向附近最需要治疗的队友逐跳扩散，
    // 在 2 人以上同时掉血时单发瞬发即可替代多次单体治疗，
    // 且享受雕文加成 (目标数量 +1，达 6 目标)。
    // 严禁在仅 1 人掉血时施放：收益低于单体直疗，纯粹浪费 6 秒 CD。
    // =========================================================================
    bool TryWildGrowth()
    {
        // 门禁一：无人处于濒死猝死线 (< 55%)
        // 群抬虽为瞬发，但优先级低于单体急救，重伤期必须让渡给 P0/P3 单体通道。
        if (groupSnapshot.lowestHpPct < 55.0f)
            return false;

        uint32 const wildGrowth = GetTalentSpell(RestorationDruidSpells::WILD_GROWTH);
        if (!wildGrowth || wildGrowthCooldown > 0)
            return false;

        // 门禁二：群抬收益判定，至少 2 名非宠物成员低于 85% 血线
        uint32 injuredCount = 0;
        for (Unit* ally : groupSnapshot.allies)
        {
            if (!ally || ally->ToPet())
                continue;

            if (!IsValidHealTarget(ally))
                continue;

            if (ally->GetHealthPct() < 85.0f)
                ++injuredCount;
        }

        if (injuredCount < 2)
            return false;

        Unit* anchor = groupSnapshot.lowestHpAlly;
        if (!anchor || anchor->ToPet() || !IsValidHealTarget(anchor))
            return false;

        if (!CanCast(anchor, wildGrowth, true))
            return false;

        // 瞬发群抬：允许在跑位与抱坦避难途中直接施放，无需 StopMoving
        if (ExecuteSpell(anchor, wildGrowth, true))
        {
            wildGrowthCooldown = CD_WILD_GROWTH;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P2-b: 续航仲裁 (激活)
    // -------------------------------------------------------------------------
    // 激活为自身高额回蓝底牌 (3 分钟 CD)。
    // 血线安全门禁必不可少：治疗随从残蓝但队友正在挨打时，
    // 回蓝的 GCD 必须无条件让渡给救命通道，回蓝留待平稳期补足。
    // =========================================================================
    bool MaintainInnervate()
    {
        if (!me->IsInCombat())
            return false;

        if (me->getPowerType() != POWER_MANA)
            return false;

        if (me->GetPowerPct(POWER_MANA) >= 40.0f)
            return false;

        if (groupSnapshot.lowestHpPct < 60.0f)
            return false;

        uint32 const innervate = GetAppropriateRank(RestorationDruidSpells::INNERVATE, false);
        if (!innervate || innervateCooldown > 0 || me->HasAura(innervate))
            return false;

        if (!CanCast(me, innervate, true))
            return false;

        if (ExecuteSpell(me, innervate, true))
        {
            innervateCooldown = CD_INNERVATE;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P3-a: 迅捷治愈 (瞬发爆发)
    // -------------------------------------------------------------------------
    // 雕文保证不消耗回春/愈合光环，是重伤目标最高效的瞬发救命手段。
    // 引擎侧要求目标持有回春或愈合，无跳板时施放会被底层直接拒绝。
    // =========================================================================
    bool TrySwiftmend(Unit* target)
    {
        uint32 const swiftmend = GetTalentSpell(RestorationDruidSpells::SWIFTMEND);
        if (!swiftmend || swiftmendCooldown > 0)
            return false;

        if (!target || target->ToPet() || target->GetHealthPct() >= 60.0f)
            return false;

        if (!IsValidHealTarget(target) || !HasSwiftmendFuel(target))
            return false;

        if (!CanCast(target, swiftmend, true))
            return false;

        // 瞬发爆发：允许在跑位与抱坦避难途中直接施放，无需 StopMoving
        if (ExecuteSpell(target, swiftmend, true))
        {
            swiftmendCooldown = CD_SWIFTMEND;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P3-b: 单体治疗技能执行器
    // =========================================================================
    bool TryRejuvenation(Unit* target)
    {
        uint32 const rejuvenation = GetAppropriateRank(RestorationDruidSpells::REJUVENATION, false);
        if (!rejuvenation || !target || target->ToPet() || !IsValidHealTarget(target))
            return false;

        // 光环防顶守卫：已挂回春时不再刷新，收益低于占用一个 GCD
        if (target->HasAura(rejuvenation))
            return false;

        if (!CanCast(target, rejuvenation, true))
            return false;

        // 瞬发 HoT：允许在跑位与抱坦避难途中直接施放，无需 StopMoving
        return ExecuteSpell(target, rejuvenation, true);
    }

    bool TryRegrowth(Unit* target)
    {
        uint32 const regrowth = GetAppropriateRank(RestorationDruidSpells::REGROWTH, false);
        if (!regrowth || !target || target->ToPet() || !IsValidHealTarget(target))
            return false;

        // 光环防顶守卫：愈合自带 HoT，重复施放只取直接治疗部分，收益被浪费
        if (target->HasAura(regrowth))
            return false;

        // 施法资格必须先通过校验再刹停：CanCast 失败 (GCD/被控/超距) 时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(target, regrowth, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, regrowth, true);
    }

    bool TryNourish(Unit* target)
    {
        uint32 const nourish = GetAppropriateRank(RestorationDruidSpells::NOURISH, false);
        if (!nourish || !target || target->ToPet() || !IsValidHealTarget(target))
            return false;

        if (!CanCast(target, nourish, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, nourish, true);
    }

    bool TryHealingTouch(Unit* target, bool isInstant = false)
    {
        uint32 const healingTouch = GetAppropriateRank(RestorationDruidSpells::HEALING_TOUCH, false);
        if (!healingTouch || !target)
            return false;

        if (!CanCast(target, healingTouch, true))
            return false;

        // 自然迅捷使治疗之触瞬发：严禁 StopMoving，保障极限救急期的机动性。
        // (ExecuteSpell 内部亦会因 CalcCastTime 归零而跳过刹停，此处显式跳过形成双保险)
        if (!isInstant && me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, healingTouch, true);
    }

    // =========================================================================
    // P3-c: 阶梯式治疗核心 (目标：lowestHpAlly)
    // =========================================================================
    bool TryLadderHeal()
    {
        Unit* target = groupSnapshot.lowestHpAlly;
        if (!target || groupSnapshot.lowestHpPct >= 95.0f)
            return false;

        if (target->ToPet() || !IsValidHealTarget(target))
            return false;

        float const hpPct = groupSnapshot.lowestHpPct;

        // ---------------------------------------------------------------------
        // 高危重伤 (< 60%)：瞬发底牌优先，最高优先级。
        // 迅捷治愈 (瞬发大抬) -> 回春术 (瞬发止血 + 铺迅捷跳板)
        // -> 滋养 (吃 HoT 加成快速抬血) -> 愈合兜底。
        // 群抬通道已被 P2 独立接管，绝不允许在单体濒死期抢走救命 GCD。
        // ---------------------------------------------------------------------
        if (hpPct < 60.0f)
        {
            // 1. 迅捷治愈：目标已有回春/愈合跳板时最高效的瞬发爆发
            if (TrySwiftmend(target)) return true;

            // 2. 缺失回春术：瞬发补挂，兼作后续迅捷治愈的跳板
            if (TryRejuvenation(target)) return true;

            // 3. 已挂 HoT：读条滋养吃满加成，是单体重伤期最高 HPS 的选择
            if (HasSwiftmendFuel(target) && TryNourish(target)) return true;

            // 4. 快速读条愈合兜底 (低等级尚无滋养时的唯一大加通道)
            if (TryRegrowth(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 中度掉血 (60% ~ 80%)：补齐双 HoT 缺口 + 滋养填充。
        // 此区间无生命危险，不占用迅捷治愈等长 CD 底牌。
        // ---------------------------------------------------------------------
        if (hpPct < 80.0f)
        {
            if (TryRejuvenation(target)) return true;
            if (TryRegrowth(target)) return true;
            if (TryNourish(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 平稳修血 (80% ~ 90%)：以回春术 HoT 覆盖受损目标，滋养收尾。
        // 此区间严禁动用高耗蓝读条大加：对 85% 血线目标几乎全部过量。
        // ---------------------------------------------------------------------
        if (hpPct < 90.0f)
        {
            if (TryRejuvenation(target)) return true;

            // 法力节流门禁：88% 血线目标并无生命危险，在此区间投入高蓝读条
            // 会把残蓝高危期最后的法力余量烧光。残蓝 (< 35%) 时直接放行，
            // 让随从停手进入五秒规则精神回蓝通道，
            // 把法力留给真正的高压救命窗口。
            if (me->GetPowerPct(POWER_MANA) >= 35.0f && TryNourish(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 极其安全 (>= 90%)：不打高耗蓝读条，依靠已有 HoT 自行跳满，
        // 让法力进入五秒规则精神回蓝通道。
        // ---------------------------------------------------------------------
        return false;
    }

    // =========================================================================
    // P4: 驱散 (驱除诅咒 / 驱毒术)
    // -------------------------------------------------------------------------
    // 德鲁伊只能驱散诅咒与中毒两类，绝不能驱散魔法与疾病：
    // 掩码由 SPELL_EFFECT_DISPEL 效果解析得出，并在末尾与
    // (DISPEL_CURSE | DISPEL_POISON) 强制收敛，
    // 从底层杜绝任何路径引入 DISPEL_MAGIC / DISPEL_DISEASE。
    // =========================================================================
    uint32 GetDruidDispelMask(SpellInfo const* dispelInfo) const
    {
        if (!dispelInfo)
            return 0;

        uint32 mask = 0;
        for (SpellEffectInfo const& effect : dispelInfo->GetEffects())
        {
            if (effect.Effect == SPELL_EFFECT_DISPEL)
                mask |= SpellInfo::GetDispelMask(static_cast<DispelType>(effect.MiscValue));
        }

        // 兜底：未显式声明 DISPEL 效果时回落到 DBC Dispel 字段
        if (!mask)
            mask = dispelInfo->GetDispelMask();

        // 硬性收敛：德鲁伊的驱散权限仅限诅咒与中毒
        mask &= SpellInfo::GetDispelMask(DISPEL_CURSE) | SpellInfo::GetDispelMask(DISPEL_POISON);

        return mask;
    }

    bool HasDispellableDebuff(Unit* unit, uint32 dispelMask) const
    {
        if (!unit || !dispelMask)
            return false;

        for (auto const& entry : unit->GetAppliedAuras())
        {
            AuraApplication* application = entry.second;
            if (!application)
                continue;

            // 严格排除正面增益：回春、愈合等自身即为可驱散类型，
            // 若不过滤会陷入「驱散 -> 补 HoT -> 再驱散」的空蓝死循环。
            if (application->IsPositive())
                continue;

            Aura* aura = application->GetBase();
            if (!aura)
                continue;

            SpellInfo const* auraInfo = aura->GetSpellInfo();
            if (!auraInfo || auraInfo->Dispel == DISPEL_NONE)
                continue;

            if (dispelMask & SpellInfo::GetDispelMask(static_cast<DispelType>(auraInfo->Dispel)))
                return true;
        }

        return false;
    }

    bool TryCleanse()
    {
        if (!groupSnapshot.lowestHpAlly || groupSnapshot.lowestHpPct < 90.0f)
            return false;

        // 两个驱散法术必须独立获取。
        // 严禁使用 !dispel 级联赋值：那会在取得第一个法术 ID 后直接短路，
        // 另一个永远得不到赋值，导致只学其一等级段的随从终生无法驱散。
        uint32 const removeCurse   = GetAppropriateRank(RestorationDruidSpells::REMOVE_CURSE, false);
        uint32 const abolishPoison = GetAppropriateRank(RestorationDruidSpells::ABOLISH_POISON, false);

        if (!removeCurse && !abolishPoison)
            return false;

        SpellInfo const* removeCurseInfo   = removeCurse   ? sSpellMgr->GetSpellInfo(removeCurse)   : nullptr;
        SpellInfo const* abolishPoisonInfo = abolishPoison ? sSpellMgr->GetSpellInfo(abolishPoison) : nullptr;

        if (!removeCurseInfo && !abolishPoisonInfo)
            return false;

        uint32 const removeCurseMask   = GetDruidDispelMask(removeCurseInfo);
        uint32 const abolishPoisonMask = GetDruidDispelMask(abolishPoisonInfo);

        for (Unit* ally : groupSnapshot.allies)
        {
            if (!IsValidHealTarget(ally) || ally->ToPet())
                continue;

            // 逐目标择法：驱除诅咒 (仅诅咒) 与驱毒术 (仅中毒) 掩码互不覆盖，
            // 目标仅中诅咒而当前仅有驱毒术时 chosenDispel 保持 0 -> continue 跳过，
            // 严禁空放驱毒术浪费 GCD。
            uint32 chosenDispel = 0;
            if (removeCurse && HasDispellableDebuff(ally, removeCurseMask))
                chosenDispel = removeCurse;
            else if (abolishPoison && HasDispellableDebuff(ally, abolishPoisonMask))
                chosenDispel = abolishPoison;

            if (!chosenDispel)
                continue;

            // 单个目标施法校验失败 (超距/被卡视线) 时继续扫描其余队友，
            // 避免个别目标直接中断整轮驱散巡检。
            if (!CanCast(ally, chosenDispel, true))
                continue;

            return ExecuteSpell(ally, chosenDispel, true);
        }

        return false;
    }

    // =========================================================================
    // P5: 站位控制 (主坦后方 8 ~ 25 码，禁止进入怪物近战范围)
    // =========================================================================
    void MaintainHealerPositioning()
    {
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        Unit* anchor = groupSnapshot.mainTank;
        if (!anchor || !anchor->IsAlive())
            anchor = GetMaster();

        // 存活校验必不可少：坦克与指挥官同时阵亡时，锚点会退化为尸体，
        // 缺此校验会导致随从盲目跟随尸体移动，放弃原地自保与救援。
        if (!anchor || !anchor->IsAlive() || !anchor->IsInWorld() || anchor->GetMap() != me->GetMap())
            return;

        bool const underMeleePressure = IsUnderPhysicalMelee(me);
        float const dist = me->GetDistance(anchor);
        bool const needsReposition = underMeleePressure || dist > MAX_SAFE_DIST || dist < MIN_SAFE_DIST;

        if (!needsReposition)
            return;

        // angle 严禁自行叠加 anchor->GetOrientation()：FollowMovementGenerator 内部已自行
        // 以目标朝向为基准加算，二次叠加会让最终站位随坦克转向不断漂移。
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();

        if (underMeleePressure)
        {
            // 被贴脸时禁止原地后撤：后撤会被持续追打并拉开与坦克的距离，
            // 必须立刻贴到坦克身侧，借坦克的 AoE 仇恨把小怪拉走。
            // angle 必须传正后方：传 0.0f 会贴在坦克脸前，被顺劈斩与正面吐息一并打死。
            // 条件必须包含 !isHuggingTank：常态远程跟随同样是 FOLLOW_MOTION_TYPE，
            // 若只判 moveType 则 18 码跟随态下抱坦分支永远无法进入，随从将被贴脸至死。
            if ((!isHuggingTank || moveType != FOLLOW_MOTION_TYPE) && dist > 3.0f)
            {
                me->GetMotionMaster()->MoveFollow(anchor, 2.0f, BEHIND_ANGLE);
                isHuggingTank = true;
            }

            return;
        }

        // 仇恨威胁解除：若此前处于贴脸避难姿态，或因击退等意外落入 8 码内，
        // 必须主动重发跟随指令强制退回 18 码远程位。
        // FollowMovementGenerator 不会随距离变化自动重建，缺此步将永久停在 2 码处吃顺劈与吐息。
        // 但仅在「握手状态跃迁」或「尚未处于跟随态」时下发，否则 2 码走回 8 码途中
        // 会每帧重建路径造成抽搐。
        if (isHuggingTank || (dist < MIN_SAFE_DIST && moveType != FOLLOW_MOTION_TYPE))
        {
            isHuggingTank = false;
            me->GetMotionMaster()->MoveFollow(anchor, IDEAL_FOLLOW_DIST, BEHIND_ANGLE);
            return;
        }

        if (moveType != FOLLOW_MOTION_TYPE)
            me->GetMotionMaster()->MoveFollow(anchor, IDEAL_FOLLOW_DIST, BEHIND_ANGLE);
    }

    // =========================================================================
    // 恢复天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
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

        SyncPassive(20, RestorationDruidSpells::GLYPH_OF_SWIFTMEND);    // 迅捷治愈雕文：迅捷治愈不再吞噬回春/愈合 (核心必带)
        SyncPassive(20, RestorationDruidSpells::GLYPH_OF_RAPID_REJUV);  // 快速回春雕文：急速使回春跳得更快
        SyncPassive(30, RestorationDruidSpells::MASTER_SHAPESHIFTER);   // 兽性大师：树形态治疗 +4%
        SyncPassive(30, RestorationDruidSpells::EMPOWERED_REJUV);       // 强化回春术：HoT 效果 +10%
        SyncPassive(35, RestorationDruidSpells::EMPOWERED_TOUCH);       // 强化之触：滋养增效
        SyncPassive(40, RestorationDruidSpells::GIFT_OF_EARTHMOTHER);   // 大地母亲的赐福：法术急速
        SyncPassive(60, RestorationDruidSpells::GLYPH_OF_WILD_GROWTH);  // 野性成长雕文：目标数量 +1 (达 6 目标)
    }
};

void AddSC_bot_restoration_druid()
{
    new AdaptiveBotScript<BotRestorationDruidAI>("bot_restoration_druid");
}
