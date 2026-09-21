/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "MarksmanshipHunterSpells.h"
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

class BotMarksmanshipHunterAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数
    // =========================================================================
    static constexpr float MELEE_BLIND_DIST  = 8.0f;   // 近战盲区阈值：进入即触发逃脱 / 垫刀
    static constexpr float MAX_ENGAGE_DIST   = 35.0f;  // 脱节上限：超出必须主动压进
    static constexpr float IDEAL_SHOT_DIST   = 20.0f;  // 理想射击站位 15 ~ 30 码，站位目标点取 20 码
    // 撤离锚定坦克背身位时的跟随距离：必须 >= 15 码。
    // 若沿用 6 码贴坦，随从与 Boss 的间距仍落在 8 ~ 12 码近战盲区内，
    // 会持续反复触发逃脱与垫刀，永远无法恢复 15 ~ 30 码射击站位。
    static constexpr float TANK_RETREAT_DIST = 15.0f;  // 近战盲区撤离时贴附坦克的距离 (确保脱离 8 码盲区)

    // 撤退迟滞退出线：必须严格大于 MELEE_BLIND_DIST。
    // 若进退撤退姿态共用 8 码单一阈值，会出现「刚被拉出 8 码即退出撤退并立定，
    // 下一帧又被 Boss 追进 8 码内重新触发撤退」的高频抽搐死锁，
    // 随从全程在原地抖动且一发子弹都打不出去。
    static constexpr float RETREAT_EXIT_DIST  = 15.0f;  // 撤退姿态的退出安全线

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 tank->GetOrientation()，否则站位会随坦克转向持续漂移。
    // M_PI 即锚点正后方：坦克背身位，可规避顺劈斩与正面吐息。
    static constexpr float BEHIND_ANGLE      = static_cast<float>(M_PI);

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的 CD 技能必须由专精自行计时，
    // 否则会因 HasSpellCooldown 恒 false 而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_CHIMERA_SHOT         = 10000;
    static constexpr uint32 CD_CHIMERA_SHOT_GLYPHED = 9000;
    static constexpr uint32 CD_AIMED_SHOT           = 10000;
    static constexpr uint32 CD_KILL_SHOT            = 15000;
    static constexpr uint32 CD_KILL_SHOT_GLYPHED    = 9000;   // 杀戮射击雕文：斩杀目标未死 CD -6s
    static constexpr uint32 CD_ARCANE_SHOT          = 6000;   // 奥术射击本体 CD，防止每帧空烧法力
    static constexpr uint32 CD_RAPTOR_STRIKE        = 6000;   // 猛禽一击本体 CD
    static constexpr uint32 CD_WING_CLIP            = 3000;   // 摔绊本体无 CD，此处仅作节流防刷屏
    static constexpr uint32 CD_DISENGAGE            = 25000;
    static constexpr uint32 CD_DETERRENCE           = 90000;
    static constexpr uint32 CD_FEIGN_DEATH          = 30000;
    static constexpr uint32 CD_MISDIRECTION         = 30000;
    static constexpr uint32 CD_RAPID_FIRE           = 180000;
    static constexpr uint32 CD_READINESS            = 180000;
    static constexpr uint32 CD_SILENCING_SHOT       = 20000;

    // 生命阈值
    static constexpr float FEIGN_DEATH_HP_PCT = 30.0f;
    static constexpr float DETERRENCE_HP_PCT  = 25.0f;
    static constexpr float KILL_SHOT_HP_PCT   = 20.0f;

    // 法力阈值
    static constexpr float VIPER_ASPECT_MANA_PCT = 20.0f;  // 低于此线切蝰蛇回蓝
    static constexpr float HAWK_ASPECT_MANA_PCT  = 55.0f;  // 回升至此线即切回输出守护 (阈值过高会把低伤害期拖得过长)

public:
    explicit BotMarksmanshipHunterAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 远程随从按远程单位接管移动逻辑，禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 猎人专修契约：物理远程与法系远程彻底解耦，输出完全由远程攻击强度 (AP) 支撑
    bool IsRangedPhysicalBot() const override { return true; }

    // 物理远程按真实装备模型结算：严禁继承法系远程的 2.0x ~ 3.3x 法伤放大乘数
    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (稳固/杀戮/毒蛇/龙鹰/蝰蛇/误导/逃脱等) 严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case MarksmanshipHunterSpells::AIMED_SHOT:      return 20;
            case MarksmanshipHunterSpells::DETERRENCE:      return 20;
            case MarksmanshipHunterSpells::SILENCING_SHOT:  return 30;
            case MarksmanshipHunterSpells::FEIGN_DEATH:     return 30;
            case MarksmanshipHunterSpells::TRUESHOT_AURA:   return 40;
            case MarksmanshipHunterSpells::READINESS:       return 50;
            case MarksmanshipHunterSpells::CHIMERA_SHOT:    return 60;
            default:                                        return 0;
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
        ResetHunterTimers();
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
        UpdateHunterTimers(diff);

        // 全局读条/通道双保险守卫：稳固射击读条期间引擎会置位 UNIT_STATE_CASTING，
        // 但引导类法术在部分状态下并不置位该标记，故追加 CURRENT_CHANNELED_SPELL 显式判定，
        // 杜绝长读条与引导被跟随/走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 0. 假死脱困闸门 (最高优先级，覆盖脱战与战斗双分支)
        // ---------------------------------------------------------------------
        // 假死光环在底层会持续压制随从行动 (定身于倒地姿态、强制脱战)，
        // 若不在解除条件达成时主动移除，随从会被永久钉死在地，形成开怪即瘫的
        // 致命死锁。解除条件：已脱离物理近战压制 且 不再被敌对单位越过主坦盯防。
        // 假死期间直接 return，严禁下发任何走位/施法指令 (否则会与倒地姿态互相拉扯)。
        // =====================================================================
        if (me->HasAura(MarksmanshipHunterSpells::FEIGN_DEATH))
        {
            // 躺地时长由自管冷却反算：施放成功时 feignDeathCooldown 被置为 CD_FEIGN_DEATH，
            // 故 (CD_FEIGN_DEATH - feignDeathCooldown) 即已卧地毫秒数。
            // 硬性 1.5 秒超时兜底必不可少：无主坦、被抗性抵抗或威胁判定未命中时，
            // 解除条件可能永远无法满足，随从会在地上躺满 6 分钟假死光环时限，
            // 期间被小怪白白围殴致死。超时站起后由 P0 顺畅接续威慑/逃脱自保链。
            bool const fdTimeout = (CD_FEIGN_DEATH - feignDeathCooldown >= 1500);

            if (fdTimeout || (!IsUnderPhysicalMelee(me) && !IsTopThreatTarget()))
                me->RemoveAurasDueToSpell(MarksmanshipHunterSpells::FEIGN_DEATH);

            return;
        }

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            if (MaintainAspect())
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
            MaintainAspect();
            return;
        }

        // 远程随从仅锚定敌对目标维持进战姿态供射击链路使用，
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- 自动射击通道维持 (远程白字与蝰蛇守护回蓝的唯一来源) ----
        // 自动射击属 CURRENT_AUTOREPEAT_SPELL 循环通道，不占公共冷却，
        // 故 CanCast 必须传 checkGcd = false，否则会被 GCD 门禁整轮拦截。
        // 生效区间与射击循环一致：仅 8 ~ 35 码内维持，近战盲区交由 P4 垫刀处理。
        {
            float const autoShotDist = me->GetDistance(victim);
            if (autoShotDist >= MELEE_BLIND_DIST && autoShotDist <= MAX_ENGAGE_DIST &&
                !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            {
                uint32 const autoShot = GetAppropriateRank(MarksmanshipHunterSpells::AUTO_SHOT, false);
                if (autoShot && CanCast(victim, autoShot, false))
                    me->CastSpell(victim, autoShot, false);
            }
        }

        // ---- P0: 极限自保与仇恨控制 (维度 C 仇恨协同) ----
        if (TrySurvivalAndThreat(victim)) return;

        // ---- P1: 守护切换状态机 ----
        if (MaintainAspect()) return;

        // ---- P2: 爆发与打断 (Off-GCD，严禁 return true，必须当帧顺下) ----
        TryBurstAndInterrupt(victim);

        // ---- P3: 远程爆发与钉刺循环 ----
        if (TryRangedRotation(victim)) return;

        // ---- P4: 近战盲区垫刀 ----
        if (TryMeleeDeadzone(victim)) return;

        // ---- P5: 站位控制 ----
        MaintainRangedPositioning(victim);
    }

private:
    // 自管冷却登记
    uint32 chimeraShotCooldown{ 0 };
    uint32 aimedShotCooldown{ 0 };
    uint32 killShotCooldown{ 0 };
    uint32 arcaneShotCooldown{ 0 };
    uint32 raptorStrikeCooldown{ 0 };
    uint32 wingClipCooldown{ 0 };
    uint32 disengageCooldown{ 0 };
    uint32 deterrenceCooldown{ 0 };
    uint32 feignDeathCooldown{ 0 };
    uint32 misdirectionCooldown{ 0 };
    uint32 rapidFireCooldown{ 0 };
    uint32 readinessCooldown{ 0 };
    uint32 silencingShotCooldown{ 0 };

    // 近战盲区撤离姿态标记：处于该姿态时必须凭此标记主动重发走位指令，
    // 否则会永久粘在坦克身后而无法恢复 15 ~ 30 码射击站位。
    bool isRetreatingToTank{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateHunterTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(chimeraShotCooldown);
        Tick(aimedShotCooldown);
        Tick(killShotCooldown);
        Tick(arcaneShotCooldown);
        Tick(raptorStrikeCooldown);
        Tick(wingClipCooldown);
        Tick(disengageCooldown);
        Tick(deterrenceCooldown);
        Tick(feignDeathCooldown);
        Tick(misdirectionCooldown);
        Tick(rapidFireCooldown);
        Tick(readinessCooldown);
        Tick(silencingShotCooldown);
    }

    void ResetHunterTimers()
    {
        chimeraShotCooldown = 0;
        aimedShotCooldown = 0;
        killShotCooldown = 0;
        arcaneShotCooldown = 0;
        raptorStrikeCooldown = 0;
        wingClipCooldown = 0;
        disengageCooldown = 0;
        deterrenceCooldown = 0;
        feignDeathCooldown = 0;
        misdirectionCooldown = 0;
        rapidFireCooldown = 0;
        readinessCooldown = 0;
        silencingShotCooldown = 0;

        isRetreatingToTank = false;
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

    bool ShouldFeignDeath()
    {
        // 情形一：生命濒危且正承受物理近战压制
        if (me->GetHealthPct() < FEIGN_DEATH_HP_PCT && IsUnderPhysicalMelee(me))
            return true;

        // 情形二：仇恨彻底失控，必须立即清仇恨脱困
        return IsTopThreatTarget();
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

    // =========================================================================
    // P0: 极限自保与仇恨控制
    // =========================================================================
    bool TrySurvivalAndThreat(Unit* victim)
    {
        // ---- 假死：生命濒危且被近战压制，或仇恨彻底失控 (OT) ----
        if (feignDeathCooldown == 0 && ShouldFeignDeath())
        {
            uint32 const feignDeath = GetAppropriateRank(MarksmanshipHunterSpells::FEIGN_DEATH, true);
            if (feignDeath && !me->HasAura(feignDeath) &&
                CanCast(me, feignDeath, true) && ExecuteSpell(me, feignDeath, true))
            {
                feignDeathCooldown = CD_FEIGN_DEATH;
                return true;
            }
        }

        // ---- 威慑：假死冷却中，或假死当前并不适用时的应急免伤 ----
        // 生命 < 25% 时若仍无任何自救手段可用，随从将直接阵亡，故此处补充
        // 「假死 CD 中 或 假死不适用」的兜底判定，避免 20% 血线无牌可打的裸奔窗口。
        if (deterrenceCooldown == 0 && me->GetHealthPct() < DETERRENCE_HP_PCT &&
            (feignDeathCooldown > 0 || !ShouldFeignDeath()))
        {
            uint32 const deterrence = GetAppropriateRank(MarksmanshipHunterSpells::DETERRENCE, true);
            if (deterrence && !me->HasAura(deterrence) &&
                CanCast(me, deterrence, true) && ExecuteSpell(me, deterrence, true))
            {
                deterrenceCooldown = CD_DETERRENCE;
                return true;
            }
        }

        // ---- 逃脱：陷入 8 码近战盲区，向后腾跃拉开距离 ----
        if (disengageCooldown == 0 && victim && me->GetDistance(victim) < MELEE_BLIND_DIST)
        {
            uint32 const disengage = GetAppropriateRank(MarksmanshipHunterSpells::DISENGAGE, false);
            if (disengage && CanCast(me, disengage, true) && ExecuteSpell(me, disengage, true))
            {
                disengageCooldown = CD_DISENGAGE;

                // 瞬发无弹道技：允许在跑位途中施放，严禁 StopMoving；
                // 腾跃后立即标记撤离姿态，由 P5 接管后续站位回正。
                isRetreatingToTank = true;
                return true;
            }
        }

        // ---- 误导：起手/冷却就绪时把仇恨预先转移给主坦 ----
        if (TryMisdirection())
            return true;

        return false;
    }

    bool TryMisdirection()
    {
        if (misdirectionCooldown > 0)
            return false;

        uint32 const misdirection = GetAppropriateRank(MarksmanshipHunterSpells::MISDIRECTION, false);
        if (!misdirection || me->HasAura(misdirection))
            return false;

        Unit* tank = GetGroupTank();
        if (!tank || tank == me || !tank->IsAlive() || !tank->IsInWorld() || tank->GetMap() != me->GetMap())
            return false;

        // 注：此处不再限制 tank 必须为玩家。坦克随从同样能承接误导，
        // 由 CanCast/ExecuteSpell 负责最终的目标类型合法性校验，
        // 严禁在上层硬编码拦截，否则纯随从队伍将永久失去仇恨转移手段。

        if (!CanCast(tank, misdirection, true) || !ExecuteSpell(tank, misdirection, true))
            return false;

        misdirectionCooldown = CD_MISDIRECTION;
        return true;
    }

    // =========================================================================
    // P1: 守护切换状态机 (蝰蛇回蓝 <=> 龙鹰/雄鹰输出)
    // =========================================================================
    uint32 GetDpsAspect() const
    {
        // 80 级优先龙鹰守护；未达等级门槛时交由 GetAppropriateRank 降阶，
        // 降阶失败则回落雄鹰守护，保证任何等级都有输出守护可挂。
        uint32 const dragonhawk = GetAppropriateRank(MarksmanshipHunterSpells::ASPECT_OF_THE_DRAGONHAWK, false);
        if (dragonhawk)
            return dragonhawk;

        return GetAppropriateRank(MarksmanshipHunterSpells::ASPECT_OF_THE_HAWK, false);
    }

    bool TrySwitchAspect(uint32 aspectSpellId)
    {
        if (!aspectSpellId || me->HasAura(aspectSpellId))
            return false;

        if (!CanCast(me, aspectSpellId, true))
            return false;

        return ExecuteSpell(me, aspectSpellId, true);
    }

    bool MaintainAspect()
    {
        // ---- 蝰蛇守护：战时法力枯竭，优先保续战能力 ----
        if (me->IsInCombat() && me->getPowerType() == POWER_MANA &&
            me->GetPowerPct(POWER_MANA) < VIPER_ASPECT_MANA_PCT)
        {
            if (TrySwitchAspect(MarksmanshipHunterSpells::ASPECT_OF_THE_VIPER))
                return true;
        }

        uint32 const dpsAspect = GetDpsAspect();
        if (!dpsAspect)
            return false;

        // ---- 蝰蛇守护挂着且法力已回升 / 已脱战：果断切回输出守护 ----
        if (me->HasAura(MarksmanshipHunterSpells::ASPECT_OF_THE_VIPER))
        {
            // 战时法力未回到安全线前不切回，避免「切龙鹰 -> 空蓝 -> 切蝰蛇」的无尽抖动
            if (me->IsInCombat() && me->GetPowerPct(POWER_MANA) <= HAWK_ASPECT_MANA_PCT)
                return false;

            return TrySwitchAspect(dpsAspect);
        }

        // ---- 输出守护常驻维护 ----
        if (!me->HasAura(dpsAspect))
            return TrySwitchAspect(dpsAspect);

        return false;
    }

    // =========================================================================
    // P2: 爆发与打断 (Off-GCD，当帧顺下绝不 return)
    // =========================================================================
    void TryBurstAndInterrupt(Unit* victim)
    {
        if (!victim)
            return;

        // ---- 沉默射击：接入阶段三基类记忆化压秒打断仲裁引擎 (Off-GCD) ----
        // 降阶参数必须传 false: SILENCING_SHOT 已在 GetTalentSpellMinLevel 登记 30 级门槛,
        // 传 true 会旁路该门槛, 使未习得的低等级随从拿到满阶 ID 并被底层拒放。
        // 打断时机交由 TryInterrupt -> ShouldInterruptTarget 依据 learnedInterruptDelays
        // 压秒出手; 无读条可断时则回落到「急速射击爆发期顺发额外伤害」通道, 绝不空烧 CD。
        if (silencingShotCooldown == 0)
        {
            uint32 const silencingShot = GetAppropriateRank(MarksmanshipHunterSpells::SILENCING_SHOT, false);
            if (silencingShot)
            {
                bool interrupted = false;
                if (victim->HasUnitState(UNIT_STATE_CASTING) && TryInterrupt(victim, silencingShot))
                {
                    silencingShotCooldown = CD_SILENCING_SHOT;
                    interrupted = true;
                }
                else if (!interrupted && me->HasAura(MarksmanshipHunterSpells::RAPID_FIRE) &&
                         CanCast(victim, silencingShot, true) && ExecuteSpell(victim, silencingShot, true))
                {
                    silencingShotCooldown = CD_SILENCING_SHOT;
                }
            }
        }

        // ---- 急速射击：首领/精英目标且仍处于 50% 以上血量(高血量窗口才值得交爆发) ----
        if (rapidFireCooldown == 0 && !me->HasAura(MarksmanshipHunterSpells::RAPID_FIRE))
        {
            uint32 const rapidFire = GetAppropriateRank(MarksmanshipHunterSpells::RAPID_FIRE, false);
            if (rapidFire && victim->GetHealthPct() > 50.0f && IsEliteOrBossTarget(victim))
            {
                if (CanCast(me, rapidFire, true) && ExecuteSpell(me, rapidFire, true))
                    rapidFireCooldown = CD_RAPID_FIRE;
            }
        }

        // ---- 准备就绪：奇美拉与急速射击双双进入 CD 时立即重置全部猎人技能 ----
        // ---- 准备就绪：必须等待第一轮急速射击「光环结束」且奇美拉/瞄准双双进入 CD ----
        // 仅判 rapidFireCooldown > 0 会在开怪瞬间急速射击光环尚未铺开时就触发准备就绪，
        // 当帧把 rapidFireCooldown 归零，2 分钟底牌被自己吞掉，且后续无任何技能可重置。
        // 追加 !HasAura(RAPID_FIRE) 判定，确保第一轮爆发真正打完收工后再刷新第二轮。
        bool const rapidFireDone = (rapidFireCooldown > 0) && !me->HasAura(MarksmanshipHunterSpells::RAPID_FIRE);
        if (readinessCooldown == 0 && rapidFireDone && chimeraShotCooldown > 0 && aimedShotCooldown > 0)
        {
            uint32 const readiness = GetAppropriateRank(MarksmanshipHunterSpells::READINESS, false);
            if (readiness && CanCast(me, readiness, true) && ExecuteSpell(me, readiness, true))
            {
                readinessCooldown = CD_READINESS;

                // 当帧立即把全技能 CD 归零，使奇美拉/急速射击能被后续梯队当帧复用
                chimeraShotCooldown = 0;
                aimedShotCooldown = 0;
                killShotCooldown = 0;
                disengageCooldown = 0;
                deterrenceCooldown = 0;
                feignDeathCooldown = 0;
                misdirectionCooldown = 0;
                rapidFireCooldown = 0;
                silencingShotCooldown = 0;

                // Off-GCD 铁律：严禁在此 return，必须允许当帧决策流顺下继续输出。
            }
        }
    }

    // =========================================================================
    // P3: 远程爆发与钉刺循环
    // =========================================================================
    bool TryKillShot(Unit* victim)
    {
        if (killShotCooldown > 0 || !victim || victim->GetHealthPct() >= KILL_SHOT_HP_PCT)
            return false;

        uint32 const killShot = GetAppropriateRank(MarksmanshipHunterSpells::KILL_SHOT, false);
        if (!killShot || !CanCast(victim, killShot, true))
            return false;

        if (ExecuteSpell(victim, killShot, true))
        {
            // 杀戮射击雕文：斩杀目标未被击杀时 CD 由 15s 缩短至 9s，
            // 使低血量窗口内能多打出一发斩杀，显著提升收尾效率。
            killShotCooldown = me->HasAura(MarksmanshipHunterSpells::GLYPH_OF_KILL_SHOT)
                             ? CD_KILL_SHOT_GLYPHED
                             : CD_KILL_SHOT;
            return true;
        }

        return false;
    }

    bool TrySerpentSting(Unit* victim)
    {
        if (!victim)
            return false;

        uint32 const serpentSting = GetAppropriateRank(MarksmanshipHunterSpells::SERPENT_STING, false);
        if (!serpentSting)
            return false;

        // 光环防顶守卫 + 施法者归属鉴别：目标已存在「本随从自己」的毒蛇钉刺时严禁重复刷新
        // (奇美拉射击会自行续期)，否则会白白顶掉剩余跳数并浪费瞬发 GCD。
        // 必须带 me->GetGUID() 自查：团队若存在其他猎人，用 HasAura 会把队友的钉刺
        // 误判为自己的，导致本随从终生不再补钉刺，奇美拉射击的 40% 自然爆击链路彻底失效。
        bool const hasMySting = (victim->GetAura(serpentSting, me->GetGUID()) != nullptr);
        if (hasMySting)
            return false;

        if (!CanCast(victim, serpentSting, true))
            return false;

        return ExecuteSpell(victim, serpentSting, true);
    }

    bool TryChimeraShot(Unit* victim)
    {
        if (chimeraShotCooldown > 0 || !victim)
            return false;

        uint32 const chimeraShot = GetAppropriateRank(MarksmanshipHunterSpells::CHIMERA_SHOT, false);
        if (!chimeraShot)
            return false;

        // 奇美拉射击的核心收益来自刷新毒蛇钉刺：无钉刺铺垫时严禁空放，
        // 应回到铺垫梯队先补钉刺。
        // 钉刺归属鉴别：必须确认目标身上挂的是「本随从自己施放」的毒蛇钉刺。
        // 若仅用 HasAura，队友猎人的钉刺会被误认为有效跳板，本随从的奇美拉射击
        // 将无法刷新自己的钉刺、也吃不到 40% 自然伤害加成，团队双猎人时收益直接归零。
        uint32 const serpentSting = GetAppropriateRank(MarksmanshipHunterSpells::SERPENT_STING, false);
        bool const hasMySting = serpentSting && (victim->GetAura(serpentSting, me->GetGUID()) != nullptr);
        if (!hasMySting)
            return false;

        if (!CanCast(victim, chimeraShot, true))
            return false;

        if (ExecuteSpell(victim, chimeraShot, true))
        {
            chimeraShotCooldown = me->HasAura(MarksmanshipHunterSpells::GLYPH_OF_CHIMERA_SHOT)
                                ? CD_CHIMERA_SHOT_GLYPHED
                                : CD_CHIMERA_SHOT;
            return true;
        }

        return false;
    }

    bool TryAimedShot(Unit* victim)
    {
        if (aimedShotCooldown > 0 || !victim)
            return false;

        uint32 const aimedShot = GetAppropriateRank(MarksmanshipHunterSpells::AIMED_SHOT, false);
        if (!aimedShot || !CanCast(victim, aimedShot, true))
            return false;

        if (ExecuteSpell(victim, aimedShot, true))
        {
            aimedShotCooldown = CD_AIMED_SHOT;
            return true;
        }

        return false;
    }

    bool TryArcaneShot(Unit* victim)
    {
        if (arcaneShotCooldown > 0 || !victim)
            return false;

        uint32 const arcaneShot = GetAppropriateRank(MarksmanshipHunterSpells::ARCANE_SHOT, false);
        if (!arcaneShot || !CanCast(victim, arcaneShot, true))
            return false;

        // 瞬发射击：允许在跑位途中直接施放，严禁 StopMoving 破坏风筝机动性
        if (ExecuteSpell(victim, arcaneShot, true))
        {
            arcaneShotCooldown = CD_ARCANE_SHOT;
            return true;
        }

        return false;
    }

    bool TrySteadyShot(Unit* victim, uint32 steadyShot)
    {
        if (!steadyShot || !victim)
            return false;

        // 施法资格必须先通过校验再刹停：CanCast 失败 (GCD/超距/被控) 时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(victim, steadyShot, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, steadyShot, true);
    }

    bool TryRangedRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P4 / P5 接管，绝不在此硬读条
        if (dist < MELEE_BLIND_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        // ---- 斩杀期：目标 20% 以下无条件优先杀戮射击 ----
        if (TryKillShot(victim))
            return true;

        // ---- 毒蛇钉刺铺垫 → 奇美拉射击刷新爆发 ----
        if (TrySerpentSting(victim))
            return true;

        if (TryChimeraShot(victim))
            return true;

        // ---- 瞄准射击：物理致死打击 ----
        if (TryAimedShot(victim))
            return true;

        // ---- 移动中或尚未习得稳固射击：瞬发奥术射击填充 ----
        uint32 const steadyShot = GetAppropriateRank(MarksmanshipHunterSpells::STEADY_SHOT, false);
        if (me->isMoving() || !steadyShot)
            return TryArcaneShot(victim);

        // ---- 站桩读条填充：稳固射击 ----
        if (TrySteadyShot(victim, steadyShot))
            return true;

        return TryArcaneShot(victim);
    }

    // =========================================================================
    // P4: 近战盲区垫刀 (逃脱冷却期内的兜底输出来源)
    // =========================================================================
    bool TryMeleeDeadzone(Unit* victim)
    {
        if (!victim)
            return false;

        if (me->GetDistance(victim) >= MELEE_BLIND_DIST)
            return false;

        // 注意：即使陷入盲区，也严禁 me->Attack(victim, true) 开启近战追击，
        // 远程随从恒以 Attack(victim, false) 锚定目标，仅靠技能垫刀过渡。

        // ---- 摔绊：先减速敌人，为逃脱 CD 争取脱身窗口 ----
        if (wingClipCooldown == 0)
        {
            uint32 const wingClip = GetAppropriateRank(MarksmanshipHunterSpells::WING_CLIP, false);
            if (wingClip && !victim->HasAura(wingClip) &&
                CanCast(victim, wingClip, true) && ExecuteSpell(victim, wingClip, true))
            {
                wingClipCooldown = CD_WING_CLIP;
                return true;
            }
        }

        // ---- 猛禽一击：近战 GCD 间隙垫刀，绝不发呆 ----
        if (raptorStrikeCooldown == 0)
        {
            uint32 const raptorStrike = GetAppropriateRank(MarksmanshipHunterSpells::RAPTOR_STRIKE, false);
            if (raptorStrike && CanCast(victim, raptorStrike, true) && ExecuteSpell(victim, raptorStrike, true))
            {
                raptorStrikeCooldown = CD_RAPTOR_STRIKE;
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P5: 站位控制 (风筝与拉开状态机，维持 15 ~ 30 码理想射击站位)
    // =========================================================================
    void MaintainRangedPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // ---- P0: APF 势场紧急避险 (火圈/毒池/顺劈强行接管走位) ----
        // 仅当自身真正落入危险禁区时才由势场规划落点; 若火圈在别处而自身安全,
        // 则坚决站桩读条, 杜绝被友军防挤压斥力推着移步而掐断稳固射击。
        if (IsUnderDangerThreat(2.0f))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;
                float const optDist = std::clamp(me->GetDistance(victim), RETREAT_EXIT_DIST, IDEAL_SHOT_DIST);
                if (PotentialField::CalculateNextPosition(me, victim, optDist, false, false, activeDangerZones, nextX, nextY, nextZ))
                {
                    me->GetMotionMaster()->MovePoint(1, nextX, nextY, nextZ);
                    apfMoveUpdateTimer = 300;
                    return;
                }
            }
            else
            {
                return; // 正在平滑执行避险航点, 不进行路径打断
            }
        }

        me->SetFacingToObject(victim);

        float const dist = me->GetDistance(victim);
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();

        // ---- A. 脱节过远 (> 35 码)：主动压进至理想射击站位 ----
        if (dist > MAX_ENGAGE_DIST)
        {
            isRetreatingToTank = false;

            if (moveType != CHASE_MOTION_TYPE)
                me->GetMotionMaster()->MoveChase(victim, IDEAL_SHOT_DIST);
            return;
        }

        // ---- 撤退迟滞门禁 ----
        // 进入撤退沿用 MELEE_BLIND_DIST (8 码)，退出撤退必须恢复到 RETREAT_EXIT_DIST (15 码)。
        // 严禁拆除 isRetreatingToTank 这一自我粘滞条件：它是迟滞状态的载体，
        // 一旦在脱离 8 码的瞬间就被清空，撤退姿态会与立定分支每帧交替触发，
        // 表现为随从在原地反复起步/刹停的抽搐，且永远无法进入稳定射击窗口。
        bool const needsRetreat = (dist < MELEE_BLIND_DIST) || (isRetreatingToTank && dist < RETREAT_EXIT_DIST);

        // ---- B. 近战盲区 / 尚未拉足安全距离：向坦克背身位撤离，借坦克 AoE 仇恨把小怪拉走 ----
        if (needsRetreat)
        {
            Unit* tank = GetGroupTank();
            bool const canAnchorTank = (tank && tank != me && tank != victim && tank->IsAlive() &&
                                        tank->IsInWorld() && tank->GetMap() == me->GetMap());

            if (canAnchorTank)
            {
                if (!isRetreatingToTank || moveType != FOLLOW_MOTION_TYPE)
                {
                    isRetreatingToTank = true;
                    // angle 必须传正后方：传 0.0f 会贴在坦克脸前，被顺劈斩与正面吐息一并打死。
                    // 严禁自行叠加 tank->GetOrientation()，引擎内部已按目标朝向结算偏移。
                    me->GetMotionMaster()->MoveFollow(tank, TANK_RETREAT_DIST, BEHIND_ANGLE);
                }
                return;
            }

            // 无坦克可依：严禁对 victim 调用 MoveChase —— ChaseMovementGenerator 在
            // 施法者已位于指定距离「以内」时会原地立定，而随从此刻恰处于 8 码盲区内，
            // 结果是发出指令却纹丝不动，形成粘脸卡死。必须改用矢量外推硬位移：
            // 以 victim 为原点，沿 victim -> me 的当前向量外推至 IDEAL_SHOT_DIST 落点。
            if (!isRetreatingToTank || moveType != POINT_MOTION_TYPE)
            {
                isRetreatingToTank = true;

                float const angle = victim->GetAngle(me);
                float const retreatX = victim->GetPositionX() + IDEAL_SHOT_DIST * std::cos(angle);
                float const retreatY = victim->GetPositionY() + IDEAL_SHOT_DIST * std::sin(angle);

                me->GetMotionMaster()->MovePoint(0, retreatX, retreatY, me->GetPositionZ());
            }
            return;
        }

        // ---- C. 已回到有效射程：立定射击，清空遗留走位发生器 ----
        // 严禁放任 chase / follow 发生器常驻：残余走位会持续拉扯随从，
        // 使稳固射击的 2 秒读条与站位反复互相打断，表现为原地抽搐。
        if (moveType == CHASE_MOTION_TYPE || moveType == FOLLOW_MOTION_TYPE)
        {
            isRetreatingToTank = false;
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveIdle();
            me->StopMoving();
        }
    }

    // =========================================================================
    // 射击天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
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

        SyncPassive(20, MarksmanshipHunterSpells::MORTAL_SHOTS);          // 致死射击：暴击伤害 +30%
        SyncPassive(20, MarksmanshipHunterSpells::GLYPH_OF_CHIMERA_SHOT); // 奇美拉射击雕文：奇美拉 CD -1s
        SyncPassive(20, MarksmanshipHunterSpells::GLYPH_OF_SERPENT_STING);// 毒蛇钉刺雕文：毒蛇持续 +6s
        SyncPassive(20, MarksmanshipHunterSpells::GLYPH_OF_KILL_SHOT);    // 杀戮射击雕文：斩杀目标未死重置 CD
        SyncPassive(30, MarksmanshipHunterSpells::PIERCING_SHOTS);        // 穿刺射击：暴击附带 30% 流血
        SyncPassive(30, MarksmanshipHunterSpells::MASTER_MARKSMAN);       // 射击大师：暴击 +5%，稳固耗蓝 -25%
        SyncPassive(40, MarksmanshipHunterSpells::TRUESHOT_AURA);         // 强击光环：全队 AP +10%
    }
};

void AddSC_bot_marksmanship_hunter()
{
    new AdaptiveBotScript<BotMarksmanshipHunterAI>("bot_marksmanship_hunter");
}
