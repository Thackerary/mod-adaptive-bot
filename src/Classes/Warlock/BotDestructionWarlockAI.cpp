/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "DestructionWarlockSpells.h"
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

class BotDestructionWarlockAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位与射程参数 (铁律 17 迟滞区间 / 铁律 36 射程收敛)
    // -------------------------------------------------------------------------
    // 毁灭术主力通道 (烧尽/混乱之箭/献祭/暗影箭) 射程均为 30~35 码,
    // 不存在短射程填充技拘束, 故最大交战距离维持 35 码, 理想站桩位 28 码。
    // =========================================================================
    static constexpr float MIN_ENGAGE_DIST       = 15.0f;  // 安全站桩读条下限线
    static constexpr float DEADZONE_RETREAT_DIST = 8.0f;   // 近战盲区撤退进入线
    static constexpr float RETREAT_SAFE_DIST     = 16.0f;  // 撤退姿态退出安全线 (必须 > 进入线)
    static constexpr float MAX_ENGAGE_DIST       = 35.0f;  // 最大交战距离 (脱节上限)
    static constexpr float IDEAL_SHOT_DIST       = 28.0f;  // 理想施法站位

    // 撤离锚定坦克背身位时的跟随距离: 必须 >= RETREAT_SAFE_DIST。
    // 若沿用贴坦距离, 随从与 Boss 的间距仍落在 8 码近战盲区内,
    // 会持续反复触发撤退, 永远无法恢复 15 码外的施法站位。
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
    static constexpr uint32 CD_CONFLAGRATE      = 10000;   // 燃烧 10s
    static constexpr uint32 CD_SHADOWBURN       = 15000;   // 暗影灼烧 15s
    static constexpr uint32 CD_DEATH_COIL       = 120000;  // 死亡缠绕 2 分钟
    static constexpr uint32 CD_SOUL_SHATTER     = 300000;  // 灵魂碎裂 5 分钟

    // 混乱之箭冷却动态结算: 基础 12s, 持【混乱之箭雕文】缩短 2s
    static constexpr uint32 CD_CHAOS_BOLT_BASE  = 12000;
    static constexpr uint32 CD_CHAOS_BOLT_GLYPH = 10000;

    // 生命分流节流: 防止雕文门禁在短窗口内逐帧空转重入
    static constexpr uint32 LIFE_TAP_RETRY      = 3000;

    // DoT 断档预判窗口 (覆盖读条 + 网络延迟)
    static constexpr int32 CORRUPTION_REFRESH_MS = 2000;
    static constexpr int32 IMMOLATE_REFRESH_MS   = 1500;
    static constexpr int32 CURSE_REFRESH_MS      = 2000;

    // 生命与法力阈值
    static constexpr float DEATH_COIL_HP_PCT    = 30.0f;  // 死亡缠绕自保血线
    static constexpr float LIFE_TAP_SAFE_HP_PCT = 50.0f;  // 生命分流安全血线 (严禁濒死自残)
    static constexpr float LIFE_TAP_MANA_PCT    = 35.0f;  // 法力枯竭补蓝线
    static constexpr float EXECUTE_HP_PCT       = 35.0f;  // 末日灾祸互斥阈值 (30% 阶段改用痛苦诅咒)
    static constexpr float SHADOWBURN_HP_PCT    = 20.0f;  // 暗影灼烧斩杀补刀线

    // 烧尽 Rank 1 于 64 级解锁: 低于该等级严禁走烧尽通道,
    // 否则取到低级不可用 ID 会因底层拒放而每帧空转, 且低级段彻底丧失填充技。
    static constexpr uint8 LEVEL_INCINERATE = 64;

    // 满阶被动天赋与雕文解锁等级 (严格注入满阶 Rank 根源, 铁律 33)
    static constexpr uint8 LEVEL_PASSIVE_TALENTS = 20;
    static constexpr uint8 LEVEL_GLYPH           = 20;

public:
    explicit BotDestructionWarlockAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    bool IsHealerBot() const override { return false; }

    // 远程随从按远程单位接管移动逻辑, 禁止迈入怪物近战范围
    bool IsRangedBot() const override { return true; }

    // 法系远程 (与猎人物理远程解耦): 伤害由法术强度与暗影/火焰乘数通道支撑
    bool IsRangedPhysicalBot() const override { return false; }

    // 专精契约：毁灭术携带小鬼（远程火焰箭内核 + 血之契印耐力光环）
    GuardianVisualType GetPreferredGuardianVisualType() const override { return GUARDIAN_VISUAL_WARLOCK_IMP; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注: 3.3.5a 中纯天赋技能 DBC SpellLevel 恒为 0, GetAppropriateRank 无法降阶,
    //     必须在此登记最低解锁等级并在施法前显式门禁。
    //     基础法术 (献祭/烧尽/暗影灼烧/暗影箭/诅咒/腐蚀/分流/邪甲/死缠/碎魂) 严禁登记于此。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case DestructionWarlockSpells::CONFLAGRATE: return 40;
            case DestructionWarlockSpells::CHAOS_BOLT:  return 60;
            default:                                    return 0;
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

        if (me->GetMaxPower(POWER_MANA) > 0)
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));

        ResetDestructionTimers();
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
        UpdateDestructionTimers(diff);

        // 全局读条/引导双保险守卫 (铁律 1): 读条期间引擎置位 UNIT_STATE_CASTING,
        // 引导类法术在部分状态下不置位, 故追加 CURRENT_CHANNELED_SPELL 显式判定,
        // 杜绝长读条 (混乱之箭/烧尽/献祭/暗影箭) 被跟随走位指令当场掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainFelArmor())
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
            MaintainFelArmor();
            return;
        }

        // 远程随从仅锚定敌对目标维持进战姿态供施法链路使用,
        // 第二参数传 false 绝不开启近战追击 (CONTEXT.md 铁律)。
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // ---- P0: 极限自保与仇恨清除 ----
        if (TrySurvival(victim)) return;

        // ---- P1: 护甲维持与控蓝分流 ----
        if (MaintainFelArmor()) return;
        if (TryManaMaintenance()) return;

        // ---- P2: 核心 DoT 维持 ----
        if (TryMaintainDots(victim)) return;

        // ---- P3: 核心毁灭法术优先级 ----
        if (TryDestructionRotation(victim)) return;

        // ---- P4: 站位控制 ----
        MaintainRangedPositioning(victim);
    }

private:
    // =========================================================================
    // 自管冷却与状态登记
    // =========================================================================
    uint32 conflagrateCooldown{ 0 };
    uint32 chaosBoltCooldown{ 0 };
    uint32 shadowburnCooldown{ 0 };
    uint32 deathCoilCooldown{ 0 };
    uint32 soulShatterCooldown{ 0 };
    uint32 lifeTapRetryTimer{ 0 };

    // 近战盲区撤离迟滞姿态标记 (铁律 17): 必须凭此标记主动重发走位指令,
    // 否则会在脱离 8 码的瞬间被清空, 与立定分支每帧交替触发形成原地抽搐。
    bool isRetreating{ false };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateDestructionTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(conflagrateCooldown);
        Tick(chaosBoltCooldown);
        Tick(shadowburnCooldown);
        Tick(deathCoilCooldown);
        Tick(soulShatterCooldown);
        Tick(lifeTapRetryTimer);
    }

    void ResetDestructionTimers()
    {
        conflagrateCooldown = 0;
        chaosBoltCooldown = 0;
        shadowburnCooldown = 0;
        deathCoilCooldown = 0;
        soulShatterCooldown = 0;
        lifeTapRetryTimer = 0;

        isRetreating = false;
    }

    // =========================================================================
    // 天赋契约等级门禁
    // -------------------------------------------------------------------------
    // 3.3.5a 中天赋法术的 DBC SpellLevel 恒为 0, GetAppropriateRank 不会因等级而降阶,
    // 低等级下依旧返回最高 Rank 的 ID。若不加门禁, 45 级术士会直接搓出混乱之箭。
    // 故所有在 GetTalentSpellMinLevel 登记的天赋必须经此函数解析。
    // =========================================================================
    uint32 GetTalentRank(uint32 spellId) const
    {
        if (me->GetLevel() < GetTalentSpellMinLevel(spellId))
            return 0;

        return GetAppropriateRank(spellId, true);
    }

    // 混乱之箭冷却动态结算: 基础 12s, 持【混乱之箭雕文】缩短 2s
    uint32 GetChaosBoltCooldown() const
    {
        return me->HasAura(DestructionWarlockSpells::GLYPH_OF_CHAOS_BOLT)
            ? CD_CHAOS_BOLT_GLYPH
            : CD_CHAOS_BOLT_BASE;
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

    // 仇恨失控判定: 敌对单位越过主坦直接盯防随从本人, 即为 OT (维度 C 归因输入信号)
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

    // 取自身施加的 DoT 剩余时间 (毫秒); 不存在则返回 0
    int32 GetOwnDotRemaining(Unit* victim, uint32 rankedSpellId) const
    {
        if (!victim)
            return 0;

        Aura* const aura = victim->GetAuraOfRankedSpell(rankedSpellId, me->GetGUID());
        return aura ? aura->GetDuration() : 0;
    }

    // =========================================================================
    // 爆燃充能消费补偿
    // -------------------------------------------------------------------------
    // 随从缺乏 Player 的 SpellModOwner 机制: 烧尽/混乱之箭消费爆燃充能的逻辑
    // 由玩家的法术修饰器在施法成功时自动扣除, Creature 实体不会触发该回调。
    // 若不手工剥层, 3 层充能会永久挂身, 使读条法术退化为无消耗的无限加速通道。
    // =========================================================================
    void ConsumeBackdraft()
    {
        if (Aura* backdraft = me->GetAura(DestructionWarlockSpells::AURA_BACKDRAFT))
            backdraft->DropCharge();
    }

    // =========================================================================
    // 通用施法通道 (立定判定与瞬发解耦)
    // =========================================================================
    // 非瞬发读条: CanCast 通过后立即刹停并当帧起手。
    // 严禁写成「移动中直接 return false」——随从在风筝跑位期间会判定读条永久失败,
    // 决策流被 P4 站位分支反复抢占, DoT 无限期缺席。
    // 瞬发技能 (allowMoving = true) 则完整保留机动性, 严禁刹停。
    bool TryCastSpell(Unit* victim, uint32 spellId, bool allowMoving)
    {
        if (!spellId || !victim)
            return false;

        // 施法资格必须先通过校验再刹停: CanCast 失败时提前立定,
        // 会让随从在重构走位期间被 StopMoving 每帧拉扯成原地抽搐。
        if (!CanCast(victim, spellId, true))
            return false;

        if (!allowMoving && me->isMoving())
            me->StopMoving();

        return ExecuteSpell(victim, spellId, true);
    }

    // DoT 维持统一通道: 以「分阶查询 + 自身 GUID」双重判定 DoT 是否存在。
    // 严禁直接用满级 ID 做 HasAura —— 低等级降阶施放时判定必然失败, 会陷入每帧空转重刷。
    // allowMoving = true 仅授予瞬发 DoT (腐蚀术 / 痛苦诅咒 / 末日灾祸),
    // 献祭等读条 DoT 必须传 false 走刹停通道。
    bool TryMaintainDot(Unit* victim, uint32 rankedSpellId, uint32 castSpellId, int32 refreshWindowMs = 0, bool allowMoving = false)
    {
        if (!victim || !castSpellId)
            return false;

        if (GetOwnDotRemaining(victim, rankedSpellId) > refreshWindowMs)
            return false;

        return TryCastSpell(victim, castSpellId, allowMoving);
    }

    // =========================================================================
    // P0: 极限自保与仇恨清除 (铁律 21)
    // =========================================================================
    bool TrySurvival(Unit* victim)
    {
        // ---- 死亡缠绕: 濒死或被物理近战压制时瞬发惊骇拉开距离并回血 ----
        // 近战压制判定必不可少: 远程随从被贴身时血线往往仍高于 30%,
        // 仅看血线会让死亡缠绕在真正需要脱身的那一刻被门禁挡下。
        if (deathCoilCooldown == 0 && victim &&
            (me->GetHealthPct() < DEATH_COIL_HP_PCT || IsUnderPhysicalMelee(me)))
        {
            uint32 const deathCoil = GetAppropriateRank(DestructionWarlockSpells::DEATH_COIL, false);
            if (deathCoil && CanCast(victim, deathCoil, true) && ExecuteSpell(victim, deathCoil, true))
            {
                deathCoilCooldown = CD_DEATH_COIL;
                return true;
            }
        }

        // ---- 灵魂碎裂: 仇恨彻底失控时瞬发削减 50% 仇恨, 把目标还给主坦 ----
        if (soulShatterCooldown == 0 && IsTopThreatTarget())
        {
            uint32 const soulShatter = GetAppropriateRank(DestructionWarlockSpells::SOUL_SHATTER, false);
            if (soulShatter && CanCast(me, soulShatter, true) && ExecuteSpell(me, soulShatter, true))
            {
                soulShatterCooldown = CD_SOUL_SHATTER;
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P1: 护甲维持与控蓝分流
    // =========================================================================
    bool MaintainFelArmor()
    {
        // 护甲为分阶法术: 以分阶查询判定, 避免低 Rank 光环被误判为缺失而反复顶替
        if (me->GetAuraOfRankedSpell(DestructionWarlockSpells::FEL_ARMOR))
            return false;

        uint32 const felArmor = GetAppropriateRank(DestructionWarlockSpells::FEL_ARMOR, false);
        if (!felArmor || !CanCast(me, felArmor, true))
            return false;

        return ExecuteSpell(me, felArmor, true);
    }

    bool TryManaMaintenance()
    {
        if (me->getPowerType() != POWER_MANA)
            return false;

        // 分流节流: 雕文门禁在短窗口内逐帧判定会形成空转重入
        if (lifeTapRetryTimer > 0)
            return false;

        float const hpPct   = me->GetHealthPct();
        float const manaPct = me->GetPowerPct(POWER_MANA);

        // 安全血线门禁: 严禁在濒死血线自残, 补蓝与维持雕文均让路给治疗
        if (hpPct <= LIFE_TAP_SAFE_HP_PCT)
            return false;

        // a) 雕文法强维持通道: 仅在【生命分流雕文】本体已注入时才启用该通道 ——
        //    若雕文缺失, 63321 光环永远不会出现, 本分支将退化为每 3 秒自残一次的无限空转。
        // b) 法力枯竭补蓝通道: 低于 35% 蓝线且血线安全 (> 50%)
        bool const hasLifeTapGlyph = me->HasAura(DestructionWarlockSpells::GLYPH_OF_LIFE_TAP);
        bool const needGlyphBuff   = hasLifeTapGlyph &&
                                     !me->HasAura(DestructionWarlockSpells::AURA_GLYPH_OF_LIFE_TAP);
        bool const needManaRefill  = (manaPct < LIFE_TAP_MANA_PCT);

        if (!needGlyphBuff && !needManaRefill)
            return false;

        uint32 const lifeTap = GetAppropriateRank(DestructionWarlockSpells::LIFE_TAP, false);
        if (!lifeTap || gcdTimer > 0)
            return false;

        // 生命分流的消耗通道为「生命值」(POWER_HEALTH): CanCast / ExecuteSpell 的能量校验
        // 会对该非法能量索引执行 GetPower 读取, 存在读到邻域字段垃圾值并误判
        // 「能量不足」从而把分流永久阻断的风险。故此处彻底绕开通用施法通道,
        // 直接以引擎底层 CastSpell 直放, 并以自管蓝线/血线/GCD 三重门禁替代资源校验。
        // 必须传 triggered = true: 非触发式通道仍会在底层执行 CheckPower 校验并越界读取
        // POWER_HEALTH 字段, 只有触发式直放才能 100% 绕过能量与目标类型门禁。
        if (me->CastSpell(me, lifeTap, true) != SPELL_CAST_OK)
            return false;

        gcdTimer = 1500;
        lifeTapRetryTimer = LIFE_TAP_RETRY;
        return true;
    }

    // =========================================================================
    // P2: 核心 DoT 维持
    // =========================================================================
    bool TryMaintainDots(Unit* victim)
    {
        if (!victim)
            return false;

        // ---- 1. 献祭: 燃烧 (Conflagrate) 的硬性机制前置与全技能增伤基石 (绝对第一优先级) ----
        // 献祭同时承担三重职责: 燃烧的施放前置、爆燃 (Backdraft) 的触发源、
        // 以及硫磺与烈火对烧尽/混乱之箭的增伤条件。任何一次断档都会让后续
        // 整个核心循环链路失效, 故其补挂优先级必须高于诅咒与腐蚀术。
        if (TryImmolate(victim))
            return true;

        // ---- 2. 诅咒分配 (curse 类别互斥, 只维持一种) ----
        if (TryCurse(victim))
            return true;

        // ---- 3. 腐蚀术: 瞬发暗影 DoT 补充 ----
        if (TryCorruption(victim))
            return true;

        return false;
    }

    // -------------------------------------------------------------------------
    // 诅咒调度 (末日灾祸 / 痛苦诅咒)
    // -------------------------------------------------------------------------
    // 末日灾祸为 60 秒单发超高伤害诅咒, 首跳与末跳时间跨度过长,
    // 只适用于血线稳定的长线首领战, 且必须「仅完全缺失时施放」——
    // 任何提前重刷都会把已累计的整段跳数连同末跳末日守卫一并吞掉。
    // 普通小怪与 35% 以下斩杀期改维持平滑的【痛苦诅咒】。
    // -------------------------------------------------------------------------
    bool TryCurse(Unit* victim)
    {
        if (!victim)
            return false;

        // 若目标身上已有未爆炸的末日灾祸, 严禁覆盖 (防止末跳巨大伤害被吞)。
        // 该门禁必须置于最前: 任何后续的末日/痛苦分支都会顶掉已累计的整段跳数,
        // 且痛苦诅咒与末日灾祸同属 curse 类别, 互相施加必定覆盖。
        if (GetOwnDotRemaining(victim, DestructionWarlockSpells::CURSE_OF_DOOM) > 0)
            return false;

        uint32 const curseOfDoom = GetAppropriateRank(DestructionWarlockSpells::CURSE_OF_DOOM, false);

        // 等级自适应: 末日灾祸未习得 (未满 60 级) 时严禁进入该分支,
        // 否则会取到非法/空法术 ID 造成每帧空转, 并让低等级随从彻底裸奔无诅咒。
        bool const useCurseOfDoom = (curseOfDoom != 0) && IsEliteOrBossTarget(victim) &&
                                    victim->GetHealthPct() > EXECUTE_HP_PCT;

        // 末日灾祸为瞬发诅咒, 允许风筝跑动途中直接挂上, 严禁刹停丢机动性
        if (useCurseOfDoom)
            return TryCastSpell(victim, curseOfDoom, true);

        // 痛苦诅咒为瞬发 DoT, 同样允许跑动中直接补挂
        uint32 const curseOfAgony = GetAppropriateRank(DestructionWarlockSpells::CURSE_OF_AGONY, false);
        return TryMaintainDot(victim, DestructionWarlockSpells::CURSE_OF_AGONY, curseOfAgony, CURSE_REFRESH_MS, true);
    }

    // -------------------------------------------------------------------------
    // 腐蚀术 (瞬发核心暗影 DoT)
    // -------------------------------------------------------------------------
    // 判定必须绑定自身 GUID, 多术士场景下严禁把他人的腐蚀术误判为自己的。
    // -------------------------------------------------------------------------
    bool TryCorruption(Unit* victim)
    {
        uint32 const corruption = GetAppropriateRank(DestructionWarlockSpells::CORRUPTION, false);
        return TryMaintainDot(victim, DestructionWarlockSpells::CORRUPTION, corruption, CORRUPTION_REFRESH_MS, true);
    }

    // -------------------------------------------------------------------------
    // 献祭 (1.5s 读条核心火焰 DoT, 燃烧机制前置)
    // -------------------------------------------------------------------------
    // 燃烧 (Conflagrate) 底层硬性要求目标身上存在献祭 DoT, 因此献祭是本专精
    // 循环链路的机制前置, 必须在目标身上持续在线, 严禁因走位放任断档。
    // 撤离途中严禁站桩读条: 走位与读条会互相打断形成原地抽搐。
    // -------------------------------------------------------------------------
    bool TryImmolate(Unit* victim)
    {
        if (isRetreating || !victim)
            return false;

        uint32 const immolate = GetAppropriateRank(DestructionWarlockSpells::IMMOLATE, false);
        return TryMaintainDot(victim, DestructionWarlockSpells::IMMOLATE, immolate, IMMOLATE_REFRESH_MS, false);
    }

    // =========================================================================
    // P3: 核心毁灭法术优先级
    // =========================================================================
    bool TryDestructionRotation(Unit* victim)
    {
        if (!victim)
            return false;

        float const dist = me->GetDistance(victim);

        // 近战盲区与超远脱节一律交由 P0 / P4 接管, 绝不在此硬读条
        if (dist < DEADZONE_RETREAT_DIST || dist > MAX_ENGAGE_DIST)
            return false;

        // ---- 1. 燃烧: 献祭前置的瞬发核爆 ----
        if (TryConflagrate(victim))
            return true;

        // ---- 2. 混乱之箭: 穿透爆击核弹 ----
        if (TryChaosBolt(victim))
            return true;

        // ---- 3. 暗影灼烧: 撤离跑位与斩杀补刀的瞬发填充 ----
        if (TryShadowburn(victim))
            return true;

        // ---- 4. 烧尽: 站桩主力读条填充 ----
        if (TryIncinerate(victim))
            return true;

        return false;
    }

    // -------------------------------------------------------------------------
    // 燃烧 (Conflagrate, 瞬发核心爆发)
    // -------------------------------------------------------------------------
    // 底层硬性要求目标身上存在施法者自己的献祭 DoT, 前置不满足时会被直接拒放,
    // 故必须显式以「自身 GUID 的献祭剩余时间 > 0」作为门禁, 杜绝空转重入。
    // 瞬发无弹道, 严禁 StopMoving 破坏机动性。
    // -------------------------------------------------------------------------
    bool TryConflagrate(Unit* victim)
    {
        if (!victim || conflagrateCooldown > 0)
            return false;

        if (GetOwnDotRemaining(victim, DestructionWarlockSpells::IMMOLATE) <= 0)
            return false;

        uint32 const conflagrate = GetTalentRank(DestructionWarlockSpells::CONFLAGRATE);
        if (!conflagrate || !CanCast(victim, conflagrate, true))
            return false;

        if (!ExecuteSpell(victim, conflagrate, true))
            return false;

        conflagrateCooldown = CD_CONFLAGRATE;
        return true;
    }

    // -------------------------------------------------------------------------
    // 混乱之箭 (Chaos Bolt, 穿透爆击核弹)
    // -------------------------------------------------------------------------
    // 60 级纯天赋, 基础读条 2.5s。穿透一切免疫与减伤并必暴, 是毁灭术的核心底牌:
    // 撤离跑位途中严禁硬读, 否则会被任何一次走位当场作废并浪费一整个 GCD。
    // -------------------------------------------------------------------------
    bool TryChaosBolt(Unit* victim)
    {
        if (!victim || chaosBoltCooldown > 0 || isRetreating)
            return false;

        uint32 const chaosBolt = GetTalentRank(DestructionWarlockSpells::CHAOS_BOLT);
        if (!chaosBolt)
            return false;

        if (!TryCastSpell(victim, chaosBolt, false))
            return false;

        chaosBoltCooldown = GetChaosBoltCooldown();

        // Creature 无 SpellModOwner: 必须手工消费一层爆燃充能 (否则充能永久挂身)
        ConsumeBackdraft();
        return true;
    }

    // -------------------------------------------------------------------------
    // 暗影灼烧 (Shadowburn, 瞬发机动填充 / 斩杀补刀)
    // -------------------------------------------------------------------------
    // 仅用于两种场景: 撤离跑位途中无法站桩读条时的瞬发填充,
    // 以及目标进入 20% 斩杀线时的瞬发补刀。瞬发无弹道, 严禁 StopMoving。
    // -------------------------------------------------------------------------
    bool TryShadowburn(Unit* victim)
    {
        if (!victim || shadowburnCooldown > 0)
            return false;

        if (!isRetreating && victim->GetHealthPct() > SHADOWBURN_HP_PCT)
            return false;

        uint32 const shadowburn = GetAppropriateRank(DestructionWarlockSpells::SHADOWBURN, false);
        if (!shadowburn || !CanCast(victim, shadowburn, true))
            return false;

        if (!ExecuteSpell(victim, shadowburn, true))
            return false;

        shadowburnCooldown = CD_SHADOWBURN;
        return true;
    }

    // -------------------------------------------------------------------------
    // 烧尽 (Incinerate, 主力读条填充技)
    // -------------------------------------------------------------------------
    // 撤离途中严禁站桩读条: 走位与读条会互相打断形成原地抽搐。
    // 全等级兜底: 烧尽 Rank 1 于 64 级解锁, 低等级段若强行走该通道会取到不可用 ID
    // 而被底层拒放, 且彻底丧失填充技 —— 必须平滑回落【暗影箭】, 后者同时负责
    // 叠加【暗影与烈焰】5% 法术暴击易伤, 保证团队增益链路在练级段也不断档。
    // -------------------------------------------------------------------------
    bool TryIncinerate(Unit* victim)
    {
        if (!victim || isRetreating)
            return false;

        uint32 const filler = (me->GetLevel() >= LEVEL_INCINERATE)
            ? GetAppropriateRank(DestructionWarlockSpells::INCINERATE, false)
            : GetAppropriateRank(DestructionWarlockSpells::SHADOW_BOLT, false);

        if (!filler)
            return false;

        if (!TryCastSpell(victim, filler, false))
            return false;

        // Creature 无 SpellModOwner: 必须手工消费一层爆燃充能 (否则充能永久挂身)
        ConsumeBackdraft();
        return true;
    }

    // =========================================================================
    // P4: 站位控制 (盲区迟滞回正模型, 维持 15 ~ 35 码施法站位)
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
        // 使读条与站位反复互相打断, 表现为原地抽搐。
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
    // 毁灭天赋被动光环与雕文补偿 (弥补 NPC 缺天赋树缺陷, 铁律 33)
    // -------------------------------------------------------------------------
    // 必须注入 Rank 3/5 满阶 Spell ID。若注入 DBC 默认 Rank 1 根源,
    // 触发概率与数值会严重缩水 (如爆燃 Rank 1 触发率大幅低于满阶)。
    // 注: 爆燃属「触发型」被动, 必须注入天赋根源而非触发光环本身 (铁律 37),
    //     否则会退化为永久常驻或被短时光环自然脱落断层。
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
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::BACKDRAFT);          // 爆燃 Rank 3: 燃烧触发 3 层 30% 极速施法 (47260)
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::FIRE_AND_BRIMSTONE); // 硫磺与烈火 Rank 5: 献祭使烧尽伤害 +10%, 混乱之箭暴击 +25%
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::RUIN);               // 毁灭 Rank 5: 毁灭系法术暴击伤害 +100%
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::SHADOW_AND_FLAME);   // 暗影与烈焰 Rank 5: 法伤加成 +20% 并附 5% 法术暴击易伤
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::IMPROVED_IMMOLATE);  // 强化献祭 Rank 3: 献祭伤害 +30%
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::EMBERSTORM);         // 灰烬风暴 Rank 5: 火焰伤害 +10%, 烧尽读条缩短 10%
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::DESTRUCTIVE_REACH);  // 毁灭延伸 Rank 2: 毁灭法术射程 +20%, 仇恨 -10%
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::BANE);               // 灾祸 Rank 5: 暗影箭/混乱之箭/献祭读条缩短 0.5s
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::BACKLASH);           // 反冲 Rank 3: 暴击率 +3%
        SyncPassive(LEVEL_PASSIVE_TALENTS, DestructionWarlockSpells::DEMONIC_AEGIS);      // 恶魔庇护 Rank 3: 邪甲术效果提高 30%

        // ---- 雕文补偿 ----
        SyncPassive(LEVEL_GLYPH, DestructionWarlockSpells::GLYPH_OF_CONFLAGRATE); // 燃烧雕文: 燃烧不再吞噬献祭
        SyncPassive(LEVEL_GLYPH, DestructionWarlockSpells::GLYPH_OF_CHAOS_BOLT);  // 混乱之箭雕文: 混乱之箭冷却缩短 2s
        SyncPassive(LEVEL_GLYPH, DestructionWarlockSpells::GLYPH_OF_INCINERATE);  // 烧尽雕文: 烧尽伤害提高 5%
        SyncPassive(LEVEL_GLYPH, DestructionWarlockSpells::GLYPH_OF_LIFE_TAP);    // 生命分流雕文: 分流后提供 SP 增益 63321
    }
};

void AddSC_bot_destruction_warlock()
{
    new AdaptiveBotScript<BotDestructionWarlockAI>("bot_destruction_warlock");
}
