/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "FrostMageSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Chat.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

class BotFrostMageAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数 (铁律 17 迟滞区间 / 铁律 36 射程收敛)
    // -------------------------------------------------------------------------
    // 冰霜法主力通道 (寒冰箭 / 冰枪术 / 霜火之箭) 与法系标准远程一致,
    // 不存在短射程填充技拘束, 故最大交战距离维持 35 码, 理想站桩位 28 码。
    // =========================================================================
    static constexpr float DEADZONE_RETREAT_DIST = 8.0f;   // 近战盲区撤退进入线
    static constexpr float MIN_ENGAGE_DIST       = 15.0f;  // 安全站桩读条下限线
    static constexpr float RETREAT_SAFE_DIST     = 16.0f;  // 撤退姿态退出安全线 (必须 > 进入线)
    static constexpr float MAX_ENGAGE_DIST       = 35.0f;  // 最大交战距离 (脱节上限)
    static constexpr float IDEAL_SHOT_DIST       = 28.0f;  // 理想施法站位

    // 冰锥术正面锥形有效半径 (以施法者为原点结算)
    static constexpr float CONE_OF_COLD_DIST     = 10.0f;

    // 撤离锚定坦克背身位时的跟随距离: 必须 >= RETREAT_SAFE_DIST。
    // 若沿用 6~8 码贴坦, 随从与 Boss 的间距仍落在近战盲区内,
    // 会持续反复触发撤退与闪现, 永远无法恢复 15 码外的施法站位。
    static constexpr float TANK_RETREAT_DIST = RETREAT_SAFE_DIST;

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」, 引擎内部已叠加目标朝向。
    // 严禁自行叠加 tank->GetOrientation(), 否则站位会随坦克转向持续漂移。
    // M_PI 即锚点正后方背身位, 可规避顺劈斩与正面吐息。
    static constexpr float BEHIND_ANGLE = static_cast<float>(M_PI);

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪 (HasSpellCooldown 恒 false),
    // 凡无「持续光环保护」的 CD 技能必须由专精自行计时,
    // 否则会因 CD 判定恒假而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_DEEP_FREEZE      = 30000;
    static constexpr uint32 CD_ICE_BARRIER      = 30000;
    static constexpr uint32 CD_ICY_VEINS        = 144000;
    static constexpr uint32 CD_COLD_SNAP        = 480000;
    static constexpr uint32 CD_CONE_OF_COLD     = 10000;
    static constexpr uint32 CD_MIRROR_IMAGE     = 180000;
    static constexpr uint32 CD_EVOCATION        = 240000;
    static constexpr uint32 CD_MANA_GEM         = 120000;
    static constexpr uint32 CD_ICE_BLOCK        = 300000;
    static constexpr uint32 CD_INVISIBILITY     = 180000;
    static constexpr uint32 CD_BLINK            = 15000;
    static constexpr uint32 CD_FROST_NOVA       = 25000;
    static constexpr uint32 CD_COUNTERSPELL     = 24000;

    // 生命与法力阈值
    static constexpr float ICE_BLOCK_HP_PCT    = 20.0f;
    static constexpr float MANA_GEM_USE_PCT    = 75.0f;
    static constexpr float EVOCATION_MANA_PCT  = 15.0f;

    // 冰箱 / 隐形的硬性超时与最小保护期兜底 (铁律 21)
    static constexpr uint32 ICE_BLOCK_TIMEOUT_MS    = 2500;
    static constexpr uint32 ICE_BLOCK_MIN_HOLD_MS   = 1500;
    static constexpr float  ICE_BLOCK_SAFE_HP_PCT   = 50.0f;
    static constexpr uint32 INVISIBILITY_TIMEOUT_MS = 3000;

    // 法力宝石的最早可习得等级: 低于此等级宝石自管冷却恒为 0,
    // 唤醒若以「宝石已进入 CD」为前置将永久无法引导, 形成低级唤醒死锁。
    static constexpr uint8 MANA_GEM_MIN_LEVEL = 28;

    // 满阶被动天赋与雕文解锁等级 (严格注入满阶 Rank 根源, 铁律 33)
    static constexpr uint8 LEVEL_GLYPH               = 20;
    static constexpr uint8 LEVEL_SHATTER             = 15;
    static constexpr uint8 LEVEL_ICE_SHARDS          = 15;
    static constexpr uint8 LEVEL_PIERCING_ICE        = 20;
    static constexpr uint8 LEVEL_FINGERS_OF_FROST    = 25;
    static constexpr uint8 LEVEL_EMPOWERED_FROSTBOLT = 25;
    static constexpr uint8 LEVEL_BRAIN_FREEZE        = 30;
    static constexpr uint8 LEVEL_ARCTIC_WINDS        = 35;
    static constexpr uint8 LEVEL_WINTERS_CHILL       = 40;
    static constexpr uint8 LEVEL_PRECISION           = 45;

public:
    explicit BotFrostMageAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 远程随从按远程单位接管移动逻辑, 禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 法系远程 (与猎人物理远程解耦): 伤害由法术强度与冰系乘数通道支撑
    bool IsRangedPhysicalBot() const override { return false; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注: 3.3.5a 中纯天赋技能 DBC SpellLevel 恒为 0, GetAppropriateRank 无法降阶,
    //     必须在此登记最低解锁等级并在施法前显式门禁。
    //     基础法术 (寒冰箭/冰枪/冰锥/护甲/唤醒/宝石/冰箱/隐形/闪现/冰环/反制/镜像/霜火之箭)
    //     严禁登记于此, 其等级门槛由 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case FrostMageSpells::ICY_VEINS:    return 30;
            case FrostMageSpells::COLD_SNAP:    return 40;
            case FrostMageSpells::ICE_BARRIER:  return 40;
            case FrostMageSpells::DEEP_FREEZE:  return 60;
            default:                            return 0;
        }
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 法力通道必须先于基类 Reset 完成配置, 保证等级同步走法力分支
        me->setPowerType(POWER_MANA);

        AdaptiveBotAI::Reset();

        ResetMageTimers();
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
        UpdateMageTimers(diff);

        // 全局读条/引导双保险守卫 (铁律 1): 读条期间引擎置位 UNIT_STATE_CASTING,
        // 引导类法术 (唤醒 / 长通道) 在部分状态下不置位,
        // 故追加 CURRENT_CHANNELED_SPELL 显式判定, 杜绝读条被跟随移动指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 0. 冰箱 / 隐形脱困闸门 (最高优先级, 覆盖脱战与战斗双分支)
        // ---------------------------------------------------------------------
        // 寒冰屏障与隐形术在底层会持续压制随从行动 (定身/渐隐),
        // 若不在解除条件达成时主动移除, 随从会被钉死在原地发呆整个光环时限。
        // 两态期间直接 return, 严禁下发任何走位/施法指令 (否则会与定身姿态互相拉扯)。
        // =====================================================================
        if (me->HasAura(FrostMageSpells::ICE_BLOCK))
        {
            // 已卧冰箱时长由自管冷却反算: 施放成功时 iceBlockCooldown 被置为 CD_ICE_BLOCK,
            // 故 (CD_ICE_BLOCK - iceBlockCooldown) 即已定身毫秒数。
            uint32 const ibDuration = CD_ICE_BLOCK - iceBlockCooldown;

            // 三重解除门禁, 缺一不可 (铁律 21):
            // a) 最小保护时长: 防止开冰箱当帧因「未被近战压制」成立而闪解自杀;
            // b) 血线已被抬起: 治疗已把血线拉回安全区, 可以出来继续输出;
            // c) 威胁已完全解除: 既无近战压制也不再被敌对单位盯防;
            // 以及 2.5 秒硬性超时兜底: 无主坦或威胁判定未命中时,
            // 解除条件可能永远无法满足, 随从会躺满整个冰箱时限全程零输出。
            bool const ibMinHoldPassed = (ibDuration >= ICE_BLOCK_MIN_HOLD_MS);
            bool const ibHealedSafe    = (me->GetHealthPct() > ICE_BLOCK_SAFE_HP_PCT);
            bool const ibTimeout       = (ibDuration >= ICE_BLOCK_TIMEOUT_MS);

            if (ibTimeout || (ibMinHoldPassed && (ibHealedSafe || (!IsUnderPhysicalMelee(me) && !IsTopThreatTarget()))))
                me->RemoveAurasDueToSpell(FrostMageSpells::ICE_BLOCK);

            return;
        }

        if (me->HasAura(FrostMageSpells::INVISIBILITY))
        {
            // 隐形术在 3.3.5a 中必须跑满 3 秒淡入期, 底层才会真正结算仇恨清零。
            // 若沿用「仇恨已解除即主动现身」的判定, 会在渐隐尚未完成时提前点掉光环,
            // 结果是仇恨根本没被清除, 随从一现身就被原样追打, 白交 3 分钟底牌。
            // 故此处只保留超时放行, 必须等淡入期完整跑满再恢复输出。
            if (CD_INVISIBILITY - invisibilityCooldown >= INVISIBILITY_TIMEOUT_MS)
                me->RemoveAurasDueToSpell(FrostMageSpells::INVISIBILITY);

            return;
        }

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainArmor())
                return;

            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);
            return;
        }

        // =====================================================================
        // 2. 战斗内 APL
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() ||
            victim->GetMap() != me->GetMap() || !me->IsValidAttackTarget(victim))
        {
            MaintainArmor();
            return;
        }

        // 远程随从仅锚定敌对目标维持进战姿态供施法链路使用,
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- P0: 极限自保与脱困 (铁律 21) ----
        if (TrySurvival(victim)) return;

        // ---- P1: 常驻护盾、护甲与法力续航 ----
        if (MaintainIceBarrier()) return;
        if (MaintainArmor()) return;
        if (TryManaMaintenance()) return;

        // ---- P3: 爆发大招与急速冷却时序 (Off-GCD, 严禁 return, 必须当帧顺下) ----
        TryBurstCooldowns(victim);

        // ---- P4: 核心冰霜伤害循环 ----
        if (TryFrostRotation(victim)) return;

        // ---- P5: 站位控制 ----
        MaintainRangedPositioning(victim);
    }

private:
    // =========================================================================
    // 自管冷却与状态登记
    // =========================================================================
    uint32 deepFreezeCooldown{ 0 };
    uint32 iceBarrierCooldown{ 0 };
    uint32 icyVeinsCooldown{ 0 };
    uint32 coldSnapCooldown{ 0 };
    uint32 coneOfColdCooldown{ 0 };
    uint32 mirrorImageCooldown{ 0 };
    uint32 evocationCooldown{ 0 };
    uint32 manaGemCooldown{ 0 };
    uint32 iceBlockCooldown{ 0 };
    uint32 invisibilityCooldown{ 0 };
    uint32 blinkCooldown{ 0 };
    uint32 frostNovaCooldown{ 0 };
    uint32 counterspellCooldown{ 0 };

    // 近战盲区撤离迟滞姿态标记 (铁律 17): 必须凭此标记主动重发走位指令,
    // 否则会在脱离 8 码的瞬间被清空, 与立定分支每帧交替触发形成原地抽搐。
    bool isRetreating{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateMageTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(deepFreezeCooldown);
        Tick(iceBarrierCooldown);
        Tick(icyVeinsCooldown);
        Tick(coldSnapCooldown);
        Tick(coneOfColdCooldown);
        Tick(mirrorImageCooldown);
        Tick(evocationCooldown);
        Tick(manaGemCooldown);
        Tick(iceBlockCooldown);
        Tick(invisibilityCooldown);
        Tick(blinkCooldown);
        Tick(frostNovaCooldown);
        Tick(counterspellCooldown);
    }

    void ResetMageTimers()
    {
        deepFreezeCooldown = 0;
        iceBarrierCooldown = 0;
        icyVeinsCooldown = 0;
        coldSnapCooldown = 0;
        coneOfColdCooldown = 0;
        mirrorImageCooldown = 0;
        evocationCooldown = 0;
        manaGemCooldown = 0;
        iceBlockCooldown = 0;
        invisibilityCooldown = 0;
        blinkCooldown = 0;
        frostNovaCooldown = 0;
        counterspellCooldown = 0;

        isRetreating = false;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0, GetAppropriateRank 不会因等级而降阶,
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁, 40 级法师会直接搓出深度冻结。
    // 故所有在 GetTalentSpellMinLevel 登记的天赋必须经此函数解析。
    // =========================================================================
    bool HasTalent(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        return minLevel == 0 || me->GetLevel() >= minLevel;
    }

    uint32 GetTalentRank(uint32 spellId) const
    {
        if (me->GetLevel() < GetTalentSpellMinLevel(spellId))
            return 0;

        return GetAppropriateRank(spellId, true);
    }

    // =========================================================================
    // 战场态势判定器
    // =========================================================================
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

    // 仇恨失控判定: 敌对单位越过主坦直接盯防随从本人, 即为 OT
    bool IsTopThreatTarget()
    {
        Unit* tank = GetGroupTank();
        bool const hasLivingTank = (tank && tank != me && tank->IsAlive());

        uint32 chasers = 0;
        for (Unit* attacker : me->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || attacker->GetMap() != me->GetMap())
                continue;

            if (attacker->GetVictim() != me)
                continue;

            // 团队存在主坦却被敌对单位盯防: 判定为随从抢走仇恨
            if (hasLivingTank)
                return true;

            ++chasers;
        }

        // 无主坦兜底: 被两只以上敌对单位同时盯防同样判定为仇恨失控
        return chasers >= 2;
    }

    // 压秒打断目标甄别 (铁律 52): 必须同时兼顾普通读条与长引导通道,
    // 仅检测 CURRENT_GENERIC_SPELL 会遗漏精神鞭笞、吸取生命等引导类灭团法术。
    bool IsInterruptibleTarget(Unit* target) const
    {
        if (!target || !target->IsAlive())
            return false;

        if (target->HasUnitState(UNIT_STATE_CASTING))
            return true;

        // 引导类法术不置位 UNIT_STATE_CASTING, 需按通道中断标记单独判定可打断性
        if (Spell* channeled = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return channeled->GetSpellInfo()->ChannelInterruptFlags != 0;

        return false;
    }

    bool IsEliteOrBossTarget(Unit* target) const
    {
        if (!target)
            return false;

        // PvP 场景直接视同精英
        if (target->IsPlayer())
            return true;

        if (Creature* creature = target->ToCreature())
            return creature->GetCreatureTemplate()->rank >= CREATURE_ELITE_ELITE;

        return false;
    }

    // 全域冻结判定: 深度冻结的合法前置是「目标处于任何冻结/定身状态」,
    // 因此必须覆盖团队与随从集群可产出的全部冻结来源, 而不仅限于自身冰霜新星。
    // 自身冰霜新星使用分阶查询 (低 Rank 定身同样要被识别, 否则低等级段门禁恒假);
    // 水元素冰冻 / 冰冻陷阱 / 冰霜新星 Rank 1 等由队友或环境施加的冻结需按原始 ID 直查。
    bool IsTargetFrozen(Unit* victim) const
    {
        if (!victim)
            return false;

        return (victim->GetAuraOfRankedSpell(FrostMageSpells::AURA_FROST_NOVA) != nullptr) ||
               victim->HasAura(33395) ||   // 水元素: 冰冻 (Freeze)
               victim->HasAura(3355)  ||   // 猎人: 冰冻陷阱 (Freezing Trap)
               victim->HasAura(122);       // 冰霜新星 Rank 1 (低级/队友施加)
    }

    // =========================================================================
    // 寒冰指充能扣减器 (铁律 10: 充能型 Buff 必须按剩余层数结算, 严禁整层移除)
    // -------------------------------------------------------------------------
    // 寒冰指为 2 层充能型光环, 每次消费必须只扣一层并保留原生剩余时间。
    //
    // 严禁采用「RemoveAurasDueToSpell 彻底移除 + SetAuraStack 重挂」的实现:
    // 重挂等于重新施加一次全新光环, 光环持续时间会被无条件刷新回满额 15 秒,
    // 使寒冰指实际覆盖时长被成倍拉长, 触发链路的资源与暴击期望全面失真。
    // 必须直接在既有 Aura 对象上原地削减层数, 光环 tick 计时器保持不动。
    // =========================================================================
    void ConsumeFingersOfFrostCharge()
    {
        Aura* const aura = me->GetAura(FrostMageSpells::AURA_FINGERS_OF_FROST);
        if (!aura)
            return;

        if (aura->GetStackAmount() > 1)
        {
            // 原地扣层: 仅削减 StackAmount, 不触碰光环持续时间与 tick 计时
            aura->SetStackAmount(aura->GetStackAmount() - 1);
        }
        else
        {
            // 最后一层才整层摘除, 此时本就不存在需要保留的剩余时间
            me->RemoveAurasDueToSpell(FrostMageSpells::AURA_FINGERS_OF_FROST);
        }
    }

    // =========================================================================
    // P0: 极限自保与脱困 (铁律 21)
    // =========================================================================
    bool TrySurvival(Unit* victim)
    {
        // ---- 寒冰屏障: 生命濒危且被物理近战压制, 冰箱硬免伤 ----
        if (iceBlockCooldown == 0 && me->GetHealthPct() < ICE_BLOCK_HP_PCT && IsUnderPhysicalMelee(me))
        {
            uint32 const iceBlock = GetAppropriateRank(FrostMageSpells::ICE_BLOCK, false);
            if (iceBlock && !me->HasAura(iceBlock) && CanCast(me, iceBlock, true) && ExecuteSpell(me, iceBlock, true))
            {
                iceBlockCooldown = CD_ICE_BLOCK;
                return true;
            }
        }

        // ---- 隐形术: 仇恨彻底失控且有活坦可接仇恨时渐隐脱困 ----
        if (invisibilityCooldown == 0 && IsTopThreatTarget())
        {
            Unit* tank = GetGroupTank();
            bool const hasLivingTank = (tank && tank != me && tank->IsAlive() && tank->IsInWorld() && tank->GetMap() == me->GetMap());

            // 无活坦时严禁隐形: 仇恨无可转移对象, 渐隐结束后会被原样追打,
            // 反而白交一张 3 分钟底牌 (应改为冰箱 / 寒冰护体硬抗)。
            if (hasLivingTank)
            {
                uint32 const invisibility = GetAppropriateRank(FrostMageSpells::INVISIBILITY, false);
                if (invisibility && !me->HasAura(invisibility) &&
                    CanCast(me, invisibility, true) && ExecuteSpell(me, invisibility, true))
                {
                    invisibilityCooldown = CD_INVISIBILITY;
                    return true;
                }
            }
        }

        // ---- 近战盲区脱困: 冰霜新星定身 + 闪现术背离突进 ----
        return TryMeleeEscape(victim);
    }

    bool TryMeleeEscape(Unit* victim)
    {
        if (!victim)
            return false;

        if (me->GetDistance(victim) >= DEADZONE_RETREAT_DIST)
            return false;

        // ---- 冰霜新星: 自身为原点的近身群体定身, 为闪现拉开距离争取窗口 ----
        if (frostNovaCooldown == 0)
        {
            uint32 const frostNova = GetAppropriateRank(FrostMageSpells::FROST_NOVA, false);
            if (frostNova && CanCast(me, frostNova, true) && ExecuteSpell(me, frostNova, true))
            {
                frostNovaCooldown = CD_FROST_NOVA;
                isRetreating = true;
                return true;
            }
        }

        // ---- 闪现术: 必须背离目标突进, 严禁锁定朝向导致闪现扎进 Boss 怀里 ----
        if (blinkCooldown == 0)
        {
            uint32 const blink = GetAppropriateRank(FrostMageSpells::BLINK, false);
            if (blink && CanCast(me, blink, true))
            {
                // victim->GetAngle(me) 即「由 victim 指向 me」的背离向量, 精确背对目标
                me->SetFacingTo(victim->GetAngle(me));

                if (ExecuteSpell(me, blink, true))
                {
                    blinkCooldown = CD_BLINK;
                    isRetreating = true;
                    return true;
                }
            }
        }

        return false;
    }

    // 已接入基类 TryInterrupt(victim, spellId)，移除单参数遮蔽函数

    // =========================================================================
    // P2: 常驻护盾、护甲与法力续航
    // =========================================================================
    bool MaintainIceBarrier()
    {
        if (iceBarrierCooldown > 0)
            return false;

        // 寒冰护体为分阶天赋法术: 以分阶查询判定, 避免低 Rank 吸收盾被误判缺失而反复顶替空蓝。
        if (me->GetAuraOfRankedSpell(FrostMageSpells::AURA_ICE_BARRIER))
            return false;

        uint32 const iceBarrier = GetTalentRank(FrostMageSpells::ICE_BARRIER);
        if (!iceBarrier || !CanCast(me, iceBarrier, true))
            return false;

        if (!ExecuteSpell(me, iceBarrier, true))
            return false;

        iceBarrierCooldown = CD_ICE_BARRIER;
        return true;
    }

    bool MaintainArmor()
    {
        // 已维持任意一种护甲 (分阶查询, 防止低 Rank 光环被误判为缺失而反复顶替)
        if (me->GetAuraOfRankedSpell(FrostMageSpells::MOLTEN_ARMOR) ||
            me->GetAuraOfRankedSpell(FrostMageSpells::MAGE_ARMOR))
            return false;

        // 优先熔甲术 (暴击收益): allowRankOneFallback = false
        // 使等级未达该法术 DBC 门槛时返回 0, 平滑回退魔甲术 (回蓝与抗性护甲)。
        uint32 armor = GetAppropriateRank(FrostMageSpells::MOLTEN_ARMOR, false);
        if (!armor)
            armor = GetAppropriateRank(FrostMageSpells::MAGE_ARMOR, false);

        if (!armor || !CanCast(me, armor, true))
            return false;

        return ExecuteSpell(me, armor, true);
    }

    bool TryManaMaintenance()
    {
        if (me->getPowerType() != POWER_MANA)
            return false;

        float const manaPct = me->GetPowerPct(POWER_MANA);

        // ---- 法力红宝石: 法力枯竭时瞬发补蓝 (Off-GCD, 顺下不清空当帧决策流) ----
        // 等级门禁必不可少: 法力宝石在 MANA_GEM_MIN_LEVEL 以下尚未习得,
        // 若不加限制, 低等级随从会偷吃这发 80 级满级宝石效果 (非法高额回蓝), 直接破坏法力平衡。
        if (manaGemCooldown == 0 && manaPct <= MANA_GEM_USE_PCT && me->GetLevel() >= MANA_GEM_MIN_LEVEL)
        {
            // 法力宝石属物品触发类法术: 随从没有实体宝石物品,
            // 常规施法通道会被物品所有权/目标物品校验直接拒绝,
            // 必须以 triggered 通道注入, 绕过物品校验并避免占用读条与 GCD。
            me->CastSpell(me, FrostMageSpells::MANA_GEM_EFFECT, true);
            manaGemCooldown = CD_MANA_GEM;
        }

        // ---- 唤醒: 宝石进入 CD 后的深度回蓝引导 (占用引导通道, 必须阻塞当帧) ----
        // 放行条件补充「尚未习得法力宝石的等级段」: 20 ~ 27 级没有宝石,
        // manaGemCooldown 恒为 0, 若仅以宝石已 CD 为前置, 随从法力耗尽后将永久停摆。
        bool const canEvocate = (manaGemCooldown > 0) || (me->GetLevel() < MANA_GEM_MIN_LEVEL);

        if (evocationCooldown == 0 && manaPct <= EVOCATION_MANA_PCT && canEvocate &&
            !IsUnderPhysicalMelee(me))
        {
            uint32 const evocation = GetAppropriateRank(FrostMageSpells::EVOCATION, false);
            if (evocation && CanCast(me, evocation, true))
            {
                // 引导类法术必须先刹停: 否则会被跟随/走位指令当场掐断, 白白交掉 4 分钟 CD
                if (me->isMoving())
                    me->StopMoving();

                if (ExecuteSpell(me, evocation, true))
                {
                    evocationCooldown = CD_EVOCATION;
                    return true;
                }
            }
        }

        return false;
    }

    // =========================================================================
    // P3: 爆发大招与急速冷却时序 (Off-GCD, 当帧顺下绝不 return, 铁律 8)
    // =========================================================================
    void TryBurstCooldowns(Unit* victim)
    {
        if (!victim)
            return;

        // ---- 法术反制：接入阶段三基类记忆化压秒打断仲裁引擎 (Off-GCD) ----
        if (counterspellCooldown == 0)
        {
            uint32 const counterspell = GetAppropriateRank(FrostMageSpells::COUNTERSPELL, false);
            if (counterspell && TryInterrupt(victim, counterspell))
                counterspellCooldown = CD_COUNTERSPELL;
        }

        // ---- 镜像: 首领/精英战起手召唤分身, 爆发同时压低初始仇恨 ----
        if (mirrorImageCooldown == 0 && IsEliteOrBossTarget(victim) && victim->GetHealthPct() > 50.0f)
        {
            uint32 const mirrorImage = GetAppropriateRank(FrostMageSpells::MIRROR_IMAGE, false);
            if (mirrorImage && !me->HasAura(mirrorImage) &&
                CanCast(me, mirrorImage, true) && ExecuteSpell(me, mirrorImage, true))
                mirrorImageCooldown = CD_MIRROR_IMAGE;
        }

        // ---- 冰冷血脉: 首领/精英战开启 20 秒 +20% 急速核心爆发窗口 ----
        // Off-GCD 光环: 施放成功后当帧交还决策流, 由 gcdTimer 阻断后续施法,
        // 严禁同帧顺下与读条指令冲突导致动作丢帧。
        if (icyVeinsCooldown == 0 && IsEliteOrBossTarget(victim) &&
            !me->HasAura(FrostMageSpells::AURA_ICY_VEINS))
        {
            uint32 const icyVeins = GetTalentRank(FrostMageSpells::ICY_VEINS);
            if (icyVeins && CanCast(me, icyVeins, true) && ExecuteSpell(me, icyVeins, true))
                icyVeinsCooldown = CD_ICY_VEINS;
        }

        // ---- 急速冷却: 首领/精英战重置换算冰霜大招链 (铁律 41) ----
        // 门禁统计口径必须与施放成功后的清零列表完全一致, 严禁
        // 「统计了却不清零」或「把不属于重置范围的技能计入门禁」。
        if (coldSnapCooldown == 0 && IsEliteOrBossTarget(victim) && HasResettableFrostCooldown())
        {
            uint32 const coldSnap = GetTalentRank(FrostMageSpells::COLD_SNAP);
            if (coldSnap && CanCast(me, coldSnap, true) && ExecuteSpell(me, coldSnap, true))
            {
                coldSnapCooldown = CD_COLD_SNAP;

                // --- 与 HasResettableFrostCooldown() 统计项严格一一对应 ---
                icyVeinsCooldown = 0;
                deepFreezeCooldown = 0;
                frostNovaCooldown = 0;
                iceBlockCooldown = 0;
            }
        }
    }

    // 急速冷却门禁统计器: 统计项必须与 TryBurstCooldowns 中的清零列表完全一致 (铁律 41)
    // -------------------------------------------------------------------------
    // 冰脉爆发期硬性封锁 (铁律 8 时序对齐): 随从起手时冰冷血脉与急速冷却往往同帧全部就绪,
    // 若放行急速冷却, 它会在冰脉光环刚亮起的当帧把 icyVeinsCooldown 直接清零,
    // 使这张 8 分钟底牌被起手秒吞, 且重置收益为零 (冰脉 20 秒窗口内 CD 本就不会自然走完)。
    // 必须等冰脉光环完整结束后再交急速冷却, 才能实打实白嫖一整轮冰脉爆发窗口。
    // -------------------------------------------------------------------------
    bool HasResettableFrostCooldown() const
    {
        // 身上仍在冰脉爆发期严禁重置, 必须等冰脉光环结束后再交急速冷却
        if (me->HasAura(FrostMageSpells::AURA_ICY_VEINS))
            return false;

        return icyVeinsCooldown > 0 || deepFreezeCooldown > 0 ||
               frostNovaCooldown > 0 || iceBlockCooldown > 0;
    }

    // =========================================================================
    // P4: 核心冰霜伤害循环
    // =========================================================================
    bool TryFrostRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P5 接管, 绝不在此硬读条
        if (dist < DEADZONE_RETREAT_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        // ---- 1. 深度冻结: 冻结/寒冰指前置的高额爆发直伤 ----
        if (TryDeepFreeze(victim))
            return true;

        // ---- 2. 思维冻结瞬发霜火之箭: 零耗蓝最高优先级瞬发填充 ----
        if (TryBrainFreezeFrostfireBolt(victim))
            return true;

        // ---- 3. 寒冰指冰枪术: 目标视同冻结, 3 倍伤害 ----
        if (TryFingersOfFrostIceLance(victim))
            return true;

        // ---- 4. 冰锥术: 正面 10 码锥形减速伤害 ----
        if (TryConeOfCold(victim))
            return true;

        // ---- 5. 寒冰箭站桩填充 ----
        if (TryFrostbolt(victim))
            return true;

        return false;
    }

    // -------------------------------------------------------------------------
    // 深度冻结 (Deep Freeze)
    // -------------------------------------------------------------------------
    // 深度冻结是冰法唯一的冻结前置高额直伤, 合法前置只有两条:
    // a) 自身持有【寒冰指】触发光环 (将目标视作冻结);
    // b) 目标已被【冰霜新星】定身。
    // 任一条件缺失时施放会被底层直接拒绝并白烧 30 秒 CD, 故必须双重判定。
    // -------------------------------------------------------------------------
    bool TryDeepFreeze(Unit* victim)
    {
        if (!victim || deepFreezeCooldown > 0)
            return false;

        if (!HasTalent(FrostMageSpells::DEEP_FREEZE))
            return false;

        bool const hasFingersOfFrost = me->HasAura(FrostMageSpells::AURA_FINGERS_OF_FROST);

        if (!hasFingersOfFrost && !IsTargetFrozen(victim))
            return false;

        // 深度冻结对首领木桩 / Boss 拥有「免疫昏迷但照常结算伤害」的原生机制,
        // 常规 CanCast 通道会依据免昏迷免疫判定把施法请求预阻断,
        // 造成对首领战核心爆发直伤永久哑火。故在此以距离 + 视线预检替代 CanCast,
        // 直接走 triggered = true 触发式直放, 绕过底层的免疫阻塞。
        if (!me->IsWithinDist(victim, MAX_ENGAGE_DIST) || !me->IsWithinLOSInMap(victim))
            return false;

        uint32 const deepFreeze = GetTalentRank(FrostMageSpells::DEEP_FREEZE);
        if (!deepFreeze)
            return false;

        me->SetFacingToObject(victim);

        if (me->CastSpell(victim, deepFreeze, true) != SPELL_CAST_OK)
            return false;

        deepFreezeCooldown = CD_DEEP_FREEZE;

        // 触发式直放不参与引擎 GCD 结算, 必须手工置位 1.5 秒公共冷却,
        // 防止同一帧决策流顺下重复打卡与后续读条指令冲突。
        gcdTimer = 1500;

        // 依赖寒冰指触发的场合必须显式消耗充能, 防止同一层光环被后续冰枪术重复白嫖
        if (hasFingersOfFrost)
            ConsumeFingersOfFrostCharge();

        return true;
    }

    // -------------------------------------------------------------------------
    // 思维冻结瞬发霜火之箭 (Brain Freeze Frostfire Bolt)
    // -------------------------------------------------------------------------
    // 霜火之箭基础读条 3 秒, 战斗中硬读会被任何一次走位/打断当场作废并浪费一整个 GCD。
    // 唯一合法通道是【思维冻结】触发后的瞬发免蓝光环, 此处严禁放行任何硬读条分支。
    // 由于是瞬发, 允许在撤离跑位途中直接出手, 严禁 StopMoving 破坏机动性。
    // -------------------------------------------------------------------------
    bool TryBrainFreezeFrostfireBolt(Unit* victim)
    {
        if (!victim || !me->HasAura(FrostMageSpells::AURA_BRAIN_FREEZE))
            return false;

        // 等级未达尚未习得霜火之箭: GetAppropriateRank(..., false) 稳定返回 0,
        // 此时让触发光环自然计时脱落即可, 严禁降级硬读寒冰箭白烧思维冻结层数。
        uint32 const frostfireBolt = GetAppropriateRank(FrostMageSpells::FROSTFIRE_BOLT, false);
        if (!frostfireBolt)
            return false;

        me->SetFacingToObject(victim);

        // Creature 缺乏玩家 SpellModOwner 机制: 【思维冻结】的「瞬发且不耗蓝」修饰
        // 无法被底层自动施加, 若走常规 CanCast / ExecuteSpell 通道,
        // 引擎仍会按霜火之箭的 3 秒基础读条与蓝耗进行校验, 造成持有光环却搓不出来的死锁。
        // 故必须以 triggered = true 触发式直放, 彻底旁路读条与法力门禁。
        if (me->CastSpell(victim, frostfireBolt, true) != SPELL_CAST_OK)
            return false;

        // 底层同样不会因触发式施法自动剥离触发光环, 必须手工移除,
        // 否则思维冻结会永久挂身, 使霜火之箭退化为无限免读条连发并吞掉常规输出节奏。
        me->RemoveAurasDueToSpell(FrostMageSpells::AURA_BRAIN_FREEZE);

        // 触发式直放不参与引擎 GCD 结算, 必须手工置位 1.5 秒公共冷却,
        // 防止同一帧决策流顺下重复打卡与后续读条指令冲突。
        gcdTimer = 1500;
        return true;
    }

    // -------------------------------------------------------------------------
    // 寒冰指冰枪术 (Fingers of Frost Ice Lance)
    // -------------------------------------------------------------------------
    // 冰枪术本身伤害极低, 全部价值来自「对冻结目标造成 3 倍伤害」与
    // 【碎冰】对冻结目标 +50% 暴击率。故仅在持有【寒冰指】时打出,
    // 严禁无触发裸放占用宝贵 GCD。
    // -------------------------------------------------------------------------
    bool TryFingersOfFrostIceLance(Unit* victim)
    {
        if (!victim || !me->HasAura(FrostMageSpells::AURA_FINGERS_OF_FROST))
            return false;

        uint32 const iceLance = GetAppropriateRank(FrostMageSpells::ICE_LANCE, false);
        if (!iceLance || !CanCast(victim, iceLance, true))
            return false;

        // 瞬发无弹道: 允许在跑位途中直接施放, 严禁 StopMoving 破坏机动性
        if (!ExecuteSpell(victim, iceLance, true))
            return false;

        ConsumeFingersOfFrostCharge();
        return true;
    }

    // -------------------------------------------------------------------------
    // 冰锥术 (Cone of Cold)
    // -------------------------------------------------------------------------
    // 3.3.5a 冰锥术为正面锥形判定, 完全依赖施法者当前朝向结算范围。
    // 随从在风筝撤离或贴坦避难时通常背对敌对目标, 若直接施放会导致锥形落空;
    // 必须在施放判定通过后显式调用 SetFacingToObject(victim) 锁定朝向再执行施法 (铁律 35)。
    // 注: 冰锥术是以施法者为原点的自身锥形法术 (TARGET_UNIT_CASTER),
    //     目标类型校验会直接拒绝以 victim 为目标的施法请求, 故 CanCast/ExecuteSpell 必须传 me。
    // -------------------------------------------------------------------------
    bool TryConeOfCold(Unit* victim)
    {
        if (!victim || coneOfColdCooldown > 0)
            return false;

        if (me->GetDistance(victim) > CONE_OF_COLD_DIST)
            return false;

        uint32 const coneOfCold = GetAppropriateRank(FrostMageSpells::CONE_OF_COLD, false);
        if (!coneOfCold || !CanCast(me, coneOfCold, true))
            return false;

        me->SetFacingToObject(victim);

        if (ExecuteSpell(me, coneOfCold, true))
        {
            coneOfColdCooldown = CD_CONE_OF_COLD;
            return true;
        }

        return false;
    }

    // -------------------------------------------------------------------------
    // 寒冰箭 (核心读条填充技)
    // -------------------------------------------------------------------------
    // 撤离途中严禁站桩读条: 走位与读条会互相打断形成原地抽搐。
    // 注: 寒冰箭射程为 0 ~ 35 码全域技能, 严禁在此叠加 MIN_ENGAGE_DIST 下限门禁 ——
    //     8 ~ 15 码区间会让寒冰箭与冰锥术 (仅 10 码内放行) 双双失效,
    //     形成「既不读条也不前压」的人造盲区呆滞死锁。
    // -------------------------------------------------------------------------
    bool TryFrostbolt(Unit* victim)
    {
        if (!victim)
            return false;

        if (isRetreating)
            return false;

        uint32 const frostbolt = GetAppropriateRank(FrostMageSpells::FROSTBOLT, false);

        // 施法资格必须先通过校验再刹停, 避免 CanCast 失败时被每帧 StopMoving 拉扯成抽搐
        if (!frostbolt || !CanCast(victim, frostbolt, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, frostbolt, true);
    }

    // =========================================================================
    // P5: 站位控制 (盲区迟滞回正模型, 维持 15 ~ 35 码施法站位)
    // =========================================================================
    void MaintainRangedPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // ---- P0: APF 势场紧急避险 (火圈/顺劈强行接管) ----
        if (IsUnderDangerThreat(2.0f))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;
                float const optDist = std::clamp(me->GetDistance(victim), MIN_ENGAGE_DIST, IDEAL_SHOT_DIST);
                if (PotentialField::CalculateNextPosition(me, victim, optDist, false, false, activeDangerZones, nextX, nextY, nextZ))
                {
                    me->GetMotionMaster()->MovePoint(1, nextX, nextY, nextZ);
                    apfMoveUpdateTimer = 300;
                    return;
                }
            }
            else
            {
                return;
            }
        }

        me->SetFacingToObject(victim);

        float const dist = me->GetDistance(victim);
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();

        // ---- A. 脱节过远 (> 35 码): 主动压进至理想施法站位 ----
        if (dist > MAX_ENGAGE_DIST)
        {
            isRetreating = false;

            if (moveType != CHASE_MOTION_TYPE)
                me->GetMotionMaster()->MoveChase(victim, IDEAL_SHOT_DIST);

            return;
        }

        // ---- 撤退迟滞门禁 (铁律 17) ----
        // 进入撤退沿用 DEADZONE_RETREAT_DIST (8 码), 退出撤退必须恢复到 RETREAT_SAFE_DIST (16 码)。
        // 严禁拆除 isRetreating 这一自我粘滞条件: 一旦在脱离 8 码的瞬间就被清空,
        // 撤退姿态会与立定分支每帧交替触发, 表现为原地反复起步/刹停的抽搐。
        bool const needsRetreat = (dist < DEADZONE_RETREAT_DIST) ||
                                  (isRetreating && dist < RETREAT_SAFE_DIST);

        // ---- B. 近战盲区 / 尚未拉足安全距离: 向坦克背身位撤离, 借主坦 AoE 仇恨把小怪拉走 ----
        if (needsRetreat)
        {
            isRetreating = true;

            Unit* tank = GetGroupTank();
            bool const canAnchorTank = (tank && tank != me && tank != victim && tank->IsAlive() &&
                                        tank->IsInWorld() && tank->GetMap() == me->GetMap());

            if (canAnchorTank)
            {
                if (moveType != FOLLOW_MOTION_TYPE)
                {
                    // angle 必须传正后方: 传 0.0f 会贴在坦克脸前, 被顺劈斩与正面吐息一并打死。
                    // 严禁自行叠加 tank->GetOrientation(), 引擎内部已按目标朝向结算偏移。
                    me->GetMotionMaster()->MoveFollow(tank, TANK_RETREAT_DIST, BEHIND_ANGLE);
                }
                return;
            }

            // 无坦克可依: 严禁对 victim 调用 MoveChase —— ChaseMovementGenerator 在
            // 施法者已位于指定距离「以内」时会原地立定, 而随从此刻恰处于 8 码盲区内,
            // 结果是发出指令却纹丝不动, 形成粘脸卡死。必须改用矢量外推硬位移:
            // 以 victim 为原点, 沿 victim -> me 的当前向量外推至 IDEAL_SHOT_DIST 落点。
            if (moveType != POINT_MOTION_TYPE)
            {
                float const angle = victim->GetAngle(me);
                float const retreatX = victim->GetPositionX() + IDEAL_SHOT_DIST * std::cos(angle);
                float const retreatY = victim->GetPositionY() + IDEAL_SHOT_DIST * std::sin(angle);

                me->GetMotionMaster()->MovePoint(0, retreatX, retreatY, me->GetPositionZ());
            }
            return;
        }

        // ---- C. 已回到有效射程: 立定施法, 清空遗留走位发生器 ----
        // 严禁放任 chase / follow 发生器常驻: 残余走位会持续拉扯随从,
        // 使寒冰箭的读条与站位反复互相打断, 表现为原地抽搐。
        if (moveType == CHASE_MOTION_TYPE || moveType == FOLLOW_MOTION_TYPE)
        {
            isRetreating = false;
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveIdle();
            me->StopMoving();
        }
        else if (dist >= RETREAT_SAFE_DIST)
        {
            isRetreating = false;
        }
    }

    // =========================================================================
    // 冰霜天赋被动光环与雕文补偿 (弥补 NPC 缺天赋树缺陷, 铁律 33)
    // -------------------------------------------------------------------------
    // 必须注入 Rank 2/3/5 满阶 Spell ID。若注入 DBC 默认 Rank 1 根源,
    // 触发概率与数值会严重缩水 (如寒冰指 Rank 1 触发率大幅低于满阶,
    // 碎冰 Rank 1 暴击加成远低于 50%), 直接导致冰枪术与深度冻结链路隐形削弱。
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

        // ---- 满阶被动天赋根源注入 ----
        SyncPassive(LEVEL_SHATTER, FrostMageSpells::SHATTER);                          // 碎冰 Rank 3: 冻结目标暴击率 +50%
        SyncPassive(LEVEL_ICE_SHARDS, FrostMageSpells::ICE_SHARDS);                    // 寒冰碎片 Rank 3: 冰系暴击伤害 +100%
        SyncPassive(LEVEL_PIERCING_ICE, FrostMageSpells::PIERCING_ICE);                // 刺骨寒冰 Rank 3: 冰霜法术伤害 +6%
        SyncPassive(LEVEL_FINGERS_OF_FROST, FrostMageSpells::FINGERS_OF_FROST);        // 寒冰指 Rank 2: 触发视同冻结 (冰枪/深冻核心前置)
        SyncPassive(LEVEL_EMPOWERED_FROSTBOLT, FrostMageSpells::EMPOWERED_FROSTBOLT);  // 强化寒冰箭 Rank 2: 读条 -0.2s, 法伤加成 +10%
        SyncPassive(LEVEL_BRAIN_FREEZE, FrostMageSpells::BRAIN_FREEZE);                // 思维冻结 Rank 3: 寒冰箭暴击触发瞬发霜火之箭
        SyncPassive(LEVEL_ARCTIC_WINDS, FrostMageSpells::ARCTIC_WINDS);                // 极寒之风 Rank 5: 冰霜法术伤害 +5%
        SyncPassive(LEVEL_WINTERS_CHILL, FrostMageSpells::WINTERS_CHILL);              // 深冬之寒 Rank 3: 冰系暴击易伤根源
        SyncPassive(LEVEL_PRECISION, FrostMageSpells::PRECISION);                      // 法术精准 Rank 3: 法术命中 +3%, 耗蓝 -3%
        SyncPassive(25, FrostMageSpells::FROST_CHANNELING);                            // 冰霜导能 Rank 3: 冰霜法术耗蓝 -10%, 仇恨 -10%

        // ---- 雕文补偿 ----
        SyncPassive(LEVEL_GLYPH, FrostMageSpells::GLYPH_OF_FROSTBOLT);                 // 寒冰箭雕文: 寒冰箭伤害 +5%
        SyncPassive(LEVEL_GLYPH, FrostMageSpells::GLYPH_OF_ICE_LANCE);                 // 冰枪术雕文: 对高于自身等级目标伤害 +30%
    }
};

void AddSC_bot_frost_mage()
{
    new AdaptiveBotScript<BotFrostMageAI>("bot_frost_mage");
}
