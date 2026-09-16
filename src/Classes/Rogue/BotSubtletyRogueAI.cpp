/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#include "SubtletyRogueSpells.h"
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

class BotSubtletyRogueAI : public AdaptiveBotAI
{
    // =========================================================================
    // 站位参数 (物理近战背后找背模型)
    // =========================================================================
    static constexpr float STEALTH_OPENER_DIST  = 5.0f;   // 潜行起手上限：进入即尝试伏击
    static constexpr float MELEE_REACH_DIST     = 4.0f;   // 近战判定区上限：超出必须重新贴背
    static constexpr float MELEE_COMFORT_DIST   = 3.0f;   // 贴身阈值：进入后保持平滑贴背输出
    static constexpr float MELEE_FOLLOW_DIST    = 1.5f;   // 理想站位：目标正后方 1.5 码

    // FollowMovementGenerator 的 angle 为「相对目标朝向的偏移」，引擎内部已叠加目标朝向。
    // 严禁自行叠加 victim->GetOrientation()，否则站位会随目标转向持续漂移。
    // M_PI 即目标正后方：背身位，可规避正面顺劈、吐息以及被招架加速。
    static constexpr float BEHIND_ANGLE         = static_cast<float>(M_PI);

    // 暗影步战术区间 (8~25 码传送背身)
    static constexpr float SHADOWSTEP_MIN_DIST  = 8.0f;
    static constexpr float SHADOWSTEP_MAX_DIST  = 25.0f;

    // =========================================================================
    // 自管冷却时长
    // -------------------------------------------------------------------------
    // Creature 不参与引擎技能 CD 追踪，凡无「持续光环保护」的 CD 技能必须由专精自行计时，
    // 否则会因 HasSpellCooldown 恒 false 而在每一帧对同一技能空转重入。
    // =========================================================================
    static constexpr uint32 CD_SHADOW_DANCE  = 60000;
    static constexpr uint32 CD_SHADOWSTEP    = 30000;
    static constexpr uint32 CD_PREMEDITATION = 20000;
    static constexpr uint32 CD_VANISH        = 180000;
    static constexpr uint32 CD_EVASION       = 180000;
    static constexpr uint32 CD_CLOAK         = 60000;
    static constexpr uint32 CD_PREPARATION   = 300000;

    // 生命阈值
    static constexpr float  VANISH_HP_PCT    = 20.0f;   // 极度濒危：消失脱困
    static constexpr float  EVASION_HP_PCT   = 30.0f;   // 濒死：闪避
    static constexpr float  SAFE_HP_PCT      = 50.0f;   // 脱困判定线 (伺机待发)

    // 能量与连击点门禁
    static constexpr uint8  FINISHER_MIN_CP       = 4;      // 终结技最低连击点
    static constexpr uint32 FINISHER_ENERGY       = 35;     // 终结技支付门槛 (不足则原地挂起等能量)
    static constexpr uint32 HEMORRHAGE_ENERGY     = 35;     // 出血耗能
    static constexpr uint32 BACKSTAB_ENERGY       = 60;     // 背刺耗能
    static constexpr uint32 SHADOW_DANCE_ENERGY   = 60;     // 影舞开启最低能量
    static constexpr float  SHADOW_DANCE_MELEE    = 4.0f;   // 影舞开启所需近战距离

    // 刷新窗口
    static constexpr int32  SND_REFRESH_MS        = 2000;   // 切割提前续订窗口
    static constexpr int32  RUPTURE_REFRESH_MS    = 2000;   // 割裂提前续订窗口

    // 潜行起手窗口上限，超时强制转入常规近战
    static constexpr uint32 STEALTH_OPEN_WINDOW   = 2000;

    // =========================================================================
    // 毒药模拟参数
    // -------------------------------------------------------------------------
    // 随从无武器涂毒对象，且底层毒药依赖 ProcFlag 武器命中事件，
    // 必须由专精以 triggered 方式手工注入，否则毒伤链路与毒药伤害永久缺失。
    // =========================================================================
    static constexpr uint32 POISON_PROC_INTERVAL     = 1000;  // 毒药巡检周期 (等效武器毒药 PPM 节流)
    static constexpr uint32 DEADLY_POISON_MAX_STACKS = 5;     // 致命毒药满层数
    static constexpr uint32 DEADLY_POISON_REFRESH_MS = 3000;  // 致命毒药提前续期窗口
    static constexpr uint32 INSTANT_POISON_CHANCE    = 50;    // 速效毒药触发概率 (%)

    // 流血机制位掩码：Unit::HasAuraWithMechanic 内部采用 1 << (mechanic - 1) 编码
    static constexpr uint64 BLEED_MECHANIC_MASK = uint64(1) << (MECHANIC_BLEED - 1);

public:
    explicit BotSubtletyRogueAI(Creature* creature) : AdaptiveBotAI(creature) {}

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
    // 注：基础法术 (伏击/背刺/刺骨/割裂/切割/消失/闪避/斗篷/毒药) 严禁登记于此，
    //     其等级门槛由 GetAppropriateRank 依据 DBC SpellLevel 自动降阶处理。
    // =========================================================================
    uint8 GetTalentSpellMinLevel(uint32 spellId) const override
    {
        switch (spellId)
        {
            case SubtletyRogueSpells::HEMORRHAGE:    return 20;
            case SubtletyRogueSpells::PREPARATION:   return 30;
            case SubtletyRogueSpells::SHADOWSTEP:    return 30;
            case SubtletyRogueSpells::PREMEDITATION: return 40;
            case SubtletyRogueSpells::SHADOW_DANCE:  return 60;
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
        ResetSubtletyTimers();
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
        UpdateSubtletyTimers(diff);

        // 全局读条/通道双保险守卫：引导类法术在部分状态下并不置位 UNIT_STATE_CASTING，
        // 故追加 CURRENT_CHANNELED_SPELL 显式判定，杜绝被跟随/走位指令掐断。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // =====================================================================
        // 1. 脱战业务维护 (潜行姿态 + 起手 + 跟随)
        // =====================================================================
        if (!me->IsInCombat())
        {
            if (MaintainStealth())
                return;

            // 潜行起手：远距离先预谋+暗影步闪背，贴身则直接伏击开怪
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

        // ---- 盗贼的尊严 (Honor Among Thieves) 被动连击点模拟 ----
        // 底层 EffectAddComboPoints 仅对 Player 生效，随从必须自管回星，
        // 按 1000ms 节流稳定 +1 星，复现「团队暴击时每秒获取 1 连击点」的被动收益。
        if (KnowsTalent(SubtletyRogueSpells::HONOR_AMONG_THIEVES) && honorAmongThievesTimer == 0)
        {
            honorAmongThievesTimer = 1000;
            AddComboPoints(victim, 1);
        }

        // 潜行起手窗口：破潜前不得开启近战追击与白字平砍，否则第一刀白字会提前破潜、
        // 直接吞掉伏击起手与敏锐大师增伤窗口；窗口最多持续 2 秒，超时强制转入常规近战。
        // 影舞期间严禁判定为潜行起手：暗影之舞是输出爆发姿态，必须放行平砍与技能顺下。
        bool const stealthHold = !me->HasAura(SubtletyRogueSpells::AURA_SHADOW_DANCE) &&
                                 IsStealthed() && (stealthOpenTimer > 0);

        // 物理近战开怪：第二参数必须传 true 开启近战平砍与双持白字输出
        // (与远程专精的 me->Attack(victim, false) 严格区分)。
        if (!stealthHold && me->GetVictim() != victim)
            me->Attack(victim, true);

        // ---- P1: 潜行/影舞起手 (预谋 + 暗影步 + 伏击) ----
        if (TryStealthOpener(victim))
            return;

        // ---- P0: 濒死自保与解控 (维度 C 协同) ----
        if (TrySurvivalAndUtility(victim))
            return;

        // ---- P1.5: 战时暗影步突进贴身 ----
        if (TryCombatShadowstep(victim))
            return;

        // ---- P2: 爆发 (暗影之舞开启，Off-GCD 顺下) ----
        TryShadowDanceBurst(victim);

        // ---- P2.5: 影舞持续期内绝对优先伏击 ----
        if (me->HasAura(SubtletyRogueSpells::AURA_SHADOW_DANCE))
        {
            if (TryShadowDanceRotation(victim))
                return;
        }

        // ---- P3: 关键增幅终结技常驻维持 (切割) ----
        if (MaintainSliceAndDice(victim))
            return;

        // ---- P3: 连击点循环与终结技 (含能量挂起) ----
        if (TryComboPointRotation(victim))
            return;

        // ---- P4: 背后找背站位 ----
        MaintainMeleeBehindPositioning(victim);

        // ---- 白色平砍驱动 ----
        // ScriptedAI::UpdateAI 已被本类完整接管，引擎不会自动驱动平砍，
        // 必须在决策流末帧显式调用，否则双持白字与毒药伤害永久缺失。
        // 潜行起手窗口内严禁驱动：挥砍会立即破潜吞掉伏击。
        if (!stealthHold)
        {
            DoMeleeAttackIfReady();

            // P3.5 白字平砍结算后立即补一次毒药模拟：维持 5 层致命毒药 + 速效毒药。
            ProcPoisons(victim);
        }
    }

private:
    // 自管冷却登记
    uint32 shadowDanceCooldown{ 0 };
    uint32 shadowstepCooldown{ 0 };
    uint32 premeditationCooldown{ 0 };
    uint32 vanishCooldown{ 0 };
    uint32 evasionCooldown{ 0 };
    uint32 cloakCooldown{ 0 };
    uint32 preparationCooldown{ 0 };

    // 潜行起手窗口倒计时 (仅潜行态且贴近起手距离时递减)
    uint32 stealthOpenTimer{ STEALTH_OPEN_WINDOW };

    // 毒药模拟巡检节流计时器
    uint32 poisonProcTimer{ 0 };

    // 盗贼的尊严 (Honor Among Thieves) 模拟回星节流计时器
    uint32 honorAmongThievesTimer{ 0 };

    // =========================================================================
    // 专精自管计时器维护
    // =========================================================================
    void UpdateSubtletyTimers(uint32 diff)
    {
        auto Tick = [diff](uint32& timer) { timer = (timer > diff) ? (timer - diff) : 0; };

        Tick(shadowDanceCooldown);
        Tick(shadowstepCooldown);
        Tick(premeditationCooldown);
        Tick(vanishCooldown);
        Tick(evasionCooldown);
        Tick(cloakCooldown);
        Tick(preparationCooldown);
        Tick(poisonProcTimer);
        Tick(honorAmongThievesTimer);

        // 潜行起手窗口：仅当「已潜行 且 已贴近起手距离」时才开始倒计时。
        // 若在赶路途中等比扣减，随从尚未走到目标背后窗口就已耗尽，
        // 必然在破潜瞬间断掉伏击起手与敏锐大师链路，因此赶路途中维持满额窗口。
        if (IsStealthed() && !me->HasAura(SubtletyRogueSpells::AURA_SHADOW_DANCE))
        {
            Unit* openerTarget = SelectAssistTarget();
            bool const inOpenerRange = (openerTarget && openerTarget->IsAlive() &&
                                        openerTarget->IsInWorld() &&
                                        openerTarget->GetMap() == me->GetMap() &&
                                        me->GetDistance(openerTarget) <= SHADOWSTEP_MAX_DIST);

            if (inOpenerRange)
                Tick(stealthOpenTimer);
        }
        else
        {
            stealthOpenTimer = STEALTH_OPEN_WINDOW;
        }
    }

    void ResetSubtletyTimers()
    {
        shadowDanceCooldown = 0;
        shadowstepCooldown = 0;
        premeditationCooldown = 0;
        vanishCooldown = 0;
        evasionCooldown = 0;
        cloakCooldown = 0;
        preparationCooldown = 0;

        stealthOpenTimer = STEALTH_OPEN_WINDOW;
        poisonProcTimer = 0;
        honorAmongThievesTimer = 0;
    }

    // =========================================================================
    // 状态判定器
    // =========================================================================
    bool KnowsTalent(uint32 spellId) const
    {
        uint8 const minLevel = GetTalentSpellMinLevel(spellId);
        return minLevel > 0 && me->GetLevel() >= minLevel;
    }

    bool IsStealthed() const
    {
        // 潜行为分阶法术，消失 (Vanish) 亦会施加潜行光环：
        // 同时按法术 ID 与光环类型双通道判定，杜绝漏检导致起手链路失效。
        return me->HasAura(SubtletyRogueSpells::STEALTH) || me->HasAuraType(SPELL_AURA_MOD_STEALTH);
    }

    bool HasSliceAndDice() const
    {
        // 切割为分阶法术，且底层 Cut to the Chase 脚本固定以 Rank 1 刷新 5 星时长，
        // 故必须按「分阶光环」查询，直接 HasAura(6774) 会漏检 Rank 1 光环。
        return me->GetAuraOfRankedSpell(SubtletyRogueSpells::SLICE_AND_DICE) != nullptr;
    }

    bool IsSliceAndDiceExpiring() const
    {
        Aura* aura = me->GetAuraOfRankedSpell(SubtletyRogueSpells::SLICE_AND_DICE);
        if (!aura)
            return true;

        // GetDuration() < 0 表示永久光环，无需续订
        return aura->GetDuration() >= 0 && aura->GetDuration() <= SND_REFRESH_MS;
    }

    bool HasRupture(Unit* victim) const
    {
        if (!victim)
            return false;

        Aura* aura = victim->GetAuraOfRankedSpell(SubtletyRogueSpells::RUPTURE, me->GetGUID());
        if (!aura)
            return false;

        // 永久光环或剩余充足则视为未断档
        return aura->GetDuration() < 0 || aura->GetDuration() > RUPTURE_REFRESH_MS;
    }

    bool HasBleedEffect(Unit* target) const
    {
        if (!target)
            return false;

        // 目标身上任意来源的流血均可为锯刃/卑鄙提供跳板 (自身割裂/队友的流血技)
        return target->HasAuraWithMechanic(BLEED_MECHANIC_MASK);
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

    // 群体敌情雷达：统计当前交战中、贴身范围内的敌对单位数量 (影舞 AoE 门禁输入)
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
    // 伏击/背刺均有背后硬性要求，提前自检可避免白白空放被底层拒绝。
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
    // P1: 脱战潜行与潜行/影舞起手 (预谋 -> 暗影步 -> 伏击)
    // =========================================================================
    bool MaintainStealth()
    {
        if (IsStealthed() || me->IsInCombat())
            return false;

        // 潜行严禁在战斗中施放 (底层会直接拒绝)，本函数仅由脱战分支调用
        uint32 const stealth = GetAppropriateRank(SubtletyRogueSpells::STEALTH, false);
        if (!stealth || !CanCast(me, stealth, true))
            return false;

        return ExecuteSpell(me, stealth, true);
    }

    bool TryStealthOpener(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || !victim->IsInWorld() || victim->GetMap() != me->GetMap())
            return false;

        // 伏击必须由潜行态施放；影舞期间另由 P2.5 分支接管，此处仅处理真实潜行
        if (!IsStealthed() || me->HasAura(SubtletyRogueSpells::AURA_SHADOW_DANCE))
            return false;

        float const dist = me->GetDistance(victim);

        // ---- 远距离战术：预谋攒星 + 暗影步闪现背身 ----
        if (dist >= SHADOWSTEP_MIN_DIST && dist <= SHADOWSTEP_MAX_DIST)
        {
            // 预谋为瞬发增益，施放成功严禁 return，必须当帧顺下接暗影步
            TryPremeditation(victim);

            if (TryShadowstep(victim))
                return true;

            return false;
        }

        if (dist > STEALTH_OPENER_DIST)
            return false;

        // 伏击为背后起手技：未占住背身位时交由 P4 走位接管，避免空放浪费起手窗口
        if (!IsBehindVictim(victim, STEALTH_OPENER_DIST))
            return false;

        // 预谋 Off-GCD 顺下：+2 星后当帧直接接伏击，绝不让光环空转被后续技能吞掉
        TryPremeditation(victim);

        return TryAmbush(victim);
    }

    bool TryPremeditation(Unit* victim)
    {
        if (premeditationCooldown > 0)
            return false;

        if (!KnowsTalent(SubtletyRogueSpells::PREMEDITATION))
            return false;

        if (!IsStealthed() && !me->HasAura(SubtletyRogueSpells::AURA_SHADOW_DANCE))
            return false;

        // 预谋的施法目标是 30 码内的敌对单位 (对自己施放会被底层直接拒绝)
        uint32 const premeditation = GetAppropriateRank(SubtletyRogueSpells::PREMEDITATION, true);
        if (!premeditation || !victim || !CanCast(victim, premeditation, true))
            return false;

        if (!ExecuteSpell(victim, premeditation, true))
            return false;

        premeditationCooldown = CD_PREMEDITATION;

        // 预谋在敌方身上不落光环，其 «+2 连击点» 语义由专精状态机等价实现
        if (victim)
            AddComboPoints(victim, 2);

        return true;
    }

    bool TryShadowstep(Unit* victim)
    {
        if (shadowstepCooldown > 0)
            return false;

        if (!victim || !victim->IsAlive())
            return false;

        if (!KnowsTalent(SubtletyRogueSpells::SHADOWSTEP))
            return false;

        float const dist = me->GetDistance(victim);
        if (dist < SHADOWSTEP_MIN_DIST || dist > SHADOWSTEP_MAX_DIST)
            return false;

        uint32 const shadowstep = GetAppropriateRank(SubtletyRogueSpells::SHADOWSTEP, true);
        if (!shadowstep || !CanCast(victim, shadowstep, true))
            return false;

        if (!ExecuteSpell(victim, shadowstep, true))
            return false;

        shadowstepCooldown = CD_SHADOWSTEP;

        // 瞬移落位需等待一帧位置同步，故本帧交还控制权，下一帧由伏击接管背身起手
        return true;
    }

    // 战时暗影步：战斗中目标处于 8~25 码时主动闪现贴身，
    // 既补足远程脱离后的贴背速度，也避免长时间脱手白字损失。
    bool TryCombatShadowstep(Unit* victim)
    {
        if (shadowstepCooldown > 0 || !victim)
            return false;

        float const dist = me->GetDistance(victim);
        if (dist < SHADOWSTEP_MIN_DIST || dist > SHADOWSTEP_MAX_DIST)
            return false;

        return TryShadowstep(victim);
    }

    // =========================================================================
    // P0: 濒死自保与解控
    // =========================================================================
    bool TrySurvivalAndUtility(Unit* victim)
    {
        // ---- 闪避：濒死且被近战压制时开启 50% 闪避硬减伤 ----
        if (evasionCooldown == 0 && me->GetHealthPct() < EVASION_HP_PCT && IsUnderPhysicalMelee(me))
        {
            uint32 const evasion = GetAppropriateRank(SubtletyRogueSpells::EVASION, false);
            if (evasion && !me->HasAura(evasion) && CanCast(me, evasion, true) && ExecuteSpell(me, evasion, true))
            {
                evasionCooldown = CD_EVASION;
                return true;
            }
        }

        // ---- 暗影斗篷：减速/定身/魔法负面压制时开启，兼顾免伤与解控 ----
        if (cloakCooldown == 0 && NeedsCloakOfShadows())
        {
            uint32 const cloak = GetAppropriateRank(SubtletyRogueSpells::CLOAK_OF_SHADOWS, false);
            if (cloak && !me->HasAura(cloak) && CanCast(me, cloak, true) && ExecuteSpell(me, cloak, true))
            {
                cloakCooldown = CD_CLOAK;

                // Off-GCD 铁律：暗影斗篷独立于公共冷却，施放成功严禁 return true，
                // 否则会白吞当帧的伏击/终结技窗口 (自保技挤占输出帧)。
            }
        }

        // ---- 消失：极度濒危且被围攻，或仇恨彻底失控 (OT) ----
        // 消失强行进潜清仇恨并重新触发伏击起手链路，是敏锐贼最高优先级的脱困底牌。
        if (vanishCooldown == 0 && me->GetHealthPct() < VANISH_HP_PCT &&
            (CountMeleeAttackers() >= 2 || IsTopThreatTarget()))
        {
            uint32 const vanish = GetAppropriateRank(SubtletyRogueSpells::VANISH, false);
            if (vanish && !me->HasAura(vanish) && CanCast(me, vanish, true) && ExecuteSpell(me, vanish, true))
            {
                vanishCooldown = CD_VANISH;

                // 进潜后立即重置起手窗口，使随从能以伏击重新起手
                stealthOpenTimer = STEALTH_OPEN_WINDOW;
                return true;
            }
        }

        // ---- 伺机待发：脱困后一次性重置全部核心自保冷却 ----
        if (preparationCooldown == 0 && KnowsTalent(SubtletyRogueSpells::PREPARATION) &&
            me->GetHealthPct() >= SAFE_HP_PCT && CountMeleeAttackers() <= 1 &&
            CountActiveSurvivalCooldowns() >= 2)
        {
            uint32 const preparation = GetAppropriateRank(SubtletyRogueSpells::PREPARATION, true);
            if (preparation && !me->HasAura(preparation) && CanCast(me, preparation, true) && ExecuteSpell(me, preparation, true))
            {
                preparationCooldown = CD_PREPARATION;

                // 伺机待发重置：急跑(无状态自管)/消失/闪避/暗影步
                vanishCooldown = 0;
                evasionCooldown = 0;
                shadowstepCooldown = 0;

                // Off-GCD 铁律：伺机待发为瞬发重置技，施放成功严禁 return true，
                // 必须允许当帧决策流顺下，让重置后的消失/闪避理论就绪状态立即被后续逻辑消费。
            }
        }

        return false;
    }

    // 统计当前处于冷却中的核心自保技能数量
    uint32 CountActiveSurvivalCooldowns() const
    {
        uint32 count = 0;
        if (vanishCooldown > 0)   ++count;
        if (evasionCooldown > 0)  ++count;
        if (cloakCooldown > 0)    ++count;
        return count;
    }

    // =========================================================================
    // P2: 暗影之舞爆发时序
    // =========================================================================
    void TryShadowDanceBurst(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return;

        if (shadowDanceCooldown > 0 || me->HasAura(SubtletyRogueSpells::AURA_SHADOW_DANCE))
            return;

        if (!KnowsTalent(SubtletyRogueSpells::SHADOW_DANCE))
            return;

        // 开启前置：近战位 + 能量充盈 + 目标值得爆发 (精英/Boss 或多目标)
        bool const inMelee = (me->GetDistance(victim) <= SHADOW_DANCE_MELEE);
        bool const energyReady = (me->GetPower(POWER_ENERGY) >= SHADOW_DANCE_ENERGY);
        bool const worthIt = IsEliteOrBossTarget(victim) || CountNearbyHostileEnemies() >= 2;

        if (!inMelee || !energyReady || !worthIt)
            return;

        // 背身门禁：怪盯防主坦时必须先占住背身位再开影舞，
        // 否则伏击会因正面站位被底层拒绝，白白空烧一轮爆发 (影舞 CD 1 分钟)。
        // 怪盯防随从本人时无背身可言，交由 P4 的 MoveChase 贴身硬刚逻辑处理，放行开启。
        bool const behind = IsBehindVictim(victim, MELEE_REACH_DIST);
        if (!behind && victim->GetVictim() != me)
            return;

        uint32 const dance = GetAppropriateRank(SubtletyRogueSpells::SHADOW_DANCE, true);
        if (!dance || !CanCast(me, dance, true))
            return;

        if (ExecuteSpell(me, dance, true))
        {
            shadowDanceCooldown = CD_SHADOW_DANCE;

            // 影舞为持续 6/8 秒的增益姿态，施放成功严禁 return true：
            // 必须允许当帧顺下，由 P2.5 立即消费预谋与伏击窗口。
        }
    }

    bool TryShadowDanceRotation(Unit* victim)
    {
        if (!victim || !victim->IsAlive())
            return false;

        // 影舞期间预谋优先：+2 星后当帧顺下继续决策
        TryPremeditation(victim);

        uint8 const cp = GetComboPoints(victim);

        // 星数已达 4~5 星：立即刺骨消费连击点，防止溢星浪费伏击收益
        if (cp >= FINISHER_MIN_CP)
        {
            // 能量不足则原地挂起等待回能，绝不用低效技能偷跑
            if (me->GetPower(POWER_ENERGY) < FINISHER_ENERGY)
                return false;

            if (TryEviscerate(victim))
                return true;
        }

        // 背身位优先伏击：吃满敏锐大师与机遇的独立增伤乘数
        if (IsBehindVictim(victim, MELEE_REACH_DIST) && TryAmbush(victim))
            return true;

        // 未占住背后时交由 P4 走位找背，本帧交还控制权
        return false;
    }

    // =========================================================================
    // P3: 关键增幅终结技常驻维持
    // =========================================================================
    bool MaintainSliceAndDice(Unit* victim)
    {
        // 切割为攻速核心增益：缺失或濒临断档时立即续订
        if (HasSliceAndDice() && !IsSliceAndDiceExpiring())
            return false;

        uint8 const cp = GetComboPoints(victim);
        if (cp < 1)
            return false;

        uint32 const sliceAndDice = GetAppropriateRank(SubtletyRogueSpells::SLICE_AND_DICE, false);
        if (!sliceAndDice || !CanCast(me, sliceAndDice, true))
            return false;

        if (ExecuteSpell(me, sliceAndDice, true))
        {
            SpendComboPoints();
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
        uint32 const deadlyPoison = GetAppropriateRank(SubtletyRogueSpells::DEADLY_POISON, false);
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
            uint32 const instantPoison = GetAppropriateRank(SubtletyRogueSpells::INSTANT_POISON, false);
            if (instantPoison)
                me->CastSpell(victim, instantPoison, true);
        }
    }

    // =========================================================================
    // P3: 连击点循环与终结技
    // =========================================================================
    bool TryComboPointRotation(Unit* victim)
    {
        uint8 const cp = GetComboPoints(victim);

        // ---- 终结技攒能挂起 (Energy Pooling) ----
        // 连击点已到 4~5 星但能量不足以支付终结技时，必须原地挂起等待能量回充。
        // 此时若贪打一发出血/背刺，会同时踩两个坑：连击点被顶到 5 星上限白白溢出，
        // 且残余能量被产星技吸干，终结技要再等一整轮回能才能打出，DPS 直接塌方。
        // 挂起 return false 后决策流顺下至 P4 贴背与白字平砍 (平砍不耗能量)，
        // 能量一到即由下方终结技分支当帧顺发。
        if (cp >= FINISHER_MIN_CP && me->GetPower(POWER_ENERGY) < FINISHER_ENERGY)
            return false;

        // ---- 终结技分支：连击点 >= 4 且能量充足 ----
        if (cp >= FINISHER_MIN_CP && TryFinisher(victim))
            return true;

        // ---- 产星分支 ----
        return TryBuilder(victim);
    }

    bool TryFinisher(Unit* victim)
    {
        // 精英/首领目标且割裂断档：优先铺满流血，联动锯刃无视护甲增伤
        if (IsEliteOrBossTarget(victim) && !HasRupture(victim) && TryRupture(victim))
            return true;

        // 核心终结技：刺骨 (物理高额直伤)
        if (TryEviscerate(victim))
            return true;

        // 末位兜底：割裂铺垫流血，至少保住锯刃/卑鄙的增伤跳板
        return TryRupture(victim);
    }

    bool TryEviscerate(Unit* victim)
    {
        uint32 const eviscerate = GetAppropriateRank(SubtletyRogueSpells::EVISCERATE, false);
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
        uint32 const rupture = GetAppropriateRank(SubtletyRogueSpells::RUPTURE, false);
        if (!rupture || !CanCast(victim, rupture, true))
            return false;

        if (ExecuteSpell(victim, rupture, true))
        {
            SpendComboPoints();
            return true;
        }

        return false;
    }

    bool TryAmbush(Unit* victim)
    {
        if (!victim)
            return false;

        // 伏击需潜行或暗影之舞支撑 (影舞为纯天赋，SpellLevel 恒 0 由等级门禁判定)
        bool const inDance = me->HasAura(SubtletyRogueSpells::AURA_SHADOW_DANCE);
        if (!IsStealthed() && !inDance)
            return false;

        if (inDance && !KnowsTalent(SubtletyRogueSpells::SHADOW_DANCE))
            return false;

        uint32 const ambush = GetAppropriateRank(SubtletyRogueSpells::AMBUSH, false);
        if (!ambush || !CanCast(victim, ambush, true))
            return false;

        if (ExecuteSpell(victim, ambush, true))
        {
            // 3.3.5a 伏击奖励 2 个连击点
            AddComboPoints(victim, 2);

            // 技能命中同样触发武器毒药结算
            ProcPoisons(victim);
            return true;
        }

        return false;
    }

    // =========================================================================
    // P3: 产星填充
    // =========================================================================
    bool TryBuilder(Unit* victim)
    {
        if (!victim)
            return false;

        bool const behind = IsBehindVictim(victim, MELEE_REACH_DIST);
        bool const energyRich = (me->GetPower(POWER_ENERGY) >= BACKSTAB_ENERGY);
        bool const mobOnTank = (victim->GetVictim() != me);

        // ---- 背刺：怪物盯防主坦、已占住背身位且能量充盈时的高效产星 ----
        // 背刺耗能 60，仅在能量富余时使用，避免挤占终结技支付窗口。
        if (mobOnTank && behind && energyRich && TryBackstab(victim))
            return true;

        // ---- 出血：35 能量稳定产星，无视站位且附带物理易伤 ----
        if (TryHemorrhage(victim))
            return true;

        // ---- 出血不可用 (未习得) 时以背刺兜底 ----
        if (behind && TryBackstab(victim))
            return true;

        return false;
    }

    bool TryHemorrhage(Unit* victim)
    {
        // 出血属天赋技能，DBC 中 SpellLevel 恒为 0，GetAppropriateRank 不会因等级而降阶，
        // 低等级下依旧会返回最高 Rank 的 ID。故必须显式按天赋契约等级门禁判定是否已习得，
        // 否则 20 级以下会误判为「已习得出血」，永久 CanCast 失败而彻底不产星。
        if (!KnowsTalent(SubtletyRogueSpells::HEMORRHAGE))
            return false;

        if (me->GetPower(POWER_ENERGY) < HEMORRHAGE_ENERGY)
            return false;

        uint32 const hemorrhage = GetAppropriateRank(SubtletyRogueSpells::HEMORRHAGE, true);
        if (!hemorrhage || !CanCast(victim, hemorrhage, true))
            return false;

        if (ExecuteSpell(victim, hemorrhage, true))
        {
            AddComboPoints(victim, 1);
            ProcPoisons(victim);
            return true;
        }

        return false;
    }

    bool TryBackstab(Unit* victim)
    {
        if (me->GetPower(POWER_ENERGY) < BACKSTAB_ENERGY)
            return false;

        uint32 const backstab = GetAppropriateRank(SubtletyRogueSpells::BACKSTAB, false);
        if (!backstab || !CanCast(victim, backstab, true))
            return false;

        if (ExecuteSpell(victim, backstab, true))
        {
            AddComboPoints(victim, 1);
            ProcPoisons(victim);
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

        // 铁律 38：读条期间严禁下发走位指令。
        // 否则本帧起手的读条法术会被同帧下发的 MoveFollow 秒断，造成永久抽搐。
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
            // 待消失/伺机待发把仇恨交回主坦后，再由下方分支恢复严格找背。
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
    // 敏锐天赋满阶被动光环补偿 (铁律 33，弥补 NPC 缺天赋树缺陷)
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

        SyncPassive(15, SubtletyRogueSpells::GLYPH_OF_EVISCERATE);      // 刺骨雕文：刺骨暴击率 +10%
        SyncPassive(20, SubtletyRogueSpells::GLYPH_OF_HEMORRHAGE);      // 出血雕文：出血伤害 +40%
        SyncPassive(20, SubtletyRogueSpells::OPPORTUNITY);              // 机遇 Rank 2：背刺/伏击伤害 +20%
        SyncPassive(25, SubtletyRogueSpells::SERRATED_BLADES);          // 锯刃 Rank 3：无视护甲 + 割裂增伤
        SyncPassive(30, SubtletyRogueSpells::DIRTY_DEEDS);              // 卑鄙 Rank 2：目标 <35% 技能增伤 20%
        SyncPassive(40, SubtletyRogueSpells::MASTER_OF_SUBTLETY);       // 敏锐大师 Rank 3：潜行及破潜后 6s 增伤 10%
        SyncPassive(45, SubtletyRogueSpells::FIND_WEAKNESS);            // 寻找弱点 Rank 3：终结技提升物理伤害 10%
        SyncPassive(50, SubtletyRogueSpells::SINISTER_CALLING);         // 险恶召唤 Rank 5：敏捷 +15%
        SyncPassive(60, SubtletyRogueSpells::HONOR_AMONG_THIEVES);      // 盗贼的尊严 Rank 3：团队暴击时 +1 星
        SyncPassive(60, SubtletyRogueSpells::GLYPH_OF_SHADOW_DANCE);    // 暗影之舞雕文：持续时间延长 2s
    }
};

void AddSC_bot_subtlety_rogue()
{
    new AdaptiveBotScript<BotSubtletyRogueAI>("bot_subtlety_rogue");
}
