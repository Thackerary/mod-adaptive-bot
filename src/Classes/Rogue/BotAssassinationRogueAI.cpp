/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "AssassinationRogueSpells.h"
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

class BotAssassinationRogueAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位参数 (物理近战背后找背模型)
    // =========================================================================
    static constexpr float STEALTH_OPENER_DIST = 5.0f;   // 潜行起手上限：进入即尝试绞喉
    static constexpr float MELEE_REACH_DIST    = 4.0f;   // 近战判定区上限：超出必须重新贴背
    static constexpr float MELEE_COMFORT_DIST  = 3.0f;   // 贴身阈值：进入后保持平滑贴背输出
    static constexpr float MELEE_FOLLOW_DIST   = 1.5f;   // 理想站位：目标正后方 1.5 码

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移。
    // M_PI 即目标正后方：背身位，可规避正面顺劈、吐息以及被招架加速。
    static constexpr float BEHIND_ANGLE        = static_cast<float>(M_PI);

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的 CD 技能必须由专精自行计时，
    // 否则会因 HasSpellCooldown 恒 false 而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_VANISH      = 180000;
    static constexpr uint32 CD_COLD_BLOOD  = 120000;
    static constexpr uint32 CD_KICK        = 10000;
    static constexpr uint32 CD_TRICKS      = 30000;
    static constexpr uint32 CD_FEINT       = 10000;
    static constexpr uint32 CD_EVASION     = 180000;
    static constexpr uint32 CD_CLOAK       = 90000;

    // 生命/能量阈值
    static constexpr float  VANISH_HP_PCT       = 20.0f;
    static constexpr float  EVASION_HP_PCT      = 35.0f;
    static constexpr float  FEINT_HP_PCT        = 60.0f;
    static constexpr uint8  ENVENOM_MIN_CP      = 4;      // 毒伤终结技最低连击点
    static constexpr uint32 ENVENOM_ENERGY      = 35;     // 毒伤能量消耗 (冷血当帧可用性预判)
    static constexpr uint32 KICK_ENERGY         = 15;     // 脚踢能量消耗
    static constexpr uint32 MAGIC_DEBUFF_COUNT  = 2;      // 暗影斗篷触发所需的魔法/疾病 Debuff 数
    static constexpr uint32 STEALTH_OPEN_WINDOW = 2000;   // 潜行起手窗口上限，超时强制转入常规近战

    // 流血机制位掩码：Unit::HasAuraWithMechanic 内部采用 1 << (mechanic - 1) 编码
    static constexpr uint64 BLEED_MECHANIC_MASK = uint64(1) << (MECHANIC_BLEED - 1);

    // =========================================================================
    // 毒药模拟参数
    // -------------------------------------------------------------------------
    // 随从无武器涂毒对象，且底层毒药依赖 ProcFlag 武器命中事件，
    // 必须由专精以 triggered 方式手工注入，否则毒伤将因缺少致命毒药被 100% 拒绝。
    // =========================================================================
    static constexpr uint32 POISON_PROC_INTERVAL     = 1000;  // 毒药巡检周期 (等效武器毒药 PPM 节流)
    static constexpr uint32 DEADLY_POISON_MAX_STACKS = 5;     // 致命毒药满层数
    static constexpr uint32 DEADLY_POISON_REFRESH_MS = 3000;  // 致命毒药提前续期窗口 (防止断档导致毒伤被拒)
    static constexpr uint32 INSTANT_POISON_CHANCE    = 50;    // 速效毒药触发概率 (%)

public:
    explicit BotAssassinationRogueAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 首个物理近战专精：站位与移动交由本专精的背后找背状态机接管
    bool IsRangedBot() const override { return false; }

    // 物理近战按真实装备模型结算：严禁继承法系远程的 2.0x ~ 3.3x 法伤放大乘数
    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (绞喉/割裂/毒伤/切割/暗影斗篷/闪避等) 严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case AssassinationRogueSpells::COLD_BLOOD:        return 20;
            case AssassinationRogueSpells::MUTILATE:          return 40;
            case AssassinationRogueSpells::HUNGER_FOR_BLOOD:  return 50;
            default:                                          return 0;
        }
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 能量通道必须在基类 Reset 之前配置，以便等级同步时正确初始化能量池
        me->setPowerType(POWER_ENERGY);
        me->SetMaxPower(POWER_ENERGY, 100);
        AdaptiveBotAI::Reset();
        ResetRogueTimers();
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 level) override
    {
        AdaptiveBotAI::OnLevelSynced(level);
        me->setPowerType(POWER_ENERGY);

        // 能量池固定 100 点，等级同步后不允许溢出
        me->SetMaxPower(POWER_ENERGY, 100);
        if (me->GetPower(POWER_ENERGY) > me->GetMaxPower(POWER_ENERGY))
            me->SetPower(POWER_ENERGY, me->GetMaxPower(POWER_ENERGY));

        ApplyPassiveTalents();
    }

    // =========================================================================
    // 核心决策循环
    // =========================================================================
    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);
        UpdateRogueTimers(diff);

        // 全局读条/通道双保险守卫：引导类法术在部分状态下并不置位 UNIT_STATE_CASTING，
        // 故追加 CURRENT_CHANNELED_SPELL 显式判定，杜绝被跟随/走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护 (潜行姿态 + 跟随)
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainStealth())
                return;

            // 潜行起手：已贴近合法协助目标背后时，直接绞喉开怪 (同时触发灭绝回能与进战)
            Unit* openerTarget = SelectAssistTarget();
            if (openerTarget && TryStealthOpener(openerTarget))
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
            MaintainStealth();
            return;
        }

        // 潜行起手窗口：破潜前不得开启近战追击与白字平砍，否则第一刀白字会提前破潜、
        // 直接吞掉绞喉起手与灭绝回能；窗口最多持续 2 秒，超时强制转入常规近战，
        // 杜绝目标免疫/背身位几何受限导致随从永久隐蔽待机。
        bool const stealthHold = IsStealthed() && (stealthOpenTimer > 0);

        // 物理近战开怪：第二参数必须传 true 开启近战平砍与双持白字输出
        // (与远程专精的 me->Attack(victim, false) 严格区分)。
        if (!stealthHold && me->GetVictim() != victim)
            me->Attack(victim, true);

        // ---- P1: 潜行起手 (绞喉) ----
        if (TryStealthOpener(victim))
            return;

        // ---- P0: 极限自保与仇恨协同 (维度 C 协同) ----
        if (TrySurvivalAndThreat(victim))
            return;

        // ---- P2: 爆发与打断 (Off-GCD，严禁 return true，必须当帧顺下) ----
        TryBurstAndInterrupt(victim);

        // ---- P3: 关键增益常驻维持 (切割 / 血之饥渴) ----
        if (MaintainSliceAndDice(victim))
            return;

        if (MaintainHungerForBlood(victim))
            return;

        // ---- P4: 连击点循环与终结技 ----
        if (TryComboPointRotation(victim))
            return;

        // ---- P5: 背后找背站位 ----
        MaintainMeleeBehindPositioning(victim);

        // ---- 白色平砍驱动 ----
        // ScriptedAI::UpdateAI 已被本类完整接管，引擎不会自动驱动平砍，
        // 必须在决策流末帧显式调用，否则双持白字与毒药伤害永久缺失。
        // 潜行起手窗口内严禁驱动：挥砍会立即破潜吞掉绞喉。
        if (!stealthHold)
        {
            DoMeleeAttackIfReady();

            // 白字平砍结算后立即补一次毒药模拟：维持 5 层致命毒药，
            // 使毒伤能够通过底层校验，打通「毒伤 -> 名天堑 -> 切割续期」闭环。
            ProcPoisons(victim);
        }
    }

private:
    // 自管冷却登记
    uint32 vanishCooldown{ 0 };
    uint32 coldBloodCooldown{ 0 };
    uint32 kickCooldown{ 0 };
    uint32 tricksCooldown{ 0 };
    uint32 feintCooldown{ 0 };
    uint32 evasionCooldown{ 0 };
    uint32 cloakCooldown{ 0 };

    // 潜行起手窗口倒计时 (仅潜行态内递减)
    uint32 stealthOpenTimer{ STEALTH_OPEN_WINDOW };

    // 灭绝 (Overkill) 回能补偿计时器
    uint32 overkillRegenTimer{ 0 };

    // 毒药模拟巡检节流计时器
    uint32 poisonProcTimer{ 0 };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateRogueTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(vanishCooldown);
        Tick(coldBloodCooldown);
        Tick(kickCooldown);
        Tick(tricksCooldown);
        Tick(feintCooldown);
        Tick(evasionCooldown);
        Tick(cloakCooldown);
        Tick(poisonProcTimer);

        // 潜行起手窗口：仅当「已潜行 且 已贴近起手距离」时才开始倒计时。
        // 若在赶路途中等比扣减，随从尚未走到目标背后窗口就已耗尽，
        // 必然在破潜瞬间断掉绞喉起手与灭绝回能链路，因此赶路途中维持满额窗口。
        if (IsStealthed())
        {
            Unit* openerTarget = SelectAssistTarget();
            bool const inOpenerRange = (openerTarget && openerTarget->IsAlive() &&
                                        openerTarget->IsInWorld() &&
                                        openerTarget->GetMap() == me->GetMap() &&
                                        me->GetDistance(openerTarget) <= STEALTH_OPENER_DIST);

            if (inOpenerRange)
                Tick(stealthOpenTimer);
        }
        else
        {
            stealthOpenTimer = STEALTH_OPEN_WINDOW;
        }

        // 灭绝 (Overkill) 回能补偿：潜行中及破潜后 20 秒内回能速度 +30%。
        // NPC 不走玩家能量再生公式，此处按 1000ms 粒度手工补足 30% 增量 (基准 10/s -> 13/s)。
        if (me->getPowerType() == POWER_ENERGY && me->HasAura(AssassinationRogueSpells::OVERKILL))
        {
            overkillRegenTimer += diff;
            if (overkillRegenTimer >= 1000)
            {
                overkillRegenTimer -= 1000;
                if (me->GetPower(POWER_ENERGY) < me->GetMaxPower(POWER_ENERGY))
                    me->ModifyPower(POWER_ENERGY, 3);
            }
        }
        else
        {
            overkillRegenTimer = 0;
        }
    }

    void ResetRogueTimers()
    {
        vanishCooldown = 0;
        coldBloodCooldown = 0;
        kickCooldown = 0;
        tricksCooldown = 0;
        feintCooldown = 0;
        evasionCooldown = 0;
        cloakCooldown = 0;

        stealthOpenTimer = STEALTH_OPEN_WINDOW;
        overkillRegenTimer = 0;
        poisonProcTimer = 0;
    }

    // =========================================================================
    // 状态判定器
    // =========================================================================
    bool IsStealthed() const
    {
        // 潜行为分阶法术，消失 (Vanish) 亦会施加潜行光环：
        // 同时按法术 ID 与光环类型双通道判定，杜绝漏检导致起手链路失效。
        return me->HasAura(AssassinationRogueSpells::STEALTH) || me->HasAuraType(SPELL_AURA_MOD_STEALTH);
    }

    bool HasSliceAndDice() const
    {
        // 切割为分阶法术，且底层 Cut to the Chase 脚本固定以 Rank 1 刷新 5 星时长，
        // 故必须按「分阶光环」查询，直接 HasAura(6774) 会漏检 Rank 1 光环。
        return me->GetAuraOfRankedSpell(AssassinationRogueSpells::SLICE_AND_DICE) != nullptr;
    }

    bool HasHungerForBlood() const
    {
        // 血之饥渴为单阶法术，按分阶查询会直接返回 nullptr，必须用 HasAura
        return me->HasAura(AssassinationRogueSpells::HUNGER_FOR_BLOOD);
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

    uint32 CountMeleeAttackers() const
    {
        uint32 count = 0;
        for (Unit* attacker : me->getAttackers())
        {
            if (attacker && attacker->IsAlive() && attacker->GetMap() == me->GetMap() && attacker->GetVictim() == me)
                ++count;
        }

        return count;
    }

    // 仇恨失控判定：敌对单位越过主坦直接盯防随从本人，即为 OT (维度 C 归因输入信号)
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

            // 团队存在主坦却被敌对单位盯防：判定为随从抢走仇恨
            if (hasLivingTank)
                return true;

            ++chasers;
        }

        // 无主坦兜底：被两只以上敌对单位同时盯防同样判定为仇恨失控
        return chasers >= 2;
    }

    uint32 CountHarmfulMagicDebuffs() const
    {
        uint32 count = 0;
        uint32 const mask = (1u << DISPEL_MAGIC) | (1u << DISPEL_DISEASE);

        for (auto const& pair : me->GetAppliedAuras())
        {
            AuraApplication* app = pair.second;
            if (!app || app->IsPositive())
                continue;

            Aura* aura = app->GetBase();
            if (!aura)
                continue;

            SpellInfo const* spellInfo = aura->GetSpellInfo();
            if (spellInfo && (spellInfo->GetDispelMask() & mask))
                ++count;
        }

        return count;
    }

    bool HasBleedEffect(Unit* target) const
    {
        if (!target)
            return false;

        // 目标身上任意来源的流血均可为血之饥渴提供跳板 (自身绞喉/割裂，或队友的流血技)
        return target->HasAuraWithMechanic(BLEED_MECHANIC_MASK);
    }

    bool IsInterruptibleTarget(Unit* target) const
    {
        if (!target || !target->IsAlive())
            return false;

        if (target->HasUnitState(UNIT_STATE_CASTING))
            return true;

        // 引导类法术不置位 UNIT_STATE_CASTING，需按通道中断标记单独判定可打断性
        if (Spell* channeled = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return channeled->GetSpellInfo()->ChannelInterruptFlags != 0;

        return false;
    }

    // 判断随从是否已占住目标背身位：目标正面 180° 锥形之外即视为背后。
    // 绞喉/毁伤均有背后硬性要求，提前自检可避免白白空放被底层拒绝。
    bool IsBehindVictim(Unit* victim, float maxDist) const
    {
        if (!victim || me->GetDistance(victim) > maxDist)
            return false;

        float diff = victim->GetAngle(me) - victim->GetOrientation();

        // 归一化至 [-PI, PI]
        while (diff > static_cast<float>(M_PI))  diff -= static_cast<float>(2.0 * M_PI);
        while (diff < static_cast<float>(-M_PI)) diff += static_cast<float>(2.0 * M_PI);

        return std::fabs(diff) > static_cast<float>(M_PI) * 0.5f;
    }

    // =========================================================================
    // P1: 潜行起手与脱战潜行
    // =========================================================================
    bool MaintainStealth()
    {
        if (IsStealthed())
            return false;

        // 潜行严禁在战斗中施放 (底层会直接拒绝)，本函数仅由脱战分支调用
        uint32 const stealth = GetAppropriateRank(AssassinationRogueSpells::STEALTH, false);
        if (!stealth || !CanCast(me, stealth, true))
            return false;

        return ExecuteSpell(me, stealth, true);
    }

    bool TryStealthOpener(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return false;

        // 绞喉必须由潜行态施放，起手成功即触发灭绝 (Overkill) 回能增益
        if (!IsStealthed())
            return false;

        if (me->GetDistance(victim) > STEALTH_OPENER_DIST)
            return false;

        // 绞喉为背后起手技：未占住背身位时交由 P5 走位接管，避免空放浪费起手窗口
        if (!IsBehindVictim(victim, STEALTH_OPENER_DIST))
            return false;

        uint32 const garrote = GetAppropriateRank(AssassinationRogueSpells::GARROTE, false);
        if (!garrote || !CanCast(victim, garrote, true))
            return false;

        if (!ExecuteSpell(victim, garrote, true))
            return false;

        AddComboPoints(victim, 1);
        return true;
    }

    // =========================================================================
    // P0: 极限自保与仇恨协同
    // =========================================================================
    bool TrySurvivalAndThreat(Unit* victim)
    {
        // ---- 消失：生命濒危且被近战围攻，或仇恨彻底失控 (OT) ----
        // 消失强行进潜清仇恨并重新触发灭绝回能，是刺杀贼最高优先级的脱困底牌。
        if (vanishCooldown == 0 && me->GetHealthPct() < VANISH_HP_PCT &&
            (IsUnderPhysicalMelee(me) || IsTopThreatTarget()))
        {
            uint32 const vanish = GetAppropriateRank(AssassinationRogueSpells::VANISH, false);
            if (vanish && !me->HasAura(vanish) && CanCast(me, vanish, true) && ExecuteSpell(me, vanish, true))
            {
                vanishCooldown = CD_VANISH;

                // 进潜后立即重置起手窗口，使随从能以绞喉重新起手
                stealthOpenTimer = STEALTH_OPEN_WINDOW;
                return true;
            }
        }

        // ---- 闪避：多重物理围攻下的应急免伤 ----
        if (evasionCooldown == 0 && me->GetHealthPct() < EVASION_HP_PCT && CountMeleeAttackers() >= 2)
        {
            uint32 const evasion = GetAppropriateRank(AssassinationRogueSpells::EVASION, false);
            if (evasion && !me->HasAura(evasion) && CanCast(me, evasion, true) && ExecuteSpell(me, evasion, true))
            {
                evasionCooldown = CD_EVASION;
                return true;
            }
        }

        // ---- 暗影斗篷：被多重魔法/疾病压制时开启，兼顾免伤与驱散 ----
        if (cloakCooldown == 0 && CountHarmfulMagicDebuffs() >= MAGIC_DEBUFF_COUNT)
        {
            uint32 const cloak = GetAppropriateRank(AssassinationRogueSpells::CLOAK_OF_SHADOWS, false);
            if (cloak && !me->HasAura(cloak) && CanCast(me, cloak, true) && ExecuteSpell(me, cloak, true))
            {
                cloakCooldown = CD_CLOAK;

                // Off-GCD 铁律：暗影斗篷独立于公共冷却，施放成功严禁 return true，
                // 否则会白吞当帧的脚踢打断与毒伤终结技窗口 (自保技挤占输出帧)。
            }
        }

        // ---- 佯攻：承伤压力下减免范围伤害并降低仇恨，防止被 AoE 直接压死 ----
        if (feintCooldown == 0 && me->GetHealthPct() < FEINT_HP_PCT && IsUnderPhysicalMelee(me))
        {
            uint32 const feint = GetAppropriateRank(AssassinationRogueSpells::FEINT, false);
            if (feint && CanCast(me, feint, true) && ExecuteSpell(me, feint, true))
            {
                feintCooldown = CD_FEINT;
                return true;
            }
        }

        // ---- 嫁祸诀窍：起手/冷却就绪时把仇恨预先转移给主坦并附赠增伤 ----
        if (TryTricksOfTheTrade())
            return true;

        return false;
    }

    bool TryTricksOfTheTrade()
    {
        if (tricksCooldown > 0)
            return false;

        uint32 const tricks = GetAppropriateRank(AssassinationRogueSpells::TRICKS_OF_THE_TRADE, false);
        if (!tricks || me->HasAura(tricks))
            return false;

        Unit* tank = GetGroupTank();
        if (!tank || tank == me || !tank->IsAlive() || !tank->IsInWorld() || tank->GetMap() != me->GetMap())
            return false;

        // 注：此处不再限制 tank 必须为玩家。坦克随从同样能承接嫁祸，
        // 由 CanCast/ExecuteSpell 负责最终的目标类型合法性校验，
        // 严禁在上层硬编码拦截，否则纯随从队伍将永久失去仇恨转移手段。
        if (!CanCast(tank, tricks, true) || !ExecuteSpell(tank, tricks, true))
            return false;

        tricksCooldown = CD_TRICKS;
        return true;
    }

    // =========================================================================
    // P2: 爆发与打断 (Off-GCD，当帧顺下绝不 return)
    // =========================================================================
    void TryBurstAndInterrupt(Unit* victim)
    {
        if (!victim)
            return;

        // ---- 脚踢：打断目标读条 (能量不足时严禁空放，交回产星循环优先保输出) ----
        if (kickCooldown == 0 && IsInterruptibleTarget(victim) && me->GetPower(POWER_ENERGY) >= KICK_ENERGY)
        {
            uint32 const kick = GetAppropriateRank(AssassinationRogueSpells::KICK, false);
            if (kick && CanCast(victim, kick, true) && ExecuteSpell(victim, kick, true))
                kickCooldown = CD_KICK;
        }

        // ---- 冷血：连击点已达终结技门槛且当帧能量足以打出毒伤时开启 ----
        // 冷血为 Off-GCD 增益，必须在同一帧内被毒伤吃掉，
        // 否则会遗留光环被后续产星技白白消耗 (经典「冷血喂毁伤」事故)。
        uint8 const cp = GetComboPoints(victim);
        if (coldBloodCooldown == 0 && cp >= ENVENOM_MIN_CP && !me->HasAura(AssassinationRogueSpells::COLD_BLOOD) &&
            me->GetPower(POWER_ENERGY) >= ENVENOM_ENERGY)
        {
            uint32 const coldBlood = GetAppropriateRank(AssassinationRogueSpells::COLD_BLOOD, true);
            if (coldBlood && CanCast(me, coldBlood, true) && ExecuteSpell(me, coldBlood, true))
                coldBloodCooldown = CD_COLD_BLOOD;

            // Off-GCD 铁律：严禁在此 return，必须允许当帧决策流顺下，让毒伤立即吃满必暴。
        }
    }

    // =========================================================================
    // P3: 关键增益常驻维持
    // =========================================================================
    bool MaintainSliceAndDice(Unit* victim)
    {
        // 切割为攻速核心增益：后续由毒伤 + 名天堑 (Cut to the Chase) 自动刷新至 5 星时长
        if (HasSliceAndDice())
            return false;

        uint8 const cp = GetComboPoints(victim);
        if (cp < 1)
            return false;

        uint32 const sliceAndDice = GetAppropriateRank(AssassinationRogueSpells::SLICE_AND_DICE, false);
        if (!sliceAndDice || !CanCast(me, sliceAndDice, true))
            return false;

        if (ExecuteSpell(me, sliceAndDice, true))
        {
            SpendComboPoints();
            return true;
        }

        return false;
    }

    bool MaintainHungerForBlood(Unit* victim)
    {
        if (HasHungerForBlood())
            return false;

        // 血之饥渴需要目标身上存在流血效果才能生效；
        // 无流血时应先由 P4 用割裂铺垫，严禁在此硬放导致被底层拒绝。
        if (!HasBleedEffect(victim))
            return false;

        uint32 const hungerForBlood = GetAppropriateRank(AssassinationRogueSpells::HUNGER_FOR_BLOOD, true);
        if (!hungerForBlood || !CanCast(me, hungerForBlood, true))
            return false;

        return ExecuteSpell(me, hungerForBlood, true);
    }

    // =========================================================================
    // 武器毒药模拟注入 (随从无武器涂毒对象，由白字平砍与毁伤命中驱动)
    // =========================================================================
    void ProcPoisons(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        // 白字命中的几何前提：脱离近战判定区时不会触发武器毒药
        if (!me->IsWithinMeleeRange(victim))
            return;

        // 巡检节流：逐帧触发会把毒药变成无上限的伤害来源，
        // 按 1000ms 周期结算，等效玩家武器的毒药 PPM 节奏。
        if (poisonProcTimer > 0)
            return;

        poisonProcTimer = POISON_PROC_INTERVAL;

        // ---- A. 致命毒药：维持满 5 层，并在剩余 3 秒时提前续期 ----
        uint32 const deadlyPoison = GetAppropriateRank(AssassinationRogueSpells::DEADLY_POISON, false);
        if (deadlyPoison)
        {
            Aura* poisonAura = victim->GetAura(deadlyPoison, me->GetGUID());

            bool const needRefresh = (!poisonAura) ||
                                     (poisonAura->GetStackAmount() < DEADLY_POISON_MAX_STACKS) ||
                                     (poisonAura->GetDuration() < static_cast<int32>(DEADLY_POISON_REFRESH_MS));

            if (needRefresh)
            {
                // triggered = true：毒药模拟属引擎外补偿，严禁占用 GCD 与读条通道
                me->CastSpell(victim, deadlyPoison, true);
            }
        }

        // ---- B. 速效毒药：按概率补充直接自然伤害 ----
        if (urand(0, 99) < INSTANT_POISON_CHANCE)
        {
            uint32 const instantPoison = GetAppropriateRank(AssassinationRogueSpells::INSTANT_POISON, false);
            if (instantPoison)
                me->CastSpell(victim, instantPoison, true);
        }
    }

    // =========================================================================
    // P4: 连击点循环与终结技
    // =========================================================================
    bool TryComboPointRotation(Unit* victim)
    {
        uint8 const cp = GetComboPoints(victim);

        // ---- 终结技攒能挂起 (Energy Pooling) ----
        // 连击点已到 4~5 星但能量不足以支付终结技时，必须原地挂起等待能量回充。
        // 此时若贪打一发毁伤/邪恶攻击，会同时踩两个坑：连击点被顶到 5 星上限白白溢出，
        // 且残余能量被产星技吸干，终结技要再等一整轮回能才能打出，DPS 直接塌方。
        // 挂起 return false 后决策流顺下至 P5 贴背与白字平砍 (平砍不耗能量)，
        // 能量一到即由下方终结技分支当帧顺发。
        if (cp >= ENVENOM_MIN_CP && me->GetPower(POWER_ENERGY) < ENVENOM_ENERGY)
            return false;

        // ---- 终结技分支：连击点 >= 4 且能量充足 ----
        if (cp >= ENVENOM_MIN_CP && TryFinisher(victim))
            return true;

        // ---- 产星分支：毁伤 (双持匕首主力，+2 星) ----
        return TryMutilate(victim);
    }

    bool TryFinisher(Unit* victim)
    {
        // 目标无流血 且 自身缺血之饥渴增益：优先割裂铺垫流血，打通增伤链路
        if (!HasHungerForBlood() && !HasBleedEffect(victim) && TryRupture(victim))
            return true;

        // 核心终结技：毒伤 (自然伤害 + 名天堑联动刷新切割)
        if (TryEnvenom(victim))
            return true;

        // 62 级前未习得毒伤，或致命毒药被打断导致毒伤被底层拒绝：
        // 以剔骨打出高额物理终结直伤兜底，绝不让 5 星满溢空转导致产星停摆。
        if (TryEviscerate(victim))
            return true;

        // 末位兜底：割裂铺垫流血，至少保住血之饥渴的增伤跳板
        return TryRupture(victim);
    }

    bool TryEnvenom(Unit* victim)
    {
        uint32 const envenom = GetAppropriateRank(AssassinationRogueSpells::ENVENOM, false);
        if (!envenom || !CanCast(victim, envenom, true))
            return false;

        if (ExecuteSpell(victim, envenom, true))
        {
            SpendComboPoints();
            return true;
        }

        return false;
    }

    bool TryEviscerate(Unit* victim)
    {
        // 剔骨为单阶法术，全程可用的物理终结技 (62 级前替代毒伤)
        uint32 const eviscerate = GetAppropriateRank(AssassinationRogueSpells::EVISCERATE, false);
        if (!eviscerate || !CanCast(victim, eviscerate, true))
            return false;

        if (ExecuteSpell(victim, eviscerate, true))
        {
            SpendComboPoints();
            return true;
        }

        return false;
    }

    bool TryRupture(Unit* victim)
    {
        uint32 const rupture = GetAppropriateRank(AssassinationRogueSpells::RUPTURE, false);
        if (!rupture || !CanCast(victim, rupture, true))
            return false;

        if (ExecuteSpell(victim, rupture, true))
        {
            SpendComboPoints();
            return true;
        }

        return false;
    }

    bool TryMutilate(Unit* victim)
    {
        // 主力产星：毁伤 (40 级天赋，双持匕首双段打击，命中 +2 星)
        uint32 const mutilate = GetAppropriateRank(AssassinationRogueSpells::MUTILATE, true);
        if (mutilate && CanCast(victim, mutilate, true) && ExecuteSpell(victim, mutilate, true))
        {
            // 毁伤为双持双段打击，命中即产 2 星 (上限 5 星由基类状态机钳制)
            AddComboPoints(victim, 2);

            // 双持打击同样触发武器毒药结算
            ProcPoisons(victim);
            return true;
        }

        // 40 级前未习得毁伤 (或被底层拒绝)：降级邪恶攻击产星，
        // 保证任何等级段的连击点循环都不出现断层。
        uint32 const sinisterStrike = GetAppropriateRank(AssassinationRogueSpells::SINISTER_STRIKE, false);
        if (!sinisterStrike || !CanCast(victim, sinisterStrike, true))
            return false;

        if (ExecuteSpell(victim, sinisterStrike, true))
        {
            AddComboPoints(victim, 1);
            return true;
        }

        return false;
    }

    // =========================================================================
    // P5: 物理近战背后找背站位模型
    // =========================================================================
    void MaintainMeleeBehindPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        me->SetFacingToObject(victim);

        float const dist = me->GetDistance(victim);
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
        bool const isFollowing = (moveType == FOLLOW_MOTION_TYPE);
        bool const isChasing   = (moveType == CHASE_MOTION_TYPE);

        // 怪物仇恨锚定随从本人：此时严禁绕后找背身位。
        // 因为怪会随随从的移动实时转向，绕后指令会让两者围绕同一圆心无限对转，
        // 形成「旋转木马」同心圆死锁，随从全程贴不上背且一发技能打不出。
        bool const mobTargetingMe = (victim->GetVictim() == me);

        if (mobTargetingMe)
        {
            // 怪看随从时只求贴身：用 MoveChase 直线贴上去，不追求背后位，
            // 待嫁祸/消失把仇恨交回主坦后，再由下方分支恢复严格找背。
            if (!isChasing || dist > MELEE_REACH_DIST)
                me->GetMotionMaster()->MoveChase(victim, MELEE_FOLLOW_DIST);

            return;
        }

        // ---- 已平滑贴身：保持贴背输出，不打断普攻节奏 ----
        if (isFollowing && dist <= MELEE_COMFORT_DIST)
            return;

        // ---- 怪物盯防主坦：严格占住目标背身位，规避正面顺劈/吐息与被招架加速 ----
        // angle 必须传正后方 BEHIND_ANGLE (M_PI)：引擎内部已按目标当前朝向结算偏移，
        // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移；
        // 传 0.0f 会贴在 Boss 脸前吃顺劈与正面吐息，物理近战必须始终占住背后位。
        if (dist > MELEE_REACH_DIST || !isFollowing)
        {
            me->GetMotionMaster()->MoveFollow(victim, MELEE_FOLLOW_DIST, BEHIND_ANGLE);
        }
    }

    // =========================================================================
    // 刺杀天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
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

        SyncPassive(20, AssassinationRogueSpells::GLYPH_OF_MUTILATE);         // 毁伤雕文：毁伤耗能 -5
        SyncPassive(20, AssassinationRogueSpells::GLYPH_OF_HUNGER_FOR_BLOOD); // 血之饥渴雕文：增伤提升至 8%
        SyncPassive(20, AssassinationRogueSpells::GLYPH_OF_TRICKS_OF_TRADE);  // 嫁祸诀窍雕文：增益时长延长
        SyncPassive(30, AssassinationRogueSpells::OVERKILL);                  // 灭绝：潜行及破潜后 20 秒回能 +30%
        SyncPassive(40, AssassinationRogueSpells::MASTER_POISONER);           // 毒药大师：中毒目标受暴击 +3%
        SyncPassive(50, AssassinationRogueSpells::CUT_TO_THE_CHASE);          // 名天堑：毒伤刷新 5 星切割
    }
};

void AddSC_bot_assassination_rogue()
{
    new AdaptiveBotScript<BotAssassinationRogueAI>("bot_assassination_rogue");
}
