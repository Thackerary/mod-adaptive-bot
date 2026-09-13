/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "DisciplinePriestSpells.h"
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

class BotDisciplinePriestAI : public AdaptiveBotAI
{
    // =========================================================================
    // 团队状态快照：单帧「打地鼠」雷达采样结果
    // =========================================================================
    struct GroupSnapshot
    {
        std::vector<Unit*> allies;          // 40 码内、视线可达、存活的友方单位
        Unit* lowestHpAlly{ nullptr };      // 全队生命百分比最低成员 (治疗阶梯锚点)
        float lowestHpPct{ 100.0f };
        Unit* mainTank{ nullptr };          // 盾预铺/减伤锚点：主坦，退化时取指挥官
        float averageHpPct{ 100.0f };       // 全队平均生命百分比 (续航仲裁依据)
    };

    // 站位与巡检参数
    static constexpr uint32 BUFF_SWEEP_INTERVAL = 3000;   // ms
    static constexpr float  HEAL_RANGE          = 40.0f;  // 治疗射程上限
    static constexpr float  IDEAL_FOLLOW_DIST   = 18.0f;  // 理想站位 (主坦后方)
    static constexpr float  MIN_SAFE_DIST       = 8.0f;   // 低于此距离判定为被贴脸
    static constexpr float  MAX_SAFE_DIST       = 25.0f;  // 高于此距离判定为脱节
    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，内部已叠加目标朝向。
    // 严禁自行叠加 anchor->GetOrientation()，否则站位会随坦克转向持续漂移。
    // M_PI 即锚点正后方：坦克背身位，可规避顺劈斩与正面吐息
    static constexpr float  BEHIND_ANGLE        = static_cast<float>(M_PI);

public:
    explicit BotDisciplinePriestAI(Creature* creature) : AdaptiveBotAI(creature) {}

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
            case DisciplinePriestSpells::INNER_FOCUS:      return 20; // 心灵专注
            case DisciplinePriestSpells::POWER_INFUSION:   return 40; // 能量注入
            case DisciplinePriestSpells::PAIN_SUPPRESSION: return 50; // 痛苦压制
            case DisciplinePriestSpells::SHADOWFIEND:      return 50; // 暗影魔
            case DisciplinePriestSpells::PENANCE:          return 60; // 苦修 (戒律顶层天赋)
            default:                                       return 0;
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
        isHuggingTank = false;
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

        // 全局读条/通道守卫：快速治疗 (读条) 与苦修 (通道) 期间引擎均会置位
        // UNIT_STATE_CASTING，此处冻结一切决策，杜绝被自身跟随移动指令
        // (UpdateFollowMaster / MoveFollow) 掐断读条与通道。
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            // 脱战残血优先救命：绝不允许常驻增益与盾预铺抢占急救 GCD
            if (groupSnapshot.lowestHpPct < 90.0f)
            {
                if (TryLadderHeal()) return;
            }

            if (sweepDue)
            {
                if (MaintainInnerFire()) return;
                if (MaintainTeamBuffs()) return;
            }

            // 脱战是全团预铺真言术：盾的最佳窗口：无 GCD 竞争、无读条被打断风险
            if (TryPreShield()) return;

            if (TryLadderHeal()) return;

            UpdateFollowMaster(diff);
            return;
        }

        // =====================================================================
        // 2. 战斗内 APL
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (victim && victim->IsAlive() && victim->GetMap() == me->GetMap() && me->GetVictim() != victim)
            me->Attack(victim, false); // 仅锚定敌对目标供暗影魔使用，绝不开启近战追击

        // ---- P0: 自保与极限减伤急救 ----
        if (TrySelfPreservation()) return;
        if (TryEmergencyHeals()) return;

        // ---- P1: 常驻增益与真言术：盾预铺 ----
        if (sweepDue)
        {
            if (MaintainInnerFire()) return;
            if (MaintainTeamBuffs()) return;
        }

        if (TryPreShield()) return;

        // ---- P2: 续航仲裁 ----
        if (MaintainManaCooldowns(victim)) return;

        // ---- P3: 阶梯式治疗核心 ----
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
    // 仇恨解除后必须凭此标记主动重发跟随指令，否则戒律牧会永久粘在坦克身后吃顺劈与吐息
    bool isHuggingTank{ false };

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

        // 盾/减伤锚点：优先主坦，无主坦时退化为指挥官；阵亡则回落指挥官
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

        // 宠物保留在 allies 名单内 (以便享受治疗祷言/群体驱散等溅射辅助)，
        // 但严禁用宠物血线触发全队恐慌：否则猎人宠物掉血会把 lowestHpAlly
        // 绑架到宠物身上，导致痛苦压制、盾预铺、恢复全部错失真正的玩家与坦克。
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
        // 直接取 front() 会把治疗锚点重新交还给宠物，抵消最低血线的过滤
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

        // 平均血线剔除宠物：猎人/术士宠物残血会把平均值拉低，
        // 误触发「团队崩溃保护」从而永久锁死能量注入与暗影魔的续航判定
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

    bool HasDispellableDebuff(Unit* unit, SpellInfo const* dispelInfo) const
    {
        if (!unit || !dispelInfo)
            return false;

        for (auto const& entry : unit->GetAppliedAuras())
        {
            AuraApplication* application = entry.second;
            if (!application)
                continue;

            // 严格排除正面增益：真言术：韧、神圣之灵、心灵之火等自身即为 DISPEL_MAGIC，
            // 若不过滤会陷入「驱散 -> 补 buff -> 再驱散」的空蓝死循环
            if (application->IsPositive())
                continue;

            Aura* aura = application->GetBase();
            if (!aura)
                continue;

            SpellInfo const* auraInfo = aura->GetSpellInfo();
            if (auraInfo && auraInfo->Dispel != DISPEL_NONE && dispelInfo->CanDispelAura(auraInfo))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P0-1: 自保与脱身
    // =========================================================================
    bool TrySelfPreservation()
    {
        // 被物理近战贴身且血线不稳：渐隐降低仇恨，把仇恨交还坦克，
        // 从根源上解除被持续追打的死循环 (P5 的抱坦走位只是治标)
        if (IsUnderPhysicalMelee(me) && me->GetHealthPct() < 60.0f)
        {
            uint32 const fade = GetAppropriateRank(DisciplinePriestSpells::FADE, false);
            if (fade && !me->HasAura(fade) && CanCast(me, fade, true) && ExecuteSpell(me, fade, true))
                return true;
        }

        if (me->GetHealthPct() >= 45.0f)
            return false;

        // 自身濒死：先瞬发自盾 (吸收) 争取生存窗口，再交给阶梯治疗补血。
        // 必须先校验灵魂虚弱，否则被灵魂虚弱压制期间会永久空转卡死这一层
        uint32 const shield = GetAppropriateRank(DisciplinePriestSpells::POWER_WORD_SHIELD, false);
        if (shield && !me->HasAura(shield) && !me->HasAura(DisciplinePriestSpells::WEAKENED_SOUL))
        {
            if (CanCast(me, shield, true) && ExecuteSpell(me, shield, true))
                return true;
        }

        uint32 const renew = GetAppropriateRank(DisciplinePriestSpells::RENEW, false);
        if (renew && !me->HasAura(renew))
        {
            if (CanCast(me, renew, true) && ExecuteSpell(me, renew, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P0-2: 极限减伤急救 (痛苦压制 + 濒死瞬发盾 + 恢复)
    // =========================================================================
    bool TryEmergencyHeals()
    {
        if (TryPainSuppression())
            return true;

        // 濒死期瞬发预铺盾：用吸收量换取后续 1.5 秒读条窗口，
        // 避免目标在快速治疗读条途中被单发尖刺伤害直接带走
        if (TryPanicShield())
            return true;

        return false;
    }

    bool TryPainSuppression()
    {
        uint32 const painSuppression = GetAppropriateRank(DisciplinePriestSpells::PAIN_SUPPRESSION, true);
        if (!painSuppression)
            return false;

        Unit* target = nullptr;

        // 首选主坦：承伤核心，40% 减伤收益最大
        if (groupSnapshot.mainTank && groupSnapshot.mainTank->IsAlive() &&
            !groupSnapshot.mainTank->ToPet() &&
            groupSnapshot.mainTank->GetHealthPct() < 35.0f &&
            IsValidHealTarget(groupSnapshot.mainTank))
        {
            target = groupSnapshot.mainTank;
        }
        else
        {
            // 主坦安全时降级扫描全队：仅对「真正承受物理致死压力」的成员施放。
            // 严禁对宠物施放：宠物可由主人复活，不值得消耗 3 分钟 CD 的战略底牌
            for (Unit* ally : groupSnapshot.allies)
            {
                if (!ally || !ally->IsAlive() || ally->GetMap() != me->GetMap())
                    continue;

                if (ally->ToPet() || ally == groupSnapshot.mainTank)
                    continue;

                if (ally->GetHealthPct() >= 25.0f)
                    continue;

                // 仅在物理集火场景下交出减伤，避免纯粹法系掉血时浪费这张底牌
                if (!IsUnderPhysicalMelee(ally))
                    continue;

                if (!IsValidHealTarget(ally))
                    continue;

                target = ally;
                break;
            }
        }

        if (!target)
            return false;

        if (target->HasAura(painSuppression))
            return false;

        if (!CanCast(target, painSuppression, true))
            return false;

        return ExecuteSpell(target, painSuppression, true);
    }

    bool TryPanicShield()
    {
        uint32 const shield = GetAppropriateRank(DisciplinePriestSpells::POWER_WORD_SHIELD, false);
        if (!shield)
            return false;

        Unit* target = groupSnapshot.lowestHpAlly;
        if (!target || groupSnapshot.lowestHpPct >= 40.0f)
            return false;

        if (!IsValidHealTarget(target) || target->ToPet())
            return false;

        if (target->HasAura(shield) || target->HasAura(DisciplinePriestSpells::WEAKENED_SOUL))
            return false;

        if (!CanCast(target, shield, true))
            return false;

        return ExecuteSpell(target, shield, true);
    }

    // =========================================================================
    // P1: 真言术：盾全团预铺与修血
    // =========================================================================
    bool TryPreShield()
    {
        uint32 const shield = GetAppropriateRank(DisciplinePriestSpells::POWER_WORD_SHIELD, false);
        if (!shield)
            return false;

        // 战时血线门禁：全队跌入 55% 以下重伤线时，施法权必须全部让给 P3 阶梯治疗
        // 与痛苦压制。盾虽是瞬发，但单次吸收量低于一次快速治疗，绝不能在重伤期抢占救命 GCD。
        // (濒死个体会由 TryPanicShield 单独开辟绿色通道，不受此门禁限制)
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 55.0f)
            return false;

        std::vector<Unit*> candidates;
        candidates.reserve(groupSnapshot.allies.size() + 2);

        // 优先级：主坦 (承伤核心) -> 全队最低血线 -> 其余成员
        if (groupSnapshot.mainTank)
            candidates.push_back(groupSnapshot.mainTank);

        if (groupSnapshot.lowestHpAlly)
            candidates.push_back(groupSnapshot.lowestHpAlly);

        for (Unit* ally : groupSnapshot.allies)
            candidates.push_back(ally);

        for (Unit* target : candidates)
        {
            if (!IsValidHealTarget(target))
                continue;

            // 宠物不给盾：吸收量与法力应全部留给玩家成员
            if (target->ToPet())
                continue;

            // 灵魂虚弱：15 秒内无法再次获得真言术：盾。
            // 必须严格校验，否则「缺盾 -> 补盾 -> 被灵魂虚弱吃下 -> 下一帧依旧判定缺盾」
            // 会造成永久无效空转，把全部 GCD 烧在无法生效的施法上。
            if (target->HasAura(DisciplinePriestSpells::WEAKENED_SOUL))
                continue;

            if (target->HasAura(shield))
                continue;

            // 单个目标校验失败 (超距/被卡视线) 时继续扫描，避免阻塞其余队友的预铺
            if (!CanCast(target, shield, true))
                continue;

            if (ExecuteSpell(target, shield, true))
                return true;
        }

        return false;
    }

    // =========================================================================
    // P1: 常驻增益 (心灵之火 / 真言术：韧 / 神圣之灵)
    // =========================================================================
    bool MaintainInnerFire()
    {
        uint32 const innerFire = GetAppropriateRank(DisciplinePriestSpells::INNER_FIRE, false);
        if (!innerFire || me->HasAura(innerFire))
            return false;

        return CanCast(me, innerFire, true) && ExecuteSpell(me, innerFire, true);
    }

    bool MaintainTeamBuffs()
    {
        if (groupSnapshot.allies.empty())
            return false;

        // 战时节流：血线不稳时全力治疗，绝不因补团队增益抢占急救 GCD
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 80.0f)
            return false;

        uint32 const fortitude = GetAppropriateRank(DisciplinePriestSpells::POWER_WORD_FORTITUDE, false);
        uint32 const divineSpirit = GetAppropriateRank(DisciplinePriestSpells::DIVINE_SPIRIT, false);

        // 每帧只补一个缺口，平滑铺满全队，避免单帧连续施法造成卡顿
        for (Unit* ally : groupSnapshot.allies)
        {
            if (!ally || !ally->IsAlive() || ally->GetMap() != me->GetMap())
                continue;

            if (fortitude && !ally->HasAura(fortitude))
            {
                if (CanCast(ally, fortitude, true) && ExecuteSpell(ally, fortitude, true))
                    return true;
            }

            // 神圣之灵仅对法力职业有收益，物理职业跳过以免浪费 GCD
            if (divineSpirit && ally->getPowerType() == POWER_MANA && !ally->HasAura(divineSpirit))
            {
                if (CanCast(ally, divineSpirit, true) && ExecuteSpell(ally, divineSpirit, true))
                    return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P2: 续航仲裁 (心灵专注 / 能量注入 / 暗影魔)
    // =========================================================================
    bool MaintainManaCooldowns(Unit* victim)
    {
        if (me->getPowerType() != POWER_MANA || !me->IsInCombat())
            return false;

        float const manaPct = me->GetPowerPct(POWER_MANA);

        // 心灵专注：法力 < 35% 时开启，让下一发治疗免费，
        // 提前覆盖后续高压刷血窗口，提升整场续航总效率
        uint32 const innerFocus = GetAppropriateRank(DisciplinePriestSpells::INNER_FOCUS, true);
        if (innerFocus && manaPct < 35.0f && !me->HasAura(innerFocus))
        {
            if (CanCast(me, innerFocus, true) && ExecuteSpell(me, innerFocus, true))
                return true;
        }

        // 能量注入：法力 < 60% 时交给自己，20% 急速 + 20% 耗蓝降低同时提升
        // 治疗输出效率与续航效率，是戒律牧最长效的续航底牌
        uint32 const powerInfusion = GetAppropriateRank(DisciplinePriestSpells::POWER_INFUSION, true);
        if (powerInfusion && manaPct < 60.0f && !me->HasAura(powerInfusion))
        {
            if (CanCast(me, powerInfusion, true) && ExecuteSpell(me, powerInfusion, true))
                return true;
        }

        if (manaPct >= 45.0f)
            return false;

        // 暗影魔：法力枯竭时的主力回蓝手段。
        // 召唤物需要敌对目标承载，无有效敌人时必须跳过，避免无目标空放
        uint32 const shadowfiend = GetAppropriateRank(DisciplinePriestSpells::SHADOWFIEND, true);
        if (!shadowfiend || !victim || !victim->IsAlive() || victim->GetMap() != me->GetMap() || victim->IsFriendlyTo(me))
            return false;

        if (!CanCast(victim, shadowfiend, true))
            return false;

        return ExecuteSpell(victim, shadowfiend, true);
    }

    // =========================================================================
    // P3: 治疗技能执行器
    // -------------------------------------------------------------------------
    // 苦修：2 秒 3 段治疗通道。通道期间引擎持续置位 UNIT_STATE_CASTING，
    //       由顶部全局守卫冻结决策，因此进入通道前必须立定，
    //       否则 0.5 秒内就会被自身移动指令掐断通道。
    // 快速治疗：1.5 秒读条，同样必须立定。
    // 盾 / 恢复 / 愈合祷言：瞬发，允许在跑位与抱坦避难途中直接施放。
    // =========================================================================
    bool TryPenance(Unit* target)
    {
        uint32 const penance = GetAppropriateRank(DisciplinePriestSpells::PENANCE, true);
        if (!penance || !target)
            return false;

        // 通道技刹停：仅在真正需要引导时才放弃跑位机动性
        if (me->isMoving())
            me->StopMoving();

        if (!CanCast(target, penance, true))
            return false;

        return ExecuteSpell(target, penance, true);
    }

    bool TryFlashHeal(Unit* target)
    {
        uint32 const flashHeal = GetAppropriateRank(DisciplinePriestSpells::FLASH_HEAL, false);
        if (!flashHeal || !target)
            return false;

        // 读条技刹停：立定校验精准下沉至此，避免为瞬发技能白白放弃自保能力
        if (me->isMoving())
            me->StopMoving();

        if (!CanCast(target, flashHeal, true))
            return false;

        return ExecuteSpell(target, flashHeal, true);
    }

    bool MaintainRenew()
    {
        uint32 const renew = GetAppropriateRank(DisciplinePriestSpells::RENEW, false);
        if (!renew)
            return false;

        // 瞬发 HoT 只铺给中危目标，满血铺 HoT 属纯浪费
        if (groupSnapshot.lowestHpPct >= 90.0f)
            return false;

        Unit* target = groupSnapshot.lowestHpAlly;
        if (!IsValidHealTarget(target) || target->ToPet())
            return false;

        if (target->HasAura(renew))
            return false;

        if (!CanCast(target, renew, true))
            return false;

        return ExecuteSpell(target, renew, true);
    }

    bool TryPrayerOfMending()
    {
        uint32 const prayerOfMending = GetAppropriateRank(DisciplinePriestSpells::PRAYER_OF_MENDING, false);
        if (!prayerOfMending)
            return false;

        // 愈合祷言瞬发且智能弹跳，挂在主坦身上可最大化弹跳收益 (每次受击触发一跳)
        Unit* target = groupSnapshot.mainTank;
        if (!IsValidHealTarget(target) || target->ToPet())
            return false;

        if (target->HasAura(prayerOfMending))
            return false;

        if (!CanCast(target, prayerOfMending, true))
            return false;

        return ExecuteSpell(target, prayerOfMending, true);
    }

    // =========================================================================
    // P3: 阶梯式治疗核心 (目标：LowestHpAlly)
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
        // 高危重伤 (< 60%)：苦修 (通道，单体 HPS 最高) 优先，
        // 冷却/等级未达时回退快速治疗读条救场
        // 此阶段严禁先铺恢复：1 个 GCD 的 HoT 不足以阻止目标在伤害尖峰下暴毙
        // ---------------------------------------------------------------------
        if (hpPct < 60.0f)
        {
            if (TryPenance(target)) return true;
            if (TryFlashHeal(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 中度掉血 (< 80%)：恢复瞬发铺 HoT -> 愈合祷言挂坦 -> 苦修 -> 快速治疗
        // ---------------------------------------------------------------------
        if (hpPct < 80.0f)
        {
            if (MaintainRenew()) return true;
            if (TryPrayerOfMending()) return true;
            if (TryPenance(target)) return true;
            if (TryFlashHeal(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 平稳掉血 (80% ~ 95%)：恢复效率铺 HoT，缺失时快速治疗平稳填充
        // ---------------------------------------------------------------------
        if (MaintainRenew()) return true;
        if (TryFlashHeal(target)) return true;

        return false;
    }

    // =========================================================================
    // P4: 驱散 (群体驱散优先，单体驱散兜底)
    // =========================================================================
    bool TryCleanse()
    {
        if (!groupSnapshot.lowestHpAlly || groupSnapshot.lowestHpPct < 90.0f)
            return false;

        // 优先驱散魔法；未习得时以祛病术/驱除疾病兜底过渡
        uint32 dispel = GetAppropriateRank(DisciplinePriestSpells::DISPEL_MAGIC, false);
        if (!dispel)
            dispel = GetAppropriateRank(DisciplinePriestSpells::ABOLISH_DISEASE, false);
        if (!dispel)
            dispel = GetAppropriateRank(DisciplinePriestSpells::CURE_DISEASE, false);

        if (!dispel)
            return false;

        SpellInfo const* dispelInfo = sSpellMgr->GetSpellInfo(dispel);
        if (!dispelInfo)
            return false;

        // 群体驱散：多名队友同时中招时以一发代替多次单体驱散，节省大量 GCD
        uint32 const massDispel = GetAppropriateRank(DisciplinePriestSpells::MASS_DISPEL, false);
        if (massDispel && massDispel != dispel)
        {
            if (SpellInfo const* massDispelInfo = sSpellMgr->GetSpellInfo(massDispel))
            {
                uint32 debuffedCount = 0;
                Unit* anchor = nullptr;

                for (Unit* ally : groupSnapshot.allies)
                {
                    if (!IsValidHealTarget(ally) || ally->ToPet())
                        continue;

                    if (!HasDispellableDebuff(ally, massDispelInfo))
                        continue;

                    ++debuffedCount;
                    if (!anchor)
                        anchor = ally;
                }

                if (debuffedCount >= 2 && anchor && CanCast(anchor, massDispel, true) &&
                    ExecuteSpell(anchor, massDispel, true))
                {
                    return true;
                }
            }
        }

        for (Unit* ally : groupSnapshot.allies)
        {
            if (!IsValidHealTarget(ally) || ally->ToPet() || !HasDispellableDebuff(ally, dispelInfo))
                continue;

            // 单个目标施法校验失败 (超距/被卡视线) 时继续扫描其余队友，
            // 避免个别目标直接中断整轮驱散巡检
            if (!CanCast(ally, dispel, true))
                continue;

            return ExecuteSpell(ally, dispel, true);
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
        // 缺此校验会导致随从盲目跟随尸体移动，放弃原地自保与救援
        if (!anchor || !anchor->IsAlive() || !anchor->IsInWorld() || anchor->GetMap() != me->GetMap())
            return;

        bool const underMeleePressure = IsUnderPhysicalMelee(me);
        float const dist = me->GetDistance(anchor);
        bool const needsReposition = underMeleePressure || dist > MAX_SAFE_DIST || dist < MIN_SAFE_DIST;

        if (!needsReposition)
            return;

        // angle 严禁自行叠加 anchor->GetOrientation()：FollowMovementGenerator 内部已自行
        // 以目标朝向为基准加算，二次叠加会让最终站位随坦克转向不断漂移
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
        // FollowMovementGenerator 不会随距离变化自动重建，缺此步戒律牧将永久停在 2 码处吃顺劈与吐息。
        // 但仅在「握手状态跃迁」或「尚未处于跟随态」时下发，否则 2 码走回 8 码途中会每帧重建路径造成抽搐。
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
    // 戒律天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
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

        SyncPassive(20, DisciplinePriestSpells::IMPROVED_INNER_FIRE);            // 强化心灵之火：护甲/法强强化
        SyncPassive(20, DisciplinePriestSpells::IMPROVED_POWER_WORD_SHIELD);     // 强化真言术：盾：吸收量 +15%
        SyncPassive(20, DisciplinePriestSpells::IMPROVED_POWER_WORD_FORTITUDE);  // 强化真言术：韧
        SyncPassive(30, DisciplinePriestSpells::MEDITATION);                     // 冥想：施法中保持法力回复
        SyncPassive(40, DisciplinePriestSpells::SPIRITUAL_GUIDANCE);             // 精神指引：精神转化法强

        // 真言术：盾雕文：盾被打破时为目标附加治疗，预铺收益的核心放大器
        SyncPassive(60, DisciplinePriestSpells::GLYPH_OF_POWER_WORD_SHIELD);
    }
};

void AddSC_bot_discipline_priest()
{
    new AdaptiveBotScript<BotDisciplinePriestAI>("bot_discipline_priest");
}
