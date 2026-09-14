/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "RestorationShamanSpells.h"
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

class BotRestorationShamanAI : public AdaptiveBotAI
{
    // =========================================================================
    // 团队状态快照：单帧「打地鼠」雷达采样结果 (与 BotHolyPriestAI 完全同构)
    // =========================================================================
    struct GroupSnapshot
    {
        std::vector<Unit*> allies;          // 40 码内、视线可达、存活的友方单位
        Unit* lowestHpAlly{ nullptr };      // 全队生命百分比最低成员 (治疗阶梯锚点，严格排除宠物)
        float lowestHpPct{ 100.0f };
        Unit* mainTank{ nullptr };          // 大地之盾/激流锚点：主坦，退化时取指挥官
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

    // 图腾矩阵参数
    static constexpr uint8  TOTEM_COUNT           = 4;
    static constexpr uint32 TOTEM_CAST_INTERVAL   = 2000;    // 图腾插拔最小间隔 (ms)
    static constexpr uint32 TOTEM_REFRESH_TIME    = 100000;  // 图腾矩阵重建周期 (ms)
    static constexpr float  TOTEM_REPOSITION_DIST = 25.0f;   // 漂移超过此距离需重播图腾

    // 自管冷却登记 (Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的
    // 瞬发 CD 技能必须由专精自行计时，否则会因 HasSpellCooldown 恒 false 而空转)
    static constexpr uint32 CD_RIPTIDE           = 6000;
    static constexpr uint32 CD_NATURES_SWIFTNESS = 120000;
    static constexpr uint32 CD_TIDAL_FORCE       = 180000;
    static constexpr uint32 CD_MANA_TIDE_TOTEM   = 300000;

public:
    explicit BotRestorationShamanAI(Creature* creature) : AdaptiveBotAI(creature) {}

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
    // 注：基础图腾 (四大元素) 与 CURE_TOXINS 均为非天赋法术，严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case RestorationShamanSpells::NATURES_SWIFTNESS: return 30; // 自然迅捷
            case RestorationShamanSpells::MANA_TIDE_TOTEM:   return 40; // 法力之潮图腾
            case RestorationShamanSpells::CLEANSE_SPIRIT:    return 40; // 净化灵魂
            case RestorationShamanSpells::TIDAL_FORCE:       return 50; // 潮汐之力
            case RestorationShamanSpells::EARTH_SHIELD:      return 50; // 大地之盾
            case RestorationShamanSpells::RIPTIDE:           return 60; // 激流 (恢复终极天赋)
            default:                                         return 0;
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
        ResetShamanTimers();
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
        UpdateShamanTimers(diff);

        if (buffSweepTimer <= diff)
            buffSweepTimer = BUFF_SWEEP_INTERVAL;
        else
            buffSweepTimer -= diff;

        bool const sweepDue = (buffSweepTimer == BUFF_SWEEP_INTERVAL);

        // 打地鼠雷达：每帧刷新队友状态快照
        RefreshGroupSnapshot();

        // 全局读条/通道双保险守卫：治疗波/治疗链 (读条) 期间引擎会置位
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

            // 脱战残血优先救命：绝不允许常驻增益与图腾抢占急救 GCD
            if (groupSnapshot.lowestHpPct < 90.0f)
            {
                if (TryLadderHeal()) return;
            }

            if (sweepDue)
            {
                if (MaintainWaterShield()) return;
                if (MaintainEarthShield()) return;
            }

            if (MaintainTotems()) return;

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

        // ---- P0: 极限急救 (自然迅捷 + 瞬发治疗波原子连招) ----
        if (TryEmergencyHeals()) return;

        // ---- P1: 常驻护盾与图腾矩阵维持 ----
        if (sweepDue)
        {
            if (MaintainWaterShield()) return;
            if (MaintainEarthShield()) return;
        }

        if (MaintainTotems()) return;

        // ---- P2: 爆发与续航仲裁 ----
        if (MaintainCooldowns()) return;

        // ---- P3: 阶梯式抬血与激流循环 ----
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
    uint32 riptideCooldown{ 0 };
    uint32 naturesSwiftnessCooldown{ 0 };
    uint32 tidalForceCooldown{ 0 };
    uint32 manaTideCooldown{ 0 };

    // 图腾矩阵状态
    uint8  totemDeployIndex{ 0 };   // 0..TOTEM_COUNT-1 为展开游标，>=TOTEM_COUNT 表示已铺满
    bool   hasTotemPos{ false };
    float  lastTotemX{ 0.0f };
    float  lastTotemY{ 0.0f };
    float  lastTotemZ{ 0.0f };
    uint32 totemCastCooldown{ 0 };
    uint32 totemRefreshTimer{ 0 };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateShamanTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(riptideCooldown);
        Tick(naturesSwiftnessCooldown);
        Tick(tidalForceCooldown);
        Tick(manaTideCooldown);
        Tick(totemCastCooldown);
        Tick(totemRefreshTimer);
    }

    void ResetShamanTimers()
    {
        riptideCooldown = 0;
        naturesSwiftnessCooldown = 0;
        tidalForceCooldown = 0;
        manaTideCooldown = 0;

        totemDeployIndex = 0;
        hasTotemPos = false;
        lastTotemX = 0.0f;
        lastTotemY = 0.0f;
        lastTotemZ = 0.0f;
        totemCastCooldown = 0;
        totemRefreshTimer = 0;
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

        // 护盾/减伤锚点：优先主坦，无主坦时退化为指挥官；阵亡则回落指挥官
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

        // 宠物保留在 allies 名单内 (以便享受治疗链跳跃等溅射辅助)，
        // 但严禁用宠物血线触发全队恐慌：否则猎人宠物掉血会把 lowestHpAlly
        // 绑架到宠物身上，导致大地之盾、激流、治疗波全部错失真正的玩家与坦克。
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
        // 误触发「团队崩溃保护」从而永久锁死潮汐之力与法力之潮的续航判定。
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

    // =========================================================================
    // 驱散掩码解析 (萨满专用)
    // -------------------------------------------------------------------------
    // 严禁直接使用 SpellInfo::CanDispelAura：Cleanse Spirit 这类「一次解多类型」
    // 的法术在 DBC 中 Dispel 字段为 0 (类型由 SPELL_EFFECT_DISPEL 的 MiscValue
    // 逐条声明)，沿用它会使驱散判定恒为假，随从终生不驱散。
    // 由效果掩码解析可精确还原「诅咒/疾病/中毒」三系，且天然排除 DISPEL_MAGIC，
    // 从底层杜绝萨满误驱魔法。
    // =========================================================================
    uint32 GetShamanDispelMask(SpellInfo const* dispelInfo) const
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

            // 严格排除正面增益：大地之盾、水之护盾、法力之泉等自身即为可驱散类型，
            // 若不过滤会陷入「驱散 -> 补盾 -> 再驱散」的空蓝死循环。
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

    // =========================================================================
    // P0: 极限急救 (自然迅捷 + 瞬发治疗波原子连招)
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
        Unit* target = SelectEmergencyTarget();
        if (!target)
            return false;

        // 情形一：自然迅捷已激活 —— 治疗波变为瞬发，当帧直接完成极限救急
        if (me->HasAura(RestorationShamanSpells::NATURES_SWIFTNESS))
            return TryHealingWave(target, true);

        // 情形二：尝试激活自然迅捷 (Off-GCD，2 分钟自家冷却)
        uint32 const naturesSwiftness = GetAppropriateRank(RestorationShamanSpells::NATURES_SWIFTNESS, true);
        if (!naturesSwiftness || naturesSwiftnessCooldown > 0)
            return false;

        if (!CanCast(me, naturesSwiftness, true) || !ExecuteSpell(me, naturesSwiftness, true))
            return false;

        naturesSwiftnessCooldown = CD_NATURES_SWIFTNESS;

        // Off-GCD 铁律：自然迅捷不占公共冷却，严禁在此无条件 return true。
        // 必须允许当帧决策流顺下，让自然迅捷光环当帧立即被后续治疗波吃下；
        // 若光环尚未可见 (极端情形)，则返回 false 交由下一帧「情形一」兜底。
        if (me->HasAura(RestorationShamanSpells::NATURES_SWIFTNESS))
            return TryHealingWave(target, true);

        return false;
    }

    // =========================================================================
    // P1-a: 水之护盾 (自身常驻回蓝)
    // =========================================================================
    bool MaintainWaterShield()
    {
        uint32 const waterShield = GetAppropriateRank(RestorationShamanSpells::WATER_SHIELD, false);
        if (!waterShield || me->HasAura(waterShield))
            return false;

        // 战时节流门禁：水之护盾属续航常驻增益，战时只要有人掉血，
        // 补盾的 GCD 必须无条件让渡给治疗通道。
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 80.0f)
            return false;

        return CanCast(me, waterShield, true) && ExecuteSpell(me, waterShield, true);
    }

    // =========================================================================
    // P1-b: 大地之盾 (严格只对主坦维持)
    // =========================================================================
    bool MaintainEarthShield()
    {
        uint32 const earthShield = GetAppropriateRank(RestorationShamanSpells::EARTH_SHIELD, true);
        if (!earthShield)
            return false;

        Unit* tank = groupSnapshot.mainTank;

        // 排己判定 (致命)：单人 / 队伍退化 (无独立主坦) 时 anchor 会退化为指挥官或自身，
        // 若不做 tank == me 排除，大地之盾会与自身的水之护盾互顶。
        // 恢复萨满自身必须恒定享受水之护盾回蓝，大地之盾只为他人主坦维持。
        if (!tank || tank == me || !tank->IsAlive() || tank->ToPet() || !IsValidHealTarget(tank))
            return false;

        // 防顶层数浪费：剩余充能 > 2 时覆盖重放纯属白烧 GCD，必须整轮跳过。
        // 致命坑位：大地之盾在 3.3.5a 属 ProcCharges 充能型光环 (初始 9 次)，
        // 其底层并不使用 StackAmount，Aura::GetStackAmount() 恒返回 1，
        // 用其判 > 2 将使门禁永久失效并触发每 3 秒一次的无脑空耗刷新。
        // 必须改走 Aura::GetCharges() 才能真实反映剩余可触发次数。
        if (Aura* aura = tank->GetAura(earthShield))
        {
            if (aura->GetCharges() > 2)
                return false;
        }

        // 战时节流门禁：全队脱离重伤承压线后才允许补盾
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 65.0f)
            return false;

        if (!CanCast(tank, earthShield, true))
            return false;

        return ExecuteSpell(tank, earthShield, true);
    }

    // =========================================================================
    // P1-c: 战斗图腾矩阵 (四大元素平滑展开)
    // =========================================================================
    uint32 GetTotemSpellForIndex(uint8 index) const
    {
        switch (index)
        {
            case 0:  return GetAppropriateRank(RestorationShamanSpells::STRENGTH_OF_EARTH_TOTEM, false);
            case 1:  return GetAppropriateRank(RestorationShamanSpells::FLAMETONGUE_TOTEM, false);
            case 2:  return GetAppropriateRank(RestorationShamanSpells::MANA_SPRING_TOTEM, false);
            case 3:  return GetAppropriateRank(RestorationShamanSpells::WRATH_OF_AIR_TOTEM, false);
            default: return 0;
        }
    }

    bool MaintainTotems()
    {
        // 防 GCD 窒息门禁：仅脱战，或战时全队最低血线 >= 80% 极其健康时才允许插图腾。
        // 图腾收益是长效增强，绝不能与救命治疗抢占同帧施法权。
        if (me->IsInCombat() && groupSnapshot.lowestHpPct < 80.0f)
            return false;

        // 图腾「消失」以自管刷新计时为代理信号：图腾 NPC 的存活无法通过
        // 可靠且稳定的引擎 API 逐槽判定，改用「重建周期 (100s) + 位置漂移」双触发，
        // 既覆盖图腾到期自然消灭，也覆盖跑位脱节，且绝不会产生每帧重播的死循环。
        bool const deployIncomplete = (totemDeployIndex > 0 && totemDeployIndex < TOTEM_COUNT);
        bool const drifted = hasTotemPos
                          && me->GetDistance(lastTotemX, lastTotemY, lastTotemZ) > TOTEM_REPOSITION_DIST;

        bool const needRedeploy = deployIncomplete
                               || !hasTotemPos
                               || (totemRefreshTimer == 0)
                               || drifted;

        if (!needRedeploy)
            return false;

        // 已完成矩阵但被位移/超时触发重播：重置展开游标，从头顺次补齐
        if (hasTotemPos && totemDeployIndex >= TOTEM_COUNT)
            totemDeployIndex = 0;

        // 平滑展开节流：间隔不足 2000ms 一律不放行，严禁单帧连续打出多个图腾
        if (totemCastCooldown > 0)
            return false;

        // 矩阵铺满：落定坐标快照与重建周期
        if (totemDeployIndex >= TOTEM_COUNT)
        {
            hasTotemPos = true;
            lastTotemX = me->GetPositionX();
            lastTotemY = me->GetPositionY();
            lastTotemZ = me->GetPositionZ();
            totemRefreshTimer = TOTEM_REFRESH_TIME;
            return false;
        }

        uint32 const totemSpellId = GetTotemSpellForIndex(totemDeployIndex);

        // 等级不足该图腾：推进游标避免死循环
        if (!totemSpellId)
        {
            ++totemDeployIndex;
            return false;
        }

        bool const casted = CanCast(me, totemSpellId, true) && ExecuteSpell(me, totemSpellId, true);

        // 无论成败均推进游标并压上节流：施法失败 (缺法力/被控) 时若原地重试，
        // 会让随从在每一帧反复空转同一个图腾，彻底饿死治疗通道。
        ++totemDeployIndex;
        totemCastCooldown = TOTEM_CAST_INTERVAL;

        return casted;
    }

    // =========================================================================
    // P2: 爆发与续航仲裁 (潮汐之力 / 法力之潮图腾)
    // =========================================================================
    bool MaintainCooldowns()
    {
        if (!me->IsInCombat())
            return false;

        // ---- 潮汐之力：全队平均血线 < 70% 且至少 3 人受损时开启 ----
        // Off-GCD 技能：施放后严禁 return true，必须允许当帧决策流顺下，
        // 使 60% 暴击率增益能被紧随其后的治疗链/治疗波当帧吃下。
        uint32 const tidalForce = GetAppropriateRank(RestorationShamanSpells::TIDAL_FORCE, true);
        if (tidalForce && tidalForceCooldown == 0 && !me->HasAura(tidalForce))
        {
            uint32 injuredCount = 0;
            for (Unit* ally : groupSnapshot.allies)
            {
                if (!ally || ally->ToPet())
                    continue;

                if (!IsValidHealTarget(ally))
                    continue;

                if (ally->GetHealthPct() < 90.0f)
                    ++injuredCount;
            }

            if (injuredCount >= 3 && groupSnapshot.averageHpPct < 70.0f)
            {
                if (CanCast(me, tidalForce, true) && ExecuteSpell(me, tidalForce, true))
                {
                    tidalForceCooldown = CD_TIDAL_FORCE;
                    // 故意不 return：Off-GCD，继续评估后续续航手段
                }
            }
        }

        // ---- 法力之潮图腾：回蓝需占用完整 GCD，血线安全门禁必不可少 ----
        if (me->getPowerType() == POWER_MANA &&
            me->GetPowerPct(POWER_MANA) < 45.0f &&
            groupSnapshot.lowestHpPct >= 65.0f)
        {
            uint32 const manaTide = GetAppropriateRank(RestorationShamanSpells::MANA_TIDE_TOTEM, true);
            if (manaTide && manaTideCooldown == 0 && CanCast(me, manaTide, true))
            {
                // 需自管冷却：Curse of the Elements 般的引擎 CD 对 Creature 不生效
                if (me->isMoving())
                    me->StopMoving();

                if (ExecuteSpell(me, manaTide, true))
                {
                    manaTideCooldown = CD_MANA_TIDE_TOTEM;
                    return true;
                }
            }
        }

        return false;
    }

    // =========================================================================
    // P3-a: 团队智能群抬 (治疗链)
    // -------------------------------------------------------------------------
    // 以 lowestHpAlly 为锚点：治疗链会自动向锚点附近最需要治疗的队友逐跳扩散，
    // 在 2 人以上同时掉血时，单发读条即可替代多次单体治疗，
    // 并触发潮汐奔涌叠层，是恢复萨满最高效的团补手段。
    // 严禁在仅 1 人掉血时施放：链子治疗量分散，不如单体次级治疗波集中兑现。
    // =========================================================================
    bool TryChainHeal()
    {
        // 门禁一：无人处于濒死猝死线 (< 55%)
        // 治疗链读条 2.5 秒，若仍有成员处于重伤承压线，这 2.5 秒足以让目标暴毙，
        // 此时施法权必须无条件让渡给 P0 瞬发急救与单体阶梯治疗。
        if (groupSnapshot.lowestHpPct < 55.0f)
            return false;

        uint32 const chainHeal = GetAppropriateRank(RestorationShamanSpells::CHAIN_HEAL, false);
        if (!chainHeal)
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

        if (!CanCast(anchor, chainHeal, true))
            return false;

        // 读条技刹停精准下沉至校验通过之后：CanCast 失败时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(anchor, chainHeal, true);
    }

    // =========================================================================
    // P3-b: 激流 (瞬发止血 + 潮汐奔涌叠层)
    // =========================================================================
    bool TryRiptide(Unit* target)
    {
        uint32 const riptide = GetAppropriateRank(RestorationShamanSpells::RIPTIDE, true);
        if (!riptide || !target || target->ToPet())
            return false;

        if (!IsValidHealTarget(target))
            return false;

        // 引擎不追踪 Creature 冷却，故用「自家冷却 + HoT 存在性」双闸门替代：
        // 目标已持激流 HoT 时不再刷新 (收益低于占用 GCD)，交由后续大加通道处理。
        if (riptideCooldown > 0 || target->HasAura(riptide))
            return false;

        if (!CanCast(target, riptide, true))
            return false;

        // 瞬发 HoT：允许在跑位与抱坦避难途中直接施放，无需 StopMoving
        if (ExecuteSpell(target, riptide, true))
        {
            riptideCooldown = CD_RIPTIDE;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P3-c: 单体治疗技能执行器
    // -------------------------------------------------------------------------
    // 治疗波 (isInstant = true 表示自然迅捷已就绪)：瞬发，允许跑位施放。
    // 治疗波 / 次级治疗波 (默认)：读条，进入前必须立定。
    // =========================================================================
    bool TryHealingWave(Unit* target, bool isInstant = false)
    {
        uint32 const healingWave = GetAppropriateRank(RestorationShamanSpells::HEALING_WAVE, false);
        if (!healingWave || !target)
            return false;

        if (!CanCast(target, healingWave, true))
            return false;

        // 自然迅捷使治疗波瞬发：严禁 StopMoving，保障极限救急期的机动性。
        // (ExecuteSpell 内部亦会因 CalcCastTime 归零而跳过刹停，此处显式跳过形成双保险)
        if (!isInstant && me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, healingWave, true);
    }

    bool TryLesserHealingWave(Unit* target)
    {
        uint32 const lesserHealingWave = GetAppropriateRank(RestorationShamanSpells::LESSER_HEALING_WAVE, false);
        if (!lesserHealingWave || !target)
            return false;

        // 施法资格必须先通过校验再刹停：CanCast 失败 (GCD/被控/超距) 时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(target, lesserHealingWave, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(target, lesserHealingWave, true);
    }

    // =========================================================================
    // P3-d: 阶梯式治疗核心 (目标：lowestHpAlly)
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
        // 激流 (瞬发止血 + 2 层潮汐奔涌) -> 潮汐奔涌加持的治疗波 -> 次级治疗波。
        // 群体抬血通道被强制排在本次分支之后：2.5 秒读条的治疗链绝不允许
        // 在单体濒死重伤期抢走救命 GCD。
        // ---------------------------------------------------------------------
        if (hpPct < 60.0f)
        {
            // 1. 激流优先：瞬发直接治疗 + 立即产出 2 层潮汐奔涌，为后续大加铺路
            if (TryRiptide(target)) return true;

            // 2. 持有潮汐奔涌且法力尚可：治疗波读条缩减 30%，
            //    等效「大治疗量 + 快疗速度」，是单体重伤期最高 HPS 的选择。
            //    层数检测必须走 Aura::GetStackAmount()：可叠加 Buff 在底层仅有一个
            //    AuraApplication 实例，GetAuraCount() 恒返回 1，用其判层会使本分支
            //    彻底沦为死代码，潮汐联动永久失效。
            Aura* tidalWaves = me->GetAura(RestorationShamanSpells::TIDAL_WAVES_PROC);
            if (tidalWaves && tidalWaves->GetStackAmount() >= 1 && me->GetPowerPct(POWER_MANA) >= 20.0f)
            {
                if (TryHealingWave(target)) return true;
            }

            // 3. 快速读条次级治疗波兜底 (蓝量 < 20% 或潮汐奔涌缺失时唯一可行通道)
            if (TryLesserHealingWave(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 团队智能群抬：必须在单体高危急救分支之后执行。
        // 内部设置 55% 安全门禁，仅在无人濒死时才允许停步读条治疗链。
        // ---------------------------------------------------------------------
        if (TryChainHeal())
            return true;

        // ---------------------------------------------------------------------
        // 中度掉血 (60% ~ 80%)：激流 (瞬发，顺带叠潮汐奔涌) -> 次级治疗波补缺口。
        // 治疗链已在上一梯队尝试，此处不再重复调用以免浪费判定开销。
        // ---------------------------------------------------------------------
        if (hpPct < 80.0f)
        {
            if (TryRiptide(target)) return true;
            if (TryLesserHealingWave(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 平稳修血 (80% ~ 90%)：以激流 HoT 覆盖受损目标，次级治疗波收尾。
        // 此区间严禁动用治疗波：3 秒高耗蓝读条对 85% 血线目标几乎全部过量。
        // ---------------------------------------------------------------------
        if (hpPct < 90.0f)
        {
            if (TryRiptide(target)) return true;
            if (TryLesserHealingWave(target)) return true;

            return false;
        }

        // ---------------------------------------------------------------------
        // 极其安全 (>= 90%)：不打高耗蓝读条，依靠已有 HoT 自行跳满，
        // 让法力进入五秒规则精神回蓝 / 水之护盾回蓝通道。
        // ---------------------------------------------------------------------
        return false;
    }

    // =========================================================================
    // P4: 驱散 (净化灵魂优先，基础消毒术兜底)
    // -------------------------------------------------------------------------
    // 萨满只能驱散诅咒 / 疾病 / 中毒三类，绝不能驱散魔法：
    // 掩码由 SPELL_EFFECT_DISPEL 效果解析得出，从底层保证不含 DISPEL_MAGIC。
    // =========================================================================
    bool TryCleanse()
    {
        if (!groupSnapshot.lowestHpAlly || groupSnapshot.lowestHpPct < 90.0f)
            return false;

        // 两个驱散法术必须独立获取。
        // 严禁使用 !dispel 级联赋值：那会在学会净化灵魂后直接短路，
        // CURE_TOXINS 永远得不到赋值，导致低等级随从终生无法驱散。
        uint32 const cleanseSpirit = GetAppropriateRank(RestorationShamanSpells::CLEANSE_SPIRIT, true);
        uint32 const cureToxins    = GetAppropriateRank(RestorationShamanSpells::CURE_TOXINS, false);

        if (!cleanseSpirit && !cureToxins)
            return false;

        SpellInfo const* cleanseSpiritInfo = cleanseSpirit ? sSpellMgr->GetSpellInfo(cleanseSpirit) : nullptr;
        SpellInfo const* cureToxinsInfo    = cureToxins    ? sSpellMgr->GetSpellInfo(cureToxins)    : nullptr;

        if (!cleanseSpiritInfo && !cureToxinsInfo)
            return false;

        uint32 const cleanseSpiritMask = GetShamanDispelMask(cleanseSpiritInfo);
        uint32 const cureToxinsMask    = GetShamanDispelMask(cureToxinsInfo);

        for (Unit* ally : groupSnapshot.allies)
        {
            if (!IsValidHealTarget(ally) || ally->ToPet())
                continue;

            // 逐目标择法：40 级天赋净化灵魂可解诅咒/疾病/中毒，优先；
            // 仅有基础消毒术 (< 40 级) 时只解疾病/中毒。
            // 若目标仅中诅咒而尚未学会净化灵魂，则掩码不匹配 -> chosenDispel 保持 0
            // -> continue 跳过，严禁空放消毒术浪费 GCD。
            uint32 chosenDispel = 0;
            if (cleanseSpirit && HasDispellableDebuff(ally, cleanseSpiritMask))
                chosenDispel = cleanseSpirit;
            else if (cureToxins && HasDispellableDebuff(ally, cureToxinsMask))
                chosenDispel = cureToxins;

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

        SyncPassive(20, RestorationShamanSpells::GLYPH_OF_CHAIN_HEAL);   // 治疗链雕文：跳跃目标 +1 (共 4 目标)
        SyncPassive(20, RestorationShamanSpells::GLYPH_OF_EARTH_SHIELD); // 大地之盾雕文：大地之盾治疗量 +20%
        SyncPassive(30, RestorationShamanSpells::MANA_SPRING);          // 强化法力之泉：法力之泉图腾回蓝 + 图腾收益
        SyncPassive(35, RestorationShamanSpells::PURIFICATION);         // 净化：治疗效果 +10%
        SyncPassive(40, RestorationShamanSpells::ANCESTRAL_AWAKENING);  // 先祖复苏：暴击治疗瞬发折射治疗
        SyncPassive(45, RestorationShamanSpells::HEALING_WAY);          // 治疗之道：治疗波增效
        SyncPassive(50, RestorationShamanSpells::TIDAL_WAVES_TALENT);   // 潮汐奔涌【天赋】：注入后由激流/链子触发 Proc
    }
};

void AddSC_bot_restoration_shaman()
{
    new AdaptiveBotScript<BotRestorationShamanAI>("bot_restoration_shaman");
}
