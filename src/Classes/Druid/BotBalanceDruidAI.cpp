/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "BalanceDruidSpells.h"
#include "AdaptiveBotAI.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include <cmath>

class BotBalanceDruidAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数 (严格遵循 CONTEXT.md 铁律 17 迟滞区间模型)
    // =========================================================================
    static constexpr float MELEE_BLIND_DIST  = 8.0f;   // 近战盲区阈值：进入即触发贴身避难后撤
    static constexpr float MAX_ENGAGE_DIST   = 30.0f;  // 脱节上限：与平衡系 30 码射程严格对齐，杜绝射程死区
    static constexpr float IDEAL_SHOT_DIST   = 25.0f;  // 理想施法站位：预留 5 码缓冲防目标前压打断读条
    static constexpr float RETREAT_EXIT_DIST = 15.0f;  // 撤退迟滞退出线：必须严格大于 MELEE_BLIND_DIST
    static constexpr float TANK_RETREAT_DIST = 15.0f;  // 锚定坦克背身位时的跟随距离 (必须 >= 迟滞退出线)

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 tank->GetOrientation()，否则站位会随坦克转向持续漂移。
    static constexpr float BEHIND_ANGLE      = static_cast<float>(M_PI);

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪 (HasSpellCooldown 恒 false)，
    // 凡无「持续光环保护」的 CD 技能必须由专精自行计时，
    // 否则会因判定恒真而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_STARFALL       = 90000;   // 星辰坠落基础 CD 90s
    static constexpr uint32 CD_STARFALL_GLYPH = 60000;   // 星辰坠落雕文：CD 缩短 30s
    static constexpr uint32 CD_TYPHOON        = 20000;   // 台风 20s CD
    static constexpr uint32 CD_BARKSKIN       = 60000;   // 树皮术 1 分钟 CD
    static constexpr uint32 CD_INNERVATE      = 180000;  // 激活 3 分钟 CD

    // 月火术断档预判窗口：非月蚀期在 DoT 归零前提前续订；
    // 月蚀星火阶段由【星火雕文】自动续期，严禁在此窗口内抢挂。
    static constexpr int32  MOONFIRE_REFRESH_WINDOW_MS = 3000;

    // 生命/法力阈值
    static constexpr float BARKSKIN_HP_PCT    = 45.0f;  // 树皮术自保血线
    static constexpr float INNERVATE_MANA_PCT = 25.0f;  // 激活回蓝蓝线
    static constexpr float STARFALL_MAX_DIST  = 30.0f;  // 星辰坠落有效作用距离
    static constexpr uint32 BOSS_HEALTH_GATE  = 200000; // 精英首领血量门槛：防止把小怪误判为首领空交底牌

public:
    explicit BotBalanceDruidAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 远程随从按远程单位接管移动逻辑，禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 法系远程 (与猎人物理远程解耦)：伤害由法术强度与奥术/自然乘数通道支撑
    bool IsRangedPhysicalBot() const override { return false; }

    // 按专精契约固定 1.0x：不继承基类为法系远程预留的装等放大通道
    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (愤怒/星火术/月火术/树皮术/激活/野性赐福/荆棘术等)
    //     严禁登记于此，其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case BalanceDruidSpells::MOONKIN_FORM: return 40;
            case BalanceDruidSpells::INSECT_SWARM: return 30;
            case BalanceDruidSpells::TYPHOON:      return 50;
            case BalanceDruidSpells::STARFALL:     return 60;
            default:                               return 0;
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

        if (me->GetMaxPower(POWER_MANA) > 0)
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));

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

        // 全局读条/通道双保险守卫：星火术/愤怒/月火为读条施法，引擎会置位 UNIT_STATE_CASTING；
        // 追加 CURRENT_CHANNELED_SPELL 显式判定，杜绝长读条被跟随/走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            // 形态优先于一切增益：未变枭兽就补爪子会导致形态成型后重复刷血施法
            if (MaintainMoonkinForm())
                return;

            if (MaintainMarkOfTheWild())
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
            return;
        }

        // 远程随从仅锚定敌对目标维持进战姿态供施法链路使用，
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- P0: 濒死自保与贴身脱困 ----
        // 树皮术为 Off-GCD 减伤，施放成功后必须当帧顺下，严禁 return 抢占决策流
        TryBarkskin();

        // 台风占 GCD：成功即交还控制权，等待走位发生器把小怪推离盲区
        if (TryTyphoon(victim))
            return;

        // 激活占用 GCD：施放成功后必须交还决策流，由 gcdTimer 阻断本帧后续施法，
        // 否则本帧会继续下发读条指令，造成「激活尚未落地就被读条顶替」的 GCD 空转。
        if (TryInnervate())
            return;

        // ---- P1: 形态维持 ----
        if (MaintainMoonkinForm())
            return;

        // ---- P2/P3: 日月蚀双相输出循环 ----
        if (TryBalanceRotation(victim))
            return;

        // ---- P4: 远程站位与贴身避难 ----
        MaintainRangedPositioning(victim);
    }

private:
    // =========================================================================
    // 日月蚀相位状态机
    // -------------------------------------------------------------------------
    // 双蚀空窗期必须沿用上一次退出的相位继续压制：若空窗期无脑切回愤怒读条，
    // 星火术的暴击触发源将被永久掐断，日蚀终生无法触发，循环退化为纯愤怒填充。
    // =========================================================================
    enum EclipsePhase
    {
        PHASE_SEEKING_LUNAR,  // 以愤怒压制，等待愤怒暴击触发月蚀
        PHASE_SEEKING_SOLAR   // 以星火术压制，等待星火暴击触发日蚀
    };

    // 自管冷却登记
    uint32 starfallCooldown{ 0 };
    uint32 typhoonCooldown{ 0 };
    uint32 barkskinCooldown{ 0 };
    uint32 innervateCooldown{ 0 };

    // 当前所处相位：驱动双蚀空窗期的填充技选择，Reset 时回落至月蚀搜索相位
    EclipsePhase eclipsePhase{ PHASE_SEEKING_LUNAR };

    // 贴身撤离姿态标记：处于该姿态时必须凭此标记主动重发走位指令，
    // 否则会永久粘在坦克身后而无法恢复 25 码稳定施法窗口。
    bool isRetreatingToTank{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateDruidTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(starfallCooldown);
        Tick(typhoonCooldown);
        Tick(barkskinCooldown);
        Tick(innervateCooldown);
    }

    void ResetDruidTimers()
    {
        starfallCooldown = 0;
        typhoonCooldown = 0;
        barkskinCooldown = 0;
        innervateCooldown = 0;

        isRetreatingToTank = false;
        eclipsePhase = PHASE_SEEKING_LUNAR;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0，GetAppropriateRank 不会因等级而降阶，
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁，40 级德鲁伊也会被判定可搓星辰坠落。
    // 故所有在 GetTalentSpellMinLevel 登记的天赋必须经此函数解析。
    // =========================================================================
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

    // 首领判定：世界首领直接放行；副本首领在 3.3.5 中多以「精英」等级呈现，
    // 故对精英单位追加血量池门槛，避免把小怪误判为首领白白交出 1 分钟底牌。
    bool IsBossTarget(Unit* victim) const
    {
        if (!victim || victim->GetTypeId() != TYPEID_UNIT)
            return false;

        Creature* const creature = victim->ToCreature();
        if (!creature || !creature->GetCreatureTemplate())
            return false;

        uint32 const rank = creature->GetCreatureTemplate()->rank;
        if (rank == CREATURE_ELITE_WORLDBOSS)
            return true;

        return rank >= CREATURE_ELITE_ELITE && creature->GetMaxHealth() >= BOSS_HEALTH_GATE;
    }

    // 星落开启收益判定：单体小怪严禁空交底牌，仅首领战或多目标集群战场允许开启
    bool ShouldUseStarfall(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return false;

        if (IsBossTarget(victim))
            return true;

        // 多目标兜底：统计正被近身缠斗的敌对单位数 (自身 + 主坦的被攻击集合)，
        // 两只以上即为实际集群战场，星辰坠落的 10 秒独立光环可完整覆盖。
        uint32 cluster = 0;

        auto CountAttackers = [this, &cluster](Unit* holder)
        {
            if (!holder || !holder->IsInWorld() || holder->GetMap() != me->GetMap())
                return;

            for (Unit* attacker : holder->getAttackers())
            {
                if (attacker && attacker->IsAlive() && attacker->GetMap() == me->GetMap())
                    ++cluster;
            }
        };

        CountAttackers(me);
        CountAttackers(GetGroupTank());

        return cluster >= 2;
    }

    // 取自身施加的 DoT 剩余时间 (毫秒)；不存在则返回 0
    int32 GetOwnDotRemaining(Unit* victim, uint32 rankedSpellId) const
    {
        if (!victim)
            return 0;

        Aura* const aura = victim->GetAuraOfRankedSpell(rankedSpellId, me->GetGUID());
        return aura ? aura->GetDuration() : 0;
    }

    // =========================================================================
    // 通用施法通道 (立定判定与瞬发解耦)
    // =========================================================================
    // 非瞬发读条：CanCast 通过后立即刹停并当帧起手。
    // 严禁写成「移动中直接 return false」——随从在风筝跑位期间会判定读条永久失败，
    // 决策流被 P4 站位分支反复抢占，DoT 无限期缺席，形成「越跑越不打、越不打越要跑」的死锁。
    // 瞬发技能 (allowMoving = true) 则完整保留机动性，严禁刹停。
    bool TryCastSpell(Unit* victim, uint32 spellId, bool allowMoving)
    {
        if (!spellId || !victim)
            return false;

        // 施法资格必须先通过校验再刹停：CanCast 失败时提前立定，
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(victim, spellId, true))
            return false;

        if (!allowMoving && me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, spellId, true);
    }

    // DoT 维持统一通道：以「分阶查询 + 自身施放」双重判定 DoT 是否存在。
    // 严禁直接用满级 ID 做 HasAura —— 低等级降阶施放时判定必然失败，会陷入每帧空转重刷。
    // allowMoving = true 授予全部瞬发 DoT (月火术、虫群)，使其可在风筝跑位途中直接挂上。
    bool TryMaintainDot(Unit* victim, uint32 rankedSpellId, uint32 castSpellId, int32 refreshWindowMs = 0, bool allowMoving = false)
    {
        if (!victim || !castSpellId)
            return false;

        if (GetOwnDotRemaining(victim, rankedSpellId) > refreshWindowMs)
            return false;

        return TryCastSpell(victim, castSpellId, allowMoving);
    }

    // =========================================================================
    // P0: 濒死自保与贴身脱困
    // =========================================================================
    bool TryBarkskin()
    {
        if (barkskinCooldown > 0)
            return false;

        if (me->GetHealthPct() >= BARKSKIN_HP_PCT && !IsUnderPhysicalMelee(me))
            return false;

        // 树皮术为 Off-GCD 减伤底牌，任何形态可用：
        // 以 checkGcd = false + applyGcd = false 施放，成功后当帧顺下继续输出。
        uint32 const barkskin = GetAppropriateRank(BalanceDruidSpells::BARKSKIN, false);
        if (!barkskin || !CanCast(me, barkskin, false))
            return false;

        if (!ExecuteSpell(me, barkskin, false))
            return false;

        barkskinCooldown = CD_BARKSKIN;
        return true;
    }

    bool TryTyphoon(Unit* victim)
    {
        if (typhoonCooldown > 0)
            return false;

        // 近战压制判定以「自身被贴身」为准，而非仅看当前主目标距离：
        // 多目标场景下贴身的小怪可能并不是当前的读条目标。
        bool const inMeleeDanger = IsUnderPhysicalMelee(me) ||
                                   (victim && me->GetDistance(victim) < MELEE_BLIND_DIST);

        if (!inMeleeDanger)
            return false;

        uint32 const typhoon = GetTalentRank(BalanceDruidSpells::TYPHOON);
        if (!typhoon)
            return false;

        // 台风为正面锥形击退：起手前必须先把自身朝向锁定到威胁目标，
        // 否则背身施放会出现「施法成功但锥形落空」的隐形浪费。
        if (victim)
            me->SetFacingToObject(victim);

        // 台风为以自身为原点的正面锥形击退光环，施法目标必须是自己，
        // 传 victim 会导致目标判定落空而静默失败。
        if (!CanCast(me, typhoon, true))
            return false;

        if (!ExecuteSpell(me, typhoon, true))
            return false;

        typhoonCooldown = CD_TYPHOON;
        return true;
    }

    bool TryInnervate()
    {
        if (innervateCooldown > 0)
            return false;

        if (me->getPowerType() != POWER_MANA)
            return false;

        if (me->GetPowerPct(POWER_MANA) >= INNERVATE_MANA_PCT)
            return false;

        uint32 const innervate = GetAppropriateRank(BalanceDruidSpells::INNERVATE, false);
        if (!innervate || !CanCast(me, innervate, true))
            return false;

        if (!ExecuteSpell(me, innervate, true))
            return false;

        innervateCooldown = CD_INNERVATE;
        return true;
    }

    // =========================================================================
    // P1: 形态与常驻增益维持
    // =========================================================================
    bool MaintainMoonkinForm()
    {
        // 枭兽形态为纯天赋：低等级段 (未习得) 直接放弃该通道，回到人形态读条输出
        uint32 const moonkin = GetTalentRank(BalanceDruidSpells::MOONKIN_FORM);
        if (!moonkin)
            return false;

        // 形态光环以分阶查询判定，避免更高 Rank 形态被误判为缺失而反复顶替
        if (me->GetAuraOfRankedSpell(BalanceDruidSpells::MOONKIN_FORM))
            return false;

        if (!CanCast(me, moonkin, true))
            return false;

        return ExecuteSpell(me, moonkin, true);
    }

    bool MaintainMarkOfTheWild()
    {
        // 野性赐福为分阶增益：以分阶查询判定，避免低 Rank 光环被误判为缺失而反复顶替
        if (me->GetAuraOfRankedSpell(BalanceDruidSpells::MARK_OF_THE_WILD))
            return false;

        uint32 const markOfTheWild = GetAppropriateRank(BalanceDruidSpells::MARK_OF_THE_WILD, false);
        if (!markOfTheWild || !CanCast(me, markOfTheWild, true))
            return false;

        return ExecuteSpell(me, markOfTheWild, true);
    }

    // =========================================================================
    // P2/P3: 日月蚀双相输出循环
    // =========================================================================
    bool TryBalanceRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P4 接管，绝不在此硬读条
        if (dist < MELEE_BLIND_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        // ---- 双蚀光环状态侦测 ----
        // 月蚀 (48518)：愤怒暴击触发，+40% 星火术暴击率；
        // 日蚀 (48517)：星火术暴击触发，+40% 愤怒伤害。
        bool const hasLunar = me->HasAura(BalanceDruidSpells::AURA_ECLIPSE_LUNAR);
        bool const hasSolar = me->HasAura(BalanceDruidSpells::AURA_ECLIPSE_SOLAR);

        // ---- 爆发底牌：星辰坠落 ----
        // 星辰坠落占用 GCD：施放成功后必须当帧交还决策流，
        // 否则本帧会立刻下发读条指令，把刚起步的星落光环节奏直接顶掉。
        if (TryUseStarfall(victim))
            return true;

        // ---- DoT 维持 ----
        if (TryMaintainDots(victim, hasLunar, hasSolar))
            return true;

        // ---- 打击流转双相仲裁 ----
        return TryEclipseArbitration(victim, hasLunar, hasSolar);
    }

    bool TryUseStarfall(Unit* victim)
    {
        if (starfallCooldown > 0)
            return false;

        if (me->GetDistance(victim) > STARFALL_MAX_DIST)
            return false;

        if (!ShouldUseStarfall(victim))
            return false;

        uint32 const starfall = GetTalentRank(BalanceDruidSpells::STARFALL);
        if (!starfall)
            return false;

        // 星辰坠落为以自身为中心的下坠光环，施法目标必须是自己
        if (!CanCast(me, starfall, true))
            return false;

        if (!ExecuteSpell(me, starfall, true))
            return false;

        // 冷却时长动态判定：已注入【星辰坠落雕文】时 CD 缩短 30s
        starfallCooldown = me->HasAura(BalanceDruidSpells::GLYPH_OF_STARFALL) ? CD_STARFALL_GLYPH : CD_STARFALL;
        return true;
    }

    bool TryMaintainDots(Unit* victim, bool hasLunar, bool hasSolar)
    {
        uint32 const insectSwarm = GetTalentRank(BalanceDruidSpells::INSECT_SWARM);
        uint32 const moonfire    = GetAppropriateRank(BalanceDruidSpells::MOONFIRE, false);

        // ---- 虫群：日蚀期 (以及未进入月蚀的平稳期) 瞬发补齐 ----
        // 月蚀星火阶段严禁抢挂虫群：该阶段应全力读条星火吃满 40% 暴击乘数，
        // 且虫群为瞬发法术，月蚀结束后的平稳期仍有充足窗口无损补齐。
        if (insectSwarm && (hasSolar || !hasLunar))
        {
            if (TryMaintainDot(victim, BalanceDruidSpells::INSECT_SWARM, insectSwarm, 0, true))
                return true;
        }

        // ---- 月火术：完全缺失时立即补挂 ----
        // 致命缺陷修复：星火雕文只能「延长已存在月火」的持续时间，无法凭空挂上 DoT。
        // 若目标是转火新目标 (斩杀/换目标起手)，仅有雕文续期将导致月火永久缺席。
        int32 const moonfireRemaining = GetOwnDotRemaining(victim, BalanceDruidSpells::MOONFIRE);
        if (moonfireRemaining == 0)
        {
            if (TryMaintainDot(victim, BalanceDruidSpells::MOONFIRE, moonfire, 0, true))
                return true;
        }
        else if (!hasLunar && moonfireRemaining <= MOONFIRE_REFRESH_WINDOW_MS)
        {
            // 月蚀星火阶段由星火雕文自动续订：只要剩余 > 3s 就严禁重复施放，
            // 否则会白白吃掉一个 GCD 并打断星火读条节奏。
            if (TryMaintainDot(victim, BalanceDruidSpells::MOONFIRE, moonfire, MOONFIRE_REFRESH_WINDOW_MS, true))
                return true;
        }

        return false;
    }

    bool TryEclipseArbitration(Unit* victim, bool hasLunar, bool hasSolar)
    {
        // a) 月蚀生效：绝对优先读条星火术，吃满 +40% 星火暴击率。
        //    同时把相位切到「搜索日蚀」：月蚀结束后必须继续用星火术压制，
        //    才能让星火暴击持续喂养日蚀触发源。
        if (hasLunar)
        {
            eclipsePhase = PHASE_SEEKING_SOLAR;

            uint32 const starfire = GetAppropriateRank(BalanceDruidSpells::STARFIRE, false);
            uint32 const filler = starfire ? starfire : GetAppropriateRank(BalanceDruidSpells::WRATH, false);
            return TryCastSpell(victim, filler, false);
        }

        // b) 日蚀生效：全力读条愤怒吃满 +40% 增伤，并把相位切回「搜索月蚀」。
        if (hasSolar)
        {
            eclipsePhase = PHASE_SEEKING_LUNAR;

            uint32 const wrath = GetAppropriateRank(BalanceDruidSpells::WRATH, false);
            return TryCastSpell(victim, wrath, false);
        }

        // c) 双蚀空窗期：必须严格沿用上一次退出的相位继续压制，严禁无脑切回愤怒。
        //    若空窗期永远打愤怒，星火术的暴击触发源被彻底掐断，
        //    【日月蚀】被动将终生只触发月蚀一侧，日蚀形同虚设。
        if (eclipsePhase == PHASE_SEEKING_SOLAR)
        {
            uint32 const starfire = GetAppropriateRank(BalanceDruidSpells::STARFIRE, false);
            if (starfire)
                return TryCastSpell(victim, starfire, false);
        }

        uint32 const wrath = GetAppropriateRank(BalanceDruidSpells::WRATH, false);
        return TryCastSpell(victim, wrath, false);
    }

    // =========================================================================
    // P4: 远程站位与贴身避难 (风筝与拉开状态机，维持 25 码施法站位)
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

        // ---- A. 脱节过远 (> 30 码)：主动压进至理想施法站位 ----
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
        // 表现为随从在原地反复起步/刹停的抽搐，且永远无法进入稳定施法窗口。
        bool const needsRetreat = (dist < MELEE_BLIND_DIST) || (isRetreatingToTank && dist < RETREAT_EXIT_DIST);

        // ---- B. 贴身盲区 / 尚未拉足安全距离：向坦克背身位撤离，借坦克 AoE 仇恨把小怪拉走 ----
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

        // ---- C. 已回到有效射程：立定施法，清空遗留走位发生器 ----
        // 严禁放任 chase / follow / point 发生器常驻：残余走位会持续拉扯随从，
        // 使读条与站位反复互相打断，表现为原地抽搐。
        if (moveType == CHASE_MOTION_TYPE || moveType == FOLLOW_MOTION_TYPE || isRetreatingToTank)
        {
            isRetreatingToTank = false;
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveIdle();
            me->StopMoving();
        }
    }

    // =========================================================================
    // 平衡天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // -------------------------------------------------------------------------
    // 【日月蚀】是整套输出循环的状态机底座，且属于典型的「暴击触发性被动天赋」，
    // 必须注入天赋根源 ID 而非仅检测触发光环，否则随从终生无法触发月蚀/日蚀，
    // 双相仲裁会退化为纯愤怒填充。
    // 三张雕文本体一并注入，否则星火雕文的月火续期与星落的 60s 冷却判定全部失效。
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

        SyncPassive(20, BalanceDruidSpells::ECLIPSE);               // 日月蚀：暴击触发月蚀/日蚀的核心被动根源
        SyncPassive(20, BalanceDruidSpells::NATURES_GRACE);         // 自然之赐：法术暴击后降低读条 0.5s
        SyncPassive(20, BalanceDruidSpells::EARTH_AND_MOON);        // 大地与月亮：法术命中附加 13% 法术易伤
        SyncPassive(20, BalanceDruidSpells::MOONKIN_AURA);          // 枭兽光环：全团法系暴击增益
        SyncPassive(20, BalanceDruidSpells::GLYPH_OF_STARFIRE);     // 星火雕文：星火术延长月火 3s (最多 9s)
        SyncPassive(20, BalanceDruidSpells::GLYPH_OF_STARFALL);     // 星辰坠落雕文：冷却缩短 30s
        SyncPassive(20, BalanceDruidSpells::GLYPH_OF_INSECT_SWARM); // 虫群雕文：虫群伤害 +30%
        SyncPassive(20, BalanceDruidSpells::IMPROVED_INSECT_SWARM); // 强化虫群：目标带虫群时愤怒增伤 / 带月火时星火暴击提升
    }
};

void AddSC_bot_balance_druid()
{
    new AdaptiveBotScript<BotBalanceDruidAI>("bot_balance_druid");
}
