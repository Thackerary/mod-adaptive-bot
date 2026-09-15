/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "RetributionPaladinSpells.h"
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

class BotRetributionPaladinAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位参数 (物理近战背后找背模型)
    // =========================================================================
    static constexpr float MELEE_REACH_DIST   = 4.0f;   // 近战判定区上限：超出必须重新贴背
    static constexpr float MELEE_COMFORT_DIST = 3.0f;   // 贴身阈值：进入后保持平滑贴背输出
    static constexpr float MELEE_FOLLOW_DIST  = 1.5f;   // 理想站位：目标正后方 1.5 码
    static constexpr float CONSECRATION_DIST  = 5.0f;   // 奉献有效半径判定

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移。
    // M_PI 即目标正后方：背身位，可规避正面顺劈、吐息以及被招架加速。
    static constexpr float BEHIND_ANGLE       = static_cast<float>(M_PI);

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的 CD 技能必须由专精自行计时，
    // 否则会因 HasSpellCooldown 恒 false 而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_CRUSADER_STRIKE   = 4000;
    static constexpr uint32 CD_DIVINE_STORM      = 10000;
    static constexpr uint32 CD_JUDGEMENT         = 8000;
    static constexpr uint32 CD_CONSECRATION      = 8000;
    static constexpr uint32 CD_EXORCISM          = 15000;
    static constexpr uint32 CD_HAMMER_OF_WRATH   = 6000;
    static constexpr uint32 CD_AVENGING_WRATH    = 120000;
    static constexpr uint32 CD_DIVINE_SHIELD     = 300000;
    static constexpr uint32 CD_LAY_ON_HANDS      = 1200000;
    static constexpr uint32 CD_HAND_OF_SALVATION = 120000;
    static constexpr uint32 CD_HAMMER_OF_JUSTICE = 60000;

    // 生命/法力阈值
    static constexpr float DIVINE_SHIELD_HP_PCT   = 20.0f;
    static constexpr float LAY_ON_HANDS_HP_PCT    = 15.0f;
    static constexpr float HAMMER_OF_WRATH_HP_PCT = 20.0f;
    static constexpr float AVENGING_WRATH_HP_PCT  = 50.0f;
    static constexpr float CONSECRATION_MANA_PCT  = 35.0f;   // 奉献法力门禁

    // 圣盾术自解门禁：
    // a) 满 2000ms 最小保护期，给坦克稳接仇恨的时间，防止开无敌当帧秒解白交 5 分钟 CD；
    // b) 血线被抬至 60% 以上 或 威胁完全解除，即可出无敌继续输出 —— 根除 -50% 伤害衰减惩罚。
    static constexpr uint32 DIVINE_SHIELD_MIN_HOLD_MS = 2000;
    static constexpr float  DIVINE_SHIELD_SAFE_HP_PCT = 60.0f;

    // 圣洁护盾重试节流：若底层因光环机制差异导致施加不生效，
    // 避免随从每帧空烧 GCD (30 秒光环，5 秒重试一次足以维持覆盖)。
    static constexpr uint32 SACRED_SHIELD_RETRY_MS = 5000;

public:
    explicit BotRetributionPaladinAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 物理近战专精：站位与移动交由本专精的背后找背状态机接管
    bool IsRangedBot() const override { return false; }

    // 与猎人/法系远程彻底解耦：不参与任何远程站位与法伤放大通道
    bool IsRangedPhysicalBot() const override { return false; }

    // 物理近战按真实装备模型结算：严禁继承法系远程的 2.0x ~ 3.3x 法伤放大乘数
    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (愤怒之锤/奉献/驱邪/圣盾术/圣疗术/审判等) 严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case RetributionPaladinSpells::CRUSADER_STRIKE: return 20;
            case RetributionPaladinSpells::DIVINE_STORM:    return 60;
            case RetributionPaladinSpells::AVENGING_WRATH:  return 70;
            case RetributionPaladinSpells::SACRED_SHIELD:   return 80;
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
        ResetPaladinTimers();
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
        UpdatePaladinTimers(diff);

        // 全局读条/通道双保险守卫：引导类法术在部分状态下并不置位 UNIT_STATE_CASTING，
        // 故追加 CURRENT_CHANNELED_SPELL 显式判定，杜绝读条与引导被跟随/走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 0. 圣盾术 (无敌) 自解闸门 (最高优先级，覆盖脱战与战斗双分支)
        // ---------------------------------------------------------------------
        // 无敌期间底层附带「自身造成的所有伤害 -50%」的惩罚光环，
        // 若不主动提前解除，随从会在仇恨已完全转移、血线已安全的情况下
        // 白白顶着半伤输出 12 秒，是一场不折不扣的 DPS 塌方。
        // 无敌持续期间直接 return，严禁下发任何走位/施法指令。
        // =====================================================================
        if (me->HasAura(RetributionPaladinSpells::DIVINE_SHIELD))
        {
            // 已开无敌时长由自管冷却反算：施放成功时 divineShieldCooldown 被置为 CD_DIVINE_SHIELD，
            // 故 (CD_DIVINE_SHIELD - divineShieldCooldown) 即已持续时间。
            uint32 const dsDuration = CD_DIVINE_SHIELD - divineShieldCooldown;

            bool const dsMinHoldPassed  = (dsDuration >= DIVINE_SHIELD_MIN_HOLD_MS);
            bool const dsThreatResolved = (!IsUnderPhysicalMelee(me) && !IsTopThreatTarget());
            bool const dsHealedSafe     = (me->GetHealthPct() > DIVINE_SHIELD_SAFE_HP_PCT);

            if (dsMinHoldPassed && (dsThreatResolved || dsHealedSafe))
                me->RemoveAurasDueToSpell(RetributionPaladinSpells::DIVINE_SHIELD);

            return;
        }

        // =====================================================================
        // 1. 脱战业务维护 (圣印 + 圣洁护盾 + 跟随)
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainSacredShield())
                return;

            if (MaintainSeal())
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
            MaintainSeal();
            return;
        }

        // 物理近战开怪：第二参数必须传 true 开启近战平砍与双手白字输出
        // (与远程专精的 me->Attack(victim, false) 严格区分)。
        if (me->GetVictim() != victim)
            me->Attack(victim, true);

        // ---- P0: 极限自保与仇恨协同 (维度 C 协同) ----
        if (TrySurvivalAndThreat())
            return;

        // ---- P1: 核心常驻增益与圣印维持 ----
        if (MaintainSacredShield())
            return;

        if (MaintainSeal())
            return;

        // ---- P2: 爆发与打断 (Off-GCD，严禁 return true，必须当帧顺下) ----
        TryBurstAndInterrupt(victim);

        // ---- P3: 惩戒 FCFS 近战伤害循环 ----
        if (TryRetributionRotation(victim))
            return;

        // ---- P4: 背后找背站位 ----
        MaintainMeleeBehindPositioning(victim);

        // ---- 白色平砍驱动 ----
        // ScriptedAI::UpdateAI 已被本类完整接管，引擎不会自动驱动平砍，
        // 必须在决策流末帧显式调用，否则双手白字与正义复仇流血永久缺失。
        DoMeleeAttackIfReady();
    }

private:
    // 自管冷却登记
    uint32 crusaderStrikeCooldown{ 0 };
    uint32 divineStormCooldown{ 0 };
    uint32 judgementCooldown{ 0 };
    uint32 consecrationCooldown{ 0 };
    uint32 exorcismCooldown{ 0 };
    uint32 hammerOfWrathCooldown{ 0 };
    uint32 avengingWrathCooldown{ 0 };
    uint32 divineShieldCooldown{ 0 };
    uint32 layOnHandsCooldown{ 0 };
    uint32 handOfSalvationCooldown{ 0 };
    uint32 hammerOfJusticeCooldown{ 0 };

    // 圣洁护盾重试节流计时器
    uint32 sacredShieldRetryTimer{ 0 };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdatePaladinTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(crusaderStrikeCooldown);
        Tick(divineStormCooldown);
        Tick(judgementCooldown);
        Tick(consecrationCooldown);
        Tick(exorcismCooldown);
        Tick(hammerOfWrathCooldown);
        Tick(avengingWrathCooldown);
        Tick(divineShieldCooldown);
        Tick(layOnHandsCooldown);
        Tick(handOfSalvationCooldown);
        Tick(hammerOfJusticeCooldown);
        Tick(sacredShieldRetryTimer);
    }

    void ResetPaladinTimers()
    {
        crusaderStrikeCooldown = 0;
        divineStormCooldown = 0;
        judgementCooldown = 0;
        consecrationCooldown = 0;
        exorcismCooldown = 0;
        hammerOfWrathCooldown = 0;
        avengingWrathCooldown = 0;
        divineShieldCooldown = 0;
        layOnHandsCooldown = 0;
        handOfSalvationCooldown = 0;
        hammerOfJusticeCooldown = 0;

        sacredShieldRetryTimer = 0;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0，GetAppropriateRank 不会因等级而降阶，
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁，10 级随从会持续尝试死搓神圣风暴。
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
    // P0: 极限自保与仇恨协同
    // =========================================================================
    bool TrySurvivalAndThreat()
    {
        // 自律 Debuff：无敌与圣疗共享 2 分钟冷却锁，必须提前读取避免无效施法
        bool const hasForbearance = me->HasAura(RetributionPaladinSpells::FORBEARANCE);

        // ---- 圣盾术：生命 < 20% 且被近战压制，或仇恨失控，且身上无自律 ----
        if (divineShieldCooldown == 0 && !hasForbearance &&
            ((me->GetHealthPct() < DIVINE_SHIELD_HP_PCT && IsUnderPhysicalMelee(me)) || IsTopThreatTarget()))
        {
            uint32 const divineShield = GetAppropriateRank(RetributionPaladinSpells::DIVINE_SHIELD, false);
            if (divineShield && !me->HasAura(divineShield) &&
                CanCast(me, divineShield, true) && ExecuteSpell(me, divineShield, true))
            {
                divineShieldCooldown = CD_DIVINE_SHIELD;
                return true;
            }
        }

        // ---- 圣疗术：生命 < 15% 且无敌进入 CD (或身上无自律) 时的极限满血自救 ----
        if (layOnHandsCooldown == 0 && me->GetHealthPct() < LAY_ON_HANDS_HP_PCT &&
            (divineShieldCooldown > 0 || !hasForbearance))
        {
            uint32 const layOnHands = GetAppropriateRank(RetributionPaladinSpells::LAY_ON_HANDS, false);
            if (layOnHands && CanCast(me, layOnHands, true) && ExecuteSpell(me, layOnHands, true))
            {
                layOnHandsCooldown = CD_LAY_ON_HANDS;
                return true;
            }
        }

        // ---- 拯救之手：仇恨失控且有活坦可承接时，对自身施放渐退仇恨 ----
        if (handOfSalvationCooldown == 0 && IsTopThreatTarget())
        {
            Unit* tank = GetGroupTank();
            bool const hasLivingTank = (tank && tank != me && tank->IsAlive() && tank->IsInWorld() && tank->GetMap() == me->GetMap());

            // 无活坦时严禁交拯救之手：仇恨无可转移对象，减仇毫无意义，纯属白烧 2 分钟 CD
            if (hasLivingTank)
            {
                uint32 const handOfSalvation = GetAppropriateRank(RetributionPaladinSpells::HAND_OF_SALVATION, false);
                if (handOfSalvation && !me->HasAura(handOfSalvation) &&
                    CanCast(me, handOfSalvation, true) && ExecuteSpell(me, handOfSalvation, true))
                {
                    handOfSalvationCooldown = CD_HAND_OF_SALVATION;
                    return true;
                }
            }
        }

        return false;
    }

    // =========================================================================
    // P1: 核心常驻增益与圣印维持
    // =========================================================================
    bool MaintainSeal()
    {
        uint32 const corruption = GetAppropriateRank(RetributionPaladinSpells::SEAL_OF_CORRUPTION, false);

        // 圣印光环与施法法术共用同一 ID，直接以自身所选 Rank 判定，
        // 避免分阶查询在单阶圣印上恒返回 nullptr 导致每帧重复顶替浪费 GCD。
        if (corruption && !me->HasAura(corruption))
        {
            if (!CanCast(me, corruption, true))
                return false;

            return ExecuteSpell(me, corruption, true);
        }

        // 未习得腐蚀圣印的低等级段：降阶挂正义圣印维持基础伤害
        if (!corruption)
        {
            uint32 const righteousness = GetAppropriateRank(RetributionPaladinSpells::SEAL_OF_RIGHTEOUSNESS, false);
            if (righteousness && !me->HasAura(righteousness))
            {
                if (!CanCast(me, righteousness, true))
                    return false;

                return ExecuteSpell(me, righteousness, true);
            }
        }

        return false;
    }

    bool MaintainSacredShield()
    {
        // 圣洁护盾属天赋：必须经天赋门禁解析，低等级不注入也不施放
        uint32 const sacredShield = GetTalentRank(RetributionPaladinSpells::SACRED_SHIELD);
        if (!sacredShield)
            return false;

        if (me->HasAura(sacredShield))
            return false;

        // 重试节流：防止光环机制差异导致施加不生效时每帧空烧 GCD
        if (sacredShieldRetryTimer > 0)
            return false;

        sacredShieldRetryTimer = SACRED_SHIELD_RETRY_MS;

        if (!CanCast(me, sacredShield, true))
            return false;

        return ExecuteSpell(me, sacredShield, true);
    }

    // =========================================================================
    // P2: 爆发与打断 (Off-GCD，当帧顺下绝不 return)
    // =========================================================================
    void TryBurstAndInterrupt(Unit* victim)
    {
        if (!victim)
            return;

        // ---- 复仇之怒：首领/精英且仍处于 50% 以上血量 (高血量窗口才值得交爆发) ----
        if (avengingWrathCooldown == 0 && IsEliteOrBossTarget(victim) &&
            victim->GetHealthPct() > AVENGING_WRATH_HP_PCT)
        {
            uint32 const avengingWrath = GetTalentRank(RetributionPaladinSpells::AVENGING_WRATH);
            if (avengingWrath && !me->HasAura(avengingWrath) &&
                CanCast(me, avengingWrath, true) && ExecuteSpell(me, avengingWrath, true))
                avengingWrathCooldown = CD_AVENGING_WRATH;

            // Off-GCD 铁律：严禁在此 return，必须允许当帧决策流顺下，
            // 让十字军打击/审判立即吃满这 20% 增伤。
        }

        // ---- 制裁之锤：目标读条/引导时近战瞬发打断 ----
        if (hammerOfJusticeCooldown == 0 && IsInterruptibleTarget(victim))
        {
            uint32 const hammerOfJustice = GetAppropriateRank(RetributionPaladinSpells::HAMMER_OF_JUSTICE, false);
            if (hammerOfJustice && CanCast(victim, hammerOfJustice, true) && ExecuteSpell(victim, hammerOfJustice, true))
                hammerOfJusticeCooldown = CD_HAMMER_OF_JUSTICE;
        }
    }

    // =========================================================================
    // P3: 惩戒 FCFS 近战伤害循环 (First Come, First Served)
    // =========================================================================
    bool TryRetributionRotation(Unit* victim)
    {
        if (!victim)
            return false;

        // ---- 1. 斩杀：目标 20% 以下优先愤怒之锤 ----
        if (TryHammerOfWrath(victim))
            return true;

        // ---- 2. 审判：核心回蓝与睿智审判维持 ----
        if (TryJudgement(victim))
            return true;

        // ---- 3. 十字军打击：主力产伤与圣印触发 ----
        if (TryCrusaderStrike(victim))
            return true;

        // ---- 4. 神圣风暴：多目标武器伤害并微量回血 ----
        if (TryDivineStorm(victim))
            return true;

        // ---- 5. 战争艺术驱邪术 ----
        if (TryExorcism(victim))
            return true;

        // ---- 6. 奉献填充 ----
        return TryConsecration(victim);
    }

    bool TryHammerOfWrath(Unit* victim)
    {
        if (hammerOfWrathCooldown > 0 || !victim || victim->GetHealthPct() >= HAMMER_OF_WRATH_HP_PCT)
            return false;

        uint32 const hammerOfWrath = GetAppropriateRank(RetributionPaladinSpells::HAMMER_OF_WRATH, false);
        if (!hammerOfWrath || !CanCast(victim, hammerOfWrath, true))
            return false;

        if (ExecuteSpell(victim, hammerOfWrath, true))
        {
            hammerOfWrathCooldown = CD_HAMMER_OF_WRATH;
            return true;
        }

        return false;
    }

    bool TryJudgement(Unit* victim)
    {
        if (judgementCooldown > 0 || !victim)
            return false;

        // 优先智慧审判：睿智审判的全团回蓝收益远高于光明审判的微量回血。
        // 但 3.3.5a 中审判技受当前激活圣印约束 (智慧审判需挂智慧圣印)，
        // 本专精常驻腐蚀圣印时智慧审判会被底层直接拒绝，
        // 故必须自动回落至与任意圣印兼容的通用光明审判，否则整条审判循环会永久死锁。
        uint32 const judgementOfWisdom = GetAppropriateRank(RetributionPaladinSpells::JUDGEMENT_OF_WISDOM, false);
        if (judgementOfWisdom && CanCast(victim, judgementOfWisdom, true) && ExecuteSpell(victim, judgementOfWisdom, true))
        {
            judgementCooldown = CD_JUDGEMENT;
            return true;
        }

        uint32 const judgementOfLight = GetAppropriateRank(RetributionPaladinSpells::JUDGEMENT_OF_LIGHT, false);
        if (!judgementOfLight || !CanCast(victim, judgementOfLight, true))
            return false;

        if (ExecuteSpell(victim, judgementOfLight, true))
        {
            judgementCooldown = CD_JUDGEMENT;
            return true;
        }

        return false;
    }

    bool TryCrusaderStrike(Unit* victim)
    {
        if (crusaderStrikeCooldown > 0 || !victim)
            return false;

        uint32 const crusaderStrike = GetTalentRank(RetributionPaladinSpells::CRUSADER_STRIKE);
        if (!crusaderStrike || !CanCast(victim, crusaderStrike, true))
            return false;

        if (ExecuteSpell(victim, crusaderStrike, true))
        {
            crusaderStrikeCooldown = CD_CRUSADER_STRIKE;
            return true;
        }

        return false;
    }

    bool TryDivineStorm(Unit* victim)
    {
        if (divineStormCooldown > 0 || !victim)
            return false;

        uint32 const divineStorm = GetTalentRank(RetributionPaladinSpells::DIVINE_STORM);
        if (!divineStorm || !CanCast(victim, divineStorm, true))
            return false;

        if (ExecuteSpell(victim, divineStorm, true))
        {
            divineStormCooldown = CD_DIVINE_STORM;
            return true;
        }

        return false;
    }

    bool TryExorcism(Unit* victim)
    {
        if (exorcismCooldown > 0 || !victim)
            return false;

        bool const hasArtOfWar = me->HasAura(RetributionPaladinSpells::AURA_THE_ART_OF_WAR);

        // 3.3.5a 驱邪术的合法目标仅限亡灵与恶魔；其余目标必须依赖战争艺术光环解锁。
        // 故无光环且目标非亡灵/恶魔时，底层必然拒绝，绝对禁止硬搓白烧一帧决策窗口。
        uint32 const creatureType = victim->GetCreatureType();
        bool const isExorcismableTarget = (creatureType == CREATURE_TYPE_UNDEAD || creatureType == CREATURE_TYPE_DEMON);

        if (!hasArtOfWar && !isExorcismableTarget)
            return false;

        uint32 const exorcism = GetAppropriateRank(RetributionPaladinSpells::EXORCISM, false);
        if (!exorcism || !CanCast(victim, exorcism, true))
            return false;

        if (ExecuteSpell(victim, exorcism, true))
        {
            exorcismCooldown = CD_EXORCISM;
            return true;
        }

        return false;
    }

    bool TryConsecration(Unit* victim)
    {
        if (consecrationCooldown > 0 || !victim)
            return false;

        // 奉献为以自身为中心的近战范围地面 DoT：目标脱离贴身范围时施放毫无收益
        if (me->GetDistance(victim) > CONSECRATION_DIST)
            return false;

        // 法力门禁：奉献耗蓝偏高，低蓝时必须把法力优先留给审判与主力打击
        if (me->GetPowerPct(POWER_MANA) < CONSECRATION_MANA_PCT)
            return false;

        // 奉献以自身为落点 (地面目标)：随从无独立地面目标，以自身为锚点施法
        uint32 const consecration = GetAppropriateRank(RetributionPaladinSpells::CONSECRATION, false);
        if (!consecration || !CanCast(me, consecration, true))
            return false;

        if (ExecuteSpell(me, consecration, true))
        {
            consecrationCooldown = CD_CONSECRATION;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P4: 物理近战背后找背站位模型
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
            // 待拯救之手/无敌把仇恨交回主坦后，再由下方分支恢复严格找背。
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
    // 惩戒天赋被动光环补偿 (弥补 NPC 缺天赋树缺陷)
    // -------------------------------------------------------------------------
    // 注入等级取各天赋在惩戒树中的近似前置门槛，保证任何等级段都有可用被动。
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

        SyncPassive(20, RetributionPaladinSpells::GLYPH_OF_JUDGEMENT);    // 审判雕文：审判伤害 +10%
        SyncPassive(20, RetributionPaladinSpells::GLYPH_OF_CONSECRATION); // 奉献雕文：持续与冷却延长
        SyncPassive(20, RetributionPaladinSpells::GLYPH_OF_EXORCISM);     // 驱邪雕文：驱邪伤害 +20%
        SyncPassive(30, RetributionPaladinSpells::FANATICISM);            // 狂热：审判暴击率 +18%
        SyncPassive(40, RetributionPaladinSpells::SHEATH_OF_LIGHT);       // 圣光之鞘：AP 转化为 SP
        SyncPassive(45, RetributionPaladinSpells::RIGHTEOUS_VENGEANCE);   // 正义复仇：暴击附带 30% 流血 DoT
        SyncPassive(50, RetributionPaladinSpells::JUDGEMENTS_OF_THE_WISE);// 睿智审判：审判回蓝与全团补蓝
    }
};

void AddSC_bot_retribution_paladin()
{
    new AdaptiveBotScript<BotRetributionPaladinAI>("bot_retribution_paladin");
}
