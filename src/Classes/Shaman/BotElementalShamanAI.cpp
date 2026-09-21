/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "ElementalShamanSpells.h"
#include "AdaptiveBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "Chat.h"
#include "Random.h"
#include <algorithm>
#include <cmath>
#include <vector>

class BotElementalShamanAI : public AdaptiveBotAI
{
public:
    explicit BotElementalShamanAI(Creature* creature) : AdaptiveBotAI(creature) {}

    // =========================================================================
    // 角色定位契约
    // -------------------------------------------------------------------------
    // 元素萨满为纯法系远程: IsRangedBot 必须为 true 且 IsRangedPhysicalBot 必须
    // 保持 false, 基类据此走「法系远程」法伤放大通道 (与猎人 AP 通道彻底解耦)。
    // 刻意不重写 GetDamageDealtMultiplier(): 基类已按指挥官装等自动适配
    // (1~79 级 1.0x~1.4x 自强过渡 / 80 级 2.0x~3.3x 团本专修),
    // 若在此硬编码 1.0f 会彻底阉割元素萨满的团本输出能力。
    // =========================================================================
    bool IsRangedBot() const override { return true; }
    bool IsRangedPhysicalBot() const override { return false; }

    // =========================================================================
    // 天赋依赖技能的最低等级契约
    // 注: 3.3.5a 中纯天赋技能 DBC SpellLevel 恒为 0, GetAppropriateRank 无法降阶,
    //     必须在此登记最低解锁等级并在施法前显式门禁 (HasTalent), 否则低等级会
    //     100% CanCast 失败并逐帧空转。
    //     基础法术 (闪电箭/闪电链/熔岩爆发/震击/风剪/图腾) 严禁登记于此。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case ElementalShamanSpells::ELEMENTAL_MASTERY: return 40;
            case ElementalShamanSpells::TOTEM_OF_WRATH:    return 50;
            case ElementalShamanSpells::THUNDERSTORM:      return 60;
            default:                                       return 0;
        }
    }

    // =========================================================================
    // 生命周期
    // =========================================================================
    void Reset() override
    {
        // 法力通道必须先于基类 Reset 完成配置, 保证等级同步走法力分支补满蓝池
        me->setPowerType(POWER_MANA);

        AdaptiveBotAI::Reset();

        buffSweepTimer = 0;
        ResetElementalTimers();
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
        UpdateElementalTimers(diff);

        bool const shieldSweepDue = TickSweep(buffSweepTimer, diff, BUFF_SWEEP_INTERVAL);

        // 全局读条/引导双保险守卫 (铁律 1): 闪电箭/熔岩爆发为读条法术, 引擎会置位
        // UNIT_STATE_CASTING; 引导通道在部分状态下不置位, 故追加
        // CURRENT_CHANNELED_SPELL 显式判定, 杜绝自身上层法术被跟随指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护 (水之护盾 / 图腾常驻 + 跟随)
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (TryEngageCombat())
                return;

            if (shieldSweepDue && MaintainWaterShield())
                return;

            if (MaintainTotems())
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
            // 战斗中断目标 (目标阵亡/切图): 仅维持自保与增益, 绝不空转走位
            if (shieldSweepDue && MaintainWaterShield())
                return;

            if (MaintainTotems())
                return;

            return;
        }

        // =====================================================================
        // 3. 施法姿态锚定
        // 物理第二参数必须为 false: 法系随从严禁跃入近战挥砍 (IsRangedBot 契约)
        // =====================================================================
        if (me->GetVictim() != victim)
            me->Attack(victim, false);

        // =====================================================================
        // 4. 常驻护盾与图腾矩阵维持 (占用 GCD, 需先于输出循环判定)
        // =====================================================================
        if (shieldSweepDue && MaintainWaterShield())
            return;

        if (MaintainTotems())
            return;

        // =====================================================================
        // 5. 核心 APL 决策流
        // =====================================================================
        PerformCombatAPL(victim);

        // =====================================================================
        // 6. 远程站桩与 APF 势场避险
        // 复用基类 ManageCasterCombat(): 其函数头部已内置铁律 21 要求的
        // IsUnderDangerThreat(2.0f) 第一优先级势场接管、射程与视线维持及朝向冻结,
        // 无需在专精内重复实现一套同构走位器。
        // =====================================================================
        ManageCasterCombat(victim, CAST_RANGE);
    }

private:
    // =========================================================================
    // 站位与射程参数 (铁律 12/21: 远程迟滞区间 + APF 势场避险)
    // =========================================================================
    // 站桩阈值收敛至 25 码 (ManageCasterCombat 内部落点为阈值 - 5 = 20 码):
    // 震击类法术射程明显短于闪电箭, 若按 30 码站位, 烈焰震击与熔岩爆发的
    // 必暴联动会因射程不足频繁断链, 因此站桩阈值必须由震击射程反向约束。
    static constexpr float CAST_RANGE                    = 25.0f;
    static constexpr float CHAIN_LIGHTNING_SPLASH_RANGE  = 12.0f;  // 闪电链多目标判定半径
    static constexpr float FIRE_TOTEM_AOE_RANGE          = 10.0f;  // 熔岩图腾 AoE 切换判定半径

    // =========================================================================
    // 巡检与图腾矩阵节流参数
    // =========================================================================
    static constexpr uint32 BUFF_SWEEP_INTERVAL   = 3000;    // 水之护盾巡检周期
    static constexpr uint32 TOTEM_CAST_INTERVAL   = 2000;    // 图腾插拔最小间隔
    static constexpr uint32 TOTEM_REFRESH_TIME    = 100000;  // 图腾矩阵重建周期
    static constexpr float  TOTEM_REPOSITION_DIST = 25.0f;   // 漂移超过此距离需重播图腾
    static constexpr uint8  TOTEM_COUNT           = 4;       // 大地 / 火焰 / 水 / 空气

    // =========================================================================
    // 自管冷却登记
    // Creature 不参与引擎技能 CD 追踪 (HasSpellCooldown 恒 false),
    // 凡无持续光环保护的 CD 技能必须由专精自行计时, 否则会逐帧空转重入。
    // =========================================================================
    static constexpr uint32 CD_LAVA_BURST        = 8000;
    static constexpr uint32 CD_CHAIN_LIGHTNING   = 4000;    // 含「风暴、大地与火焰」天赋减 CD
    static constexpr uint32 CD_SHOCK             = 5000;    // 含「混响」天赋 -1s
    static constexpr uint32 CD_WIND_SHEAR        = 6000;
    static constexpr uint32 CD_ELEMENTAL_MASTERY = 180000;
    static constexpr uint32 CD_THUNDERSTORM      = 45000;
    static constexpr uint32 CD_BLOODLUST         = 300000;

    static constexpr uint32 CASTER_GCD           = 1500;    // 法系公共冷却

    // =========================================================================
    // 危机与判定阈值
    // =========================================================================
    static constexpr uint32 FLAME_SHOCK_REFRESH_WINDOW   = 4000;  // 烈焰震击剩余时间窗口
    static constexpr uint8  WATER_SHIELD_REFRESH_CHARGES = 1;     // 水之护盾剩余充能阈值
    static constexpr float  EMERGENCY_MANA_PCT           = 35.0f; // 雷霆风暴回蓝触发线
    static constexpr float  LOW_MANA_PCT                 = 20.0f; // 残蓝停手填充线

    // 清晰预兆模拟补偿参数 (Creature 缺乏玩家暴击 Proc 回调)
    static constexpr uint32 CLEARCASTING_PROC_INTERVAL = 1500;
    static constexpr uint32 CLEARCASTING_PROC_CHANCE   = 20;

    // CreatureTemplate::rank: 1=精英 2=稀有精英 3=首领 4=稀有
    static constexpr uint32 CREATURE_RANK_ELITE = 1;

    // =========================================================================
    // 专精自管状态
    // =========================================================================
    uint32 buffSweepTimer{ 0 };

    uint32 lavaBurstCooldown{ 0 };
    uint32 chainLightningCooldown{ 0 };
    uint32 shockCooldown{ 0 };
    uint32 windShearCooldown{ 0 };

    uint32 elementalMasteryCooldown{ 0 };
    uint32 thunderstormCooldown{ 0 };
    uint32 bloodlustCooldown{ 0 };

    uint32 clearcastingProcCooldown{ 0 };

    // 图腾矩阵状态
    uint8  totemDeployIndex{ 0 };   // 0..TOTEM_COUNT-1 为展开游标, >=TOTEM_COUNT 表示已铺满
    bool   hasTotemPos{ false };
    float  lastTotemX{ 0.0f };
    float  lastTotemY{ 0.0f };
    float  lastTotemZ{ 0.0f };
    uint32 totemCastCooldown{ 0 };
    uint32 totemRefreshTimer{ 0 };

    // =========================================================================
    // 巡检计时器推进器
    // 返回 true 表示本轮周期已到并完成重置
    // =========================================================================
    static bool TickSweep(uint32& timer, uint32 diff, uint32 interval)
    {
        if (timer <= diff)
        {
            timer = interval;
            return true;
        }

        timer -= diff;
        return false;
    }

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateElementalTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(lavaBurstCooldown);
        Tick(chainLightningCooldown);
        Tick(shockCooldown);
        Tick(windShearCooldown);

        Tick(elementalMasteryCooldown);
        Tick(thunderstormCooldown);
        Tick(bloodlustCooldown);

        Tick(clearcastingProcCooldown);
        Tick(totemCastCooldown);
        Tick(totemRefreshTimer);
    }

    void ResetElementalTimers()
    {
        lavaBurstCooldown = 0;
        chainLightningCooldown = 0;
        shockCooldown = 0;
        windShearCooldown = 0;

        elementalMasteryCooldown = 0;
        thunderstormCooldown = 0;
        bloodlustCooldown = 0;

        clearcastingProcCooldown = 0;

        totemDeployIndex = 0;
        hasTotemPos = false;
        lastTotemX = 0.0f;
        lastTotemY = 0.0f;
        lastTotemZ = 0.0f;
        totemCastCooldown = 0;
        totemRefreshTimer = 0;
    }

    // =========================================================================
    // 状态判定器
    // =========================================================================
    // 天赋门禁: 登记在 GetTalentSpellMinLevel 的技能必须显式校验等级, 否则
    // GetAppropriateRank(..., true) 会无视等级返回最高 Rank ID, 造成「已习得」假象。
    bool HasTalent(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        return minLevel == 0 || me->GetLevel() >= minLevel;
    }

    bool IsBossOrEliteTarget(Unit* target) const
    {
        if (!target)
            return false;

        if (target->IsPlayer())
            return true;

        if (Creature* creature = target->ToCreature())
        {
            if (CreatureTemplate const* proto = creature->GetCreatureTemplate())
                return proto->rank >= CREATURE_RANK_ELITE;
        }

        return false;
    }

    bool IsManaStarvedForNuke() const
    {
        return me->getPowerType() == POWER_MANA && me->GetPowerPct(POWER_MANA) < LOW_MANA_PCT;
    }

    // 被贴身判定: 法系随从唯一的自救硬解是雷霆风暴击退,
    // 判定必须锚定「攻击者当前正以本人为目标」, 否则路过的小怪仇恨会误触发核弹级减益。
    bool IsUnderPhysicalMeleePressure() const
    {
        for (Unit* attacker : me->getAttackers())
        {
            if (attacker && attacker->IsAlive() && attacker->GetMap() == me->GetMap() &&
                attacker->GetVictim() == me && attacker->IsWithinMeleeRange(me))
                return true;
        }

        return false;
    }

    bool IsMultiTargetEncounter() const
    {
        return CountNearbyEnemies(CHAIN_LIGHTNING_SPLASH_RANGE) >= 2;
    }

    bool IsMultiTargetFireTotem() const
    {
        return CountNearbyEnemies(FIRE_TOTEM_AOE_RANGE) >= 2;
    }

    // =========================================================================
    // 周围可攻击敌人计数器 (跨来源去重采样: 自身 + 主人/队友 + 随从集群)
    // =========================================================================
    uint32 CountNearbyEnemies(float range) const
    {
        std::vector<Unit*> enemies;

        auto Consider = [&](Unit* candidate)
        {
            if (!candidate || !candidate->IsAlive() || !candidate->IsInWorld())
                return;
            if (candidate == me || candidate->GetMap() != me->GetMap())
                return;
            if (!me->IsWithinDist(candidate, range) || !me->IsValidAttackTarget(candidate))
                return;
            if (std::find(enemies.begin(), enemies.end(), candidate) != enemies.end())
                return;

            enemies.push_back(candidate);
        };

        for (Unit* attacker : me->getAttackers())
            Consider(attacker);

        if (Player* master = GetMaster())
        {
            for (Unit* attacker : master->getAttackers())
                Consider(attacker);

            if (Unit* masterPet = master->GetPet())
            {
                for (Unit* attacker : masterPet->getAttackers())
                    Consider(attacker);
            }

            if (Group* group = master->GetGroup())
            {
                for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* member = itr->GetSource();
                    if (!member || member == master)
                        continue;

                    for (Unit* attacker : member->getAttackers())
                        Consider(attacker);
                }
            }
        }

        return static_cast<uint32>(enemies.size());
    }

    // =========================================================================
    // 阵营自适应全团爆发口径 (部落嗜血 / 联盟英勇)
    // =========================================================================
    bool IsMasterHorde() const
    {
        Player* master = GetMaster();
        if (!master)
            return true; // 无指挥官时回落部落口径, 避免爆发通道永久死亡

        uint8 const race = master->getRace();
        return race == RACE_ORC || race == RACE_UNDEAD_PLAYER || race == RACE_TAUREN ||
               race == RACE_TROLL || race == RACE_BLOODELF;
    }

    uint32 GetBloodlustSpellId() const
    {
        return GetAppropriateRank(IsMasterHorde() ? ElementalShamanSpells::BLOODLUST
                                                  : ElementalShamanSpells::HEROISM, false);
    }

    // =========================================================================
    // 铁律 25: 清晰预兆 (元素集中节能施法) 旁路直放
    // -------------------------------------------------------------------------
    // NPC 随从没有玩家的 SpellMod 体系, 清晰预兆的「下 2 次法术免蓝」无法经底层
    // SpellMod 生效, 若沿用 CanCast 正常通道会因法力校验被拒放, 免费次数被白白吞掉。
    // 故持有该光环时改以 triggered 触发式通道施放主力耗能技, 旁路底层法力检验,
    // 随后手工原地扣减 1 层充能并自行置位公共冷却。
    // 致命坑位: 严禁使用 triggered = true (即 TRIGGERED_FULL_MASK), 其中包含
    // TRIGGERED_CAST_DIRECTLY 会把闪电箭/熔岩爆发读条强行降为瞬发, 彻底破坏读条语义,
    // 必须只挂 TRIGGERED_IGNORE_POWER_AND_REAGENT_COST 精确旁路蓝耗。
    // =========================================================================
    bool TryConsumeClearcasting(Unit* target, uint32 spellId)
    {
        if (!target || spellId == 0 || !me->HasAura(ElementalShamanSpells::AURA_CLEARCASTING))
            return false;

        if (!me->IsWithinLOSInMap(target))
            return false;

        // 致命坑位: TriggerCastFlags 未定义 operator|, 直接写 A | B | C 会被推导为 int,
        // 从而匹配到 CastSpell 的 bool triggered 重载 (等价 TRIGGERED_FULL_MASK,
        // 含 TRIGGERED_CAST_DIRECTLY 与 IGNORE_SPELL_AND_CATEGORY_CD), 会把读条强行
        // 降为瞬发并绕过全部冷却。必须显式构造枚举类型以选中正确的 TriggerCastFlags 重载。
        TriggerCastFlags const clearcastingFlags =
            TriggerCastFlags(TRIGGERED_IGNORE_POWER_AND_REAGENT_COST | TRIGGERED_IGNORE_GCD | TRIGGERED_DONT_REPORT_CAST_ERROR);

        SpellCastResult const result = me->CastSpell(target, spellId, clearcastingFlags);
        if (result != SPELL_CAST_OK)
            return false;

        // 充能型 Buff 扣减铁律 (铁律 10): 清晰预兆覆盖 2 次施法, 每次消费必须原地
        // 削减 1 层, 严禁整层 RemoveAurasDueToSpell 吞掉剩余免费次数。
        if (Aura* clearcasting = me->GetAura(ElementalShamanSpells::AURA_CLEARCASTING))
        {
            if (clearcasting->GetStackAmount() > 1)
                me->SetAuraStack(ElementalShamanSpells::AURA_CLEARCASTING, me, clearcasting->GetStackAmount() - 1);
            else
                me->RemoveAurasDueToSpell(ElementalShamanSpells::AURA_CLEARCASTING);
        }

        gcdTimer = CASTER_GCD; // 手工置位公共冷却 (triggered 通道绕过了引擎 GCD)
        return true;
    }

    // =========================================================================
    // 元素集中清晰预兆的随从模拟补偿
    // -------------------------------------------------------------------------
    // Creature 缺乏玩家暴击 Proc 回调, 注入的 ELEMENTAL_FOCUS 无法自然产出
    // 清晰预兆光环; 若不模拟, 铁律 25 的旁路通道将永久沦为死代码。
    // 以节流 + 概率方式在每次伤害法术落地后手工补层。
    // =========================================================================
    void TryRollClearcasting()
    {
        if (!me->HasAura(ElementalShamanSpells::ELEMENTAL_FOCUS))
            return;

        if (clearcastingProcCooldown > 0)
            return;

        clearcastingProcCooldown = CLEARCASTING_PROC_INTERVAL;

        if (urand(1, 100) > CLEARCASTING_PROC_CHANCE)
            return;

        if (!me->HasAura(ElementalShamanSpells::AURA_CLEARCASTING))
            me->AddAura(ElementalShamanSpells::AURA_CLEARCASTING, me);
    }

    // =========================================================================
    // APL 主决策流
    // =========================================================================
    void PerformCombatAPL(Unit* victim)
    {
        // ---- P0: 打断与极限自保 (Off-GCD 顺下, 严禁 return 中断当帧决策流) ----
        TryEmergencySurvival(victim);

        // ---- P1: 爆发大招 ----
        if (TryBurstCooldowns(victim))
            return;

        // ---- P2: 核心输出优先级 ----
        PerformDamageRotation(victim);
    }

    // =========================================================================
    // P0: 打断与极限自保
    // =========================================================================
    void TryEmergencySurvival(Unit* victim)
    {
        // ---- 风剪: 阶段三记忆化压秒打断 (Off-GCD, 顺下不阻断输出) ----
        TryWindShear(victim);

        // ---- 雷霆风暴: 残蓝回蓝 + 被贴身击退自救 (占 GCD, 成功后当帧收束) ----
        TryThunderstorm();
    }

    bool TryWindShear(Unit* victim)
    {
        if (!victim || windShearCooldown > 0)
            return false;

        uint32 const windShear = GetAppropriateRank(ElementalShamanSpells::WIND_SHEAR, false);
        if (!windShear)
            return false;

        // 接入阶段三基类记忆化压秒打断仲裁引擎 (自动覆盖普通读条与长引导两类通道,
        // 并按 learnedInterruptDelays 压秒; Off-GCD, 严禁 return 阻断当帧决策流)
        if (TryInterrupt(victim, windShear))
        {
            windShearCooldown = CD_WIND_SHEAR;
            return true;
        }

        return false;
    }

    bool TryThunderstorm()
    {
        if (thunderstormCooldown > 0 || !HasTalent(ElementalShamanSpells::THUNDERSTORM))
            return false;

        bool const lowMana = (me->getPowerType() == POWER_MANA &&
                              me->GetPowerPct(POWER_MANA) < EMERGENCY_MANA_PCT);
        bool const underMelee = IsUnderPhysicalMeleePressure();

        // 雷霆风暴以自身为原点结算击退与回蓝, 目标必须传 me;
        // 低优先级场景 (满蓝且未被贴身) 严禁空放, 否则只会把怪推出队友的 AoE。
        if (!lowMana && !underMelee)
            return false;

        uint32 const thunderstorm = GetAppropriateRank(ElementalShamanSpells::THUNDERSTORM, true);
        if (!thunderstorm || !CanCast(me, thunderstorm, true))
            return false;

        if (ExecuteSpell(me, thunderstorm, true))
        {
            thunderstormCooldown = CD_THUNDERSTORM;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P1: 爆发大招
    // =========================================================================
    bool TryBurstCooldowns(Unit* victim)
    {
        if (!victim || !IsBossOrEliteTarget(victim))
            return false;

        // ---- 元素掌握: Off-GCD 瞬发增益, 施放后严禁 return (铁律 8),
        //      必须让当帧决策流继续顺下, 使急速增益当帧即被后续熔岩爆发/闪电箭吃下。----
        if (elementalMasteryCooldown == 0 &&
            HasTalent(ElementalShamanSpells::ELEMENTAL_MASTERY) &&
            !me->HasAura(ElementalShamanSpells::ELEMENTAL_MASTERY))
        {
            if (CanCast(me, ElementalShamanSpells::ELEMENTAL_MASTERY, true) &&
                ExecuteSpell(me, ElementalShamanSpells::ELEMENTAL_MASTERY, true))
            {
                elementalMasteryCooldown = CD_ELEMENTAL_MASTERY;
            }
        }

        return TryBloodlust(victim);
    }

    bool TryBloodlust(Unit* victim)
    {
        if (!victim || bloodlustCooldown > 0)
            return false;

        float const hpPct = victim->GetHealthPct();
        bool const opening = (hpPct > 90.0f);
        bool const execute = (hpPct <= 20.0f);
        if (!opening && !execute)
            return false;

        uint32 const bloodlustSpellId = GetBloodlustSpellId();
        if (!bloodlustSpellId || me->HasAura(bloodlustSpellId))
            return false;

        if (!CanCast(me, bloodlustSpellId, true))
            return false;

        if (ExecuteSpell(me, bloodlustSpellId, true))
        {
            bloodlustCooldown = CD_BLOODLUST;
            return true;
        }

        return false;
    }

    // =========================================================================
    // P2: 核心输出优先级 (FCFS)
    // =========================================================================
    void PerformDamageRotation(Unit* victim)
    {
        if (!victim)
            return;

        // ---- 1. 烈焰震击维持: 熔岩爆发必暴链的前置 DoT, 优先级最高 ----
        if (PerformFlameShock(victim))
            return;

        // ---- 2. 熔岩爆发: 烈焰震击存续期必暴, 单体最高 DPET ----
        if (PerformLavaBurst(victim))
            return;

        // ---- 3. 闪电链: 仅多目标溅射成立时参与优先级 ----
        if (PerformChainLightning(victim))
            return;

        // ---- 4. 震击填充: 烈焰震击无需续期且熔岩爆发在冷却时的空窗补偿 ----
        if (PerformShockFiller(victim))
            return;

        // ---- 5. 闪电箭: 核心读条填充 ----
        PerformLightningBolt(victim);
    }

    // =========================================================================
    // 烈焰震击 (共享震击冷却)
    // =========================================================================
    bool PerformFlameShock(Unit* victim)
    {
        if (!victim || shockCooldown > 0)
            return false;

        // 仅统计本专精自己施加的烈焰震击, 避免误判其他萨满随从的 DoT 而放弃维持
        Aura* flameShock = victim->GetAuraOfRankedSpell(ElementalShamanSpells::FLAME_SHOCK, me->GetGUID());
        bool const needRefresh = (!flameShock ||
                                  flameShock->GetDuration() <= static_cast<int32>(FLAME_SHOCK_REFRESH_WINDOW));
        if (!needRefresh)
            return false;

        uint32 const flameShockSpell = GetAppropriateRank(ElementalShamanSpells::FLAME_SHOCK, false);
        if (!flameShockSpell || !CanCast(victim, flameShockSpell, true))
            return false;

        if (ExecuteSpell(victim, flameShockSpell, true))
        {
            shockCooldown = CD_SHOCK;
            TryRollClearcasting();
            return true;
        }

        return false;
    }

    // =========================================================================
    // 熔岩爆发 (烈焰震击存续期必暴)
    // =========================================================================
    bool PerformLavaBurst(Unit* victim)
    {
        if (!victim || lavaBurstCooldown > 0)
            return false;

        // 目标身上缺失本随从的烈焰震击时严禁读条: 否则只是一发普通火焰伤害,
        // 既丢掉了 100% 暴击的核心收益, 又白烧 8 秒 CD。
        if (!victim->GetAuraOfRankedSpell(ElementalShamanSpells::FLAME_SHOCK, me->GetGUID()))
            return false;

        uint32 const lavaBurst = GetAppropriateRank(ElementalShamanSpells::LAVA_BURST, false);
        if (!lavaBurst)
            return false;

        if (TryConsumeClearcasting(victim, lavaBurst))
            return true;

        if (!CanCast(victim, lavaBurst, true))
            return false;

        if (ExecuteSpell(victim, lavaBurst, true))
        {
            lavaBurstCooldown = CD_LAVA_BURST;
            TryRollClearcasting();
            return true;
        }

        return false;
    }

    // =========================================================================
    // 闪电链 (多目标溅射)
    // =========================================================================
    bool PerformChainLightning(Unit* victim)
    {
        if (!victim || chainLightningCooldown > 0)
            return false;

        // 单体目标时闪电链 DPCT 低于闪电箭, 且会白占自家冷却与一个 GCD
        if (!IsMultiTargetEncounter())
            return false;

        uint32 const chainLightning = GetAppropriateRank(ElementalShamanSpells::CHAIN_LIGHTNING, false);
        if (!chainLightning)
            return false;

        // 清晰预兆旁路必须先于残蓝节流判定: 免费次数正是残蓝期唯一的输出通道
        if (TryConsumeClearcasting(victim, chainLightning))
            return true;

        if (IsManaStarvedForNuke())
            return false;

        if (!CanCast(victim, chainLightning, true))
            return false;

        if (ExecuteSpell(victim, chainLightning, true))
        {
            chainLightningCooldown = CD_CHAIN_LIGHTNING;
            TryRollClearcasting();
            return true;
        }

        return false;
    }

    // =========================================================================
    // 震击填充 (与烈焰震击共享冷却)
    // -------------------------------------------------------------------------
    // 跑动中优先冰霜震击: 瞬发可跑动施放且附带减速, 顺带拉开贴身近战;
    // 站桩时改用大地震击 (自然系伤害, 享受风暴打击以外的法伤加成口径)。
    // =========================================================================
    bool PerformShockFiller(Unit* victim)
    {
        if (!victim || shockCooldown > 0)
            return false;

        bool const moving = me->isMoving();
        uint32 const shock = GetAppropriateRank(moving ? ElementalShamanSpells::FROST_SHOCK
                                                       : ElementalShamanSpells::EARTH_SHOCK, false);
        if (!shock || !CanCast(victim, shock, true))
            return false;

        if (ExecuteSpell(victim, shock, true))
        {
            shockCooldown = CD_SHOCK;
            TryRollClearcasting();
            return true;
        }

        return false;
    }

    // =========================================================================
    // 闪电箭 (核心读条填充)
    // =========================================================================
    bool PerformLightningBolt(Unit* victim)
    {
        if (!victim)
            return false;

        uint32 const lightningBolt = GetAppropriateRank(ElementalShamanSpells::LIGHTNING_BOLT, false);
        if (!lightningBolt)
            return false;

        if (TryConsumeClearcasting(victim, lightningBolt))
            return true;

        // 法力节流: 残蓝期停手填充, 让水之护盾与法力之泉把蓝线拉回安全区,
        // 避免把最后一个 GCD 烧在填充技上导致高压期彻底空蓝。
        if (IsManaStarvedForNuke())
            return false;

        if (!CanCast(victim, lightningBolt, true))
            return false;

        if (ExecuteSpell(victim, lightningBolt, true))
        {
            TryRollClearcasting();
            return true;
        }

        return false;
    }

    // =========================================================================
    // 水之护盾常驻维持 (自身回蓝核心)
    // -------------------------------------------------------------------------
    // 致命坑位: 水之护盾底层为 ProcCharges 充能型光环, 不使用 StackAmount,
    // Aura::GetStackAmount() 恒返回 1, 用其判层会使门禁永久失效并导致每轮巡检
    // 无脑刷盾空烧 GCD。必须改走 Aura::GetCharges() 真实反映剩余次数。
    // =========================================================================
    bool MaintainWaterShield()
    {
        uint32 const waterShield = GetAppropriateRank(ElementalShamanSpells::WATER_SHIELD, false);
        if (!waterShield)
            return false;

        if (Aura* aura = me->GetAura(waterShield))
        {
            if (aura->GetCharges() > WATER_SHIELD_REFRESH_CHARGES)
                return false;
        }

        if (!CanCast(me, waterShield, true))
            return false;

        return ExecuteSpell(me, waterShield, true);
    }

    // =========================================================================
    // 战斗图腾矩阵 (大地 / 火焰 / 水 / 空气)
    // =========================================================================
    uint32 GetTotemSpellForIndex(uint8 index)
    {
        switch (index)
        {
            case 0:
                // 大地图腾: 队伍存在坦克/近战锚点时投放大地之力图腾 (力量敏捷增益),
                // 纯法系编队时改投石肤图腾 (护甲), 避免增益在无受益者时空转。
                return GetAppropriateRank(GetGroupTank() ? ElementalShamanSpells::STRENGTH_OF_EARTH_TOTEM
                                                         : ElementalShamanSpells::STONESKIN_TOTEM, false);
            case 1:
                // 火焰图腾: 习得天怒图腾 (50级天赋) 时恒定优先——全团法强与法暴增益
                // 对法系编队的收益远超图腾本体 DPS; 未习得时按多目标切换熔岩/灼热。
                if (HasTalent(ElementalShamanSpells::TOTEM_OF_WRATH))
                {
                    uint32 const totemOfWrath = GetAppropriateRank(ElementalShamanSpells::TOTEM_OF_WRATH, true);
                    if (totemOfWrath)
                        return totemOfWrath;
                }

                return GetAppropriateRank(IsMultiTargetFireTotem() ? ElementalShamanSpells::MAGMA_TOTEM
                                                                    : ElementalShamanSpells::SEARING_TOTEM, false);
            case 2:
                return GetAppropriateRank(ElementalShamanSpells::MANA_SPRING_TOTEM, false);
            case 3:
                return GetAppropriateRank(ElementalShamanSpells::WRATH_OF_AIR_TOTEM, false);
            default:
                return 0;
        }
    }

    bool MaintainTotems()
    {
        // ---------------------------------------------------------------------
        // 图腾「消失」以自管刷新计时为代理信号: 图腾 NPC 的存活无法通过可靠且
        // 稳定的引擎 API 逐槽判定, 改用「重建周期 + 位置漂移」双触发,
        // 既覆盖图腾到期自然消灭, 也覆盖跑位脱节, 且绝不会产生每帧重播死循环。
        // ---------------------------------------------------------------------
        bool const deployIncomplete = (totemDeployIndex > 0 && totemDeployIndex < TOTEM_COUNT);
        bool const drifted = hasTotemPos
                          && me->GetDistance(lastTotemX, lastTotemY, lastTotemZ) > TOTEM_REPOSITION_DIST;

        bool const needRedeploy = deployIncomplete
                               || !hasTotemPos
                               || (totemRefreshTimer == 0)
                               || drifted;

        if (!needRedeploy)
            return false;

        // 已完成矩阵但被位移/超时触发重播: 重置展开游标, 从头顺次补齐
        if (hasTotemPos && totemDeployIndex >= TOTEM_COUNT)
            totemDeployIndex = 0;

        // 平滑展开节流: 间隔不足一律不放行, 严禁单帧连续打出多个图腾
        if (totemCastCooldown > 0)
            return false;

        // 矩阵铺满: 落定坐标快照与重建周期
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

        // 等级不足该图腾: 推进游标避免死循环
        if (!totemSpellId)
        {
            ++totemDeployIndex;
            return false;
        }

        bool const casted = CanCast(me, totemSpellId, true) && ExecuteSpell(me, totemSpellId, true);

        // 无论成败均推进游标并压上节流: 施法失败 (缺法力/被控) 时若原地重试,
        // 会让随从在每一帧反复空转同一个图腾, 彻底饿死输出通道。
        ++totemDeployIndex;
        totemCastCooldown = TOTEM_CAST_INTERVAL;

        return casted;
    }

    // =========================================================================
    // 元素天赋被动光环与雕文补偿 (弥补 NPC 缺天赋树缺陷, 铁律 17)
    // -------------------------------------------------------------------------
    // 必须注入满阶 Spell ID。若注入 DBC 默认 Rank 1 根源, 触发概率与数值会
    // 严重缩水 (闪电掌握 Rank 1 仅降低极少读条时间)。
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
        SyncPassive(10, ElementalShamanSpells::CONCUSSION);            // 震荡 Rank 5
        SyncPassive(10, ElementalShamanSpells::CALL_OF_FLAME);         // 烈焰召唤 Rank 3
        SyncPassive(10, ElementalShamanSpells::CALL_OF_THUNDER);       // 雷霆召唤 Rank 1
        SyncPassive(10, ElementalShamanSpells::ELEMENTAL_FOCUS);       // 元素集中
        SyncPassive(20, ElementalShamanSpells::REVERBERATION);         // 混响 Rank 5
        SyncPassive(20, ElementalShamanSpells::UNRELENTING_STORM);     // 无情风暴 Rank 3
        SyncPassive(20, ElementalShamanSpells::ELEMENTAL_PRECISION);   // 元素精准 Rank 3
        SyncPassive(20, ElementalShamanSpells::LIGHTNING_MASTERY);     // 闪电掌握 Rank 5
        SyncPassive(40, ElementalShamanSpells::ELEMENTAL_OATH);        // 元素誓约 Rank 2
        SyncPassive(40, ElementalShamanSpells::LIGHTNING_OVERLOAD);    // 闪电过载 Rank 3
        SyncPassive(40, ElementalShamanSpells::LAVA_FLOWS);            // 熔岩流动 Rank 3
        SyncPassive(45, ElementalShamanSpells::STORM_EARTH_AND_FIRE);  // 风暴、大地与火焰 Rank 3
        SyncPassive(50, ElementalShamanSpells::SHAMANISM);             // 萨满教义 Rank 5

        // ---- 雕文补偿 ----
        SyncPassive(20, ElementalShamanSpells::GLYPH_OF_LAVA);
        SyncPassive(20, ElementalShamanSpells::GLYPH_OF_LIGHTNING_BOLT);
        SyncPassive(20, ElementalShamanSpells::GLYPH_OF_FLAME_SHOCK);
        SyncPassive(50, ElementalShamanSpells::GLYPH_OF_TOTEM_OF_WRATH);
    }
};

void AddSC_bot_elemental_shaman()
{
    new AdaptiveBotScript<BotElementalShamanAI>("bot_elemental_shaman");
}
