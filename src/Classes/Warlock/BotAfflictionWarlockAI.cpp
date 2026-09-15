/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "AfflictionWarlockSpells.h"
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

class BotAfflictionWarlockAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数 (严格遵循 CONTEXT.md 铁律 17 迟滞区间模型)
    // =========================================================================
    static constexpr float MELEE_BLIND_DIST  = 8.0f;   // 近战盲区阈值：进入即触发贴身避难后撤
    static constexpr float MAX_ENGAGE_DIST   = 30.0f;  // 脱节上限：与痛苦系 30 码射程严格对齐，杜绝射程死区
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
    static constexpr uint32 CD_HAUNT       = 8000;    // 鬼影缠身 8s 本体 CD
    static constexpr uint32 CD_DEATH_COIL  = 120000;  // 死亡缠绕 2 分钟 CD
    static constexpr uint32 CD_SOULSHATTER = 300000;  // 灵魂碎裂 5 分钟 CD
    static constexpr uint32 LIFE_TAP_RETRY = 3000;    // 生命分流节流：防止雕文门禁在短窗口内逐帧空转重入

    // DoT 断档预判窗口：引导通道 (吸取灵魂) 耗时极长，DoT 必须在通道开始前预留安全余量
    static constexpr int32  DOT_REFRESH_WINDOW_MS = 3000;

    // 鬼影缠身刷新窗口：1.5s 读条 + 约 1s 弹道飞行，必须在 Debuff 归零前 2.5s 就起手，
    // 否则整条 DoT 链会出现 20% 暗影增伤空窗，并连带把斩杀期的引导开启门禁永久锁死。
    static constexpr int32  HAUNT_REFRESH_WINDOW_MS = 2500;

    // 吸取灵魂引导开启门禁：引导单次持续十余秒，鬼影与痛苦无常的剩余余量
    // 必须同时 >= 3.5s，否则中途断档会让剩余全部跳数丢失增伤乘数。
    static constexpr int32  DRAIN_GATE_REMAINING_MS = 3500;

    // 生命/法力阈值
    static constexpr float DEATH_COIL_HP_PCT    = 35.0f;  // 死亡缠绕自保血线
    static constexpr float LIFE_TAP_MANA_PCT    = 40.0f;  // 法力枯竭补蓝线
    static constexpr float LIFE_TAP_SAFE_HP_PCT = 50.0f;  // 补蓝分流安全血线：严禁在濒死血线自残
    static constexpr float GLYPH_TAP_HP_PCT     = 60.0f;  // 雕文分流维持血线
    static constexpr float EXECUTE_HP_PCT       = 25.0f;  // 斩杀期阈值：死亡之拥 + 吸取灵魂 4 倍伤害窗口
    static constexpr float DRAIN_SOUL_MAX_DIST  = 30.0f;  // 吸取灵魂引导射程

public:
    explicit BotAfflictionWarlockAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 远程随从按远程单位接管移动逻辑，禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 法系远程 (与猎人物理远程解耦)：伤害由法术强度与暗影乘数通道支撑
    bool IsRangedPhysicalBot() const override { return false; }

    // 按专精契约固定 1.0x：不继承基类为法系远程预留的 2.0x ~ 3.3x 装备装等放大通道
    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (腐蚀术/痛苦诅咒/暗影箭/吸取灵魂/生命分流/死亡缠绕/护甲等) 严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case AfflictionWarlockSpells::HAUNT:               return 60;
            case AfflictionWarlockSpells::UNSTABLE_AFFLICTION: return 50;
            default:                                           return 0;
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

        ResetWarlockTimers();
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
        UpdateWarlockTimers(diff);

        // =====================================================================
        // 引导死锁破除 (必须置于全局通道守卫之前)
        // ---------------------------------------------------------------------
        // 吸取灵魂单次引导长达十余秒，期间全局通道守卫会锁定整个决策流。
        // 若引导途中目标身上的鬼影已完全断档，剩余全部跳数都将在缺失 20%
        // 暗影增伤乘数 (以及死亡之拥乘数底座) 的情况下空抽。
        // 必须主动掐断通道，让当帧决策流立即重补鬼影。
        // =====================================================================
        if (Spell* const channeled = me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            SpellInfo const* const channeledInfo = channeled->GetSpellInfo();
            SpellInfo const* const drainSoulInfo = sSpellMgr->GetSpellInfo(AfflictionWarlockSpells::DRAIN_SOUL);

            // 以「同根 Rank」判定而非裸 ID 比较：低等级降阶施放的吸取灵魂同样是合法通道
            if (channeledInfo && drainSoulInfo && channeledInfo->IsRankOf(drainSoulInfo) &&
                hauntCooldown == 0)
            {
                Unit* const channelTarget = me->GetVictim();
                if (channelTarget && GetOwnDotRemaining(channelTarget, AfflictionWarlockSpells::HAUNT) == 0)
                    me->InterruptSpell(CURRENT_CHANNELED_SPELL);
            }
        }

        // 全局读条/通道双保险守卫：痛苦无常/鬼影缠身为读条施法，引擎会置位 UNIT_STATE_CASTING；
        // 但吸取灵魂属引导类法术，在部分状态下并不置位该标记，故追加
        // CURRENT_CHANNELED_SPELL 显式判定，杜绝长读条与引导被跟随/走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

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

        // 远程随从仅锚定敌对目标维持进战姿态供施法链路使用，
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- P0: 仇恨脱困与濒死自保 ----
        if (TrySurvival(victim))
            return;

        // ---- P1: 护甲维持与控蓝分流 ----
        if (MaintainArmor())
            return;

        if (TryManaManagement())
            return;

        // ---- P2/P3: 核心 DoT 循环与斩杀通道 ----
        if (TryAfflictionRotation(victim))
            return;

        // ---- P4: 远程站位与贴身避难 ----
        MaintainRangedPositioning(victim);
    }

private:
    // 自管冷却登记
    uint32 hauntCooldown{ 0 };
    uint32 deathCoilCooldown{ 0 };
    uint32 soulshatterCooldown{ 0 };
    uint32 lifeTapRetryTimer{ 0 };

    // 贴身撤离姿态标记：处于该姿态时必须凭此标记主动重发走位指令，
    // 否则会永久粘在坦克身后而无法恢复 25 码稳定施法窗口。
    bool isRetreatingToTank{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateWarlockTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(hauntCooldown);
        Tick(deathCoilCooldown);
        Tick(soulshatterCooldown);
        Tick(lifeTapRetryTimer);
    }

    void ResetWarlockTimers()
    {
        hauntCooldown = 0;
        deathCoilCooldown = 0;
        soulshatterCooldown = 0;
        lifeTapRetryTimer = 0;

        isRetreatingToTank = false;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0，GetAppropriateRank 不会因等级而降阶，
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁，40 级术士会直接搓出鬼影缠身。
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

    // 取自身施加的 DoT 剩余时间 (毫秒)；不存在则返回 0
    int32 GetOwnDotRemaining(Unit* victim, uint32 rankedSpellId) const
    {
        if (!victim)
            return 0;

        Aura* const aura = victim->GetAuraOfRankedSpell(rankedSpellId, me->GetGUID());
        return aura ? aura->GetDuration() : 0;
    }

    // 腐蚀术是否已由【强化腐蚀术】转为瞬发：仅在天赋已注入时才允许跑动中直接上 DoT
    bool IsCorruptionInstant() const
    {
        return me->HasAura(AfflictionWarlockSpells::IMPROVED_CORRUPTION);
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
    // allowMoving = true 仅授予瞬发 DoT (痛苦诅咒，以及已点出【强化腐蚀术】的腐蚀术)，
    // 使其可在风筝跑位途中直接挂上；痛苦无常等读条 DoT 必须传 false 走刹停通道。
    bool TryMaintainDot(Unit* victim, uint32 rankedSpellId, uint32 castSpellId, int32 refreshWindowMs = 0, bool allowMoving = false)
    {
        if (!victim || !castSpellId)
            return false;

        if (GetOwnDotRemaining(victim, rankedSpellId) > refreshWindowMs)
            return false;

        return TryCastSpell(victim, castSpellId, allowMoving);
    }

    // =========================================================================
    // P0: 仇恨脱困与濒死自保
    // =========================================================================
    bool TrySurvival(Unit* victim)
    {
        // ---- 灵魂碎裂：仇恨彻底失控时瞬发削减 50% 仇恨，把目标还给主坦 ----
        if (soulshatterCooldown == 0 && IsTopThreatTarget())
        {
            uint32 const soulshatter = GetAppropriateRank(AfflictionWarlockSpells::SOULSHATTER, false);
            if (soulshatter && CanCast(me, soulshatter, true) && ExecuteSpell(me, soulshatter, true))
            {
                soulshatterCooldown = CD_SOULSHATTER;
                return true;
            }
        }

        // ---- 死亡缠绕：濒死或被物理近战压制时瞬发恐惧拉开距离并回血 ----
        if (deathCoilCooldown == 0 && victim &&
            (me->GetHealthPct() < DEATH_COIL_HP_PCT || IsUnderPhysicalMelee(me)))
        {
            uint32 const deathCoil = GetAppropriateRank(AfflictionWarlockSpells::DEATH_COIL, false);
            if (deathCoil && CanCast(victim, deathCoil, true) && ExecuteSpell(victim, deathCoil, true))
            {
                deathCoilCooldown = CD_DEATH_COIL;
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P1: 护甲维持与控蓝分流
    // =========================================================================
    bool MaintainArmor()
    {
        // 护甲为分阶法术：以分阶查询判定，避免低 Rank 光环被误判为缺失而反复顶替
        uint32 const felArmor = GetAppropriateRank(AfflictionWarlockSpells::FEL_ARMOR, false);

        if (felArmor)
        {
            // 邪甲术优先：核心输出护甲 (精神转法伤)，覆盖时会由底层自动顶掉恶魔护甲
            if (me->GetAuraOfRankedSpell(AfflictionWarlockSpells::FEL_ARMOR))
                return false;
        }
        else if (me->GetAuraOfRankedSpell(AfflictionWarlockSpells::DEMON_ARMOR))
        {
            // 未习得邪甲术 (等级不足) 且恶魔护甲已在身：维持现状
            return false;
        }

        uint32 const armor = felArmor ? felArmor : GetAppropriateRank(AfflictionWarlockSpells::DEMON_ARMOR, false);
        if (!armor || !CanCast(me, armor, true))
            return false;

        return ExecuteSpell(me, armor, true);
    }

    bool TryManaManagement()
    {
        if (me->getPowerType() != POWER_MANA)
            return false;

        // 分流节流：雕文 SP 门禁在短窗口内逐帧判定会形成空转重入
        if (lifeTapRetryTimer > 0)
            return false;

        float const manaPct = me->GetPowerPct(POWER_MANA);
        float const hpPct   = me->GetHealthPct();

        // a) 法力枯竭补蓝通道：低于 40% 蓝线且血线安全 (> 50%)；
        // b) 雕文 SP 维持通道：仅在【生命分流雕文】本体已注入时才启用该通道 ——
        //    若雕文缺失，63321 光环永远不会出现，本分支将退化为每 3 秒自残一次的无限空转。
        bool const needManaRefill  = (manaPct < LIFE_TAP_MANA_PCT) && (hpPct > LIFE_TAP_SAFE_HP_PCT);
        bool const hasLifeTapGlyph = me->HasAura(AfflictionWarlockSpells::GLYPH_OF_LIFE_TAP);
        bool const needGlyphBuff   = hasLifeTapGlyph &&
                                     !me->HasAura(AfflictionWarlockSpells::AURA_GLYPH_OF_LIFE_TAP) &&
                                     (hpPct > GLYPH_TAP_HP_PCT);

        if (!needManaRefill && !needGlyphBuff)
            return false;

        uint32 const lifeTap = GetAppropriateRank(AfflictionWarlockSpells::LIFE_TAP, false);
        if (!lifeTap || gcdTimer > 0)
            return false;

        // 生命分流的消耗通道为「生命值」(POWER_HEALTH)：CanCast / ExecuteSpell 的能量校验
        // 会对该非法能量索引执行 GetPower 读取，存在读到邻域字段垃圾值并误判
        // 「能量不足」从而把分流永久阻断的风险。故此处彻底绕开通用施法通道，
        // 直接以引擎底层 CastSpell 直放，并以自管蓝线/血线/GCD 三重门禁替代资源校验。
        if (me->CastSpell(me, lifeTap, false) != SPELL_CAST_OK)
            return false;

        gcdTimer = 1500;
        lifeTapRetryTimer = LIFE_TAP_RETRY;
        return true;
    }

    // =========================================================================
    // P2/P3: 痛苦系核心 DoT 循环与斩杀通道
    // =========================================================================
    bool TryAfflictionRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P4 接管，绝不在此硬读条
        if (dist < MELEE_BLIND_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        // ---- 绝对最高优先级：鬼影缠身 ----
        // 目标缺失自身施加的鬼影 Debuff 时必须最优先补齐：该 Debuff 提供 20% 全域暗影增伤，
        // 是整条 DoT 链的乘数底座，一旦空窗，腐蚀/无常/吸取灵魂全线掉 20%。
        if (TryHaunt(victim, GetTalentRank(AfflictionWarlockSpells::HAUNT)))
            return true;

        uint32 const unstableAffliction = GetTalentRank(AfflictionWarlockSpells::UNSTABLE_AFFLICTION);

        if (victim->GetHealthPct() < EXECUTE_HP_PCT)
            return TryExecutePhase(victim, unstableAffliction);

        return TrySteadyPhase(victim, unstableAffliction);
    }

    bool TryHaunt(Unit* victim, uint32 spellId)
    {
        if (!spellId || !victim)
            return false;

        if (hauntCooldown > 0)
            return false;

        // Debuff 判定：只认自身施加的鬼影，多术士场景下严禁把他人的鬼影误判为自己的。
        // 剩余时间 <= 2.5s 即视为断档并立即读条刷新 (覆盖 1.5s 读条 + 约 1s 弹道飞行)：
        // 若等 Debuff 真正归零才起手，读条与飞行全程都没有 20% 增伤覆盖，
        // 还会连带把斩杀期的吸取灵魂开启门禁永久锁死，退化为无增伤盲抽。
        if (GetOwnDotRemaining(victim, AfflictionWarlockSpells::HAUNT) > HAUNT_REFRESH_WINDOW_MS)
            return false;

        if (!TryCastSpell(victim, spellId, false))
            return false;

        hauntCooldown = CD_HAUNT;
        return true;
    }

    // =========================================================================
    // 平稳期循环 (目标生命 >= 25%)
    // =========================================================================
    bool TrySteadyPhase(Unit* victim, uint32 unstableAffliction)
    {
        // ---- 痛苦无常：目标缺失时补齐 ----
        if (TryMaintainDot(victim, AfflictionWarlockSpells::UNSTABLE_AFFLICTION, unstableAffliction))
            return true;

        // ---- 腐蚀术：目标缺失时补齐 (后续由鬼影/吸取灵魂的【永恒痛苦】自动刷新) ----
        // 强化腐蚀术生效后腐蚀术为瞬发，允许风筝跑动途中直接补 DoT，严禁刹停丢机动性
        uint32 const corruption = GetAppropriateRank(AfflictionWarlockSpells::CORRUPTION, false);
        if (TryMaintainDot(victim, AfflictionWarlockSpells::CORRUPTION, corruption, 0, IsCorruptionInstant()))
            return true;

        // ---- 痛苦诅咒：缺失自身诅咒时瞬发维持 (瞬发法术，允许跑动中直接打出) ----
        uint32 const curseOfAgony = GetAppropriateRank(AfflictionWarlockSpells::CURSE_OF_AGONY, false);
        if (TryMaintainDot(victim, AfflictionWarlockSpells::CURSE_OF_AGONY, curseOfAgony, 0, true))
            return true;

        // ---- 暗影箭填充：全 DoT 齐备后的站桩填充，持续叠满并维持 3 层暗影之拥 ----
        uint32 const shadowBolt = GetAppropriateRank(AfflictionWarlockSpells::SHADOW_BOLT, false);
        return TryCastSpell(victim, shadowBolt, false);
    }

    // =========================================================================
    // 斩杀期通道 (目标生命 < 25%)
    // -------------------------------------------------------------------------
    // 【死亡之拥】被动对 < 25% 目标提供吸取灵魂 400% 增伤，此为痛苦术士斩杀期核心输出通道。
    // 但引导吸取灵魂的前提是鬼影 + 痛苦无常双 Debuff 齐备：二者共同构成暗影增伤乘数底座，
    // 缺一即等于把整段 4 倍伤害窗口白白喂给残缺的乘数。
    // =========================================================================
    bool TryExecutePhase(Unit* victim, uint32 unstableAffliction)
    {
        // ---- 1. 痛苦无常：缺失或即将断档一律优先补齐 ----
        if (TryMaintainDot(victim, AfflictionWarlockSpells::UNSTABLE_AFFLICTION, unstableAffliction, DOT_REFRESH_WINDOW_MS))
            return true;

        // ---- 2. 腐蚀术：必须先判定 DoT 是否「已存在」，再决定是否依赖被动续期 ----
        // 致命缺陷修复：【永恒痛苦】只能刷新「已存在」的腐蚀术，无法凭空挂上 DoT。
        // 若目标是在 25% 以下血线才被转火 (斩杀起步阶段腐蚀术从未上过)，
        // 仅依赖永恒痛苦将导致腐蚀术在整个斩杀期永久缺席，白白损失一条核心 DoT。
        uint32 const corruption = GetAppropriateRank(AfflictionWarlockSpells::CORRUPTION, false);
        if (GetOwnDotRemaining(victim, AfflictionWarlockSpells::CORRUPTION) == 0)
        {
            // 基础 DoT 完全缺失：立即补挂，不受永恒痛苦门禁约束
            if (TryMaintainDot(victim, AfflictionWarlockSpells::CORRUPTION, corruption, 0, IsCorruptionInstant()))
                return true;
        }
        else if (!me->HasAura(AfflictionWarlockSpells::EVERLASTING_AFFLICTION))
        {
            // 腐蚀术已在身但被动尚未生效：手工在断档前续期
            if (TryMaintainDot(victim, AfflictionWarlockSpells::CORRUPTION, corruption, DOT_REFRESH_WINDOW_MS, IsCorruptionInstant()))
                return true;
        }

        // ---- 3. 双 Debuff 余量充足且射程内：引导吸取灵魂 (死亡之拥 4 倍伤害通道) ----
        // 引导单次长达十余秒，必须强制要求鬼影与痛苦无常剩余余量同时 >= 3.5s。
        // 若余量不足即开抽，中途任一 Debuff 断档都会让剩余全部跳数丢失增伤乘数，
        // 同时触发引导死锁破除机制掐断通道，白白浪费一发引导起手。
        int32 const hauntRemaining = GetOwnDotRemaining(victim, AfflictionWarlockSpells::HAUNT);
        int32 const uaRemaining    = GetOwnDotRemaining(victim, AfflictionWarlockSpells::UNSTABLE_AFFLICTION);
        bool const inDrainRange    = (me->GetDistance(victim) <= DRAIN_SOUL_MAX_DIST);

        if (hauntRemaining >= DRAIN_GATE_REMAINING_MS && uaRemaining >= DRAIN_GATE_REMAINING_MS && inDrainRange)
        {
            uint32 const drainSoul = GetAppropriateRank(AfflictionWarlockSpells::DRAIN_SOUL, false);
            if (TryChannelDrainSoul(victim, drainSoul))
                return true;
        }

        // ---- 4. 斩杀前置未齐备的兜底：仍以暗影箭填充，最大化死亡之拥窗口内的有效输出 ----
        uint32 const shadowBolt = GetAppropriateRank(AfflictionWarlockSpells::SHADOW_BOLT, false);
        return TryCastSpell(victim, shadowBolt, false);
    }

    // 引导类法术通道：必须先通过 CanCast 校验再刹停，否则 CanCast 失败时提前立定，
    // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
    bool TryChannelDrainSoul(Unit* victim, uint32 spellId)
    {
        if (!spellId || !victim)
            return false;

        if (!CanCast(victim, spellId, true))
            return false;

        // 引导类法术：必须立定后引导，否则会被跟随/走位指令掐断整段通道
        if (me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, spellId, true);
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
    // 痛苦天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // -------------------------------------------------------------------------
    // 注入等级取各天赋前置点数的近似门槛；其中【永恒痛苦】与【死亡之拥】是斩杀期核心链条的
    // 硬依赖 (腐蚀术自动续期 / 吸取灵魂 4 倍增伤)，必须在低等级段即注入，
    // 否则斩杀通道会静默退化。生命分流雕文本体一并注入，否则 63321 SP 光环永远不会出现。
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

        SyncPassive(20, AfflictionWarlockSpells::DEATHS_EMBRACE);          // 死亡之拥：< 25% 目标吸取灵魂 4 倍增伤
        SyncPassive(20, AfflictionWarlockSpells::SHADOW_EMBRACE);          // 暗影之拥：暗影伤害加成 Debuff 触发源
        SyncPassive(20, AfflictionWarlockSpells::EVERLASTING_AFFLICTION);  // 永恒痛苦：鬼影/吸取灵魂自动刷新腐蚀术
        SyncPassive(20, AfflictionWarlockSpells::ERADICATION);             // 灭绝：腐蚀术跳数几率触发急速
        SyncPassive(20, AfflictionWarlockSpells::PANDEMIC);                // 传染：痛苦无常与腐蚀术可以暴击
        SyncPassive(20, AfflictionWarlockSpells::GLYPH_OF_QUICK_DECAY);    // 急速凋零雕文：急速缩短腐蚀术跳数间隔
        SyncPassive(20, AfflictionWarlockSpells::GLYPH_OF_HAUNT);          // 鬼影缠身雕文：鬼影增伤 +3%
        SyncPassive(20, AfflictionWarlockSpells::GLYPH_OF_LIFE_TAP);       // 生命分流雕文：分流后提供 SP 增益 63321
        // 强化腐蚀术 (10 级即点满)：使腐蚀术变为瞬发，是风筝跑位期 DoT 维持与斩杀期补挂的硬依赖
        SyncPassive(10, AfflictionWarlockSpells::IMPROVED_CORRUPTION);
    }
};

void AddSC_bot_affliction_warlock()
{
    new AdaptiveBotScript<BotAfflictionWarlockAI>("bot_affliction_warlock");
}
