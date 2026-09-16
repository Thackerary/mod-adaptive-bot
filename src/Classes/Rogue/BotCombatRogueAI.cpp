/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "CombatRogueSpells.h"
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

class BotCombatRogueAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位参数 (物理近战背后找背模型)
    // =========================================================================
    static constexpr float MELEE_REACH_DIST    = 4.0f;   // 近战判定区上限：超出必须重新贴背
    static constexpr float MELEE_COMFORT_DIST  = 3.0f;   // 贴身阈值：进入后保持平滑贴背输出
    static constexpr float MELEE_FOLLOW_DIST   = 1.5f;   // 理想站位：目标正后方 1.5 码
    static constexpr float KILLING_SPREE_DIST  = 4.0f;   // 杀戮盛宴交出所需的最大贴身距离

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
    static constexpr uint32 CD_BLADE_FLURRY        = 120000;
    static constexpr uint32 CD_ADRENALINE_RUSH     = 180000;
    static constexpr uint32 CD_KILLING_SPREE       = 120000; // 未插雕文基准冷却
    static constexpr uint32 CD_KILLING_SPREE_GLYPH = 75000;  // 杀戮盛宴雕文：缩短 45 秒
    static constexpr uint32 CD_TRICKS              = 30000;
    static constexpr uint32 CD_VANISH              = 180000;
    static constexpr uint32 CD_EVASION             = 180000;
    static constexpr uint32 CD_CLOAK               = 60000;

    // 生命 / 能量门禁
    static constexpr float  EVASION_HP_PCT          = 30.0f; // 濒死闪避
    static constexpr float  VANISH_HP_PCT           = 20.0f; // 极度濒危消失
    static constexpr uint8  FINISHER_MIN_CP         = 4;     // 终结技最低连击点
    static constexpr uint32 FINISHER_ENERGY         = 35;    // 终结技支付门槛 (不足则原地挂起等能量)
    static constexpr uint32 SINISTER_STRIKE_ENERGY  = 40;    // 邪恶攻击耗能
    static constexpr uint32 AR_MAX_ENERGY           = 40;    // 冲动开启能量上限 (防溢能)
    static constexpr uint32 KS_MAX_ENERGY           = 35;    // 杀戮盛宴开启能量上限 (防溢能)
    static constexpr int32  SND_REFRESH_MS          = 2000;  // 切割提前续订窗口
    static constexpr int32  RUPTURE_REFRESH_MS      = 2000;  // 割裂提前续订窗口

    // =========================================================================
    // 毒药模拟参数
    // -------------------------------------------------------------------------
    // 随从无武器涂毒对象，且底层毒药依赖 ProcFlag 武器命中事件，
    // 必须由专精以 triggered 方式手工注入，否则毒药伤害与野蛮战斗增幅永久缺失。
    // =========================================================================
    static constexpr uint32 POISON_PROC_INTERVAL     = 1000; // 毒药巡检周期 (等效武器毒药 PPM 节流)
    static constexpr uint32 DEADLY_POISON_MAX_STACKS = 5;    // 致命毒药满层数
    static constexpr uint32 DEADLY_POISON_REFRESH_MS = 3000; // 致命毒药提前续期窗口
    static constexpr uint32 INSTANT_POISON_CHANCE    = 50;   // 速效毒药触发概率 (%)

public:
    explicit BotCombatRogueAI(Creature* creature) : AdaptiveBotAI(creature) {}

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
    // 天赋依赖技能的最低等级契约
    // 注：基础法术 (潜行/邪恶攻击/切割/刺骨/割裂/消失/闪避/斗篷/毒药) 严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case CombatRogueSpells::BLADE_FLURRY:    return 30;
            case CombatRogueSpells::ADRENALINE_RUSH: return 40;
            case CombatRogueSpells::KILLING_SPREE:   return 60;
            default:                                 return 0;
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
        ResetCombatTimers();
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
        UpdateCombatTimers(diff);

        // ---- 杀戮盛宴飞斩无敌期：严禁下发任何打击与读条指令 ----
        // 飞斩期间随从处于引擎托管位移 + 全技能锁定状态，任何主动指令都会被底层拒绝，
        // 且会污染 gcdTimer 与运动发生器，必须整体交还控制权直接 return。
        if (me->HasAura(CombatRogueSpells::AURA_KILLING_SPREE))
            return;

        // 全局读条/通道双保险守卫：引导类法术在部分状态下并不置位 UNIT_STATE_CASTING，
        // 故追加 CURRENT_CHANNELED_SPELL 显式判定，杜绝被跟随/走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护 (潜行姿态 + 开怪 + 跟随)
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainStealth())
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

        // 物理近战开怪：第二参数必须传 true 开启近战平砍与双持白字输出
        // (与远程专精的 me->Attack(victim, false) 严格区分)。
        if (me->GetVictim() != victim)
            me->Attack(victim, true);

        // ---- P0: 濒死自保与解控 (维度 C 协同) ----
        if (TrySurvivalAndUtility(victim))
            return;

        // ---- P1: 嫁祸诀窍 (仇恨转移 + 增伤协同) ----
        if (TryTricksOfTheTrade())
            return;

        // ---- P2: 爆发大招时序 (Off-GCD，严禁 return true，必须当帧顺下) ----
        TryBurstCooldowns(victim);

        // ---- P3: 核心打击与终结技优先级 ----
        if (TryCoreRotation(victim))
            return;

        // ---- P4: 背后找背站位 ----
        MaintainMeleeBehindPositioning(victim);

        // ---- 白色平砍驱动 ----
        // ScriptedAI::UpdateAI 已被本类完整接管，引擎不会自动驱动平砍，
        // 必须在决策流末帧显式调用，否则双持白字与毒药伤害永久缺失。
        DoMeleeAttackIfReady();

        // 白字平砍结算后立即补一次毒药模拟：维持 5 层致命毒药 + 速效毒药，
        // 打通「野蛮战斗中毒目标物理增伤」闭环。
        ProcPoisons(victim);
    }

private:
    // 自管冷却登记
    uint32 bladeFlurryCooldown{ 0 };
    uint32 adrenalineRushCooldown{ 0 };
    uint32 killingSpreeCooldown{ 0 };
    uint32 tricksCooldown{ 0 };
    uint32 vanishCooldown{ 0 };
    uint32 evasionCooldown{ 0 };
    uint32 cloakCooldown{ 0 };

    // 毒药模拟巡检节流计时器
    uint32 poisonProcTimer{ 0 };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateCombatTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(bladeFlurryCooldown);
        Tick(adrenalineRushCooldown);
        Tick(killingSpreeCooldown);
        Tick(tricksCooldown);
        Tick(vanishCooldown);
        Tick(evasionCooldown);
        Tick(cloakCooldown);
        Tick(poisonProcTimer);
    }

    void ResetCombatTimers()
    {
        bladeFlurryCooldown = 0;
        adrenalineRushCooldown = 0;
        killingSpreeCooldown = 0;
        tricksCooldown = 0;
        vanishCooldown = 0;
        evasionCooldown = 0;
        cloakCooldown = 0;

        poisonProcTimer = 0;
    }

    // =========================================================================
    // 状态判定器
    // =========================================================================
    bool IsStealthed() const
    {
        // 潜行为分阶法术，消失 (Vanish) 亦会施加潜行光环：
        // 同时按法术 ID 与光环类型双通道判定，杜绝漏检导致脱战姿态失效。
        return me->HasAura(CombatRogueSpells::STEALTH) || me->HasAuraType(SPELL_AURA_MOD_STEALTH);
    }

    // 切割为分阶法术，必须按「分阶光环」查询，直接 HasAura(6774) 会漏检低阶光环
    bool NeedSliceAndDice() const
    {
        Aura* aura = me->GetAuraOfRankedSpell(CombatRogueSpells::SLICE_AND_DICE);
        if (!aura)
            return true;

        // GetDuration() < 0 表示永久光环，无需续订
        return aura->GetDuration() >= 0 && aura->GetDuration() <= SND_REFRESH_MS;
    }

    bool HasRupture(Unit* victim) const
    {
        if (!victim)
            return false;

        Aura* aura = victim->GetAuraOfRankedSpell(CombatRogueSpells::RUPTURE, me->GetGUID());
        if (!aura)
            return false;

        // 永久光环或剩余充足则视为未断档
        return aura->GetDuration() < 0 || aura->GetDuration() > RUPTURE_REFRESH_MS;
    }

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

    // 群体敌情雷达：统计当前交战中、贴身范围内的敌对单位数量 (剑刃乱舞 AoE 门禁输入)
    uint32 CountNearbyHostileEnemies(float range = 10.0f) const
    {
        std::vector<Unit*> hostiles;

        auto Consider = [this, range, &hostiles](Unit* unit)
        {
            if (!unit || unit == me || !unit->IsAlive() || !unit->IsInWorld())
                return;

            if (unit->GetMap() != me->GetMap() || !unit->IsHostileTo(me))
                return;

            if (!me->IsWithinDist(unit, range) || !me->IsWithinLOSInMap(unit))
                return;

            if (std::find(hostiles.begin(), hostiles.end(), unit) == hostiles.end())
                hostiles.push_back(unit);
        };

        auto ConsiderAttackersOf = [&Consider](Unit* unit)
        {
            if (!unit)
                return;

            for (Unit* attacker : unit->getAttackers())
                Consider(attacker);
        };

        ConsiderAttackersOf(me);

        Player* master = GetMaster();
        if (!master)
            return static_cast<uint32>(hostiles.size());

        ConsiderAttackersOf(master);
        ConsiderAttackersOf(master->GetPet());

        if (Group* group = master->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                if (Player* member = itr->GetSource())
                {
                    if (member->GetMap() == me->GetMap())
                    {
                        ConsiderAttackersOf(member);
                        ConsiderAttackersOf(member->GetPet());
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(s_botRegistryMutex);
            auto it = s_masterBotRegistry.find(master->GetGUID());
            if (it != s_masterBotRegistry.end())
            {
                for (AdaptiveBotAI* allyBot : it->second)
                {
                    if (allyBot && allyBot->me)
                        ConsiderAttackersOf(allyBot->me);
                }
            }
        }

        return static_cast<uint32>(hostiles.size());
    }

    // 解控/驱散需求：身上存在可驱散的魔法负面 或 减速/定身类移动妨碍负面
    bool NeedsCloakOfShadows() const
    {
        uint32 const magicMask = (uint32(1) << DISPEL_MAGIC);

        for (auto const& pair : me->GetAppliedAuras())
        {
            AuraApplication* app = pair.second;
            if (!app || app->IsPositive())
                continue;

            Aura* aura = app->GetBase();
            if (!aura)
                continue;

            SpellInfo const* spellInfo = aura->GetSpellInfo();
            if (spellInfo && (spellInfo->GetDispelMask() & magicMask))
                return true;
        }

        return me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) || me->HasAuraType(SPELL_AURA_MOD_ROOT);
    }

    // 判断随从是否已占住目标背身位：目标正面 180° 锥形之外即视为背后。
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
    // 脱战潜行维持
    // =========================================================================
    bool MaintainStealth()
    {
        if (IsStealthed() || me->IsInCombat())
            return false;

        // 潜行严禁在战斗中施放 (底层会直接拒绝)，本函数仅由脱战分支调用
        uint32 const stealth = GetAppropriateRank(CombatRogueSpells::STEALTH, false);
        if (!stealth || !CanCast(me, stealth, true))
            return false;

        return ExecuteSpell(me, stealth, true);
    }

    // =========================================================================
    // P0: 濒死自保与解控
    // =========================================================================
    bool TrySurvivalAndUtility(Unit* /*victim*/)
    {
        // ---- 闪避：濒死且被近战压制时开启 50% 闪避硬减伤 ----
        if (evasionCooldown == 0 && me->GetHealthPct() < EVASION_HP_PCT && IsUnderPhysicalMelee(me))
        {
            uint32 const evasion = GetAppropriateRank(CombatRogueSpells::EVASION, false);
            if (evasion && !me->HasAura(evasion) && CanCast(me, evasion, true) && ExecuteSpell(me, evasion, true))
            {
                evasionCooldown = CD_EVASION;
                return true;
            }
        }

        // ---- 暗影斗篷：魔法负面或减速/定身压制时开启，兼顾免伤与解控 ----
        if (cloakCooldown == 0 && NeedsCloakOfShadows())
        {
            uint32 const cloak = GetAppropriateRank(CombatRogueSpells::CLOAK_OF_SHADOWS, false);
            if (cloak && !me->HasAura(cloak) && CanCast(me, cloak, true) && ExecuteSpell(me, cloak, true))
            {
                cloakCooldown = CD_CLOAK;

                // Off-GCD 铁律：暗影斗篷独立于公共冷却，施放成功严禁 return true，
                // 否则会白吞当帧的邪恶攻击与终结技窗口 (自保技挤占输出帧)。
            }
        }

        // ---- 消失：极度濒危且被围攻，或仇恨彻底失控 (OT) ----
        // 消失强行进潜清仇恨，是战斗贼最高优先级的脱困底牌。
        if (vanishCooldown == 0 && me->GetHealthPct() < VANISH_HP_PCT &&
            (CountMeleeAttackers() >= 2 || IsTopThreatTarget()))
        {
            uint32 const vanish = GetAppropriateRank(CombatRogueSpells::VANISH, false);
            if (vanish && !me->HasAura(vanish) && CanCast(me, vanish, true) && ExecuteSpell(me, vanish, true))
            {
                vanishCooldown = CD_VANISH;
                return true;
            }
        }

        return false;
    }

    // =========================================================================
    // P1: 嫁祸诀窍 (仇恨转移 + 增伤)
    // =========================================================================
    bool TryTricksOfTheTrade()
    {
        if (tricksCooldown > 0)
            return false;

        uint32 const tricks = GetAppropriateRank(CombatRogueSpells::TRICKS_OF_THE_TRADE, false);
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
    // P2: 爆发大招时序与防溢能仲裁 (Off-GCD，当帧顺下绝不 return)
    // =========================================================================
    void TryBurstCooldowns(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return;

        bool const eliteOrBoss = IsEliteOrBossTarget(victim);
        bool const inMelee = (me->GetDistance(victim) <= MELEE_REACH_DIST);

        // ---- 冲动：BOSS/精英战近战位且能量见底时开启，15 秒内回能速度翻倍 ----
        // 能量门禁必须卡在上限以下才交，满能量时开启会把双倍回能全部浪费在溢出上。
        if (adrenalineRushCooldown == 0 && eliteOrBoss && inMelee &&
            me->GetPower(POWER_ENERGY) <= AR_MAX_ENERGY &&
            !me->HasAura(CombatRogueSpells::AURA_ADRENALINE_RUSH))
        {
            uint32 const adrenalineRush = GetAppropriateRank(CombatRogueSpells::ADRENALINE_RUSH, true);
            if (adrenalineRush && CanCast(me, adrenalineRush, true) && ExecuteSpell(me, adrenalineRush, true))
                adrenalineRushCooldown = CD_ADRENALINE_RUSH;

            // Off-GCD 铁律：冲动为纯增益姿态，严禁 return，当帧顺下允许剑刃乱舞叠加极速爆发。
        }

        // ---- 剑刃乱舞：多目标泼溅输出，或首领战冲动期间叠加攻速爆发 ----
        if (bladeFlurryCooldown == 0 && !me->HasAura(CombatRogueSpells::AURA_BLADE_FLURRY))
        {
            bool const multiTarget = (CountNearbyHostileEnemies(10.0f) >= 2);
            bool const burstWindow = eliteOrBoss && me->HasAura(CombatRogueSpells::AURA_ADRENALINE_RUSH);

            if (multiTarget || burstWindow)
            {
                uint32 const bladeFlurry = GetAppropriateRank(CombatRogueSpells::BLADE_FLURRY, true);
                if (bladeFlurry && CanCast(me, bladeFlurry, true) && ExecuteSpell(me, bladeFlurry, true))
                    bladeFlurryCooldown = CD_BLADE_FLURRY;
            }
        }

        // ---- 杀戮盛宴：飞斩无敌期，开启门禁极其严苛 ----
        // 1) 必须已进入近战攻击与贴身 4 码；
        // 2) 能量 <= 35：飞斩期间随从无法主动施法，若满能量交出会把整段回能全部浪费溢出；
        // 3) 严禁与冲动光环并存：两者回能叠加会瞬间击穿能量上限，且飞斩期间冲动时长被白白烧掉。
        if (killingSpreeCooldown == 0 &&
            !me->HasAura(CombatRogueSpells::AURA_KILLING_SPREE) &&
            !me->HasAura(CombatRogueSpells::AURA_ADRENALINE_RUSH) &&
            me->GetVictim() == victim &&
            me->GetDistance(victim) <= KILLING_SPREE_DIST &&
            me->GetPower(POWER_ENERGY) <= KS_MAX_ENERGY)
        {
            uint32 const killingSpree = GetAppropriateRank(CombatRogueSpells::KILLING_SPREE, true);
            if (killingSpree && CanCast(victim, killingSpree, true) && ExecuteSpell(victim, killingSpree, true))
                killingSpreeCooldown = GetKillingSpreeCooldown();
        }
    }

    // 杀戮盛宴冷却动态结算：插有雕文时由 120s 缩短至 75s
    uint32 GetKillingSpreeCooldown() const
    {
        return me->HasAura(CombatRogueSpells::GLYPH_OF_KILLING_SPREE) ? CD_KILLING_SPREE_GLYPH : CD_KILLING_SPREE;
    }

    // =========================================================================
    // P3: 核心打击与终结技优先级
    // =========================================================================
    bool TryCoreRotation(Unit* victim)
    {
        uint8 const cp = GetComboPoints(victim);

        // ---- 1. 切割：攻速核心增益，绝对最高优先级 ----
        // 缺失或剩余 <= 2000ms 时，只要还有 1 星就立即续订；
        // 攻速断档会同时击穿毒药 PPM、白字伤害与能量回充三条链路，优先级高于一切终端伤害。
        if (NeedSliceAndDice() && cp >= 1)
        {
            uint32 const sliceAndDice = GetAppropriateRank(CombatRogueSpells::SLICE_AND_DICE, false);
            if (sliceAndDice && CanCast(me, sliceAndDice, true) && ExecuteSpell(me, sliceAndDice, true))
            {
                SpendComboPoints();
                return true;
            }
        }

        // ---- 2. 终结技分支：连击点 >= 4 ----
        if (cp >= FINISHER_MIN_CP)
        {
            // ---- 终结技攒能挂起 (Energy Pooling) ----
            // 连击点已达 4~5 星但能量不足以支付终结技时，必须原地挂起等待能量回充。
            // 若贪打一发邪恶攻击，会同时踩两个坑：连击点被顶到 5 星上限白白溢出，
            // 且残余能量被产星技吸干，终结技要再等一整轮回能才能打出，DPS 直接塌方。
            // 挂起 return false 后决策流顺下至 P4 贴背与白字平砍 (平砍不耗能量)，
            // 能量一到即由下方终结技分支当帧顺发。
            if (me->GetPower(POWER_ENERGY) < FINISHER_ENERGY)
                return false;

            if (TryFinisher(victim))
                return true;
        }

        // ---- 3. 产星填充 ----
        return TrySinisterStrike(victim);
    }

    bool TryFinisher(Unit* victim)
    {
        // 精英/首领目标且割裂断档：优先铺满物理流血，联动野蛮战斗与猝不及防增伤
        if (IsEliteOrBossTarget(victim) && !HasRupture(victim) && TryRupture(victim))
            return true;

        // 核心终结技：刺骨 (物理高额直伤)
        return TryEviscerate(victim);
    }

    bool TryEviscerate(Unit* victim)
    {
        uint32 const eviscerate = GetAppropriateRank(CombatRogueSpells::EVISCERATE, false);
        if (!eviscerate || !CanCast(victim, eviscerate, true))
            return false;

        if (ExecuteSpell(victim, eviscerate, true))
        {
            SpendComboPoints();
            ProcPoisons(victim);
            return true;
        }

        return false;
    }

    bool TryRupture(Unit* victim)
    {
        uint32 const rupture = GetAppropriateRank(CombatRogueSpells::RUPTURE, false);
        if (!rupture || !CanCast(victim, rupture, true))
            return false;

        if (ExecuteSpell(victim, rupture, true))
        {
            SpendComboPoints();
            ProcPoisons(victim);
            return true;
        }

        return false;
    }

    bool TrySinisterStrike(Unit* victim)
    {
        // 能量不足时原地等待回充，绝不降级施放其他低效填充
        if (me->GetPower(POWER_ENERGY) < SINISTER_STRIKE_ENERGY)
            return false;

        uint32 const sinisterStrike = GetAppropriateRank(CombatRogueSpells::SINISTER_STRIKE, false);
        if (!sinisterStrike || !CanCast(victim, sinisterStrike, true))
            return false;

        if (ExecuteSpell(victim, sinisterStrike, true))
        {
            // 邪恶攻击为单手主手打击，命中即产 1 星 (上限 5 星由基类状态机钳制)
            AddComboPoints(victim, 1);

            // 技能命中同样触发武器毒药结算
            ProcPoisons(victim);
            return true;
        }

        return false;
    }

    // =========================================================================
    // P3.5: 武器毒药模拟注入 (随从无武器涂毒对象，由白字平砍与技能命中驱动)
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
        uint32 const deadlyPoison = GetAppropriateRank(CombatRogueSpells::DEADLY_POISON, false);
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
            uint32 const instantPoison = GetAppropriateRank(CombatRogueSpells::INSTANT_POISON, false);
            if (instantPoison)
                me->CastSpell(victim, instantPoison, true);
        }
    }

    // =========================================================================
    // P4: 物理近战背后找背站位模型
    // =========================================================================
    void MaintainMeleeBehindPositioning(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return;

        // 读条期间严禁下发走位指令，否则本帧起手的读条法术会被同帧 MoveFollow 秒断。
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
    // 战斗天赋满阶被动光环补偿 (铁律 33，弥补 NPC 缺天赋树缺陷)
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

        SyncPassive(20, CombatRogueSpells::GLYPH_OF_RUPTURE);            // 割裂雕文：割裂时长 +4s
        SyncPassive(20, CombatRogueSpells::GLYPH_OF_SINISTER_STRIKE);    // 邪恶攻击雕文：暴击额外 +1 星
        SyncPassive(30, CombatRogueSpells::HACK_AND_SLASH);              // 砍击与劈砍 Rank 5：剑/斧命中额外攻击
        SyncPassive(30, CombatRogueSpells::DUAL_WIELD_SPEC);             // 双武器专精 Rank 5：副手伤害 +50%
        SyncPassive(40, CombatRogueSpells::COMBAT_POTENCY);              // 战斗潜能 Rank 5：副手命中回能
        SyncPassive(40, CombatRogueSpells::VITALITY);                    // 活力 Rank 3：能量恢复速度 +12%
        SyncPassive(45, CombatRogueSpells::PREY_ON_THE_WEAK);            // 欺凌弱小 Rank 5：暴击伤害 +20%
        SyncPassive(50, CombatRogueSpells::SAVAGE_COMBAT);               // 野蛮战斗 Rank 2：中毒目标物理易伤 +4%
        SyncPassive(55, CombatRogueSpells::SURPRISE_ATTACKS);            // 猝不及防：终结技不可招闪 + 技能伤害 +10%
        SyncPassive(60, CombatRogueSpells::GLYPH_OF_KILLING_SPREE);      // 杀戮盛宴雕文：冷却缩短 45s
    }
};

void AddSC_bot_combat_rogue()
{
    new AdaptiveBotScript<BotCombatRogueAI>("bot_combat_rogue");
}
