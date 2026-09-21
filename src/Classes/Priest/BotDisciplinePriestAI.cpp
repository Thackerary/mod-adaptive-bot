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

    // 阶段三记忆化预警预读：高危机制落地前的抢铺窗口 (必须覆盖一次瞬发预铺 + 网络延迟)
    static constexpr uint32 PREHEAL_LEAD_MS     = 1500;

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
    // 注：暗影魔 (34433) 为 66 级基础技能而非天赋，不得登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case DisciplinePriestSpells::INNER_FOCUS:      return 20; // 心灵专注
            case DisciplinePriestSpells::POWER_INFUSION:   return 40; // 能量注入
            case DisciplinePriestSpells::PAIN_SUPPRESSION: return 50; // 痛苦压制
            case DisciplinePriestSpells::DESPERATE_PRAYER: return 20; // 绝望祷言 (自保底牌)
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
        // UNIT_STATE_CASTING，但引导类法术在部分状态下并不置位该标记，
        // 因此追加 CURRENT_CHANNELED_SPELL 显式判定形成双保险，
        // 杜绝苦修引导被自身跟随移动指令 (UpdateFollowMaster / MoveFollow) 掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
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

        // ---- P0.5: 阶段三记忆化预警预读 (高危机制落地前抢铺盾/HoT) ----
        if (TryPredictivePreHoT()) return;

        // ---- P1: 常驻增益与真言术：盾预铺 ----
        if (sweepDue)
        {
            if (MaintainInnerFire()) return;
            if (MaintainTeamBuffs()) return;
        }

        if (MaintainTankShield()) return;
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

    // 阶段三记忆化预警预读的武装状态：判定本轮高危机制是否已完成预铺。
    // 缺此标记会在 1500ms 窗口期内每帧重复抢铺，把救命 GCD 全部烧在预备动作上。
    bool isPreHealing{ false };

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
        // 从根源上解除被持续追打的死循环 (P5 的抱坦走位只是治标)。
        // 门禁收紧至 [45%, 85%)：< 45% 濒死期生存第一，渐隐不带任何减伤与
        // 吸收，丢在这里只会白烧一个救命 GCD，此时必须交给盾与绝望祷言；
        // >= 85% 时血线尚稳，也无需为了一点仇恨提前消耗渐隐。
        float const selfHpPct = me->GetHealthPct();
        if (selfHpPct >= 45.0f && selfHpPct < 85.0f && IsUnderPhysicalMelee(me))
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

        // 绝望祷言：瞬发、无读条、无移动惩罚的自疗大招，是唯一能在濒死期
        // 不牺牲机动性的自救底牌。原先在此处铺【恢复】属严重误配：
        // 戒律天赋不强化 HoT，单跳治疗量远低于苦修/快速治疗，
        // 只会白占一个救命 GCD。
        // 绝望祷言已登记于 GetTalentSpellMinLevel，第二参数必须传 true，
        // 否则会被误判为基础技能而绕开天赋等级契约，导致低等级段空放
        uint32 const desperatePrayer = GetAppropriateRank(DisciplinePriestSpells::DESPERATE_PRAYER, true);
        if (desperatePrayer && !me->HasAura(desperatePrayer))
        {
            if (CanCast(me, desperatePrayer, true) && ExecuteSpell(me, desperatePrayer, true))
                return true;
        }

        // 绝望祷言冷却或未达等级时直接放行：决策流自然落入 P3 TryLadderHeal，
        // 由苦修 (首跳瞬抬) 或快速治疗直接对自身执行高 HPS 治疗
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

        // P0 只保留瞬发底牌：治疗祷言为 3 秒读条，在单体濒死期占用通道等同于
        // 放弃急救，已下沉至 P3 群体抬血分支，在无濒死危险时才择机施放。
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
            groupSnapshot.mainTank->GetHealthPct() < 40.0f &&
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

                if (!IsValidHealTarget(ally))
                    continue;

                // 承伤判定：只要该成员正处于承伤状态 (被物理贴身，或仇恨列表非空 /
                // 处于战斗中)，即便伤害来源是法术、远程或环境机制，也应果断交出
                // 40% 减伤底牌。原先强制要求物理贴身，会把法系尖刺与环境致死压力
                // 整体漏判，底牌烂在手里直至队友暴毙。
                if (!IsUnderPhysicalMelee(ally) && !ally->IsInCombat() && ally->getAttackers().empty())
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

    // 阶梯治疗专用补盾：目标无盾且未处于灵魂虚弱时瞬发止血，
    // 并借争分夺秒 (BORROWED_TIME) 为后续苦修/快速治疗挂上 25% 急速，
    // 是戒律单体连招的起手式，与全团预铺盾 (TryPreShield) 门禁完全解耦。
    bool TryLadderShield(Unit* target)
    {
        uint32 const shield = GetAppropriateRank(DisciplinePriestSpells::POWER_WORD_SHIELD, false);
        if (!shield || !target || target->ToPet())
            return false;

        if (target->HasAura(shield) || target->HasAura(DisciplinePriestSpells::WEAKENED_SOUL))
            return false;

        if (!CanCast(target, shield, true))
            return false;

        return ExecuteSpell(target, shield, true);
    }

    // =========================================================================
    // P0-3: 全队崩溃保护 (群体急救底牌)
    // -------------------------------------------------------------------------
    // 单点阶梯治疗 (苦修/快速治疗) 在多成员同时掉血时会被 GCD 逐个击破，
    // 此时必须切入群体治疗通道。averageHpPct 是唯一能反映「全队整体崩坏」
    // 而非「单点尖刺」的采样值，不参与仲裁会令群体底牌永久闲置。
    // =========================================================================
    bool TryGroupEmergencyHeal()
    {
        // 安全门禁：全队最低血线 >= 65% 才允许停步读条。
        // 治疗祷言读条 3 秒，若仍有成员处于重伤承压线，这 3 秒足以让目标暴毙，
        // 此时施法权必须无条件让渡给 P0 瞬发急救与 P3 单体阶梯治疗。
        if (groupSnapshot.lowestHpPct < 65.0f)
            return false;

        // 触发门禁：全队平均血线跌入 70% 以下，且至少 3 名非宠物成员低于 80%
        if (groupSnapshot.allies.size() < 3 || groupSnapshot.averageHpPct >= 70.0f)
            return false;

        uint32 injuredCount = 0;
        for (Unit* ally : groupSnapshot.allies)
        {
            if (!IsValidHealTarget(ally) || ally->ToPet())
                continue;

            if (ally->GetHealthPct() < 80.0f)
                ++injuredCount;
        }

        if (injuredCount < 3)
            return false;

        uint32 const prayerOfHealing = GetAppropriateRank(DisciplinePriestSpells::PRAYER_OF_HEALING, false);
        if (!prayerOfHealing)
            return false;

        // 锚点必须落在承伤核心身上：治疗祷言只覆盖目标所属小队，
        // 以残血散人为锚点会漏掉坦克，背离群体急救的初衷
        Unit* anchor = groupSnapshot.mainTank ? groupSnapshot.mainTank : groupSnapshot.lowestHpAlly;
        if (!IsValidHealTarget(anchor) || anchor->ToPet())
            return false;

        if (!CanCast(anchor, prayerOfHealing, true))
            return false;

        // 读条技刹停精准下沉至校验通过之后，避免校验失败时白白放弃跑位机动性
        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(anchor, prayerOfHealing, true);
    }

    // =========================================================================
    // P0.5: 阶段三记忆化预警预读 (认知记忆在治疗侧的唯一落地形态)
    // -------------------------------------------------------------------------
    // 戒律牧没有可稳定作用于远程读条的硬打断 (心灵尖啸为近身恐惧)，
    // 因此不消费 learnedInterruptDelays，而是消费同一份认知记忆的另一个维度
    // ——危险禁区的 spellId 列表。基类 UpdateTimers 每帧纯 diff 驱动地追踪
    // 当前敌方读条进度 (与打断 CD 无关)，本函数据此在「已学到的高危机制」
    // 落地前提前 1500ms 给主坦抢铺真言术：盾，把治疗从被动打地鼠升级为主动预铺。
    // 数据来源零新增：spellId 命中 activeDangerZones 即视为高危机制，
    // 该列表由中层归因器在团灭时逆向提炼落库，战前由 PreloadBossKnowledge 预热灌入。
    // =========================================================================
    bool TryPredictivePreHoT()
    {
        if (!me->IsInCombat() || currentEnemyCastingSpellId == 0 || currentEnemyCastingTotalMs == 0)
        {
            isPreHealing = false;
            return false;
        }

        bool isLearnedHazard = false;
        for (auto const& zone : activeDangerZones)
        {
            if (zone.spellId == currentEnemyCastingSpellId)
            {
                isLearnedHazard = true;
                break;
            }
        }

        // 未命中记忆库的读条即普通技能 (平砍强化 / 杂兵小法术)，绝不预铺：
        // 对每一发小读条都预铺会把法力烧空，反而失去真正高压期的续航。
        if (!isLearnedHazard)
        {
            isPreHealing = false;
            return false;
        }

        uint32 const remainingMs = (currentEnemyCastingTotalMs > currentEnemyCastingElapsedMs)
            ? (currentEnemyCastingTotalMs - currentEnemyCastingElapsedMs) : 0;

        // 尚未进入预警窗口：保持待机；本轮已完成预铺：严禁重复抢铺
        if (remainingMs > PREHEAL_LEAD_MS || isPreHealing)
            return false;

        Unit* tank = groupSnapshot.mainTank;

        // 排己与宠物排除：锚点退化为自身或指挥官宠物时无预铺意义
        if (!tank || tank == me || !tank->IsAlive() || tank->ToPet() || !IsValidHealTarget(tank))
            return false;

        uint32 const shield = GetAppropriateRank(DisciplinePriestSpells::POWER_WORD_SHIELD, false);

        // 优先盾：瞬发吸收量在机制落地前的边际收益最高。
        // 灵魂虚弱期无法再次获得真言术：盾，必须严格校验，
        // 否则会陷入「缺盾 -> 补盾 -> 被虚弱吃下 -> 下一帧依旧缺盾」的无效空转。
        if (shield && !tank->HasAura(shield) && !tank->HasAura(DisciplinePriestSpells::WEAKENED_SOUL))
        {
            if (CanCast(tank, shield, true) && ExecuteSpell(tank, shield, true))
            {
                isPreHealing = true;

                if (isDebugLogging)
                    LOG_INFO("scripts", "[Bot: {}] 记忆化预警预读：高危机制 [{}] 落地前 {}ms，已为 [{}] 预铺真言术：盾。",
                        me->GetName(), currentEnemyCastingSpellId, remainingMs, tank->GetName());
                return true;
            }
        }

        // 盾不可用 (灵魂虚弱 / 未习得 / 法力不足) 时的退化通道：瞬发 HoT 抢铺。
        // 恢复在此仅作为预读兜底，不作为常规循环填充 (戒律天赋不强化 HoT)。
        if (TryRenew(tank))
        {
            isPreHealing = true;

            if (isDebugLogging)
                LOG_INFO("scripts", "[Bot: {}] 记忆化预警预读：高危机制 [{}] 落地前 {}ms，已为 [{}] 预铺恢复。",
                    me->GetName(), currentEnemyCastingSpellId, remainingMs, tank->GetName());
            return true;
        }

        return false;
    }

    // 恢复：瞬发 HoT，仅作为预警预读的抢铺手段与盾被灵魂虚弱封锁时的退化选项
    bool TryRenew(Unit* target)
    {
        uint32 const renew = GetAppropriateRank(DisciplinePriestSpells::RENEW, false);
        if (!renew || !target || target->ToPet() || !IsValidHealTarget(target))
            return false;

        // 光环防顶守卫：已挂恢复时不再刷新，收益低于占用一个 GCD
        if (target->HasAura(renew))
            return false;

        if (!CanCast(target, renew, true))
            return false;

        // 瞬发 HoT：允许在跑位途中直接施放，无需 StopMoving
        return ExecuteSpell(target, renew, true);
    }

    // =========================================================================
    // P1-a: 主坦专属真言术：盾维护 (战时承伤核心)
    // -------------------------------------------------------------------------
    // 主坦的盾兼具「吸收尖刺伤害」与「降低治疗压力」双重收益，
    // 必须与「全团无差别预铺」彻底解耦：无论团队血线如何，只要主坦缺盾
    // 且未处于灵魂虚弱，就应在血线安全时优先保持覆盖。
    // =========================================================================
    bool MaintainTankShield()
    {
        // 战时节流门禁：全队只要出现需要正经治疗的目标，主坦的预铺盾必须
        // 无条件让位给 P3 单体急救。否则会出现「队友濒死、牧师却在给 75%
        // 血线的坦克补盾」的治疗倒挂，最终演变为减员事故。
        // (主坦自身濒死由 TryPanicShield 单独开辟瞬发绿色通道，不受此门禁限制)
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 70.0f)
            return false;

        uint32 const shield = GetAppropriateRank(DisciplinePriestSpells::POWER_WORD_SHIELD, false);
        if (!shield)
            return false;

        // 主坦锚点缺失 (团队中无坦克定位成员) 时交由 TryPreShield 兜底
        Unit* tank = groupSnapshot.mainTank;
        if (!tank || tank->ToPet() || !IsValidHealTarget(tank))
            return false;

        // 主坦自身血线告急时严禁套盾：单次盾的吸收量低于一发快速治疗，
        // 此时必须把 GCD 让渡给直接治疗通道，否则就是拿救命 GCD 去换等效更低的吸收。
        // (主坦濒死会由 TryPanicShield 单独开辟瞬发绿色通道，不受此门禁限制)
        if (tank->GetHealthPct() < 60.0f)
            return false;

        // 法力门禁：为主坦维持盾的前提是不能抽空后续高压窗口的治疗余量
        if (me->GetPowerPct(POWER_MANA) < 35.0f)
            return false;

        // 灵魂虚弱期无法再次获得真言术：盾，必须严格校验，
        // 否则会陷入「缺盾 -> 补盾 -> 被灵魂虚弱吃下 -> 下一帧依旧缺盾」的无效空转
        if (tank->HasAura(shield) || tank->HasAura(DisciplinePriestSpells::WEAKENED_SOUL))
            return false;

        if (!CanCast(tank, shield, true))
            return false;

        return ExecuteSpell(tank, shield, true);
    }

    // =========================================================================
    // P1-b: 真言术：盾全团预铺 (仅限团队极其健康的窗口)
    // =========================================================================
    bool TryPreShield()
    {
        uint32 const shield = GetAppropriateRank(DisciplinePriestSpells::POWER_WORD_SHIELD, false);
        if (!shield)
            return false;

        bool const inCombat = me->IsInCombat();
        float const manaPct = me->GetPowerPct(POWER_MANA);

        // 战时双门禁：全员极其健康 (>= 90%) 且法力充裕 (>= 60%)，才允许把 GCD
        // 花在给满血队友套盾上。全队只要有人掉血，施法权必须无条件让渡给
        // P3 阶梯治疗与 P0 急救，否则会出现「主坦残血挂着灵魂虚弱，
        // 牧师却连续给满血队友套盾」的治疗真空，最终演变为倒坦事故。
        // 主坦自身的盾由 MaintainTankShield 独立维护，不受此门禁影响。
        if (inCombat && (groupSnapshot.lowestHpPct < 90.0f || manaPct < 60.0f))
            return false;

        // 脱战是全团预铺的最佳窗口：无 GCD 竞争、无读条被打断风险，仅保留法力门禁
        if (!inCombat && manaPct < 60.0f)
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
            // 战时只覆盖满血成员：任何掉血成员都是 P3 阶梯治疗的服务对象，
            // 用盾去顶掉他们的治疗位等同于制造治疗真空
            if (inCombat && target->GetHealthPct() < 100.0f)
                continue;

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

        // 战时节流门禁：心灵之火属常驻增益，掉落后补挂不急于一时。
        // 战时只要有人掉血，补 buff 的 GCD 必须让渡给治疗通道；
        // 否则一次 1.5 秒的读条就可能错过救命窗口。脱战时无此限制。
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 75.0f)
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

        // 心灵专注：法力 < 35% 时开启，让下一发治疗免费且暴击率 +25%。
        // 该技能在 3.3.5a 中为 Off-GCD 瞬发增益，施放成功后严禁 return true：
        // 必须允许当帧决策流顺下执行 (继续判定能量注入，或直接下沉 P3 阶梯治疗)，
        // 使「下一发治疗免费 + 25% 暴击」的光环能被当帧的苦修/快速治疗立即吃下，
        // 否则白白空转一帧决策，高压窗口下等同浪费一个救命时间片。
        uint32 const innerFocus = GetAppropriateRank(DisciplinePriestSpells::INNER_FOCUS, true);
        if (innerFocus && manaPct < 35.0f && !me->HasAura(innerFocus))
        {
            if (CanCast(me, innerFocus, true))
                ExecuteSpell(me, innerFocus, true);
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

        // 血线安全门禁：暗影魔需敌对目标承载且占用一个完整 GCD，
        // 必须在团队脱离濒死承压线后才允许交出。否则「队友正在暴毙，
        // 牧师却停下来招回蓝宠」会直接导致减员，回蓝收益远低于人命成本。
        if (groupSnapshot.lowestHpPct < 65.0f)
            return false;

        // 暗影魔：法力枯竭时的主力回蓝手段。
        // 该技能为 66 级基础技能而非天赋，GetAppropriateRank 第二参数必须传 false，
        // 否则会被误判为天赋技能而绕开等级降阶逻辑。
        // 召唤物需要敌对目标承载，无有效敌人时必须跳过，避免无目标空放
        uint32 const shadowfiend = GetAppropriateRank(DisciplinePriestSpells::SHADOWFIEND, false);
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

        // 施法资格必须先通过校验再刹停：CanCast 失败 (GCD/被控/超距) 时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐
        if (!CanCast(target, penance, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, penance, true);
    }

    bool TryFlashHeal(Unit* target)
    {
        uint32 const flashHeal = GetAppropriateRank(DisciplinePriestSpells::FLASH_HEAL, false);
        if (!flashHeal || !target)
            return false;

        // 同上：校验通过后才立定，避免 CanCast 失败时白白放弃跑位机动性
        if (!CanCast(target, flashHeal, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, flashHeal, true);
    }

    bool TryPrayerOfMending(Unit* target)
    {
        uint32 const prayerOfMending = GetAppropriateRank(DisciplinePriestSpells::PRAYER_OF_MENDING, false);
        if (!prayerOfMending)
            return false;

        // 愈合祷言瞬发且智能弹跳，挂在主坦身上可最大化弹跳收益 (每次受击触发一跳)；
        // 无独立主坦 (单人跟随 / 未识别出坦克) 时由调用方回落到正在掉血的成员
        if (!IsValidHealTarget(target) || target->ToPet())
            return false;

        // 防顶光环：愈合祷言全团同时只能存在一个弹跳实体，
        // 必须有任一成员持有即整轮跳过。若只校验主坦自身，会在坦克持盾、
        // 祷言却已弹跳到队友身上的场景下反复重放，白白烧掉救命 GCD。
        // 此处不过滤宠物：祷言弹跳同样会落在宠物身上，漏检会造成同样的顶替浪费。
        for (Unit* ally : groupSnapshot.allies)
        {
            if (ally && ally->HasAura(prayerOfMending))
                return false;
        }

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
        // 高危重伤 (< 60%)：戒律瞬发三连起手，最高优先级。
        // 补盾 (瞬发止血 + 争分夺秒 25% 急速) -> 苦修 (首跳瞬抬，单体 HPS 峰值)
        // -> 快速治疗读条兜底。
        // 群体抬血通道被强制排在本次分支之后：3 秒读条的治疗祷言绝不允许
        // 在单体濒死重伤期抢走救命 GCD。
        // ---------------------------------------------------------------------
        if (hpPct < 60.0f)
        {
            if (TryLadderShield(target)) return true;
            if (TryPenance(target)) return true;
            if (TryFlashHeal(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 群体抬血通道：必须在单体高危急救分支之后执行。
        // 治疗祷言读条 3 秒，在单体重伤期占用通道等同于放弃急救，
        // 因此内部设置 65% 安全门禁，仅在全员脱离重伤承压线时才允许停步读条。
        // (恢复已被移除：戒律天赋不强化 HoT，铺恢复属纯粹抢占救命 GCD)
        // ---------------------------------------------------------------------
        if (TryGroupEmergencyHeal())
            return true;

        // ---------------------------------------------------------------------
        // 中度掉血 (< 80%)：补盾连招 -> 愈合祷言挂主坦弹跳 -> 苦修 -> 快速治疗
        // ---------------------------------------------------------------------
        if (hpPct < 80.0f)
        {
            // 苦修 (首跳瞬抬，单体 HPS 峰值) 必须排在愈合祷言之前：
            // 祷言属挂坦辅助弹跳技，单跳治疗量低于苦修首跳，
            // 让辅助技抢占残血救急的 GCD 会直接拖慢止血速度
            if (TryLadderShield(target)) return true;
            if (TryPenance(target)) return true;

            // 弹跳锚点优先主坦 (受击最频繁)；无主坦时回落到当前最需要治疗的成员
            Unit* pomTank = groupSnapshot.mainTank ? groupSnapshot.mainTank : target;
            if (TryPrayerOfMending(pomTank)) return true;

            if (TryFlashHeal(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 平稳掉血 (80% ~ 95%)：补盾联动 -> 苦修效率填充 -> 快速治疗平稳收尾。
        // 受损目标补盾可同时兑现三重收益：吸收伤害、触发争分夺秒 25% 急速、
        // 盾被吸收时触发狂喜 (RAPTURE) 返还法力，是平稳期性价比最高的起手式。
        // (满血成员的预铺仍由 P1 TryPreShield 统一调度，二者不冲突)
        // 此区间严禁动用苦修：苦修是戒律单体最高爆发抬血底牌且带冷却，
        // 对 92% 血线的目标施放会造成约 80% 过量治疗并白烧长冷却，
        // 必须严格封存于 hpPct < 80.0f 的区间。
        //
        // 快速治疗血线门禁 (< 90%)：90%~95% 属「轻微掉血」，此时若目标已有盾
        // 或处于灵魂虚弱，单发快速治疗的过量治疗比例极高，且会持续拖延
        // 五秒规则下的精神回蓝重启。此区间只做零耗蓝/低耗蓝的补盾与祷言，
        // 血量跌回 90% 以下才允许投入高耗蓝读条。
        // ---------------------------------------------------------------------
        if (TryLadderShield(target)) return true;

        Unit* pomTarget = groupSnapshot.mainTank ? groupSnapshot.mainTank : target;
        if (TryPrayerOfMending(pomTarget)) return true;

        if (hpPct < 90.0f && TryFlashHeal(target)) return true;

        return false;
    }

    // =========================================================================
    // P4: 驱散 (群体驱散优先，单体驱散兜底)
    // =========================================================================
    bool TryCleanse()
    {
        if (!groupSnapshot.lowestHpAlly || groupSnapshot.lowestHpPct < 90.0f)
            return false;

        // 魔法与疾病驱散法术必须独立获取。
        // 原先的 if (!dispel) 级联赋值会在学会驱散魔法后直接短路，
        // ABOLISH_DISEASE / CURE_DISEASE 永远得不到赋值，
        // 导致随从终生无法驱散疾病，遇到疾病类机制只能干看着队友掉血。
        uint32 const magicDispel = GetAppropriateRank(DisciplinePriestSpells::DISPEL_MAGIC, false);

        uint32 diseaseDispel = GetAppropriateRank(DisciplinePriestSpells::ABOLISH_DISEASE, false);
        if (!diseaseDispel)
            diseaseDispel = GetAppropriateRank(DisciplinePriestSpells::CURE_DISEASE, false);

        if (!magicDispel && !diseaseDispel)
            return false;

        SpellInfo const* magicDispelInfo = magicDispel ? sSpellMgr->GetSpellInfo(magicDispel) : nullptr;
        SpellInfo const* diseaseDispelInfo = diseaseDispel ? sSpellMgr->GetSpellInfo(diseaseDispel) : nullptr;

        if (!magicDispelInfo && !diseaseDispelInfo)
            return false;

        // 群体驱散：多名队友同时中招时以一发代替多次单体驱散，节省大量 GCD
        uint32 const massDispel = GetAppropriateRank(DisciplinePriestSpells::MASS_DISPEL, false);
        if (massDispel && massDispel != magicDispel)
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

                // 群体驱散为读条法术：必须先刹停，否则移动中施法会被引擎
                // 以 SPELL_FAILED_MOVING 直接拒绝，白烧一轮 GCD。
                // 刹停时机严格下沉至 CanCast 通过之后，避免校验失败白白放弃机动性。
                if (debuffedCount >= 2 && anchor && CanCast(anchor, massDispel, true))
                {
                    if (me->isMoving())
                        me->StopMoving();

                    if (ExecuteSpell(anchor, massDispel, true))
                        return true;
                }
            }
        }

        for (Unit* ally : groupSnapshot.allies)
        {
            if (!IsValidHealTarget(ally) || ally->ToPet())
                continue;

            // 逐目标择法：魔法优先 (法术伤害与控场类负面通常比疾病更致命)，
            // 无魔法可驱时再回落疾病驱散通道
            uint32 chosenDispel = 0;
            if (magicDispelInfo && HasDispellableDebuff(ally, magicDispelInfo))
                chosenDispel = magicDispel;
            else if (diseaseDispelInfo && HasDispellableDebuff(ally, diseaseDispelInfo))
                chosenDispel = diseaseDispel;

            if (!chosenDispel)
                continue;

            // 单个目标施法校验失败 (超距/被卡视线) 时继续扫描其余队友，
            // 避免个别目标直接中断整轮驱散巡检
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

        // ---- 基础位: APF 势场紧急避险 (火圈/顺劈强行接管) ----
        // 读条路径被 CanCast 的「危险区封锁」截断后，若此处不下发主动位移，
        // 治疗随从只会原地站桩瞬发，被火圈与锥形区域活活烧穿。
        // 避险优先级必须高于一切跟随/贴坦握手逻辑。
        if (IsUnderDangerThreat(2.0f))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;
                Unit* avoidAnchor = groupSnapshot.mainTank ? groupSnapshot.mainTank : GetMaster();
                if (avoidAnchor)
                {
                    if (PotentialField::CalculateNextPosition(me, avoidAnchor, IDEAL_FOLLOW_DIST, false, false, activeDangerZones, nextX, nextY, nextZ))
                    {
                        me->GetMotionMaster()->MovePoint(1, nextX, nextY, nextZ);
                        apfMoveUpdateTimer = 300;
                        return;
                    }
                }
            }
            else
            {
                return; // 正在平滑执行上一个避险航点，暂不打断
            }
        }

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

        // 戒律核心天赋：精神指引属神圣系天赋，戒律专精不注入，已移除。
        // 下列天赋缺失将导致盾量、回蓝与治疗增效全面亏模，团本高压期必然崩盘。
        SyncPassive(40, DisciplinePriestSpells::SOUL_WARDING);                   // 灵魂护体：盾 CD 归零 + 蓝耗 -30%
        SyncPassive(40, DisciplinePriestSpells::RAPTURE);                        // 狂喜：盾吸收/被驱散时返还法力
        SyncPassive(50, DisciplinePriestSpells::DIVINE_AEGIS);                   // 神圣庇护：暴击治疗转 30% 吸收盾
        SyncPassive(50, DisciplinePriestSpells::GRACE);                          // 恩赐：提升目标受到的治疗效果
        SyncPassive(60, DisciplinePriestSpells::BORROWED_TIME);                  // 争分夺秒：套盾后下个法术急速 +25%

        // 真言术：盾雕文：盾被打破时为目标附加治疗，预铺收益的核心放大器
        SyncPassive(60, DisciplinePriestSpells::GLYPH_OF_POWER_WORD_SHIELD);
    }
};

void AddSC_bot_discipline_priest()
{
    new AdaptiveBotScript<BotDisciplinePriestAI>("bot_discipline_priest");
}
