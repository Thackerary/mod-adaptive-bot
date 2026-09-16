/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "SurvivalHunterSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Chat.h"
#include <cmath>

class BotSurvivalHunterAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数 (铁律 17 迟滞区间 / 铁律 36 射程收敛)
    // -------------------------------------------------------------------------
    // 生存猎人的主力射击通道 (稳固/瞄准/杀戮/爆炸) 均为标准 35 码体系,
    // 不存在 24 码短射程填充技拘束, 故最大交战距离维持 35 码, 理想站桩位 28 码。
    // =========================================================================
    static constexpr float DEADZONE_RETREAT_DIST = 8.0f;   // 近战盲区撤退进入线
    static constexpr float MIN_ENGAGE_DIST       = 15.0f;  // 安全站桩读条下限线
    static constexpr float RETREAT_SAFE_DIST     = 16.0f;  // 撤退姿态退出安全线 (必须 > 进入线)
    static constexpr float MAX_ENGAGE_DIST       = 35.0f;  // 最大交战距离 (脱节上限)
    static constexpr float IDEAL_SHOT_DIST       = 28.0f;  // 理想射击站位

    // 撤离锚定坦克背身位时的跟随距离: 必须 >= RETREAT_SAFE_DIST。
    // 若沿用 6~8 码贴坦, 随从与 Boss 的间距仍落在 8 码近战盲区内,
    // 会持续反复触发逃脱与撤退, 永远无法恢复 15 码外的射击站位。
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
    static constexpr uint32 CD_EXPLOSIVE_SHOT      = 6000;
    static constexpr uint32 CD_BLACK_ARROW         = 24000;
    static constexpr uint32 CD_AIMED_SHOT          = 10000;
    static constexpr uint32 CD_KILL_SHOT           = 15000;
    static constexpr uint32 CD_KILL_SHOT_GLYPHED   = 9000;   // 杀戮射击雕文: CD -6s
    static constexpr uint32 CD_RAPID_FIRE          = 180000;
    static constexpr uint32 CD_MISDIRECTION        = 30000;
    static constexpr uint32 CD_FEIGN_DEATH         = 30000;
    static constexpr uint32 CD_DETERRENCE          = 90000;
    static constexpr uint32 CD_DISENGAGE           = 25000;

    // 危机阈值
    static constexpr float DETERRENCE_HP_PCT  = 35.0f;
    static constexpr float FEIGN_DEATH_HP_PCT = 30.0f;
    static constexpr float KILL_SHOT_HP_PCT   = 20.0f;

    // 法力阈值 (守护切换状态机)
    static constexpr float VIPER_ASPECT_MANA_PCT      = 20.0f;  // <= 20% 切入蝰蛇守护回蓝
    static constexpr float DRAGONHAWK_ASPECT_MANA_PCT = 60.0f;  // >= 60% 切回龙鹰输出守护

    // 刷新与仲裁窗口
    static constexpr int32  LOCK_AND_LOAD_DOT_WINDOW    = 1000;  // 荷枪实弹防吞跳安全窗口
    static constexpr int32  SERPENT_STING_REFRESH_WINDOW = 2000;  // 毒蛇钉刺补挂窗口
    static constexpr uint32 FEIGN_DEATH_ESCAPE_TIMEOUT  = 1500;  // 假死硬性超时逃逸

    // 满阶被动天赋与雕文解锁等级 (严格注入满阶 Rank 根源, 铁律 33)
    static constexpr uint8 LEVEL_GLYPH              = 20;
    static constexpr uint8 LEVEL_THRILL_OF_THE_HUNT = 35;
    static constexpr uint8 LEVEL_TNT                = 40;
    static constexpr uint8 LEVEL_SURVIVAL_EXPERIENCE = 40;
    static constexpr uint8 LEVEL_TRAP_MASTERY       = 45;
    static constexpr uint8 LEVEL_SNIPER_TRAINING    = 45;
    static constexpr uint8 LEVEL_NOXIOUS_STINGS     = 50;
    static constexpr uint8 LEVEL_HUNTING_PARTY      = 55;
    static constexpr uint8 LEVEL_LOCK_AND_LOAD      = 60;

public:
    explicit BotSurvivalHunterAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // 生存猎人为纯物理远程: 走真实装备模型结算,
    // 严禁继承法系远程的 2.0x ~ 3.3x 法伤放大乘数。
    // =========================================================================
    bool IsHealerBot() const override { return false; }
    bool IsRangedBot() const override { return true; }
    bool IsRangedPhysicalBot() const override { return true; }

    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注: 3.3.5a 中纯天赋技能 DBC SpellLevel 恒为 0, GetAppropriateRank 无法降阶,
    //     必须在此登记最低解锁等级并在施法前显式门禁。
    //     基础法术 (自动射击/稳固/瞄准/杀戮/毒蛇/误导/逃脱/威慑/假死/守护) 严禁登记于此。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case SurvivalHunterSpells::BLACK_ARROW:    return 50;
            case SurvivalHunterSpells::EXPLOSIVE_SHOT: return 60;
            default:                                   return 0;
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

        ResetSurvivalTimers();
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
        UpdateSurvivalTimers(diff);

        // 全局读条/引导双保险守卫: 读条期间引擎置位 UNIT_STATE_CASTING,
        // 引导类法术在部分状态下不置位, 故追加 CURRENT_CHANNELED_SPELL 显式判定,
        // 杜绝读条被跟随移动指令 (UpdateFollowMaster / MoveFollow) 掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 0. 假死脱困闸门 (最高优先级, 覆盖脱战与战斗双分支)
        // ---------------------------------------------------------------------
        // 假死光环在底层会持续压制随从行动 (定身于倒地姿态、强制脱战),
        // 若不在解除条件达成时主动移除, 随从会被永久钉死在地形成致命死锁。
        // 解除条件: 已脱离物理近战压制 且 不再被敌对单位越过主坦盯防。
        // 硬性 1.5 秒超时兜底必不可少: 无主坦或被抵抗时解除条件可能永远无法满足,
        // 随从会在地上躺满 6 分钟假死光环时限, 期间被小怪白白围殴致死。
        // =====================================================================
        if (me->HasAura(SurvivalHunterSpells::FEIGN_DEATH))
        {
            bool const fdTimeout = (CD_FEIGN_DEATH - feignDeathCooldown >= FEIGN_DEATH_ESCAPE_TIMEOUT);

            if (fdTimeout || (!IsUnderPhysicalMelee(me) && !IsTopThreatTarget()))
                me->RemoveAurasDueToSpell(SurvivalHunterSpells::FEIGN_DEATH);

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
        // 2. 索敌仲裁
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() ||
            victim->GetMap() != me->GetMap() || !me->IsValidAttackTarget(victim))
        {
            MaintainAspect();
            return;
        }

        // 远程随从仅锚定敌对目标维持进战姿态供射击链路使用,
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- 自动射击通道维持 (铁律 18, 不占 GCD 必须 checkGcd = false) ----
        MaintainAutoShot(victim);

        // ---- P0: 濒死自保与脱困 (铁律 21) ----
        if (TrySurvivalAndThreat(victim)) return;

        // ---- P1: 守护切换与误导 (维度 C 仇恨协同) ----
        if (MaintainAspect()) return;
        if (TryMisdirection()) return;

        // ---- P2: 爆发大招 (Off-GCD, 严禁 return, 必须当帧顺下) ----
        TryBurstCooldowns(victim);

        // ---- P3: 核心打击 FCFS ----
        if (TryRangedRotation(victim)) return;

        // ---- P5: 站位控制 ----
        MaintainRangedPositioning(victim);
    }

private:
    // =========================================================================
    // 自管冷却与状态登记
    // =========================================================================
    uint32 explosiveShotCooldown{ 0 };
    uint32 blackArrowCooldown{ 0 };
    uint32 aimedShotCooldown{ 0 };
    uint32 killShotCooldown{ 0 };
    uint32 rapidFireCooldown{ 0 };
    uint32 misdirectionCooldown{ 0 };
    uint32 feignDeathCooldown{ 0 };
    uint32 deterrenceCooldown{ 0 };
    uint32 disengageCooldown{ 0 };

    // 近战盲区撤离迟滞姿态标记 (铁律 17): 必须凭此标记主动重发走位指令,
    // 否则会在脱离 8 码的瞬间被清空, 与立定分支每帧交替触发形成原地抽搐。
    bool isRetreating{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateSurvivalTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(explosiveShotCooldown);
        Tick(blackArrowCooldown);
        Tick(aimedShotCooldown);
        Tick(killShotCooldown);
        Tick(rapidFireCooldown);
        Tick(misdirectionCooldown);
        Tick(feignDeathCooldown);
        Tick(deterrenceCooldown);
        Tick(disengageCooldown);
    }

    void ResetSurvivalTimers()
    {
        explosiveShotCooldown = 0;
        blackArrowCooldown = 0;
        aimedShotCooldown = 0;
        killShotCooldown = 0;
        rapidFireCooldown = 0;
        misdirectionCooldown = 0;
        feignDeathCooldown = 0;
        deterrenceCooldown = 0;
        disengageCooldown = 0;

        isRetreating = false;
    }

    // =========================================================================
    // 战场态势判定器
    // =========================================================================
    // 天赋门禁: 登记在 GetTalentSpellMinLevel 的技能必须显式校验等级,
    // 否则 GetAppropriateRank(..., true) 会无视等级返回最高 Rank ID, 造成「已习得」假象。
    bool HasTalent(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        return minLevel == 0 || me->GetLevel() >= minLevel;
    }

    static bool IsUnderPhysicalMelee(Unit* unit)
    {
        if (!unit)
            return false;

        for (Unit* attacker : unit->getAttackers())
        {
            if (attacker && attacker->IsAlive() && attacker->GetMap() == unit->GetMap() && attacker->IsWithinMeleeRange(unit))
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

    bool ShouldFeignDeath()
    {
        // 情形一: 生命濒危且正承受物理近战压制
        if (me->GetHealthPct() < FEIGN_DEATH_HP_PCT && IsUnderPhysicalMelee(me))
            return true;

        // 情形二: 仇恨彻底失控, 必须立即清仇恨脱困
        return IsTopThreatTarget();
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

    // 本随从施加的爆炸射击 DoT 是否仍在安全跳动窗口内 (铁律: 荷枪实弹防吞跳仲裁输入信号)
    bool HasFreshExplosiveDot(Unit* victim) const
    {
        if (!victim)
            return false;

        // 必须使用 GetAuraOfRankedSpell: 施法入口传入的是满阶 60053 (Rank 4),
        // 而 DoT 落地的 Rank 1 光环 53301 与满阶 ID 不同, 用精确 ID 查验会在
        // 80 级恒返回 nullptr, 导致荷枪实弹防吞跳仲裁彻底失效 (DoT 被无脑顶掉)。
        Aura* dot = victim->GetAuraOfRankedSpell(SurvivalHunterSpells::EXPLOSIVE_SHOT, me->GetGUID());
        return dot && dot->GetDuration() > LOCK_AND_LOAD_DOT_WINDOW;
    }

    // =========================================================================
    // P0: 濒死自保与脱困
    // =========================================================================
    bool TrySurvivalAndThreat(Unit* victim)
    {
        // ---- 假死: 仇恨彻底失控 (OT) 或濒死且被近战压制 ----
        if (feignDeathCooldown == 0 && ShouldFeignDeath())
        {
            uint32 const feignDeath = GetAppropriateRank(SurvivalHunterSpells::FEIGN_DEATH, false);
            if (feignDeath && !me->HasAura(feignDeath) &&
                CanCast(me, feignDeath, true) && ExecuteSpell(me, feignDeath, true))
            {
                feignDeathCooldown = CD_FEIGN_DEATH;
                return true;
            }
        }

        // ---- 威慑: 生命 < 35% 的硬减伤 (100% 招架/偏斜) ----
        // 补充「假死 CD 中 或 假死不适用」判定, 避免 35% 血线无牌可打的裸奔窗口。
        if (deterrenceCooldown == 0 && me->GetHealthPct() < DETERRENCE_HP_PCT &&
            (feignDeathCooldown > 0 || !ShouldFeignDeath()))
        {
            uint32 const deterrence = GetAppropriateRank(SurvivalHunterSpells::DETERRENCE, false);
            if (deterrence && !me->HasAura(deterrence) &&
                CanCast(me, deterrence, true) && ExecuteSpell(me, deterrence, true))
            {
                deterrenceCooldown = CD_DETERRENCE;
                return true;
            }
        }

        // ---- 逃脱: 陷入 8 码近战盲区, 向后腾跃拉开距离 ----
        if (disengageCooldown == 0 && victim && me->GetDistance(victim) < DEADZONE_RETREAT_DIST)
        {
            uint32 const disengage = GetAppropriateRank(SurvivalHunterSpells::DISENGAGE, false);
            if (disengage && CanCast(me, disengage, true) && ExecuteSpell(me, disengage, true))
            {
                disengageCooldown = CD_DISENGAGE;

                // 腾跃后立即标记撤离姿态, 由 P5 接管后续站位回正
                isRetreating = true;
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P1-a: 守护切换状态机 (蝰蛇回蓝 <=> 龙鹰输出)
    // =========================================================================
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
        uint32 const viperAspect = GetAppropriateRank(SurvivalHunterSpells::ASPECT_OF_THE_VIPER, false);
        uint32 const dpsAspect = GetAppropriateRank(SurvivalHunterSpells::ASPECT_OF_THE_DRAGONHAWK, false);

        // ---- 蝰蛇守护: 战时法力枯竭, 优先保续战能力 ----
        if (me->IsInCombat() && me->getPowerType() == POWER_MANA &&
            me->GetPowerPct(POWER_MANA) <= VIPER_ASPECT_MANA_PCT)
        {
            if (TrySwitchAspect(viperAspect))
                return true;
        }

        if (!dpsAspect)
            return false;

        // ---- 蝰蛇守护挂着且法力已回升 / 已脱战: 果断切回输出守护 ----
        if (me->HasAura(SurvivalHunterSpells::ASPECT_OF_THE_VIPER))
        {
            // 战时法力未回到 60% 安全线前不切回, 避免「切龙鹰 -> 空蓝 -> 切蝰蛇」的无尽抖动
            if (me->IsInCombat() && me->GetPowerPct(POWER_MANA) < DRAGONHAWK_ASPECT_MANA_PCT)
                return false;

            return TrySwitchAspect(dpsAspect);
        }

        // ---- 输出守护常驻维护 ----
        if (!me->HasAura(dpsAspect))
            return TrySwitchAspect(dpsAspect);

        return false;
    }

    // =========================================================================
    // P1-b: 误导 (起手/冷却就绪时把仇恨预先转移给主坦)
    // =========================================================================
    bool TryMisdirection()
    {
        if (misdirectionCooldown > 0)
            return false;

        uint32 const misdirection = GetAppropriateRank(SurvivalHunterSpells::MISDIRECTION, false);
        if (!misdirection || me->HasAura(misdirection))
            return false;

        Unit* tank = GetGroupTank();
        if (!tank || tank == me || !tank->IsAlive() || !tank->IsInWorld() || tank->GetMap() != me->GetMap())
            return false;

        // 注: 此处不再限制 tank 必须为玩家, 坦克随从同样能承接误导,
        // 目标类型合法性交由 CanCast / ExecuteSpell 最终校验。
        if (!CanCast(tank, misdirection, true) || !ExecuteSpell(tank, misdirection, true))
            return false;

        misdirectionCooldown = CD_MISDIRECTION;
        return true;
    }

    // =========================================================================
    // P2: 爆发大招 (Off-GCD, 当帧顺下绝不 return)
    // =========================================================================
    void TryBurstCooldowns(Unit* victim)
    {
        if (!victim)
            return;

        // ---- 急速射击: 首领/精英目标且仍处于 20% 以上血量(高血量窗口才值得交爆发) ----
        // 铁律 8: Off-GCD 技能施放成功后严禁 return, 必须允许当帧决策流顺下,
        // 使 40% 远程急速光环能当帧被后续射击链路消费。
        if (rapidFireCooldown == 0 && !me->HasAura(SurvivalHunterSpells::RAPID_FIRE) &&
            victim->GetHealthPct() > KILL_SHOT_HP_PCT && IsEliteOrBossTarget(victim))
        {
            uint32 const rapidFire = GetAppropriateRank(SurvivalHunterSpells::RAPID_FIRE, false);
            if (rapidFire && CanCast(me, rapidFire, true) && ExecuteSpell(me, rapidFire, true))
            {
                rapidFireCooldown = CD_RAPID_FIRE;
            }
        }
    }

    // =========================================================================
    // P4: 自动射击通道维持 (铁律 18)
    // -------------------------------------------------------------------------
    // 原生引擎中 me->Attack(victim, false) 仅维持进战姿态, 不驱动 Creature 的远程白字。
    // 自动射击属 CURRENT_AUTOREPEAT_SPELL 循环通道, 不占公共冷却,
    // 故 CanCast 必须传 checkGcd = false, 否则会被 GCD 门禁整轮拦截。
    // =========================================================================
    void MaintainAutoShot(Unit* victim)
    {
        if (!victim)
            return;

        float const dist = me->GetDistance(victim);
        if (dist < DEADZONE_RETREAT_DIST || dist > MAX_ENGAGE_DIST)
            return;

        if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            return;

        uint32 const autoShot = GetAppropriateRank(SurvivalHunterSpells::AUTO_SHOT, false);
        if (autoShot && CanCast(victim, autoShot, false))
            me->CastSpell(victim, autoShot, false);
    }

    // =========================================================================
    // P3: 核心打击 FCFS 优先级
    // =========================================================================
    bool TryRangedRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P5 接管, 绝不在此硬读条
        if (dist < DEADZONE_RETREAT_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        // ---- 1. 斩杀期: 目标 < 20% 绝对最高优先级 ----
        if (TryKillShot(victim))
            return true;

        // ---- 2/3. 荷枪实弹防吞跳仲裁 vs 常规爆炸射击 ----
        bool const lockAndLoadActive = me->HasAura(SurvivalHunterSpells::AURA_LOCK_AND_LOAD);
        bool const explosiveDotFresh = HasFreshExplosiveDot(victim);

        if (lockAndLoadActive)
        {
            // 荷枪实弹期间严禁「DoT 尚在跳动」时补打第二发爆炸射击:
            // 第二发会直接顶掉第一发的剩余跳数, 白白吃掉荷枪实弹层数、GCD 与免 CD 收益。
            // 仅当无 DoT 或剩余 <= 1000ms 时才打出免费爆炸射击,
            // 否则顺下由瞄准射击 / 黑箭 / 毒蛇钉刺完成穿插。
            if (!explosiveDotFresh && TryExplosiveShot(victim, true))
                return true;
        }
        else if (TryExplosiveShot(victim, false))
        {
            return true;
        }

        // ---- 4. 黑箭: 核心暗影 DoT 兼荷枪实弹触发源 ----
        if (TryBlackArrow(victim))
            return true;

        // ---- 5. 毒蛇钉刺: 维持毒性钉刺 3% 独立增伤 ----
        if (TrySerpentSting(victim))
            return true;

        // ---- 6. 瞄准射击: 瞬发远程物理填充 ----
        if (TryAimedShot(victim))
            return true;

        // ---- 7. 稳固射击: 站桩读条填充 ----
        if (TrySteadyShot(victim))
            return true;

        return false;
    }

    bool TryKillShot(Unit* victim)
    {
        if (killShotCooldown > 0 || !victim || victim->GetHealthPct() >= KILL_SHOT_HP_PCT)
            return false;

        uint32 const killShot = GetAppropriateRank(SurvivalHunterSpells::KILL_SHOT, false);
        if (!killShot || !CanCast(victim, killShot, true))
            return false;

        if (ExecuteSpell(victim, killShot, true))
        {
            // 杀戮射击雕文: 斩杀目标未被击杀时 CD 由 15s 缩短至 9s,
            // 使低血量窗口内能多打出一发斩杀, 显著提升收尾效率。
            killShotCooldown = me->HasAura(SurvivalHunterSpells::GLYPH_OF_KILL_SHOT)
                             ? CD_KILL_SHOT_GLYPHED
                             : CD_KILL_SHOT;
            return true;
        }

        return false;
    }

    // -------------------------------------------------------------------------
    // 爆炸射击 (含荷枪实弹免费通道)
    // isFreeCast = true 时由荷枪实弹光环驱动: 免费且不占自身 CD,
    // 严禁写入 explosiveShotCooldown, 否则会吞掉后续常规射击节奏。
    // -------------------------------------------------------------------------
    bool TryExplosiveShot(Unit* victim, bool isFreeCast)
    {
        if (!victim)
            return false;

        if (!isFreeCast && explosiveShotCooldown > 0)
            return false;

        uint32 const explosiveShot = GetAppropriateRank(SurvivalHunterSpells::EXPLOSIVE_SHOT, true);
        if (!explosiveShot || !CanCast(victim, explosiveShot, true))
            return false;

        if (!ExecuteSpell(victim, explosiveShot, true))
            return false;

        if (!isFreeCast)
        {
            explosiveShotCooldown = CD_EXPLOSIVE_SHOT;
        }
        else
        {
            // Creature 缺乏玩家 SpellModOwner/充能扣除机制, 荷枪实弹被消费后
            // 底层不会自动剥层, 若不在免费通道显式扣减充能, 光环会永久挂身,
            // 使爆炸射击退化为无限免 CD 连发并彻底吞掉常规射击节奏。
            if (Aura* lnl = me->GetAura(SurvivalHunterSpells::AURA_LOCK_AND_LOAD))
                lnl->DropCharge();
        }

        return true;
    }

    bool TryBlackArrow(Unit* victim)
    {
        if (blackArrowCooldown > 0 || !victim)
            return false;

        if (!HasTalent(SurvivalHunterSpells::BLACK_ARROW))
            return false;

        uint32 const blackArrow = GetAppropriateRank(SurvivalHunterSpells::BLACK_ARROW, true);
        if (!blackArrow || !CanCast(victim, blackArrow, true))
            return false;

        if (ExecuteSpell(victim, blackArrow, true))
        {
            blackArrowCooldown = CD_BLACK_ARROW;
            return true;
        }

        return false;
    }

    bool TrySerpentSting(Unit* victim)
    {
        if (!victim)
            return false;

        uint32 const serpentSting = GetAppropriateRank(SurvivalHunterSpells::SERPENT_STING, false);
        if (!serpentSting)
            return false;

        // 施法者归属鉴别: 必须确认目标身上挂的是「本随从自己施放」的毒蛇钉刺。
        // 若仅用 HasAura, 队友猎人的钉刺会被误认为已挂, 导致本随从终生不再补钉刺,
        // 毒性钉刺 3% 独立增伤链路彻底失效。
        // 必须使用 GetAuraOfRankedSpell: 1~79 级经 GetAppropriateRank 降阶后施放的
        // 是低阶毒蛇钉刺 ID, 与满阶 49001 不匹配, 用精确 ID 查验会恒返回 nullptr,
        // 造成随从每一帧都判定「未挂钉刺」并无限连发, 形成法力抽空死锁。
        Aura* sting = victim->GetAuraOfRankedSpell(SurvivalHunterSpells::SERPENT_STING, me->GetGUID());
        if (sting && sting->GetDuration() > SERPENT_STING_REFRESH_WINDOW)
            return false;

        if (!CanCast(victim, serpentSting, true))
            return false;

        return ExecuteSpell(victim, serpentSting, true);
    }

    bool TryAimedShot(Unit* victim)
    {
        if (aimedShotCooldown > 0 || !victim)
            return false;

        uint32 const aimedShot = GetAppropriateRank(SurvivalHunterSpells::AIMED_SHOT, false);
        if (!aimedShot || !CanCast(victim, aimedShot, true))
            return false;

        if (ExecuteSpell(victim, aimedShot, true))
        {
            aimedShotCooldown = CD_AIMED_SHOT;
            return true;
        }

        return false;
    }

    bool TrySteadyShot(Unit* victim)
    {
        if (!victim)
            return false;

        // 撤离途中严禁站桩读条: 走位与读条会互相打断形成原地抽搐
        if (isRetreating)
            return false;

        // 距离低于安全站桩下限 (15 码) 时改以瞬发链路过渡, 等待站位回正
        if (me->GetDistance(victim) < MIN_ENGAGE_DIST)
            return false;

        uint32 const steadyShot = GetAppropriateRank(SurvivalHunterSpells::STEADY_SHOT, false);
        if (!steadyShot)
            return false;

        // 施法资格必须先通过校验再刹停: CanCast 失败 (GCD/超距/被控) 时提前立定,
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(victim, steadyShot, true))
            return false;

        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, steadyShot, true);
    }

    // =========================================================================
    // P5: 站位控制 (盲区迟滞回正模型, 维持 15 ~ 35 码射击站位)
    // =========================================================================
    void MaintainRangedPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        me->SetFacingToObject(victim);

        float const dist = me->GetDistance(victim);
        MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();

        // ---- A. 脱节过远 (> 35 码): 主动压进至理想射击站位 ----
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

        // ---- C. 已回到有效射程: 立定射击, 清空遗留走位发生器 ----
        // 严禁放任 chase / follow 发生器常驻: 残余走位会持续拉扯随从,
        // 使稳固射击的 2 秒读条与站位反复互相打断, 表现为原地抽搐。
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
    // 生存被动光环与雕文补偿 (弥补 NPC 缺天赋树缺陷, 铁律 33)
    // -------------------------------------------------------------------------
    // 必须注入 Rank 3/2 满阶 Spell ID。若注入 DBC 默认 Rank 1 根源,
    // 触发概率与数值会严重缩水 (如 T.N.T. Rank 1 仅 2% 暴击与伤害加成)。
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
        SyncPassive(LEVEL_THRILL_OF_THE_HUNT, SurvivalHunterSpells::THRILL_OF_THE_HUNT);   // 狩猎刺激 Rank 3: 技能暴击返还法力
        SyncPassive(LEVEL_TNT, SurvivalHunterSpells::TNT);                                 // T.N.T. Rank 3: 爆炸射击/黑箭暴击+6%
        SyncPassive(LEVEL_SURVIVAL_EXPERIENCE, SurvivalHunterSpells::SURVIVAL_EXPERIENCE); // 生存本能 Rank 2: 敏捷与暴击加成
        SyncPassive(LEVEL_TRAP_MASTERY, SurvivalHunterSpells::TRAP_MASTERY);               // 陷阱掌握 Rank 3
        SyncPassive(LEVEL_SNIPER_TRAINING, SurvivalHunterSpells::SNIPER_TRAINING);         // 狙击训练 Rank 3: 暴击与伤害 +6%
        SyncPassive(LEVEL_NOXIOUS_STINGS, SurvivalHunterSpells::NOXIOUS_STINGS);           // 毒性钉刺 Rank 3: 目标带钉刺全伤害 +3%
        SyncPassive(LEVEL_HUNTING_PARTY, SurvivalHunterSpells::HUNTING_PARTY);             // 狩猎小队 Rank 3: 暴击回蓝
        SyncPassive(LEVEL_LOCK_AND_LOAD, SurvivalHunterSpells::LOCK_AND_LOAD);             // 荷枪实弹 Rank 3: 黑箭/陷阱触发免费爆炸射击
        SyncPassive(25, SurvivalHunterSpells::RESOURCEFULNESS);                            // 足智多谋 Rank 3: 黑箭与陷阱 CD 缩短 6s

        // ---- 雕文补偿 ----
        SyncPassive(LEVEL_GLYPH, SurvivalHunterSpells::GLYPH_OF_EXPLOSIVE_SHOT);  // 爆炸射击雕文: 暴击率 +4%
        SyncPassive(LEVEL_GLYPH, SurvivalHunterSpells::GLYPH_OF_KILL_SHOT);       // 杀戮射击雕文: CD -6s
        SyncPassive(LEVEL_GLYPH, SurvivalHunterSpells::GLYPH_OF_SERPENT_STING);   // 毒蛇钉刺雕文: 持续时间 +6s
    }
};

void AddSC_bot_survival_hunter()
{
    new AdaptiveBotScript<BotSurvivalHunterAI>("bot_survival_hunter");
}
