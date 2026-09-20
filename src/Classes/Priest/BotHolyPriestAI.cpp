/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "HolyPriestSpells.h"
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

class BotHolyPriestAI : public AdaptiveBotAI
{
    // =========================================================================
    // 团队状态快照：单帧「打地鼠」雷达采样结果
    // =========================================================================
    struct GroupSnapshot
    {
        std::vector<Unit*> allies;          // 40 码内、视线可达、存活的友方单位
        Unit* lowestHpAlly{ nullptr };      // 全队生命百分比最低成员 (治疗阶梯锚点，严格排除宠物)
        float lowestHpPct{ 100.0f };
        Unit* mainTank{ nullptr };          // 盾/减伤/恢复锚点：主坦，退化时取指挥官
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

public:
    explicit BotHolyPriestAI(Creature* creature) : AdaptiveBotAI(creature) {}

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
    // 注：暗影魔 (34433) / 希望圣歌 (64901) 均为基础法术而非天赋，不得登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case HolyPriestSpells::DESPERATE_PRAYER: return 20; // 绝望祷言 (自保底牌)
            case HolyPriestSpells::CIRCLE_OF_HEALING: return 50; // 治疗之环 (神圣核心群抬)
            case HolyPriestSpells::GUARDIAN_SPIRIT:  return 60; // 守护之魂 (神圣终极天赋)
            default:                                 return 0;
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

        // 全局读条/通道守卫：快速治疗/强效治疗术 (读条) 与 希望圣歌 (通道) 期间
        // 引擎均会置位 UNIT_STATE_CASTING，但引导类法术在部分状态下并不置位该标记，
        // 因此追加 CURRENT_CHANNELED_SPELL 显式判定形成双保险，
        // 杜绝希望圣歌引导被自身跟随移动指令 (UpdateFollowMaster / MoveFollow) 掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            // 脱战残血优先救命：绝不允许常驻增益抢占急救 GCD
            if (groupSnapshot.lowestHpPct < 90.0f)
            {
                if (TryLadderHeal()) return;
            }

            if (sweepDue)
            {
                if (MaintainInnerFire()) return;
                if (MaintainTeamBuffs()) return;
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
            me->Attack(victim, false); // 仅锚定敌对目标供暗影魔使用，绝不开启近战追击

        // ---- P0: 极限自保与战略免死急救 ----
        if (TrySelfPreservation()) return;
        if (TryEmergencyHeals()) return;

        // ---- P1: 常驻增益、群体爆发群抬与辅助 ----
        if (sweepDue)
        {
            if (MaintainInnerFire()) return;
            if (MaintainTeamBuffs()) return;
        }

        if (TryCircleOfHealing()) return;
        if (TryPrayerOfMending()) return;

        // ---- P2: 续航仲裁 ----
        if (MaintainManaCooldowns(victim)) return;

        // ---- P3: 阶梯式单体抬血与 HoT 维持 ----
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

        // 恢复/减伤锚点：优先主坦，无主坦时退化为指挥官；阵亡则回落指挥官
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

        // 宠物保留在 allies 名单内 (以便享受治疗之环/治疗祷言等溅射辅助)，
        // 但严禁用宠物血线触发全队恐慌：否则猎人宠物掉血会把 lowestHpAlly
        // 绑架到宠物身上，导致守护之魂、恢复、快速治疗全部错失真正的玩家与坦克。
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
        // 误触发「团队崩溃保护」从而永久锁死希望圣歌与暗影魔的续航判定。
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
            // 若不过滤会陷入「驱散 -> 补 buff -> 再驱散」的空蓝死循环。
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
    // P0-1: 极限自保 (渐隐脱身 / 濒死瞬发自救)
    // =========================================================================
    bool TrySelfPreservation()
    {
        // 被物理近战贴身且血线不稳：渐隐降低仇恨，把仇恨交还坦克。
        // 门禁收紧至 [45%, 85%)：< 45% 濒死期生存第一，渐隐不带任何减伤与
        // 吸收，丢在这里只会白烧一个救命 GCD，此时必须交给盾与绝望祷言。
        float const selfHpPct = me->GetHealthPct();
        if (selfHpPct >= 45.0f && selfHpPct < 85.0f && IsUnderPhysicalMelee(me))
        {
            uint32 const fade = GetAppropriateRank(HolyPriestSpells::FADE, false);
            if (fade && !me->HasAura(fade) && CanCast(me, fade, true) && ExecuteSpell(me, fade, true))
                return true;
        }

        if (me->GetHealthPct() >= 40.0f)
            return false;

        // 自身濒死：先瞬发自盾 (吸收) 争取生存窗口，再交给绝望祷言大抬。
        // 必须先校验灵魂虚弱，否则被灵魂虚弱压制期间会永久空转卡死这一层。
        uint32 const shield = GetAppropriateRank(HolyPriestSpells::POWER_WORD_SHIELD, false);
        if (shield && !me->HasAura(shield) && !me->HasAura(HolyPriestSpells::WEAKENED_SOUL))
        {
            if (CanCast(me, shield, true) && ExecuteSpell(me, shield, true))
                return true;
        }

        // 绝望祷言：瞬发、无读条、无移动惩罚的自疗大招，是唯一能在濒死期
        // 不牺牲机动性的自救底牌。已登记于 GetTalentSpellMinLevel，
        // 第二参数必须传 true，否则会被误判为基础技能而绕开天赋等级契约。
        uint32 const desperatePrayer = GetAppropriateRank(HolyPriestSpells::DESPERATE_PRAYER, true);
        if (desperatePrayer && !me->HasAura(desperatePrayer))
        {
            if (CanCast(me, desperatePrayer, true) && ExecuteSpell(me, desperatePrayer, true))
                return true;
        }

        // 绝望祷言冷却或未达等级时直接放行：决策流自然落入 P3 TryLadderHeal，
        // 由快速治疗/强效治疗术直接对自身执行高 HPS 治疗。
        return false;
    }

    // =========================================================================
    // P0-2: 极限急救 (守护之魂战略免死 + 濒死瞬发盾)
    // =========================================================================
    bool TryEmergencyHeals()
    {
        if (TryGuardianSpirit())
            return true;

        if (TryPanicShield())
            return true;

        return false;
    }

    // 守护之魂：3 分钟 CD 的战略免死底牌，40% 受疗加成 + 致死时免死一次。
    // 严禁对宠物施放：宠物可由主人复活，不值得消耗团队级救命资源。
    bool TryGuardianSpirit()
    {
        uint32 const guardianSpirit = GetAppropriateRank(HolyPriestSpells::GUARDIAN_SPIRIT, true);
        if (!guardianSpirit)
            return false;

        Unit* target = nullptr;

        // 首选主坦：承伤核心，免死收益最大
        if (groupSnapshot.mainTank && groupSnapshot.mainTank->IsAlive() &&
            !groupSnapshot.mainTank->ToPet() &&
            groupSnapshot.mainTank->GetHealthPct() < 30.0f &&
            IsValidHealTarget(groupSnapshot.mainTank))
        {
            target = groupSnapshot.mainTank;
        }
        else
        {
            // 主坦安全时降级扫描全队：仅对「真正承受致死压力」的成员施放
            for (Unit* ally : groupSnapshot.allies)
            {
                if (!ally || !ally->IsAlive() || ally->GetMap() != me->GetMap())
                    continue;

                if (ally->ToPet() || ally == groupSnapshot.mainTank)
                    continue;

                if (ally->GetHealthPct() >= 20.0f)
                    continue;

                if (!IsValidHealTarget(ally))
                    continue;

                // 承伤判定：物理贴身、仇恨列表非空或处于战斗中，任一成立即视为
                // 正承受致死压力 (兼容法系尖刺、远程与环境机制伤害)。
                if (!IsUnderPhysicalMelee(ally) && !ally->IsInCombat() && ally->getAttackers().empty())
                    continue;

                target = ally;
                break;
            }
        }

        if (!target)
            return false;

        if (target->HasAura(guardianSpirit))
            return false;

        if (!CanCast(target, guardianSpirit, true))
            return false;

        return ExecuteSpell(target, guardianSpirit, true);
    }

    // 濒死期瞬发预铺盾：用吸收量换取后续 1.5~3 秒读条窗口，
    // 避免目标在快速治疗/强效治疗术读条途中被单发尖刺伤害直接带走。
    bool TryPanicShield()
    {
        uint32 const shield = GetAppropriateRank(HolyPriestSpells::POWER_WORD_SHIELD, false);
        if (!shield)
            return false;

        Unit* target = groupSnapshot.lowestHpAlly;
        if (!target || groupSnapshot.lowestHpPct >= 35.0f)
            return false;

        if (!IsValidHealTarget(target) || target->ToPet())
            return false;

        // 灵魂虚弱期无法再次获得真言术：盾，必须严格校验，
        // 否则会陷入「缺盾 -> 补盾 -> 被灵魂虚弱吃下 -> 下一帧依旧缺盾」的无效空转。
        if (target->HasAura(shield) || target->HasAura(HolyPriestSpells::WEAKENED_SOUL))
            return false;

        if (!CanCast(target, shield, true))
            return false;

        return ExecuteSpell(target, shield, true);
    }

    // =========================================================================
    // P1-a: 治疗之环 (瞬发智能群抬，吃雕文 6 目标)
    // -------------------------------------------------------------------------
    // 以 lowestHpAlly 为锚点：环会自动选取锚点 15 码内最需要治疗的队友，
    // 在 2 人以上同时掉血时，单发瞬发即可替代多次单体读条，是神牧最高效的群抬。
    // 严禁在仅 1 人掉血时施放：单发环的治疗量分散，不如单体快速治疗集中兑现。
    // =========================================================================
    bool TryCircleOfHealing()
    {
        uint32 const circleOfHealing = GetAppropriateRank(HolyPriestSpells::CIRCLE_OF_HEALING, true);
        if (!circleOfHealing)
            return false;

        // 群抬收益门禁：至少 2 名非宠物成员低于 85% 血线
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

        // 治疗之环为瞬发法术，允许在跑位与抱坦避难途中直接施放，无需 StopMoving
        if (!CanCast(anchor, circleOfHealing, true))
            return false;

        return ExecuteSpell(anchor, circleOfHealing, true);
    }

    // =========================================================================
    // P1-b: 愈合祷言 (挂主坦智能弹跳)
    // =========================================================================
    bool TryPrayerOfMending()
    {
        uint32 const prayerOfMending = GetAppropriateRank(HolyPriestSpells::PRAYER_OF_MENDING, false);
        if (!prayerOfMending)
            return false;

        // 战时节流门禁：全队出现重伤 (< 60%) 时，愈合祷言作为「挂坦辅助弹跳技」，
        // 单跳治疗量低于快速治疗/强效治疗术，必须无条件让渡施法权给 P3 单体直接抬血，
        // 否则会出现「队友濒死、牧师却在给主坦补祷言」的治疗倒挂。
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 60.0f)
            return false;

        // 愈合祷言瞬发且智能弹跳，挂在主坦身上可最大化弹跳收益 (每次受击触发一跳)
        Unit* target = groupSnapshot.mainTank;
        if (!IsValidHealTarget(target) || target->ToPet())
            return false;

        // 防顶光环：愈合祷言全团同时只能存在一个弹跳实体，
        // 必须有任一成员持有即整轮跳过。若只校验主坦自身，会在坦克持盾、
        // 祷言却已弹跳到队友身上的场景下反复重放，白白烧掉救命 GCD。
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
    // P1-c: 常驻增益 (心灵之火 / 真言术：韧 / 神圣之灵)
    // =========================================================================
    bool MaintainInnerFire()
    {
        uint32 const innerFire = GetAppropriateRank(HolyPriestSpells::INNER_FIRE, false);
        if (!innerFire || me->HasAura(innerFire))
            return false;

        // 战时节流门禁：心灵之火属常驻增益，掉落后补挂不急于一时。
        // 战时只要有人掉血，补 buff 的 GCD 必须让渡给治疗通道。
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

        uint32 const fortitude = GetAppropriateRank(HolyPriestSpells::POWER_WORD_FORTITUDE, false);
        uint32 const divineSpirit = GetAppropriateRank(HolyPriestSpells::DIVINE_SPIRIT, false);

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
    // P2: 续航仲裁 (暗影魔 / 希望圣歌)
    // =========================================================================
    bool MaintainManaCooldowns(Unit* victim)
    {
        if (me->getPowerType() != POWER_MANA || !me->IsInCombat())
            return false;

        float const manaPct = me->GetPowerPct(POWER_MANA);

        // 暗影魔：法力枯竭时的主力回蓝手段 (66 级基础法术，第二参数传 false)。
        // 血线安全门禁：召唤需敌对目标承载且占用一个完整 GCD，必须在团队脱离
        // 濒死承压线后才允许交出，否则「队友正在暴毙，牧师却停下来招回蓝宠」
        // 会直接导致减员，回蓝收益远低于人命成本。
        if (manaPct < 40.0f && groupSnapshot.lowestHpPct >= 65.0f)
        {
            uint32 const shadowfiend = GetAppropriateRank(HolyPriestSpells::SHADOWFIEND, false);
            if (shadowfiend && victim && victim->IsAlive() &&
                victim->GetMap() == me->GetMap() && !victim->IsFriendlyTo(me))
            {
                if (CanCast(victim, shadowfiend, true) && ExecuteSpell(victim, shadowfiend, true))
                    return true;
            }
        }

        // 希望圣歌：法力见底且全队整体健康时引导回蓝。
        // 双门禁：平均血线达标只能证明「整体没崩」，不能证明「没人要死」。
        // 该技能为长达 8 秒的引导，期间随从完全丧失治疗能力，必须追加单体安全门禁：
        // 凡有任一成员跌破 70% 承压线 (尤其是主坦)，施法权无条件让渡给治疗通道，
        // 否则极易出现「全队均值健康、主坦濒死，牧师站桩引导 8 秒」的倒坦事故。
        // 引导技还必须刹停，否则移动中施法会被引擎以 SPELL_FAILED_MOVING 直接拒绝；
        // 刹停时机严格下沉至 CanCast 通过之后，避免校验失败白白放弃机动性。
        if (manaPct < 25.0f && groupSnapshot.averageHpPct >= 75.0f && groupSnapshot.lowestHpPct >= 70.0f)
        {
            uint32 const hymnOfHope = GetAppropriateRank(HolyPriestSpells::HYMN_OF_HOPE, false);
            if (hymnOfHope && !me->HasAura(hymnOfHope) && CanCast(me, hymnOfHope, true))
            {
                if (me->isMoving())
                    me->StopMoving();

                if (ExecuteSpell(me, hymnOfHope, true))
                    return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P3-a: 群体崩溃预警 (治疗祷言停步读条)
    // -------------------------------------------------------------------------
    // 单点阶梯治疗 (快速治疗) 在多成员同时掉血时会被 GCD 逐个击破，
    // 此时必须切入群体治疗通道。averageHpPct 是唯一能反映「全队整体崩坏」
    // 而非「单点尖刺」的采样值，不参与仲裁会令群体底牌永久闲置。
    // =========================================================================
    bool TryPrayerOfHealing()
    {
        // 安全门禁：全队最低血线 >= 65% 才允许停步读条。
        // 治疗祷言读条 3 秒，若仍有成员处于重伤承压线，这 3 秒足以让目标暴毙，
        // 此时施法权必须无条件让渡给 P0 瞬发急救与 P3 单体阶梯治疗。
        if (groupSnapshot.lowestHpPct < 65.0f)
            return false;

        // 触发门禁：全队平均血线跌入 75% 以下
        if (groupSnapshot.allies.size() < 3 || groupSnapshot.averageHpPct >= 75.0f)
            return false;

        uint32 const prayerOfHealing = GetAppropriateRank(HolyPriestSpells::PRAYER_OF_HEALING, false);
        if (!prayerOfHealing)
            return false;

        // 锚点必须落在承伤核心身上：治疗祷言只覆盖目标所属小队，
        // 以残血散人为锚点会漏掉坦克，背离群体急救的初衷。
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
    // P3-b: 单体治疗技能执行器
    // -------------------------------------------------------------------------
    // 恢复 (HoT)：瞬发，允许在跑位与抱坦避难途中直接施放。
    // 快速治疗 / 强效治疗术：读条，进入前必须立定。
    // =========================================================================
    bool TryRenew(Unit* target)
    {
        uint32 const renew = GetAppropriateRank(HolyPriestSpells::RENEW, false);
        if (!renew || !target || target->ToPet())
            return false;

        // 已存在恢复时不再刷新：神牧天赋不强化重复刷新收益，
        // 无脑重放只会白烧 GCD 拖慢后续急救。
        if (target->HasAura(renew))
            return false;

        if (!CanCast(target, renew, true))
            return false;

        // 瞬发 HoT，无需 StopMoving，保障跑位机动性
        return ExecuteSpell(target, renew, true);
    }

    bool TrySurgeFlashHeal(Unit* target)
    {
        // 圣光涌动【Proc】判定：天赋被动 (SURGE_OF_LIGHT_TALENT) 常驻注入自身，
        // 真正的施法资格必须校验「治疗暴击触发的临时 Proc 光环」(SURGE_OF_LIGHT_PROC)。
        // 若误把天赋被动当作施法条件，该条件将恒为真，随从会在无 Proc 时反复
        // 抢占 GCD 尝试瞬发快速治疗；若误判为 Proc 缺失又会永久空转，二者皆致命。
        if (!me->HasAura(HolyPriestSpells::SURGE_OF_LIGHT_PROC))
            return false;

        uint32 const flashHeal = GetAppropriateRank(HolyPriestSpells::FLASH_HEAL, false);
        if (!flashHeal || !target)
            return false;

        if (!CanCast(target, flashHeal, true))
            return false;

        // 圣光涌动使快速治疗变为瞬发，无需刹停
        return ExecuteSpell(target, flashHeal, true);
    }

    bool TryFlashHeal(Unit* target)
    {
        uint32 const flashHeal = GetAppropriateRank(HolyPriestSpells::FLASH_HEAL, false);
        if (!flashHeal || !target)
            return false;

        // 施法资格必须先通过校验再刹停：CanCast 失败 (GCD/被控/超距) 时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(target, flashHeal, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, flashHeal, true);
    }

    bool TryGreaterHeal(Unit* target, bool isSerendipity = false)
    {
        uint32 const greaterHeal = GetAppropriateRank(HolyPriestSpells::GREATER_HEAL, false);
        if (!greaterHeal || !target)
            return false;

        // 强效治疗术为 3 秒高耗蓝读条：仅在法力充裕时作为兜底，
        // 否则会抽空后续高压窗口的治疗余量。
        // 注：在持有 2 层好运 (SERENDIPITY_PROC) 时读条被压缩至约 1.8 秒且蓝耗降 20%，
        //     此时它由「兜底技」升格为单体最高 HPS 选择，属于救命性质施法，
        //     法力门禁必须相应放宽：若仍沿用 35% 常规门禁，会在 32% 蓝量的重伤窗口
        //     误拦截这发 1.8 秒急救大加，直接导致倒坦。
        float const manaGate = isSerendipity ? 15.0f : 35.0f;
        if (me->GetPowerPct(POWER_MANA) < manaGate)
            return false;

        if (!CanCast(target, greaterHeal, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, greaterHeal, true);
    }

    // =========================================================================
    // P3-b: 联结治疗 (自身与目标同时受益 + 叠加好运层数)
    // -------------------------------------------------------------------------
    // 联结治疗为 1.5 秒读条，一次施法同时治疗施法者与目标，并触发好运
    // (SERENDIPITY) 叠层。仅当「自身也在承压」时才有替换快速治疗的价值：
    //  + 自身健康时用它等于白付一份蓝耗，不如快速治疗集中治疗量给目标。
    // =========================================================================
    bool TryBindingHeal(Unit* target)
    {
        uint32 const bindingHeal = GetAppropriateRank(HolyPriestSpells::BINDING_HEAL, false);
        if (!bindingHeal || !target)
            return false;

        // 门禁一：目标即自身时，联结治疗退化为单体治疗，无联动收益，
        // 直接放行给快速治疗通道，避免无意义地拉长施法链条。
        if (target == me)
            return false;

        // 门禁二：仅在自身血线不稳 (< 75%) 时才用联结治疗替代快速治疗，
        // 兼顾「自疗 + 目标治疗」双收益并顺带叠加好运层数；
        // 自身健康时回归快速治疗，把蓝耗集中在最需要治疗的目标身上。
        if (me->GetHealthPct() >= 75.0f)
            return false;

        // 施法资格必须先通过校验再刹停：CanCast 失败时提前立定会让随从
        // 在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(target, bindingHeal, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, bindingHeal, true);
    }

    // =========================================================================
    // P3-c: 阶梯式治疗核心 (目标：lowestHpAlly)
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
        // 高危重伤 (< 60%)：瞬发底牌优先，最高优先级。
        // 圣光涌动 (免费瞬发快疗) -> 快速治疗读条 -> 强效治疗术兜底。
        // 群体抬血通道被强制排在本次分支之后：3 秒读条的治疗祷言绝不允许
        // 在单体濒死重伤期抢走救命 GCD。
        // ---------------------------------------------------------------------
        if (hpPct < 60.0f)
        {
            // 圣光涌动 Proc 优先：免费且瞬发，零 GCD 成本止血
            if (TrySurgeFlashHeal(target)) return true;

            // 好运联动：目标 < 55% 且已叠满 2 层好运 (SERENDIPITY_PROC) 时，
            // 强效治疗术读条被压缩至约 1.8 秒，等效「大治疗量 + 快疗速度」，
            // 是单体重伤期最高 HPS 的选择，必须前置优先。
            // 未叠满好运时严禁抢占：3 秒原速读条在重伤期足以让目标被尖刺带走。
            //
            // 层数检测必须走 Aura::GetStackAmount()：可叠加 Buff 在底层仅有一个
            // AuraApplication 实例，GetAuraCount() 恒返回 1，用其判 >= 2 会使
            // 好运联动永久失效，本分支彻底沦为死代码。
            Aura* serendipity = me->GetAura(HolyPriestSpells::SERENDIPITY_PROC);
            if (hpPct < 55.0f && serendipity && serendipity->GetStackAmount() >= 2 &&
                TryGreaterHeal(target, true))
            {
                return true;
            }

            // 联结治疗替代快速治疗：自身血线不稳时一次施法同时治疗自己与目标，
            // 并顺带叠加好运层数，为下一次强效治疗术铺路。
            if (TryBindingHeal(target)) return true;

            if (TryFlashHeal(target)) return true;

            // 兜底常规大加已被移除：本分支已由好运大加 (isSerendipity) 与
            // 快速治疗覆盖，此处再排一发不带好运的 3 秒原速大加毫无意义——
            // 走到这一步说明前两者均不可用 (好运不足或蓝量 < 15%)，
            // 该调用只会在 35% 蓝量门禁下被驳回，是永不执行的死代码。
            return false;
        }

        // ---------------------------------------------------------------------
        // 群体崩溃预警：必须在单体高危急救分支之后执行。
        // 内部设置 65% 安全门禁，仅在全员脱离重伤承压线时才允许停步读条。
        // ---------------------------------------------------------------------
        if (TryPrayerOfHealing())
            return true;

        // ---------------------------------------------------------------------
        // 中度掉血 (< 80%)：受损目标缺失【恢复】优先补【恢复】
        // (享受强化恢复额外加成与立即生效一跳)，再以快速治疗补足缺口。
        // ---------------------------------------------------------------------
        if (hpPct < 80.0f)
        {
            if (TryRenew(target)) return true;

            // 联结治疗优先替代快速治疗：中度掉血期自身常伴随 AoE 溅射损伤，
            // 一次施法同时覆盖自身与目标，并叠加好运层数为后续大加预铺。
            if (TryBindingHeal(target)) return true;

            if (TryFlashHeal(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 平稳修血 (80% ~ 90%)：主坦优先保持【恢复】滚动维持，受损目标补【恢复】，
        // 血线更低成员才允许投入快速治疗。此区间严禁动用强效治疗术：
        // 3 秒读条对 85% 血线目标几乎全部过量，且会白烧大量法力。
        // 快速治疗血线门禁 (< 90%)：90% 以上属「轻微掉血」，此时若目标已有恢复，
        // 单发快速治疗的过量治疗比例极高，且会拖延五秒规则下的精神回蓝重启。
        // ---------------------------------------------------------------------
        if (groupSnapshot.mainTank && groupSnapshot.mainTank->GetHealthPct() < 95.0f &&
            IsValidHealTarget(groupSnapshot.mainTank) && TryRenew(groupSnapshot.mainTank))
        {
            return true;
        }

        if (hpPct < 90.0f)
        {
            if (TryRenew(target)) return true;
            if (TryFlashHeal(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 极其安全 (>= 90%)：不打快速治疗，依靠已有 HoT 自行跳满，
        // 让法力进入精神回蓝通道。
        // 法力节流门禁：此区间目标并无生命危险，补恢复的收益远低于法力成本。
        // 残蓝 (法力 < 50%) 时必须直接放行，让随从停手进入五秒规则精神回蓝，
        // 否则会在 94% 血线的成员身上持续空耗法力，把高压窗口的治疗余量烧光。
        // ---------------------------------------------------------------------
        if (me->GetPowerPct(POWER_MANA) >= 50.0f && TryRenew(target))
            return true;

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
        // 严禁使用 !dispel 级联赋值：那会在学会驱散魔法后直接短路，
        // ABOLISH_DISEASE / CURE_DISEASE 永远得不到赋值，
        // 导致随从终生无法驱散疾病，遇到疾病类机制只能干看着队友掉血。
        uint32 const magicDispel = GetAppropriateRank(HolyPriestSpells::DISPEL_MAGIC, false);

        uint32 diseaseDispel = GetAppropriateRank(HolyPriestSpells::ABOLISH_DISEASE, false);
        if (!diseaseDispel)
            diseaseDispel = GetAppropriateRank(HolyPriestSpells::CURE_DISEASE, false);

        if (!magicDispel && !diseaseDispel)
            return false;

        SpellInfo const* magicDispelInfo = magicDispel ? sSpellMgr->GetSpellInfo(magicDispel) : nullptr;
        SpellInfo const* diseaseDispelInfo = diseaseDispel ? sSpellMgr->GetSpellInfo(diseaseDispel) : nullptr;

        if (!magicDispelInfo && !diseaseDispelInfo)
            return false;

        // 群体驱散：多名队友同时中招时以一发代替多次单体驱散，节省大量 GCD
        uint32 const massDispel = GetAppropriateRank(HolyPriestSpells::MASS_DISPEL, false);
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
                // 刹停时机严格下沉至 CanCast 通过之后。
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
            // 无魔法可驱时再回落疾病驱散通道。
            uint32 chosenDispel = 0;
            if (magicDispelInfo && HasDispellableDebuff(ally, magicDispelInfo))
                chosenDispel = magicDispel;
            else if (diseaseDispelInfo && HasDispellableDebuff(ally, diseaseDispelInfo))
                chosenDispel = diseaseDispel;

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

        SyncPassive(20, HolyPriestSpells::INSPIRATION);                 // 灵感：暴击治疗后目标受物理伤害 -10%
        SyncPassive(30, HolyPriestSpells::EMPOWERED_RENEW);            // 强化恢复：恢复额外加成 + 立即生效一跳
        SyncPassive(30, HolyPriestSpells::MEDITATION);                 // 冥想：施法中仍保持 50% 精神回蓝
        SyncPassive(30, HolyPriestSpells::SURGE_OF_LIGHT_TALENT);      // 圣光涌动【天赋】：注入后由治疗暴击触发 Proc
        SyncPassive(40, HolyPriestSpells::SPIRITUAL_GUIDANCE);         // 精神指引：25% 精神转法强
        SyncPassive(40, HolyPriestSpells::DIVINE_PROVIDENCE);          // 神圣天恩：环/祷言治疗量 +10%
        SyncPassive(50, HolyPriestSpells::SERENDIPITY);                // 好运：快疗/联结使下发大加/祷言读条 -20%
        SyncPassive(50, HolyPriestSpells::GLYPH_OF_CIRCLE_OF_HEALING); // 治疗之环雕文：目标数量增至 6 个
        SyncPassive(60, HolyPriestSpells::GLYPH_OF_PRAYER_OF_HEALING); // 治疗祷言雕文：附加持续 HoT
    }
};

void AddSC_bot_holy_priest()
{
    new AdaptiveBotScript<BotHolyPriestAI>("bot_holy_priest");
}
