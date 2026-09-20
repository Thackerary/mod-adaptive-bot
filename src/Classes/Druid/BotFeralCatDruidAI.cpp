/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "FeralCatDruidSpells.h"
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
#include <vector>

class BotFeralCatDruidAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位参数 (物理近战背后找背模型)
    // =========================================================================
    static constexpr float MELEE_REACH_DIST   = 4.0f;   // 近战判定区上限：超出必须重新贴背
    static constexpr float MELEE_COMFORT_DIST = 3.0f;   // 贴身阈值：进入后保持平滑贴背输出
    static constexpr float MELEE_FOLLOW_DIST  = 1.5f;   // 理想站位：目标正后方 1.5 码

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已自动叠加目标朝向。
    // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移。
    // M_PI 即目标正后方：背身位，可规避正面顺劈与吐息，并解锁撕碎的背后伤害。
    static constexpr float BEHIND_ANGLE       = static_cast<float>(M_PI);

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的 CD 技能必须由专精自行计时，
    // 否则会因 HasSpellCooldown 恒 false 而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_BERSERK            = 180000;
    static constexpr uint32 CD_TIGERS_FURY        = 30000;
    static constexpr uint32 CD_SURVIVAL_INSTINCTS = 180000;
    static constexpr uint32 CD_BARKSKIN           = 60000;
    static constexpr uint32 CD_FAERIE_FIRE        = 6000;

    // 能量门禁
    static constexpr uint32 FINISHER_ENERGY    = 30;   // 终结技支付门槛 (不足则原地挂起等能量)
    static constexpr uint32 SHRED_ENERGY       = 42;   // 撕碎耗能 (撕碎攻击天赋 -18)
    static constexpr uint32 MANGLE_ENERGY      = 35;   // 裂伤-猎豹耗能
    static constexpr uint32 RAKE_ENERGY        = 35;   // 斜掠耗能
    static constexpr uint32 TF_MAX_ENERGY      = 30;   // 猛虎之怒开启能量上限 (低能量补满才不溢出)
    static constexpr uint32 BERSERK_MIN_ENERGY = 80;   // 狂暴开启能量下限 (爆发窗口防溢能)

    // 续订 / 仲裁窗口
    static constexpr int32 SND_REFRESH_MS     = 2000;  // 野性咆哮提前续订窗口
    static constexpr int32 RIP_REFRESH_MS     = 2000;  // 割裂提前续订窗口
    static constexpr int32 RAKE_REFRESH_MS    = 1500;  // 斜掠提前续订窗口
    static constexpr int32 BITE_MIN_MARGIN_MS = 8000;  // 凶猛撕咬泄能安全余量 (咆哮与割裂均需高于此值)

    // 生命门槛
    static constexpr float BARKSKIN_HP_PCT           = 40.0f;
    static constexpr float SURVIVAL_INSTINCTS_HP_PCT = 30.0f;

    // 终结技连击点门槛上限
    static constexpr uint8 FINISHER_MAX_CP = 5;

public:
    explicit BotFeralCatDruidAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 物理近战：站位与移动交由本专精的背后找背状态机接管
    bool IsRangedBot() const override { return false; }

    bool IsRangedPhysicalBot() const override { return false; }

    // 物理近战按真实装备模型结算：严禁继承法系远程的 2.0x ~ 3.3x 法伤放大乘数
    float GetDamageDealtMultiplier() const override { return 1.0f; }

    // =========================================================================
    // 纯天赋等级契约
    // 注：基础法术 (猎豹形态 / 撕碎 / 割裂 / 斜掠 / 凶猛撕咬 / 树皮术) 严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case FeralCatDruidSpells::SURVIVAL_INSTINCTS:
                return FeralCatDruidSpells::SURVIVAL_INSTINCTS_MIN_LEVEL;
            case FeralCatDruidSpells::FAERIE_FIRE_FERAL:
                return FeralCatDruidSpells::FAERIE_FIRE_MIN_LEVEL;
            case FeralCatDruidSpells::MANGLE_CAT:
                return FeralCatDruidSpells::MANGLE_MIN_LEVEL;
            case FeralCatDruidSpells::BERSERK:
                return FeralCatDruidSpells::BERSERK_MIN_LEVEL;
            default:
                return 0;
        }
    }

    // =========================================================================
    // 形态契约豁免：允许从任意形态 (旅行 / 熊 / 人形) 直接切入猎豹形态
    // =========================================================================
    bool CheckShapeshiftExemption(SpellInfo const* spellInfo) const override
    {
        if (!spellInfo)
            return false;

        if (spellInfo->Id == FeralCatDruidSpells::CAT_FORM)
            return true;

        return AdaptiveBotAI::CheckShapeshiftExemption(spellInfo);
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 能量通道必须在基类 Reset 之前配置，以便等级同步时正确初始化能量池
        me->setPowerType(POWER_ENERGY);
        me->SetMaxPower(POWER_ENERGY, 100);
        me->SetPower(POWER_ENERGY, 100);

        AdaptiveBotAI::Reset();

        ResetFeralTimers();
        MaintainResourcePools();
        ApplyPassiveTalents();
    }

    void OnLevelSynced(uint8 level) override
    {
        AdaptiveBotAI::OnLevelSynced(level);

        me->setPowerType(POWER_ENERGY);
        me->SetMaxPower(POWER_ENERGY, 100);

        if (me->GetPower(POWER_ENERGY) > me->GetMaxPower(POWER_ENERGY))
            me->SetPower(POWER_ENERGY, me->GetMaxPower(POWER_ENERGY));

        MaintainResourcePools();
        ApplyPassiveTalents();
    }

    // =========================================================================
    // 核心决策循环
    // =========================================================================
    void UpdateAI(uint32 diff) override
    {
        UpdateTimers(diff);
        UpdateFeralTimers(diff);

        // 全局读条 / 通道双保险守卫：引导类法术在部分状态下并不置位 UNIT_STATE_CASTING，
        // 故追加 CURRENT_CHANNELED_SPELL 显式判定，杜绝被跟随 / 走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // P0: 形态维护 (脱战与战斗均常驻猎豹形态)
        // 变身占用 GCD，成功施放后本帧让出，避免后续技能被 GCD 阻断
        // =====================================================================
        if (MaintainCatForm())
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            UpdateFollowMaster(diff);

            MaintainResourcePools();
            return;
        }

        // =====================================================================
        // 2. 索敌仲裁
        // =====================================================================
        Unit* victim = SelectAssistTarget();
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() ||
            victim->GetMap() != me->GetMap() || !me->IsValidAttackTarget(victim))
            return;

        // 物理近战开怪：第二参数必须传 true 开启近战平砍与白字输出
        if (me->GetVictim() != victim)
            me->Attack(victim, true);

        // ---------------------------------------------------------------------
        // P0.5: 濒死自保 (Off-GCD 顺下，严禁 return)
        // ---------------------------------------------------------------------
        TrySurvivalCooldowns();

        // ---------------------------------------------------------------------
        // P1: 非 GCD 增能与爆发 (Off-GCD 顺下，严禁 return)
        // ---------------------------------------------------------------------
        TryOffensiveCooldowns(victim);

        // ---------------------------------------------------------------------
        // P2: 破甲与弱化前置
        // ---------------------------------------------------------------------
        if (MaintainFaerieFireFeral(victim))
            return;

        // ---------------------------------------------------------------------
        // P3: 连击点核心打击与终结技仲裁
        // ---------------------------------------------------------------------
        if (TryCoreRotation(victim))
            return;

        // ---------------------------------------------------------------------
        // P4: 背后找背站位
        // ---------------------------------------------------------------------
        MaintainMeleeBehindPositioning(victim);

        // ---- 白色平砍驱动 ----
        // ScriptedAI::UpdateAI 已被本类完整接管，引擎不会自动驱动平砍，
        // 必须在决策流末帧显式调用，否则白字伤害与清晰预兆触发链路全部缺失。
        DoMeleeAttackIfReady();
    }

private:
    // 自管冷却登记
    uint32 berserkCooldown{ 0 };
    uint32 tigersFuryCooldown{ 0 };
    uint32 survivalInstinctsCooldown{ 0 };
    uint32 barkskinCooldown{ 0 };
    uint32 faerieFireCooldown{ 0 };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateFeralTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(berserkCooldown);
        Tick(tigersFuryCooldown);
        Tick(survivalInstinctsCooldown);
        Tick(barkskinCooldown);
        Tick(faerieFireCooldown);
    }

    void ResetFeralTimers()
    {
        berserkCooldown = 0;
        tigersFuryCooldown = 0;
        survivalInstinctsCooldown = 0;
        barkskinCooldown = 0;
        faerieFireCooldown = 0;
    }

    // =========================================================================
    // 形态与资源维护
    // =========================================================================
    bool MaintainCatForm()
    {
        if (me->HasAura(FeralCatDruidSpells::AURA_CAT_FORM))
            return false;

        if (!CanCast(me, FeralCatDruidSpells::CAT_FORM, true))
            return false;

        return ExecuteSpell(me, FeralCatDruidSpells::CAT_FORM, true);
    }

    void MaintainResourcePools()
    {
        // 能量池固定 100 点 (引擎若按 10 倍存储自动上调，此处仅做下限保障)
        if (me->GetMaxPower(POWER_ENERGY) < 100)
            me->SetMaxPower(POWER_ENERGY, 100);

        if (me->GetPower(POWER_ENERGY) > me->GetMaxPower(POWER_ENERGY))
            me->SetPower(POWER_ENERGY, me->GetMaxPower(POWER_ENERGY));

        // 猎豹形态 / 精灵之火 / 树皮术等自然系技能仍消耗法力，必须保留法力池，
        // 否则形态掉落后因法力枯竭将永久无法重新变形。
        if (me->GetMaxPower(POWER_MANA) < 5000)
            me->SetMaxPower(POWER_MANA, 5000);

        if (me->GetPower(POWER_MANA) < me->GetMaxPower(POWER_MANA))
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));
    }

    // =========================================================================
    // 天赋习得判定
    // -------------------------------------------------------------------------
    // 3.3.5a 天赋法术 DBC SpellLevel 恒为 0，GetAppropriateRank 永远返回最高 Rank，
    // 不会因等级不足而降阶。必须按 GetTalentSpellMinLevel 契约显式门禁，
    // 否则 50 级以下随从会误判「已习得裂伤」，永久 CanCast 失败而彻底不产星。
    // =========================================================================
    bool HasTalentSpell(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        return (minLevel == 0) || (me->GetLevel() >= minLevel);
    }

    // =========================================================================
    // 状态判定器
    // =========================================================================
    bool IsEliteOrBossTarget(Unit* target) const
    {
        if (!target)
            return false;

        if (target->GetTypeId() == TYPEID_PLAYER)
            return true;

        if (Creature* creature = target->ToCreature())
        {
            CreatureTemplate const* tmpl = creature->GetCreatureTemplate();
            return tmpl && tmpl->rank >= CREATURE_ELITE_ELITE;
        }

        return false;
    }

    // 咆哮为单阶法术，优先按分阶光环查询并回退到直接 ID 查询，杜绝漏检。
    Aura* GetSavageRoarAura() const
    {
        Aura* roar = me->GetAuraOfRankedSpell(FeralCatDruidSpells::AURA_SAVAGE_ROAR);
        if (!roar)
            roar = me->GetAura(FeralCatDruidSpells::AURA_SAVAGE_ROAR);

        return roar;
    }

    // 剩余时间读取：无光环返回 0；永久光环 (duration < 0) 返回极大值
    int32 GetSavageRoarRemainingMs() const
    {
        Aura* roar = GetSavageRoarAura();
        if (!roar)
            return 0;

        int32 const duration = roar->GetDuration();
        return (duration < 0) ? 3600000 : duration;
    }

    bool NeedsSavageRoarRefresh() const
    {
        // 野性咆哮为 75 级天赋，低等级随从未习得该技能。
        // 若不做等级门禁，咆哮光环将永远缺失，NeedsSavageRoarRefresh 恒为 true，
        // 使决策流在「补咆哮」分支中因 CanCast 永久失败而陷入虚假挂起发呆死锁。
        if (me->GetLevel() < 75)
            return false;

        return GetSavageRoarRemainingMs() <= SND_REFRESH_MS;
    }

    // 割裂 / 斜掠必须按「自身 GUID」校验来源，防止团队多德鲁伊互相顶替导致误判已覆盖。
    int32 GetOwnDebuffRemainingMs(Unit* victim, uint32 spellId) const
    {
        if (!victim || !spellId)
            return 0;

        Aura* aura = victim->GetAuraOfRankedSpell(spellId, me->GetGUID());
        if (!aura)
            aura = victim->GetAura(spellId, me->GetGUID());

        if (!aura)
            return 0;

        int32 const duration = aura->GetDuration();
        return (duration < 0) ? 3600000 : duration;
    }

    bool NeedsRipRefresh(Unit* victim, int32 marginMs = RIP_REFRESH_MS) const
    {
        return GetOwnDebuffRemainingMs(victim, FeralCatDruidSpells::AURA_RIP) <= marginMs;
    }

    bool NeedsRakeRefresh(Unit* victim) const
    {
        return GetOwnDebuffRemainingMs(victim, FeralCatDruidSpells::AURA_RAKE) <= RAKE_REFRESH_MS;
    }

    // 流血易伤：猎豹裂伤 / 熊裂伤 / 武器战创伤 均可为割裂提供 +30% 增伤跳板。
    // 裂伤与创伤均为分阶法术，且低阶 Debuff 的 ID 与最高阶不同，
    // 直接 HasAura(最高阶) 会漏检低阶 Debuff，导致「已挂易伤却判定缺失」，
    // 决策流每帧重打裂伤形成 100% 死锁吞能事故。必须走 GetAuraOfRankedSpell 全分阶查验。
    bool HasBleedVulnerability(Unit* victim) const
    {
        if (!victim)
            return false;

        return (victim->GetAuraOfRankedSpell(FeralCatDruidSpells::MANGLE_CAT) != nullptr) ||
               (victim->GetAuraOfRankedSpell(33878) != nullptr) || // 熊裂伤全分阶
               (victim->GetAuraOfRankedSpell(46856) != nullptr);   // 武器战创伤全分阶
    }

    // 判断随从是否已占住目标背身位：目标正面 180° 锥形之外即视为背后。
    // 撕碎 / 裂伤均有背后硬性要求，提前自检可避免白白空放被底层拒绝。
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
    // P0.5: 濒死自保
    // -------------------------------------------------------------------------
    // 树皮术与生存本能均为 Off-GCD 瞬发防御，施放成功严禁 return：
    // 一旦 return 会白吞当帧的能量充盈输出窗口 (自保技挤占输出帧)。
    // =========================================================================
    void TrySurvivalCooldowns()
    {
        // ---- 树皮术：20% 硬减伤，允许在猎豹形态内直接施放 ----
        if (barkskinCooldown == 0 && me->GetHealthPct() < BARKSKIN_HP_PCT &&
            !me->HasAura(FeralCatDruidSpells::BARKSKIN))
        {
            uint32 const barkskin = GetAppropriateRank(FeralCatDruidSpells::BARKSKIN, false);
            if (barkskin && CanCast(me, barkskin, true) && ExecuteSpell(me, barkskin, true))
                barkskinCooldown = CD_BARKSKIN;
        }

        // ---- 生存本能：生命上限提高 30%，纯天赋 20 级解锁 ----
        if (survivalInstinctsCooldown == 0 && me->GetHealthPct() < SURVIVAL_INSTINCTS_HP_PCT &&
            HasTalentSpell(FeralCatDruidSpells::SURVIVAL_INSTINCTS) &&
            !me->HasAura(FeralCatDruidSpells::SURVIVAL_INSTINCTS))
        {
            uint32 const survivalInstincts = GetAppropriateRank(FeralCatDruidSpells::SURVIVAL_INSTINCTS, true);
            if (survivalInstincts && CanCast(me, survivalInstincts, true) && ExecuteSpell(me, survivalInstincts, true))
                survivalInstinctsCooldown = CD_SURVIVAL_INSTINCTS;
        }
    }

    // =========================================================================
    // P1: 非 GCD 增能与爆发 (Off-GCD，当帧顺下绝不 return)
    // =========================================================================
    void TryOffensiveCooldowns(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return;

        bool const eliteOrBoss = IsEliteOrBossTarget(victim);
        bool const inMelee = (me->GetDistance(victim) <= MELEE_REACH_DIST);

        // ---- 猛虎之怒：能量见底时立即补满 60 能量并附加增伤 ----
        // 冷却与光环双门禁，既防重入也防溢出 (满能量时开启会把回能全部浪费)。
        if (tigersFuryCooldown == 0 &&
            me->GetPower(POWER_ENERGY) <= TF_MAX_ENERGY &&
            !me->HasAura(FeralCatDruidSpells::AURA_TIGERS_FURY) &&
            !me->HasAura(FeralCatDruidSpells::AURA_BERSERK))
        {
            uint32 const tigersFury = GetAppropriateRank(FeralCatDruidSpells::TIGERS_FURY, false);
            if (tigersFury && CanCast(me, tigersFury, true) && ExecuteSpell(me, tigersFury, true))
                tigersFuryCooldown = CD_TIGERS_FURY;

            // Off-GCD 铁律：增能增益严禁 return，必须允许当帧决策流顺下，
            // 立即把满额能量以终结技/撕碎形式兑现，避免能量自然溢出。
        }

        // ---- 狂暴：Boss / 精英近战位且能量充盈时开启，猎豹技能能量消耗 -50% ----
        if (berserkCooldown == 0 && eliteOrBoss && inMelee &&
            me->GetPower(POWER_ENERGY) >= BERSERK_MIN_ENERGY &&
            HasTalentSpell(FeralCatDruidSpells::BERSERK) &&
            !me->HasAura(FeralCatDruidSpells::AURA_BERSERK))
        {
            uint32 const berserk = GetAppropriateRank(FeralCatDruidSpells::BERSERK, true);
            if (berserk && CanCast(me, berserk, true) && ExecuteSpell(me, berserk, true))
                berserkCooldown = CD_BERSERK;
        }
    }

    // =========================================================================
    // P2: 精灵之火 (野性) 破甲与弱化前置
    // =========================================================================
    bool MaintainFaerieFireFeral(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return false;

        if (!HasTalentSpell(FeralCatDruidSpells::FAERIE_FIRE_FERAL))
            return false;

        if (faerieFireCooldown > 0)
            return false;

        // 野性精灵之火与平衡系精灵之火共享破甲效果，任一存在即无需重挂。
        // 精灵之火同为分阶法术，低阶 Debuff ID 与最高阶不同，
        // 必须走 GetAuraOfRankedSpell 全分阶查验，否则每 6 秒白烧一个 GCD 重复破甲。
        if (victim->GetAuraOfRankedSpell(FeralCatDruidSpells::FAERIE_FIRE_FERAL) ||
            victim->GetAuraOfRankedSpell(FeralCatDruidSpells::AURA_FAERIE_FIRE))
            return false;

        uint32 const faerieFire = GetAppropriateRank(FeralCatDruidSpells::FAERIE_FIRE_FERAL, true);
        if (!faerieFire || !CanCast(victim, faerieFire, true))
            return false;

        if (!ExecuteSpell(victim, faerieFire, true))
            return false;

        faerieFireCooldown = CD_FAERIE_FIRE;
        return true;
    }

    // =========================================================================
    // P3: 连击点核心打击与终结技仲裁
    // =========================================================================
    bool TryCoreRotation(Unit* victim)
    {
        // ---- 1. 清晰预兆 (节能施法)：免费撕碎，最高优先级当帧交出 ----
        if (TryClearcastShred(victim))
            return true;

        uint8 const cp = GetComboPoints(victim);

        // ---- 2. 野性咆哮起步与断档保护 ----
        // 咆哮 = 猫德「切割」，攻速/伤害全链路增益，断档等于全程 DPS 塌方。
        // 只要还有 1 星就补；连 1 星都没有时交由下方产星分支铺垫 (严禁死等)。
        if (NeedsSavageRoarRefresh() && cp >= 1)
        {
            // 终结技攒能挂起：能量不足支付终结技时原地挂起回能，
            // 绝不偷跑其他技能透支能量，否则咆哮会被无限期推迟。
            if (me->GetPower(POWER_ENERGY) < FINISHER_ENERGY)
                return false;

            if (TrySavageRoar(victim))
                return true;
        }

        // ---- 3. 5 星终结技仲裁 (产星通道彻底锁死) ----
        if (cp >= FINISHER_MAX_CP)
        {
            // 攒能挂起 (铁律 20)：5 星且能量不足以支付终结技时原地等待，
            // 贪打产星会同时造成连击点溢出 + 终结技被推迟一整轮回能。
            if (me->GetPower(POWER_ENERGY) < FINISHER_ENERGY)
                return false;

            // 达到 5 星后必须无条件交由终结技处置，严禁 fall through 到产星分支。
            return TryFinisher(victim);
        }

        // ---- 4. 核心流血易伤与 DoT 维持 (产星分支) ----
        // 裂伤 / 创伤缺失时优先铺满 +30% 流血易伤，直接放大割裂与斜掠占比。
        if (!HasBleedVulnerability(victim) && TryMangle(victim))
            return true;

        // 斜掠核心流血 DoT：缺失或 <= 1500ms 立即续订
        if (NeedsRakeRefresh(victim) && TryRake(victim))
            return true;

        // ---- 5. 产星填充 (撕碎 / 裂伤) ----
        return TryBuilder(victim);
    }

    // =========================================================================
    // 5 星终结技精准仲裁
    // -------------------------------------------------------------------------
    // 核心原则：连击点是最稀缺资源，5 星必须打在「真正需要它」的技能上。
    // 严禁用低星咆哮 / 低星割裂去顶替 5 星窗口 (会白白损失 4 星的持续时间收益)，
    // 也严禁在割裂进入刷新窗口前用凶猛撕咬提前顶掉主力流血。
    // 割裂处于 4~8 秒的中间区间时，唯一正确动作是原地挂起等待其进入
    // 最终刷新窗口，而不是把 5 星浪费在咆哮或撕咬上。
    // =========================================================================
    bool TryFinisher(Unit* victim)
    {
        // ---- 1. 咆哮剩余 <= 9s：优先以 5 星续订满额 34s 咆哮 ----
        if (me->GetLevel() >= 75 && GetSavageRoarRemainingMs() <= 9000)
        {
            if (TrySavageRoar(victim))
                return true;
        }

        // ---- 2. 割裂剩余 <= 4s (或完全缺失)：以 5 星打出满额割裂 ----
        if (NeedsRipRefresh(victim, 4000))
        {
            if (TryRip(victim))
                return true;
        }

        // ---- 3. 咆哮与割裂均充裕 (> 8s)：凶猛撕咬强力泄能 ----
        if (GetSavageRoarRemainingMs() > BITE_MIN_MARGIN_MS &&
            GetOwnDebuffRemainingMs(victim, FeralCatDruidSpells::AURA_RIP) > BITE_MIN_MARGIN_MS)
        {
            if (TryFerociousBite(victim))
                return true;
        }

        // ---- 4. 割裂处于 4~8s 的待刷新区间且咆哮充足：原地挂起控星 ----
        // 此区间打出凶猛撕咬会让割裂在数秒后彻底断档，丢失整段主力流血伤害；
        // 以挂起等待割裂滑入 4s 刷新窗口，届时当帧以 5 星补齐，收益最大。
        return false;
    }

    // =========================================================================
    // 产星填充：占住背后位优先撕碎，否则降级裂伤正面产星
    // =========================================================================
    bool TryBuilder(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return false;

        // 怪盯防随从本人时随从无法占住背身位，此时撕碎会被底层直接拒绝，
        // 交由下方裂伤分支正面产星，杜绝围绕目标无限对转的贴背空转。
        if (IsBehindVictim(victim, MELEE_REACH_DIST))
        {
            // 已占住背后位：坚决控能等待撕碎。
            // 撕碎是唯一具备完整伤害系数的产星技 (裂伤仅为易伤铺垫的正面填充)，
            // 在 35~41 能量区间偷跑一记裂伤会同时踩两个坑：吃掉本该留给撕碎的能量，
            // 并把连击点推高导致下一发撕碎被 5 星上限浪费，产星效率反而下降。
            if (me->GetPower(POWER_ENERGY) < SHRED_ENERGY)
                return false;

            return TryShred(victim);
        }

        return TryMangle(victim);
    }

    // ---- 撕碎：背后高伤害产星重击 (撕碎雕文可顺带延长割裂持续时间) ----
    bool TryShred(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return false;

        if (me->GetPower(POWER_ENERGY) < SHRED_ENERGY)
            return false;

        uint32 const shred = GetAppropriateRank(FeralCatDruidSpells::SHRED, false);
        if (!shred || !CanCast(victim, shred, true))
            return false;

        if (!ExecuteSpell(victim, shred, true))
            return false;

        AddComboPoints(victim, 1);
        return true;
    }

    // ---- 裂伤-猎豹：纯天赋 50 级解锁，正面填充并铺垫 +30% 流血易伤 ----
    bool TryMangle(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return false;

        if (!HasTalentSpell(FeralCatDruidSpells::MANGLE_CAT))
            return false;

        if (me->GetPower(POWER_ENERGY) < MANGLE_ENERGY)
            return false;

        uint32 const mangle = GetAppropriateRank(FeralCatDruidSpells::MANGLE_CAT, true);
        if (!mangle || !CanCast(victim, mangle, true))
            return false;

        if (!ExecuteSpell(victim, mangle, true))
            return false;

        AddComboPoints(victim, 1);
        return true;
    }

    // ---- 斜掠：核心产星流血 DoT ----
    bool TryRake(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return false;

        if (me->GetPower(POWER_ENERGY) < RAKE_ENERGY)
            return false;

        uint32 const rake = GetAppropriateRank(FeralCatDruidSpells::RAKE, false);
        if (!rake || !CanCast(victim, rake, true))
            return false;

        if (!ExecuteSpell(victim, rake, true))
            return false;

        AddComboPoints(victim, 1);
        return true;
    }

    // ---- 野性咆哮：自身增益型终结技 ----
    bool TrySavageRoar(Unit* /*victim*/)
    {
        uint32 const savageRoar = GetAppropriateRank(FeralCatDruidSpells::SAVAGE_ROAR, false);
        if (!savageRoar || !CanCast(me, savageRoar, true))
            return false;

        if (!ExecuteSpell(me, savageRoar, true))
            return false;

        SpendComboPoints();
        return true;
    }

    // ---- 割裂：5 星主力物理流血终结技 ----
    bool TryRip(Unit* victim)
    {
        uint32 const rip = GetAppropriateRank(FeralCatDruidSpells::RIP, false);
        if (!rip || !CanCast(victim, rip, true))
            return false;

        if (!ExecuteSpell(victim, rip, true))
            return false;

        SpendComboPoints();
        return true;
    }

    // ---- 凶猛撕咬：5 星直接物理爆发泄能终结技 ----
    bool TryFerociousBite(Unit* victim)
    {
        uint32 const bite = GetAppropriateRank(FeralCatDruidSpells::FEROCIOUS_BITE, false);
        if (!bite || !CanCast(victim, bite, true))
            return false;

        if (!ExecuteSpell(victim, bite, true))
            return false;

        SpendComboPoints();
        return true;
    }

    // =========================================================================
    // 清晰预兆 (节能施法) 免费撕碎
    // -------------------------------------------------------------------------
    // Omen of Clarity 触发时会将下一个技能的能量消耗清零，但基类 CanCast 的能量
    // 校验读取的是 DBC 基础消耗，且 Creature 实体没有 SpellModOwner，
    // 无法感知该 SpellMod 折扣，会把「0 耗能的免费撕碎」误判为能量不足而永久阻断。
    // 因此持有节能施法时允许旁路能量门禁，仅保留几何与目标合法性校验后直放。
    // =========================================================================
    bool TryClearcastShred(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return false;

        if (!me->HasAura(FeralCatDruidSpells::AURA_CLEARCASTING))
            return false;

        // 撕碎为背后技，未占住背身位时直接让路给裂伤正面产星
        if (!IsBehindVictim(victim, MELEE_REACH_DIST) || !me->IsWithinMeleeRange(victim))
            return false;

        uint32 const shred = GetAppropriateRank(FeralCatDruidSpells::SHRED, false);
        if (!shred)
            return false;

        // 强制 triggered = true 直放：
        // triggerFlags 会旁路底层 CheckPower 能量校验，真正兑现「节能施法 0 消耗」。
        // 若走普通 CastSpell，引擎仍按 DBC 基础消耗 (42 能量) 校验，
        // 低能量时免费撕碎会被直接拒放，清晰预兆光环被白白遗留过期。
        me->SetFacingToObject(victim);
        if (me->CastSpell(victim, shred, true) != SPELL_CAST_OK)
            return false;

        // 手工消费节能施法光环并置位能量职业的 1000ms 公共冷却
        // (triggered 施法不会自动扣减光环，也不会自行占 GCD)
        me->RemoveAurasDueToSpell(FeralCatDruidSpells::AURA_CLEARCASTING);
        gcdTimer = 1000;
        AddComboPoints(victim, 1);
        return true;
    }

    // =========================================================================
    // P4: 物理近战背后找背站位模型 (铁律 16 / 38)
    // =========================================================================
    void MaintainMeleeBehindPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        // 读条期间严禁下发走位指令，否则本帧起手的读条法术会被同帧 MoveFollow 秒断。
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // ---- P0: APF 势场紧急避险 (火圈/顺劈强行接管，规避背后盲区站立吃火) ----
        // 找背站位以 M_PI 锁定目标正后方，该方位在动态战局中常与地面火圈重叠；
        // 若不允许势场接管，撕碎读条会与火圈伤害互相叠加导致暴毙。
        // 势场单步外推为定长，逐帧重算会无限掐断起跑动画形成原地抽搐，
        // 故做 300ms 帧节流，并仅在脱离 POINT 生成器(被打断/被抢占)时才重规划。
        if (IsUnderDangerThreat(2.0f))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;
                if (PotentialField::CalculateNextPosition(me, victim, MELEE_FOLLOW_DIST, true, false, activeDangerZones, nextX, nextY, nextZ))
                {
                    me->GetMotionMaster()->MovePoint(1, nextX, nextY, nextZ);
                    apfMoveUpdateTimer = 300;
                    return;
                }
            }
            else
            {
                return; // 正在平滑执行 APF 避险航点, 不打断既有路径
            }
        }

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
            // 由 P3 的 TryBuilder 自动降级裂伤正面产星维持输出。
            if (!isChasing || dist > MELEE_REACH_DIST)
                me->GetMotionMaster()->MoveChase(victim, MELEE_FOLLOW_DIST);

            return;
        }

        // ---- 已平滑贴身：保持贴背输出，不打断普攻节奏 ----
        if (isFollowing && dist <= MELEE_COMFORT_DIST)
            return;

        // ---- 怪物盯防主坦：严格占住目标背身位 ----
        // angle 必须传正后方 BEHIND_ANGLE (M_PI)：引擎内部已按目标当前朝向结算偏移，
        // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移；
        // 传 0.0f 会贴在 Boss 脸前吃顺劈与正面吐息，物理近战必须始终占住背后位。
        if (dist > MELEE_REACH_DIST || !isFollowing)
        {
            me->GetMotionMaster()->MoveFollow(victim, MELEE_FOLLOW_DIST, BEHIND_ANGLE);
        }
    }

    // =========================================================================
    // 野性战斗天赋满阶被动光环补偿 (铁律 33，弥补 NPC 缺天赋树缺陷)
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

        // 基础野性被动 (满阶 Rank 根源，杜绝 Rank 1 数值缩水)
        SyncPassive(FeralCatDruidSpells::PASSIVE_TALENT_MIN_LEVEL, FeralCatDruidSpells::OMEN_OF_CLARITY);        // 清晰预兆：平砍几率触发节能施法
        SyncPassive(FeralCatDruidSpells::PASSIVE_TALENT_MIN_LEVEL, FeralCatDruidSpells::FEROCITY);              // 凶暴 Rank 5：裂伤/斜掠/撕咬耗能 -5
        SyncPassive(FeralCatDruidSpells::PASSIVE_TALENT_MIN_LEVEL, FeralCatDruidSpells::PREDATORY_STRIKES);     // 猛兽攻击 Rank 3：近战攻强加成
        SyncPassive(15, FeralCatDruidSpells::SHREDDING_ATTACKS);                                                // 撕碎攻击 Rank 2：撕碎耗能 -18
        SyncPassive(15, FeralCatDruidSpells::NATURAL_REACTION);                                                 // 自然反应 Rank 2：躲闪 +6%
        SyncPassive(20, FeralCatDruidSpells::FERAL_AGGRESSION);                                                 // 野性侵略 Rank 5：凶猛撕咬伤害 +15%
        SyncPassive(20, FeralCatDruidSpells::HEART_OF_THE_WILD);                                                // 野性之心 Rank 5：属性加成
        SyncPassive(20, FeralCatDruidSpells::REND_AND_TEAR);                                                    // 撕扯 Rank 5：流血目标撕碎 +20%
        SyncPassive(20, FeralCatDruidSpells::SURVIVAL_OF_THE_FITTEST);                                          // 适者生存 Rank 3：属性 +6%
        SyncPassive(40, FeralCatDruidSpells::KING_OF_THE_JUNGLE);                                               // 丛林之王 Rank 3：猛虎之怒立即回 60 能量
        SyncPassive(55, FeralCatDruidSpells::PRIMAL_GORE);                                                      // 原始血腥：割裂每跳可暴击

        // 雕文补偿
        SyncPassive(20, FeralCatDruidSpells::GLYPH_OF_SHRED);                                                   // 撕碎雕文：撕碎延长割裂最多 6s
        SyncPassive(20, FeralCatDruidSpells::GLYPH_OF_RIP);                                                     // 割裂雕文：割裂时长 +4s
        SyncPassive(20, FeralCatDruidSpells::GLYPH_OF_SAVAGE_ROAR);                                             // 野性咆哮雕文：额外 +3% 伤害
    }
};

void AddSC_bot_feral_cat_druid()
{
    new AdaptiveBotScript<BotFeralCatDruidAI>("bot_feral_cat_druid");
}
