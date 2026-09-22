/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "ScriptedCreature.h"
#include "ScriptMgr.h"
#include "Creature.h"
#include "Pet.h"
#include "Unit.h"
#include "Player.h"
#include "Group.h"
#include "Item.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "BotGuardianDisplays.h"
#include "ObjectAccessor.h"
#include "GossipDef.h"
#include "ScriptedGossip.h"
#include "Chat.h"
#include "Log.h"
#include "Analytics/CombatAnalyzer.h"
#include "Movement/PotentialField.h"
#include "Movement/DangerZones.h"
#include "Storage/BotMemoryDB.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <mutex>

class AdaptiveBotAI : public ScriptedAI
{
public:
    explicit AdaptiveBotAI(Creature* creature) : ScriptedAI(creature) {}
    
    virtual ~AdaptiveBotAI()
    {
        UnregisterFromMaster();
    }

    /// @brief 宿主实体安全访问器。
    ///        `me` 由 UnitAI/CreatureAI 基类置于 protected 保护域，类外部的宏观
    ///        指令调度系统（BotCommandMgr）需要直接编排随从的瞬移、表情与战斗
    ///        状态，故在此以最小代价暴露一个只读实体句柄，避免类外保护访问错误。
    [[nodiscard]] Creature* GetBotCreature() const { return me; }

    ObjectGuid masterGuid;
    uint32 followCheckTimer{ 0 };
    uint32 gcdTimer{ 0 };
    uint32 levelSyncTimer{ 0 };
    uint32 energyRegenTimer{ 0 };
    uint32 manaRegenTimer{ 0 };
    bool wasInCombat{ false };

    /// @brief 就地休息状态门禁标记。
    ///        由宏观战术指挥系统 (.bot rest) 置位：休息期间彻底封锁索敌开怪与
    ///        一切技能释放意图，仅保留普通跟随/待命行为。
    bool isResting{ false };

    /// @brief 阵型保持标记。
    ///        由宏观战术指挥系统 (.bot stack/fan/spread) 置位：处于阵型点位期间
    ///        禁止 UpdateFollowMaster 的常规 6 码跟随抢占，否则每秒巡检会把
    ///        刚计算好的阵型坐标整体冲刷回跟随队形（仅 >45 码防丢失瞬移保留）。
    bool isHoldingFormation{ false };

    // 伴随型战斗护卫（Combat Guardian）实时句柄与保活轮询计时器
    ObjectGuid guardianGuid;
    uint32 guardianCheckTimer{ 0 };

    // =========================================================================
    // 阶段三：顶层持久化认知记忆预热档案与实时打断追踪器
    // =========================================================================
    ObjectGuid memoryPreloadTargetGuid;                       // 已预热 Boss 句柄（防重复读库）
    std::unordered_map<uint32, uint32> learnedInterruptDelays; // spellId -> 压秒提前量(ms)

    // 敌方施法进度追踪器 (纯 diff 驱动，避开底层 Spell 成员访问限制)
    ObjectGuid currentEnemyCastingGuid;
    uint32 currentEnemyCastingSpellId{ 0 };
    uint32 currentEnemyCastingElapsedMs{ 0 };
    uint32 currentEnemyCastingTotalMs{ 0 };

    // =========================================================================
    // 中层归因分析：战斗事件环形缓冲区与动态危险禁区（栈内定长，零堆分配）
    // =========================================================================
    BotCombatRingBuffer<256> combatEventBuffer;
    uint32 combatTimerMs{ 0 };
    uint32 tankSampleTimer{ 0 };
    uint32 otCheckTimer{ 0 };
    uint32 apfMoveUpdateTimer{ 0 }; // APF 势场走位决策节流器 (300ms 防抖)
    uint32 restRegenTimer{ 0 };     // 就地休息强力补给心跳节流器 (1000ms)

    // 当前感知到的动态危险斥力源（由战后归因逆向提炼，供 APF 势场避险消费）
    std::vector<DangerZone> activeDangerZones;

    /// @brief 检查随从自身当前是否处于任一危险禁区的影响范围内。
    ///        仅用于「是否需要紧急走位」的判定门禁：远程/治疗专精若不在
    ///        危险区内，即便场上存在火圈也必须保持站桩，杜绝被友军防挤压
    ///        斥力推着频繁移步而掐断读条。
    bool IsUnderDangerThreat(float buffer = 2.0f) const
    {
        uint32 const currentMapId = me->GetMapId();
        for (auto const& zone : activeDangerZones)
        {
            if (zone.mapId != 0 && zone.mapId != currentMapId)
                continue;

            if (zone.type == DangerZoneType::CIRCLE)
            {
                if (me->GetDistance2d(zone.x, zone.y) < (zone.radius + buffer))
                    return true;
            }
            else if (zone.type == DangerZoneType::FRONTAL_CONE)
            {
                // 坦克位豁免：正面承伤聚怪是坦克的本职，严禁因避险转头
                if (IsTankBot())
                    continue;

                // 优先取当前交战目标；治疗随从贴身奶队友时 GetVictim() 常为空，
                // 此时回退至开怪预热锁定的 Boss 实体，确保治疗位同样能感知正面顺劈。
                Unit* enemy = me->GetVictim();
                if (!enemy && !memoryPreloadTargetGuid.IsEmpty())
                    enemy = ObjectAccessor::GetUnit(*me, memoryPreloadTargetGuid);

                // 战场上已无存活施法首领时，锥形区无从锚定，直接忽略该威胁。
                if (!enemy || !enemy->IsAlive())
                    continue;

                float const originX = enemy->GetPositionX();
                float const originY = enemy->GetPositionY();
                float const coneOrient = enemy->GetOrientation();

                float const dx = me->GetPositionX() - originX;
                float const dy = me->GetPositionY() - originY;
                float const dist = std::sqrt(dx * dx + dy * dy);

                if (dist < (zone.radius + buffer) && dist > 0.001f)
                {
                    float const phi = std::atan2(dy, dx);
                    float diffAngle = phi - coneOrient;
                    while (diffAngle > static_cast<float>(M_PI))  diffAngle -= static_cast<float>(2.0 * M_PI);
                    while (diffAngle < -static_cast<float>(M_PI)) diffAngle += static_cast<float>(2.0 * M_PI);

                    if (std::abs(diffAngle) < ((zone.coneAngle * 0.5f) + 0.15f))
                        return true;
                }
            }
        }
        return false;
    }

    // 缓存指挥官平均装等 (实现战斗算伤绝对 O(1))
    float cachedMasterItemLevel{ 200.0f };

    // 调试日志总开关 (可在 Gossip 对话菜单中实时切换)
    bool isDebugLogging{ true };

    // 连击点状态机 (依附目标纯函数架构)
    uint8 comboPoints{ 0 };
    ObjectGuid comboTargetGuid;

    // =========================================================================
    // 角色定位契约
    // =========================================================================
    virtual bool IsTankBot() const { return false; }
    virtual bool IsHealerBot() const { return false; }
    virtual bool IsRangedBot() const { return false; }
    virtual bool IsRangedPhysicalBot() const { return false; } // 猎人专修契约 (与法系远程解耦)

    // =========================================================================
    // 工业级数值平衡乘数契约 (绝对 O(1) 吞吐)
    // =========================================================================
    virtual float GetDamageDealtMultiplier() const
    {
        if (IsTankBot())
            return 0.75f;

        uint8 const lvl = me->GetLevel();

        // 1~79 级自强过渡：弥补法系无装备法强 (物理远程由 AP 支撑，不走法伤放大通道)
        if (lvl < 80)
        {
            if (IsRangedBot() && !IsRangedPhysicalBot())
                return 1.0f + (lvl / 80.0f) * 0.40f;
            return 1.0f;
        }

        // 80 级团本专修：法系输出根据指挥官装等（200~284）跨越狂暴秒伤及格线
        if (IsRangedBot() && !IsRangedPhysicalBot())
        {
            float const tierDelta = std::clamp(cachedMasterItemLevel - 200.0f, 0.0f, 84.0f);
            return 2.0f + (tierDelta / 84.0f) * 1.30f; // 200装等=2.0x, 284装等=3.3x
        }

        return 1.0f;
    }

    virtual float GetDamageTakenMultiplier() const
    {
        // 坦克随从常驻模拟 50% 装备减伤，普通 DPS/治疗常驻 15% 减伤
        return IsTankBot() ? 0.50f : 0.85f;
    }

    virtual float GetHealingReceivedMultiplier() const
    {
        // 坦克随从受疗放大，弥补大型副本高血量池下的治疗缺口
        return IsTankBot() ? 1.30f : 1.0f;
    }

    // =========================================================================
    // 同指挥官随从集群总线
    // =========================================================================
    static inline std::mutex s_botRegistryMutex;
    static inline std::unordered_map<ObjectGuid, std::vector<AdaptiveBotAI*>> s_masterBotRegistry;

    void RegisterToMaster(ObjectGuid const& guid)
    {
        std::lock_guard<std::mutex> lock(s_botRegistryMutex);
        auto& list = s_masterBotRegistry[guid];
        if (std::find(list.begin(), list.end(), this) == list.end())
            list.push_back(this);
    }

    void UnregisterFromMaster()
    {
        if (!masterGuid.IsEmpty())
        {
            std::lock_guard<std::mutex> lock(s_botRegistryMutex);
            auto it = s_masterBotRegistry.find(masterGuid);
            if (it != s_masterBotRegistry.end())
            {
                auto& list = it->second;
                list.erase(std::remove(list.begin(), list.end(), this), list.end());
                if (list.empty())
                    s_masterBotRegistry.erase(it);
            }
        }
    }

    Unit* GetGroupTank()
    {
        Player* master = GetMaster();
        if (!master)
            return nullptr;

        {
            std::lock_guard<std::mutex> lock(s_botRegistryMutex);
            auto it = s_masterBotRegistry.find(master->GetGUID());
            if (it != s_masterBotRegistry.end())
            {
                for (AdaptiveBotAI* allyBot : it->second)
                {
                    if (allyBot && allyBot->me && allyBot->me->IsInWorld() && allyBot->me->IsAlive() && allyBot->me->GetMap() == me->GetMap() && allyBot->IsTankBot())
                        return allyBot->me;
                }
            }
        }

        bool const isMasterTank = master->HasAura(71)     // 战士: 防御姿态
                               || master->HasAura(5487)   // 德鲁伊: 熊形态
                               || master->HasAura(9634)   // 德鲁伊: 巨熊形态
                               || master->HasAura(25780)  // 圣骑士: 正义之怒
                               || master->HasAura(48263); // 死亡骑士: 冰霜灵气

        if (isMasterTank)
            return master;

        // 扫描小队 / 团队中的其他真人队友是否处于坦克姿态。
        // 若坦克由二号玩家担任，必须正确识别，否则 GetUrgentThreatTarget()
        // 会把主坦误判为 OT 队员而触发嘲讽抢怪，远程随从也会失去背身避难锚点。
        if (Group* group = master->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || member == master || !member->IsAlive() || !member->IsInWorld() || member->GetMap() != me->GetMap())
                    continue;

                bool const isMemberTank = member->HasAura(71)     // 战士: 防御姿态
                                       || member->HasAura(5487)   // 德鲁伊: 熊形态
                                       || member->HasAura(9634)   // 德鲁伊: 巨熊形态
                                       || member->HasAura(25780)  // 圣骑士: 正义之怒
                                       || member->HasAura(48263); // 死亡骑士: 冰霜灵气

                if (isMemberTank)
                    return member;
            }
        }

        return nullptr;
    }

    void Reset() override
    {
        ScriptedAI::Reset();
        followCheckTimer = 0;
        gcdTimer = 0;
        levelSyncTimer = 0;
        energyRegenTimer = 0;
        manaRegenTimer = 0;
        wasInCombat = false;
        isResting = false;
        isHoldingFormation = false;
        restRegenTimer = 0;
        combatTimerMs = 0;
        tankSampleTimer = 0;
        otCheckTimer = 0;
        apfMoveUpdateTimer = 0;
        memoryPreloadTargetGuid.Clear();
        learnedInterruptDelays.clear();
        currentEnemyCastingGuid.Clear();
        currentEnemyCastingSpellId = 0;
        currentEnemyCastingElapsedMs = 0;
        currentEnemyCastingTotalMs = 0;
        combatEventBuffer.Clear();
        // 刻意保留 activeDangerZones：团灭跑尸的 Reset() 不得冲刷已学到的
        // 危险禁区，否则每一次团灭都会把当次归因成果清零，永远无法跨战斗避险。
        ResetComboPoints();
        me->SetNpcFlag(UNIT_NPC_FLAG_GOSSIP);

        SyncLevelWithMaster();
        ApplyAdaptiveBalanceStats();

        if (Player* master = GetMaster())
        {
            me->SetFaction(master->GetFaction());
            me->SetPhaseMask(master->GetPhaseMask(), true);
        }

        // 出生即常驻：Reset() 是随从被创建/重置的必经入口，在此确保护卫就位，
        // 使机器人一落地便带宠，杜绝首次进战才发现缺宠造成的契约空窗。
        EnsureGuardianAlive();
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        me->CombatStop(true);

        if (isDebugLogging)
            LOG_INFO("scripts", "[Bot: {}] 脱离战斗，进入规避/待命状态。", me->GetName());

        if (Player* master = GetMaster())
        {
            me->GetMotionMaster()->Clear();
            if (master->IsAlive())
            {
                if (me->GetMap() == master->GetMap())
                    me->GetMotionMaster()->MoveFollow(master, 3.0f, me->GetAngle(master));
                else
                    me->GetMotionMaster()->MoveIdle();
            }
            else
            {
                me->GetMotionMaster()->MoveIdle();
            }
        }
        else
        {
            ScriptedAI::EnterEvadeMode();
        }
    }

    void SetMaster(Player* player)
    {
        if (player)
        {
            if (masterGuid != player->GetGUID())
            {
                UnregisterFromMaster();
                masterGuid = player->GetGUID();
                RegisterToMaster(masterGuid);
            }
            me->SetFaction(player->GetFaction());
            me->SetPhaseMask(player->GetPhaseMask(), true);
            SyncLevelWithMaster();
            ApplyAdaptiveBalanceStats();

            if (isDebugLogging)
                LOG_INFO("scripts", "[Bot: {}] 成功绑定指挥官 [{}]。", me->GetName(), player->GetName());
        }
    }

    Player* GetMaster() const
    {
        return ObjectAccessor::GetPlayer(*me, masterGuid);
    }

    // =========================================================================
    // 伴随型战斗护卫常驻体系（出生即随行 + 战后秒补 + 脱战保活）
    // =========================================================================
    /// @brief 该专精机器人是否需要常驻伴随护卫。
    ///        猎人（座狼）、术士（三大恶魔）、死亡骑士（食尸鬼）天然契约带宠，
    ///        其余职业不召唤，避免无意义占用服务器实体配额。
    virtual bool ShouldHaveGuardian() const
    {
        // 默认仅猎人与术士全系常驻随从。
        // 3.3.5a 死亡骑士中只有邪 DK 契约性常驻天灾军团食尸鬼，血 DK 与冰 DK
        // 均无随从席位，故不再在基类按 CLASS_DEATH_KNIGHT 无差别放行，
        // 改由邪 DK 专精显式覆写开启。
        uint8 const botClass = me->getClass();
        return botClass == CLASS_HUNTER || botClass == CLASS_WARLOCK;
    }

    /// @brief 该专精偏好的伴随护卫机制内核（外观池 + 技能通道）。
    ///        护卫实体在生成瞬间会回查宿主的专精偏好，因此术士三系必须在此分流：
    ///          - 默认（含未覆写的职业级兜底）走小鬼池；
    ///          - 痛苦术覆写为地狱犬、恶魔术覆写为恶魔卫士。
    ///        否则三系术士会被统一灌成小鬼内核，与专精契约完全脱节。
    virtual GuardianVisualType GetPreferredGuardianVisualType() const
    {
        switch (me->getClass())
        {
            case CLASS_HUNTER:       return GUARDIAN_VISUAL_HUNTER_BEAST;
            case CLASS_DEATH_KNIGHT: return GUARDIAN_VISUAL_DK_UNDEAD;
            case CLASS_WARLOCK:      return GUARDIAN_VISUAL_WARLOCK_IMP;
            default:                 return GUARDIAN_VISUAL_HUNTER_BEAST;
        }
    }

    /// @brief 护卫存在性仲裁与幂等补招。
    ///        仅在本体存活、处于世界内且专精需要护卫时执行；句柄失效
    ///        （随从被销毁 / 阵亡 / 跨地图丢失）时清除句柄并重新召唤，
    ///        保证机器人任意时刻都有一名合法护卫跟随。
    void EnsureGuardianAlive()
    {
        if (!me->IsAlive() || !me->IsInWorld() || !ShouldHaveGuardian())
            return;

        if (!guardianGuid.IsEmpty())
        {
            Creature* guardian = ObjectAccessor::GetCreature(*me, guardianGuid);
            if (guardian && guardian->IsAlive() && guardian->GetMap() == me->GetMap())
                return;

            guardianGuid.Clear();
        }

        if (TempSummon* summon = me->SummonCreature(NPC_BOT_GUARDIAN, me->GetPosition(), TEMPSUMMON_MANUAL_DESPAWN))
        {
            guardianGuid = summon->GetGUID();

            // 归属绑定与属性镜像：护卫必须与宿主同阵营、同位面，
            // 且被显式标记为宿主所属单位，避免被视作野生怪物而遭敌对势力攻击。
            summon->SetOwnerGUID(me->GetGUID());
            summon->SetCreatorGUID(me->GetGUID());
            summon->SetFaction(me->GetFaction());
            summon->SetPhaseMask(me->GetPhaseMask(), true);

            // 归属绑定后再复位一次：SummonCreature 内部的首次 AI 初始化发生在我们
            // 写入 OwnerGUID / Faction / PhaseMask 之前，此时随从无法正确推断主人
            // 职业与阵营，必须以绑定后的快照重跑完整初始化，否则术士会拿到猎人
            // 内核（近战白字）而彻底失去远程读条能力。
            if (CreatureAI* guardianAI = summon->AI())
                guardianAI->Reset();
        }
    }

    // =========================================================================
    // 阶段三：世界常驻实体的物理身份标识
    // =========================================================================
    /// @brief 获取随从在 world 数据库 creature 表中的物理身份 spawnId。
    ///        只有由 .sql 世界刷新出来的常驻 NPC 才具备有效 spawnId；
    ///        运行时 SummonCreature 的临时实体（含伴随护卫）恒返回 0，
    ///        据此天然隔离「可跨战斗积累经验的常驻随从」与「一次性召唤物」。
    uint32 GetBotSpawnId() const
    {
        // AzerothCore 已移除 GetDBTableGUIDLow()，世界常驻实体的数据库物理 GUID
        // 唯一合法入口为 GetSpawnId()：
        //   - 由 .sql 世界刷新出来的常驻 NPC 返回非零 spawnId；
        //   - 运行时 SummonCreature 的临时实体（含伴随护卫）恒返回 0。
        // 据此天然隔离「可跨战斗积累经验的常驻随从」与「一次性召唤物」。
        return static_cast<uint32>(me->GetSpawnId());
    }

    // =========================================================================
    // 阶段三：Boss 认知记忆预热（战前 O(1) 单行读取）
    // =========================================================================
    /// @brief 将个体认知档案中的危险禁区与打断余量灌入运行时状态。
    ///        老兵随从首次面对该首领即可提前避火，无需再拿命试错。
    void PreloadBossKnowledge(uint32 bossEntry)
    {
        uint32 const spawnId = GetBotSpawnId();
        if (spawnId == 0 || bossEntry == 0)
            return;

        BotCognitionRecord record;
        if (!sBotMemory->LoadBotKnowledge(spawnId, bossEntry, record))
            return;

        uint32 const currentMapId = me->GetMapId();

        for (auto const& learned : record.learnedHazards)
        {
            bool merged = false;
            for (auto& existing : activeDangerZones)
            {
                // 顺劈锥形动态依附首领本体、不占固定坐标，同技能即视为同源直接刷新；
                // 只有地面圆形火圈/毒池才需要比对几何间距。
                bool const isSameCone = (learned.type == DangerZoneType::FRONTAL_CONE && existing.spellId == learned.spellId);
                float const dx = existing.x - learned.x;
                float const dy = existing.y - learned.y;
                bool const isSameCircle = (existing.spellId == learned.spellId && (dx * dx + dy * dy) < 16.0f);

                if (isSameCone || isSameCircle)
                {
                    existing.mapId = currentMapId;
                    existing.durationMs = 600000;
                    existing.x = learned.x;
                    existing.y = learned.y;
                    existing.z = learned.z;
                    existing.radius = std::max(existing.radius, learned.radius);
                    existing.type = std::max(existing.type, learned.type);
                    merged = true;
                    break;
                }
            }

            if (!merged)
            {
                DangerZone zone = learned;
                // 数据库不持久化地图归属：跨副本复用时必须由运行时盖章，
                // 否则 mapId 为 0 会被 IsUnderDangerThreat() 视作全地图生效。
                zone.mapId = currentMapId;
                zone.durationMs = 600000;
                activeDangerZones.push_back(zone);
            }
        }

        learnedInterruptDelays = record.interruptDelays;

        if (isDebugLogging)
            LOG_INFO("scripts", "[Bot: {}] 已从个体认知库预热 Boss [{}] 经验: 熟练度 {}/3 | 危险区 {} 处 | 打断档案 {} 条 | 历史 {} 次灭团 / {} 次击杀",
                me->GetName(), bossEntry, static_cast<uint32>(record.proficiencyLevel),
                record.learnedHazards.size(), record.interruptDelays.size(),
                record.wipeCount, record.killCount);
    }

    /// @brief 进战瞬间的记忆预热保险丝（按 Boss 句柄去重，同场战斗只读库一次）。
    void TryPreloadBossKnowledge(Unit* who)
    {
        if (!who || who->GetTypeId() != TYPEID_UNIT)
            return;

        Creature* targetCreature = who->ToCreature();
        if (!targetCreature)
            return;

        if (who->GetGUID() == memoryPreloadTargetGuid)
            return;

        // 首领防抢占锁：若已预热过首领且该首领依然存活，
        // 战斗途中转火杂兵小怪时严禁覆盖句柄，确保战后归因准确落盘至该首领档案中。
        if (!memoryPreloadTargetGuid.IsEmpty())
        {
            if (Creature* currentBoss = ObjectAccessor::GetCreature(*me, memoryPreloadTargetGuid))
            {
                if (currentBoss->IsAlive() && (currentBoss->isWorldBoss() || currentBoss->IsDungeonBoss()))
                {
                    if (!targetCreature->isWorldBoss() && !targetCreature->IsDungeonBoss())
                        return;
                }
            }
        }

        memoryPreloadTargetGuid = who->GetGUID();
        PreloadBossKnowledge(targetCreature->GetEntry());
    }

    void AttackStart(Unit* victim) override
    {
        if (!victim)
            return;

        if (IsRangedBot())
        {
            if (me->Attack(victim, false))
                me->GetMotionMaster()->Clear();
        }
        else
        {
            me->Attack(victim, true);
        }
    }

    // =========================================================================
    // 最终伤害与受疗管线拦截
    // =========================================================================
    void DamageDealt(Unit* doneTo, uint32& damage, DamageEffectType damagetype, SpellSchoolMask damageSchoolMask) override
    {
        damage = static_cast<uint32>(damage * GetDamageDealtMultiplier());

        if (me->IsInCombat() && damage > 0)
        {
            BotCombatEvent ev;
            ev.combatTimeMs = combatTimerMs;
            ev.eventType = BotCombatEventType::DAMAGE_DEALT;
            ev.amount = damage;
            ev.sourceGuid = doneTo ? doneTo->GetGUID() : ObjectGuid::Empty;
            ev.schoolMask = static_cast<uint8>(damageSchoolMask);
            combatEventBuffer.Push(ev);
        }

        ScriptedAI::DamageDealt(doneTo, damage, damagetype, damageSchoolMask);
    }

    void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType damagetype, SpellSchoolMask damageSchoolMask) override
    {
        // 受击唤醒保险丝：休息态为纯门禁标记，若不在受击瞬间解除，
        // 被巡逻怪偷袭的随从会因锁死索敌与技能而原地挨打直至阵亡。
        if (isResting)
        {
            isResting = false;
            me->HandleEmoteCommand(EMOTE_STATE_STAND);
        }

        damage = static_cast<uint32>(damage * GetDamageTakenMultiplier());

        if (me->IsInCombat())
        {
            BotCombatEvent ev;
            ev.combatTimeMs = combatTimerMs;
            ev.eventType = (damage >= me->GetHealth()) ? BotCombatEventType::LETHAL_DAMAGE : BotCombatEventType::DAMAGE_TAKEN;
            ev.amount = damage;
            ev.x = me->GetPositionX();
            ev.y = me->GetPositionY();
            ev.z = me->GetPositionZ();
            ev.sourceGuid = attacker ? attacker->GetGUID() : ObjectGuid::Empty;
            ev.schoolMask = static_cast<uint8>(damageSchoolMask);
            combatEventBuffer.Push(ev);
        }

        ScriptedAI::DamageTaken(attacker, damage, damagetype, damageSchoolMask);
    }

    void HealReceived(Unit* done_by, uint32& addhealth) override
    {
        addhealth = static_cast<uint32>(addhealth * GetHealingReceivedMultiplier());
        ScriptedAI::HealReceived(done_by, addhealth);
    }

    void SpellHit(Unit* caster, SpellInfo const* spell) override
    {
        ScriptedAI::SpellHit(caster, spell);

        // 归因采样：仅记录敌方「有读条」的非正向法术命中，作为漏打断审计口径
        if (me->IsInCombat() && spell && !spell->IsPositive() && spell->CalcCastTime() > 0)
        {
            BotCombatEvent ev;
            ev.combatTimeMs = combatTimerMs;
            ev.eventType = BotCombatEventType::SPELL_HIT_TAKEN;
            ev.spellId = spell->Id;
            ev.x = me->GetPositionX();
            ev.y = me->GetPositionY();
            ev.z = me->GetPositionZ();
            ev.sourceGuid = caster ? caster->GetGUID() : ObjectGuid::Empty;
            ev.schoolMask = static_cast<uint8>(spell->GetSchoolMask());
            combatEventBuffer.Push(ev);
        }

        OnSpellHitTaken(caster, spell);
    }

    void JustDied(Unit* killer) override
    {
        ScriptedAI::JustDied(killer);
        if (isDebugLogging)
            LOG_INFO("scripts", "[Bot: {}] 阵亡！致命伤害来源: [{}]", me->GetName(), killer ? killer->GetName() : "未知/环境伤害");

        OnBotDied(killer);
    }

    void KilledUnit(Unit* victim) override
    {
        ScriptedAI::KilledUnit(victim);
        if (isDebugLogging)
            LOG_INFO("scripts", "[Bot: {}] 击杀目标: [{}]", me->GetName(), victim ? victim->GetName() : "未知");

        OnKilledUnit(victim);
    }

    void JustEngagedWith(Unit* who) override
    {
        // 同上：引擎因协助/受击把随从拖入战斗时，同样必须立即解除休息门禁，
        // 否则随从会以「休整中」姿态在战斗中彻底挂机。
        if (isResting)
        {
            isResting = false;
            me->HandleEmoteCommand(EMOTE_STATE_STAND);
        }

        ScriptedAI::JustEngagedWith(who);

        // 阶段三：无论开怪是由 TryEngageCombat 主动发起，还是引擎因受击/协助
        // 把随从拖进战斗，都在进战瞬间补齐一次 Boss 认知预热。
        TryPreloadBossKnowledge(who);

        OnEngaged(who);
    }

    virtual void OnSpellHitTaken(Unit* /*caster*/, SpellInfo const* /*spell*/) {}
    virtual void OnBotDied(Unit* /*killer*/) {}
    virtual void OnKilledUnit(Unit* /*victim*/) {}
    virtual void OnEngaged(Unit* /*who*/) {}

    virtual void OnCombatEnded(bool victory)
    {
        if (isDebugLogging)
            LOG_INFO("scripts", "[Bot: {}] 战斗结算完成: {}", me->GetName(), victory ? "击杀胜利" : "团灭重置");

        // =====================================================================
        // 中层：战斗回溯与归因分析（仅战斗终结瞬间一次性执行，零运行时开销）
        // =====================================================================
        if (!combatEventBuffer.Empty())
        {
            // 结算时 GetVictim() 可能已被清空，优先复用开怪瞬间预热的 Boss 句柄，
            // 保证认知档案永远落在正确的 (spawn_id, boss_entry) 复合主键上。
            uint32 bossEntry = 0;
            if (!memoryPreloadTargetGuid.IsEmpty())
                bossEntry = memoryPreloadTargetGuid.GetEntry();

            if (bossEntry == 0)
            {
                if (Unit* victim = me->GetVictim())
                {
                    if (Creature* creature = victim->ToCreature())
                        bossEntry = creature->GetEntry();
                }
            }

            AttributionReport const report = CombatAnalyzer::Analyze(combatEventBuffer, combatTimerMs, victory, bossEntry);

            if (isDebugLogging)
            {
                LOG_INFO("scripts", "================= [Post-Combat Attribution: {}] =================", me->GetName());
                LOG_INFO("scripts", "战斗结果: {} | 战斗耗时: {:.2f}s | 事件采样总数: {}",
                    report.isWipe ? "团灭脱战" : "击杀胜利", report.totalCombatTimeMs / 1000.0f, combatEventBuffer.Size());

                if (report.fatalDamage > 0)
                    LOG_INFO("scripts", "[维度1-致死归因] 致死法术ID: {} | 致命伤害: {} | 击杀者: {}",
                        report.fatalSpellId, report.fatalDamage, report.fatalSourceGuid.ToString());

                LOG_INFO("scripts", "[维度1-承伤峰值] 承受最高伤害技能ID: {} | 峰值伤害: {}",
                    report.peakDamageSpellId, report.peakDamage);

                LOG_INFO("scripts", "[维度2-漏断审计] 承受敌方未打断施法总数: {} 次 (末次法术ID: {})",
                    report.missedInterruptsCount, report.lastMissedSpellId);

                LOG_INFO("scripts", "[维度3-起手OT] 前10秒是否OT: {} (OT触发时点: {} ms)",
                    report.earlyOtDetected ? "【是】" : "否", report.otTimeMs);

                LOG_INFO("scripts", "[维度4-仇恨速率] 主坦前10秒建立仇恨速率 (TPS): {:.1f}",
                    report.tankFirst10sTps);

                if (!report.derivedDangerZones.empty())
                    LOG_INFO("scripts", "[空间感知] 逆向提炼动态危险区: {} 处 (已载入避险势场)", report.derivedDangerZones.size());

                LOG_INFO("scripts", "==================================================================");
            }

            // 知识记忆池更新：
            if (victory)
            {
                // 首领击杀胜利：清空危险禁区记忆，避免把上一任首领的机制禁区
                // 误带入后续杂兵战与下一场首领战。
                activeDangerZones.clear();
            }
            else
            {
                // 灭团失败：增量合并新提炼的危险禁区。若直接整体赋值，
                // 第二把死于平砍（derivedDangerZones 为空）会彻底抹除第一把
                // 学到的火圈知识；此处改为同图同技能近距刷新 + 异技能累加。
                uint32 const currentMapId = me->GetMapId();
                for (auto const& newZone : report.derivedDangerZones)
                {
                    bool merged = false;
                    for (auto& existing : activeDangerZones)
                    {
                        // 顺劈锥形动态依附首领本体、不占固定坐标，同技能即视为同源直接刷新；
                        // 地面圆形火圈才比对几何间距。此前统一按距离比对时，
                        // 顺劈区因首领位移/随从跑尸反复失配，多轮灭团后斥力线性暴涨。
                        bool const isSameCone = (newZone.type == DangerZoneType::FRONTAL_CONE && existing.spellId == newZone.spellId);
                        float const dx = existing.x - newZone.x;
                        float const dy = existing.y - newZone.y;
                        bool const isSameCircle = (existing.spellId == newZone.spellId && (dx * dx + dy * dy) < 16.0f);

                        if (existing.mapId == currentMapId && (isSameCone || isSameCircle))
                        {
                            existing.durationMs = 600000; // 同源机制刷新为 10 分钟长效记忆
                            existing.x = newZone.x;
                            existing.y = newZone.y;
                            existing.z = newZone.z;
                            existing.radius = std::max(existing.radius, newZone.radius);
                            existing.type = std::max(existing.type, newZone.type);
                            merged = true;
                            break;
                        }
                    }

                    if (!merged)
                    {
                        DangerZone zone = newZone;
                        zone.mapId = currentMapId;
                        zone.durationMs = 600000; // 赋予 10 分钟跨战斗长效记忆
                        activeDangerZones.push_back(zone);
                    }
                }
            }

            // =================================================================
            // 阶段三：顶层持久化记忆库落盘
            // 主线程此处只做一次 AttributionReport 深拷贝入队（< 1 微秒），
            // 真正的 INSERT/UPDATE 与 fsync 全部由后台工作线程在单一事务中完成，
            // 绝不阻塞游戏主世界心跳。仅「世界常驻实体」的认知才会落盘。
            // =================================================================
            uint32 const spawnId = GetBotSpawnId();
            if (spawnId > 0 && bossEntry != 0)
                sBotMemory->EnqueuePersistTask(spawnId, bossEntry, me->GetMapId(), report);
        }

        // 战后秒补：护卫在团本 AoE 中阵亡属常态，战斗结算瞬间立即补齐，
        // 保证下一场战斗（连战 / 转阶段）拥有完整的机制内核与增益覆盖。
        EnsureGuardianAlive();
    }

    // =========================================================================
    // 装备装等异步缓存计算器 (仅在脱战同步时调用)
    // =========================================================================
    static float CalculateMasterAverageItemLevel(Player* player)
    {
        if (!player)
            return 200.0f;

        uint32 totalIlvl = 0;
        uint32 validCount = 0;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_TABARD || slot == EQUIPMENT_SLOT_BODY)
                continue;

            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                if (ItemTemplate const* proto = item->GetTemplate())
                {
                    totalIlvl += proto->ItemLevel;
                    ++validCount;
                }
            }
        }

        return (validCount > 0) ? (static_cast<float>(totalIlvl) / validCount) : 200.0f;
    }

    // =========================================================================
    // 工业级全专精双轨制属性自适应引擎 (攻/防/疗/血量全面闭环)
    // =========================================================================
    void ApplyAdaptiveBalanceStats()
    {
        uint8 const level = me->GetLevel();

        // ---------------------------------------------------------------------
        // 轨道 A: 1 ~ 79 级（自强练级与普通地下城平滑过渡）
        // ---------------------------------------------------------------------
        if (level < 80)
        {
            uint32 const baseArmor = level * (IsTankBot() ? 120 : 40);
            me->SetArmor(baseArmor);

            // 1~79 级治疗专精：平滑注入系统治疗增效光环，弥补零装备法强
            if (IsHealerBot())
            {
                int32 const healPercent = static_cast<int32>((level / 80.0f) * 60.0f);
                if (healPercent > 0)
                {
                    me->RemoveAurasDueToSpell(23569);
                    me->CastCustomSpell(me, 23569, &healPercent, nullptr, nullptr, true);
                }
            }
            else
            {
                me->RemoveAurasDueToSpell(23569);
            }
            return;
        }

        // ---------------------------------------------------------------------
        // 轨道 B: 80 级团本与英雄本专修（锚定指挥官装等）
        // ---------------------------------------------------------------------
        float const tierDelta = std::clamp(cachedMasterItemLevel - 200.0f, 0.0f, 84.0f);

        // 1. 坦克生存有效生命 (EHP) 与护甲免伤线 (65% ~ 75% 免伤)
        if (IsTankBot())
        {
            uint32 const targetMaxHealth = static_cast<uint32>(34000 + tierDelta * 220.0f);
            if (me->GetMaxHealth() != targetMaxHealth)
            {
                float const hpPct = me->GetHealthPct();
                me->SetMaxHealth(targetMaxHealth);
                me->SetHealth(std::max<uint32>(1, static_cast<uint32>(targetMaxHealth * (hpPct / 100.0f))));
            }

            uint32 const raidArmor = static_cast<uint32>(26000 + tierDelta * 160.0f);
            me->SetArmor(raidArmor);
            me->RemoveAurasDueToSpell(23569);
        }
        else
        {
            // 2. 非坦随从注入匹配装等的标准血量 (18k~25k)，防止吃团本 AoE 暴毙
            uint32 const targetMaxHealth = static_cast<uint32>(18000 + tierDelta * 80.0f);
            if (me->GetMaxHealth() != targetMaxHealth)
            {
                float const hpPct = me->GetHealthPct();
                me->SetMaxHealth(targetMaxHealth);
                me->SetHealth(std::max<uint32>(1, static_cast<uint32>(targetMaxHealth * (hpPct / 100.0f))));
            }

            // 3. 治疗专精：注入系统治疗增效光环 (80%~180% 提升)，彻底激活团补吞吐
            if (IsHealerBot())
            {
                int32 const healPercent = static_cast<int32>(80 + (tierDelta / 84.0f) * 100);
                me->RemoveAurasDueToSpell(23569);
                me->CastCustomSpell(me, 23569, &healPercent, nullptr, nullptr, true);
            }
            else
            {
                me->RemoveAurasDueToSpell(23569);

                // 4. 物理输出 (近战 DPS + 猎人)：同步注入近战与远程攻击强度
                float const raidAP = 3200.0f + tierDelta * 48.0f;
                me->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER, BASE_VALUE, raidAP);
                me->SetStatFlatModifier(UNIT_MOD_ATTACK_POWER_RANGED, BASE_VALUE, raidAP);
                me->UpdateAttackPowerAndDamage();
            }
        }
    }

    // =========================================================================
    // 基础属性与等级同步引擎
    // =========================================================================
    void SyncLevelWithMaster()
    {
        Player* master = GetMaster();
        if (!master)
            return;

        cachedMasterItemLevel = CalculateMasterAverageItemLevel(master);

        uint8 const masterLevel = master->GetLevel();
        bool const levelChanged = (me->GetLevel() != masterLevel);

        if (levelChanged)
        {
            me->SetLevel(masterLevel);
            me->SetHealth(me->GetMaxHealth());

            if (me->getPowerType() == POWER_MANA)
            {
                me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA));
            }
            else if (me->getPowerType() == POWER_ENERGY)
            {
                if (me->GetMaxPower(POWER_ENERGY) == 0)
                    me->SetMaxPower(POWER_ENERGY, 100);
                me->SetPower(POWER_ENERGY, me->GetMaxPower(POWER_ENERGY));
            }

            if (isDebugLogging)
                LOG_INFO("scripts", "[Bot: {}] 等级已同步至 [{}] 级。", me->GetName(), masterLevel);
        }

        ApplyAdaptiveBalanceStats();

        if (levelChanged)
            OnLevelSynced(me->GetLevel());
    }

    virtual void OnLevelSynced(uint8 /*level*/) {}

    // =========================================================================
    // 核心开怪进战状态机驱动 (彻底消除远程开怪挂机发呆)
    // =========================================================================
    bool TryEngageCombat()
    {
        // 休息状态禁止主动开怪：即便指挥官已身处交战中心，
        // 被明确下令休整的随从也绝不自行卷入战斗。
        if (isResting)
            return false;

        if (me->IsInCombat())
            return true;

        Unit* target = SelectAssistTarget();
        if (target && target->IsAlive())
        {
            // 阶段三：开怪锁定首领瞬间完成记忆预热（主键索引 O(1)，
            // 且由 memoryPreloadTargetGuid 去重，连战期间不会反复读库）。
            TryPreloadBossKnowledge(target);

            AttackStart(target);
            me->SetInCombatWith(target);
            target->SetInCombatWith(me);
            return true;
        }
        return false;
    }

    // =========================================================================
    // 跨目标紧急仇恨监控
    // =========================================================================
    Unit* GetUrgentThreatTarget(float maxRange = 30.0f)
    {
        Player* master = GetMaster();
        if (!master)
            return nullptr;

        Unit* groupTank = GetGroupTank();

        auto CheckUnitAttackers = [&](Unit* friendlyUnit) -> Unit*
        {
            if (!friendlyUnit || !friendlyUnit->IsAlive() || !friendlyUnit->IsInWorld())
                return nullptr;

            if (friendlyUnit->GetMap() != me->GetMap() || !friendlyUnit->IsFriendlyTo(me))
                return nullptr;

            if (groupTank && friendlyUnit == groupTank)
                return nullptr;

            for (Unit* attacker : friendlyUnit->getAttackers())
            {
                if (attacker && attacker->IsAlive() && attacker->IsInWorld() && attacker->GetMap() == me->GetMap())
                {
                    if (attacker != me && (!groupTank || attacker->GetVictim() != groupTank))
                    {
                        if (me->IsWithinDist(attacker, maxRange) && me->IsWithinLOSInMap(attacker))
                            return attacker;
                    }
                }
            }
            return nullptr;
        };

        if (Unit* target = CheckUnitAttackers(master))
            return target;
        if (Unit* target = CheckUnitAttackers(master->GetPet()))
            return target;

        if (Group* group = master->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                if (Player* member = itr->GetSource())
                {
                    if (member != master && member->GetMap() == me->GetMap() && me->IsWithinDist(member, maxRange))
                    {
                        if (Unit* target = CheckUnitAttackers(member))
                            return target;
                        if (Unit* target = CheckUnitAttackers(member->GetPet()))
                            return target;
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
                    if (allyBot && allyBot->me && allyBot->me != me)
                    {
                        if (Unit* target = CheckUnitAttackers(allyBot->me))
                            return target;
                    }
                }
            }
        }

        return nullptr;
    }

    // =========================================================================
    // 治疗专精友方血网雷达
    // =========================================================================
    Unit* SelectLowestHealthAlly(float maxRange = 40.0f, uint8 healthThreshold = 100)
    {
        Player* master = GetMaster();
        if (!master)
            return nullptr;

        Unit* lowestTarget = nullptr;
        float lowestHp = static_cast<float>(healthThreshold);

        auto CheckUnit = [&](Unit* unit)
        {
            if (!unit || !unit->IsAlive() || !unit->IsInWorld())
                return;

            if (unit->GetMap() != me->GetMap())
                return;

            if (!unit->IsFriendlyTo(me))
                return;

            // 排除图腾与伴随型战斗护卫（Combat Guardian）：护卫自身具备
            // IsCombatGuardian 语义且血量为投影值，绝不进入治疗打地鼠雷达，
            // 杜绝治疗机器人把 GCD 浪费在护卫身上导致主坦断疗。
            if (unit->GetTypeId() == TYPEID_UNIT)
            {
                Creature* creature = unit->ToCreature();
                if (creature->IsTotem() || creature->GetScriptName() == "BotGuardianAI")
                    return;
            }

            if (me->IsWithinDist(unit, maxRange) && me->IsWithinLOSInMap(unit))
            {
                float const hp = unit->GetHealthPct();
                if (hp < lowestHp)
                {
                    lowestHp = hp;
                    lowestTarget = unit;
                }
            }
        };

        CheckUnit(me);
        CheckUnit(master);
        CheckUnit(master->GetPet());

        if (Group* group = master->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                if (Player* member = itr->GetSource())
                {
                    CheckUnit(member);
                    CheckUnit(member->GetPet());
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
                    if (allyBot && allyBot->me && allyBot->me != me)
                        CheckUnit(allyBot->me);
                }
            }
        }

        return lowestTarget;
    }

    // =========================================================================
    // 索敌仲裁器 (含防引怪灭团守卫)
    // =========================================================================
    virtual Unit* SelectAssistTarget()
    {
        if (Unit* urgentTarget = GetUrgentThreatTarget())
        {
            if (me->IsValidAttackTarget(urgentTarget))
            {
                if (isDebugLogging && me->GetVictim() != urgentTarget)
                    LOG_INFO("scripts", "[Bot: {}] 仇恨雷达触发紧急换防/拆火 -> [{}]", me->GetName(), urgentTarget->GetName());
                return urgentTarget;
            }
        }

        Player* master = GetMaster();

        if (master)
        {
            if (Unit* masterTarget = master->GetSelectedUnit())
            {
                if (masterTarget->IsAlive() && masterTarget != me && masterTarget->GetMap() == me->GetMap() && me->IsValidAttackTarget(masterTarget))
                {
                    bool const isEngaged = masterTarget->IsInCombat() || master->GetVictim() == masterTarget;
                    if (isEngaged)
                        return masterTarget;
                }
            }
        }

        if (Unit* tank = GetGroupTank())
        {
            if (tank != me)
            {
                if (Unit* tankVictim = tank->GetVictim())
                {
                    if (tankVictim->IsAlive() && tankVictim->GetMap() == me->GetMap() && me->IsValidAttackTarget(tankVictim))
                        return tankVictim;
                }
            }
        }

        if (Unit* myAttacker = me->getAttackerForHelper())
        {
            if (myAttacker->IsAlive() && myAttacker->GetMap() == me->GetMap() && me->IsValidAttackTarget(myAttacker))
                return myAttacker;
        }

        if (Unit* threatVictim = me->SelectVictim())
        {
            if (threatVictim->IsAlive() && threatVictim->GetMap() == me->GetMap() && me->IsValidAttackTarget(threatVictim))
                return threatVictim;
        }

        if (isDebugLogging && me->IsInCombat())
            LOG_INFO("scripts", "[Bot: {}] 索敌失败：当前小队与自身仇恨列表中均无合法可攻击目标。", me->GetName());

        return me->GetVictim();
    }

    // =========================================================================
    // 近战平砍与追击守护 (支持动态切目标与消除跟随锁死)
    // =========================================================================
    void ManageMeleeCombat(Unit* victim)
    {
        if (!victim || !victim->IsAlive() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        // 优先由 APF 人工势场接管走位：势场单步外推为 3.0 码定长，
        // 若按「距离 > 0.8f」放行，服务端每 50ms 心跳都会重新下发 MovePoint，
        // 起跑动画被无限掐断重置，表现为原地剧烈抽搐。故改为 300ms 帧节流：
        // 仅在节流窗口结束、或当前已脱离 POINT 生成器（被打断/被抢占）时才重算。
        bool const threatened = IsUnderDangerThreat(2.0f);
        if ((threatened || !activeDangerZones.empty()) && !me->HasUnitState(UNIT_STATE_CHARGING))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;
                if (PotentialField::CalculateNextPosition(me, victim, 2.0f, !IsTankBot(), IsTankBot(), activeDangerZones, nextX, nextY, nextZ))
                {
                    if (me->GetVictim() != victim || !me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                        me->Attack(victim, true);

                    me->GetMotionMaster()->MovePoint(1, nextX, nextY, nextZ);
                    apfMoveUpdateTimer = 300;
                    return;
                }
            }
            else
            {
                return; // 正在平滑执行上一个 APF 导航航点，不进行路径打断
            }

            // 核心关键修复：
            // 1. 若自身仍在危险区覆盖边缘 (threatened)，就地站桩挥砍，
            //    杜绝向下击穿 MoveChase 把随从拉回火圈形成溜溜球折返；
            // 2. 若已被势场引导进近战攻击范围且合力衰减（说明绕背就位完成），
            //    同样就地挥砍，严禁 MoveChase 把随从贴向首领正中心，
            //    否则已站好的背后/侧翼攻击位会被破坏并引发原地滑步抖动。
            if (threatened || me->IsWithinMeleeRange(victim))
            {
                if (me->GetVictim() != victim || !me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    me->Attack(victim, true);

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
                    me->GetMotionMaster()->Clear();

                return;
            }
        }

        if (me->GetVictim() != victim)
        {
            me->Attack(victim, true);
            if (!me->HasUnitState(UNIT_STATE_CHARGING))
                me->GetMotionMaster()->MoveChase(victim);
            return;
        }

        if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            me->Attack(victim, true);

        if (!me->HasUnitState(UNIT_STATE_CHARGING))
        {
            MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
            if (moveType != CHASE_MOTION_TYPE && moveType != POINT_MOTION_TYPE)
            {
                me->GetMotionMaster()->MoveChase(victim);
            }
        }
    }

    // =========================================================================
    // 远程物理平射与放风筝雷达
    // =========================================================================
    void ManageRangedPhysicalCombat(Unit* victim, float minDist = 5.0f, float maxDist = 30.0f)
    {
        if (!victim || !victim->IsAlive() || victim->GetMap() != me->GetMap())
            return;

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        bool const targetChanged = (me->GetVictim() != victim);
        me->SetFacingToObject(victim);
        float const dist = me->GetDistance(victim);

        // 动态避险：仅当猎人自身落入火圈/毒水时，才由势场规划安全射击位；
        // 若火圈在别处而自身安全，则坚决站桩平射，避免被友军斥力推着移步。
        if (IsUnderDangerThreat(2.0f))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;
                float const optDist = std::clamp(dist, minDist + 3.0f, maxDist - 3.0f);
                if (PotentialField::CalculateNextPosition(me, victim, optDist, false, false, activeDangerZones, nextX, nextY, nextZ))
                {
                    me->GetMotionMaster()->MovePoint(1, nextX, nextY, nextZ);
                    apfMoveUpdateTimer = 300;
                    return;
                }
            }
            else
            {
                return; // 正在平滑执行 APF 避险航点，暂不打断
            }
        }

        if (dist < minDist)
        {
            if (targetChanged || !me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                me->Attack(victim, true);

            if (targetChanged || me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE)
                me->GetMotionMaster()->MoveChase(victim);
            return;
        }

        if (targetChanged || me->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            me->Attack(victim, false);

        if (dist >= minDist && dist <= maxDist)
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                me->GetMotionMaster()->Clear();
            if (me->isMoving())
                me->StopMoving();
            return;
        }

        if (dist > maxDist)
        {
            MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
            if (targetChanged || (moveType != CHASE_MOTION_TYPE && moveType != POINT_MOTION_TYPE))
            {
                me->GetMotionMaster()->MoveChase(victim, maxDist - 2.0f);
            }
        }
    }

    // =========================================================================
    // 远程法系站桩与射程/视线守护
    // =========================================================================
    void ManageCasterCombat(Unit* victim, float maxRange = 30.0f)
    {
        if (!victim || !victim->IsAlive() || victim->GetMap() != me->GetMap())
            return;

        // 引导类法术与读条一同置顶守卫：势场走位的 MovePoint 指令会掐断引导通道，
        // 若仅拦截 UNIT_STATE_CASTING，法系会在吸取灵魂/精神鞭笞中途被迫断条。
        if (me->HasUnitState(UNIT_STATE_CASTING) || me->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            return;

        bool const targetChanged = (me->GetVictim() != victim);
        if (targetChanged)
            me->Attack(victim, false);

        // 动态避险：法系远程仅在自身踏入火圈/毒水时才移步，安全距离坚决站桩读条
        if (IsUnderDangerThreat(2.0f))
        {
            if (apfMoveUpdateTimer == 0 || me->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            {
                float nextX = 0.0f, nextY = 0.0f, nextZ = 0.0f;
                float const optDist = std::clamp(me->GetDistance(victim), 18.0f, maxRange - 4.0f);
                if (PotentialField::CalculateNextPosition(me, victim, optDist, false, false, activeDangerZones, nextX, nextY, nextZ))
                {
                    me->GetMotionMaster()->MovePoint(1, nextX, nextY, nextZ);
                    apfMoveUpdateTimer = 300;
                    return;
                }
            }
            else
            {
                return; // 正在平滑执行 APF 避险航点，暂不打断
            }
        }

        bool const outOfRange = !me->IsWithinCombatRange(victim, maxRange);
        bool const outOfLos   = !me->IsWithinLOSInMap(victim);

        if (outOfRange || outOfLos)
        {
            MovementGeneratorType const moveType = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
            if (targetChanged || (moveType != CHASE_MOTION_TYPE && moveType != POINT_MOTION_TYPE))
            {
                me->GetMotionMaster()->MoveChase(victim, std::max(5.0f, maxRange - 5.0f));
            }
        }
        else
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
            {
                me->GetMotionMaster()->Clear();
                me->StopMoving();
            }
            me->SetFacingToObject(victim);
        }
    }

    // 连击点状态机
    uint8 GetComboPoints(Unit* target) const
    {
        if (!target || target->GetGUID() != comboTargetGuid)
            return 0;
        return comboPoints;
    }

    void AddComboPoints(Unit* target, uint8 count = 1)
    {
        if (!target)
            return;

        if (target->GetGUID() != comboTargetGuid)
        {
            comboTargetGuid = target->GetGUID();
            comboPoints = 0;
        }
        comboPoints = std::min<uint8>(5, comboPoints + count);
    }

    void SpendComboPoints()
    {
        comboPoints = 0;
    }

    void ResetComboPoints()
    {
        comboPoints = 0;
        comboTargetGuid.Clear();
    }

    bool IsBreakableCC(Unit* target) const
    {
        if (!target)
            return false;

        return target->HasAuraType(SPELL_AURA_MOD_CONFUSE) ||
               target->HasAuraType(SPELL_AURA_TRANSFORM) ||
               target->HasAuraType(SPELL_AURA_MOD_FEAR);
    }

    // =========================================================================
    // 姿态/形态豁免 Hook
    // =========================================================================
    virtual bool CheckShapeshiftExemption(SpellInfo const* spellInfo) const
    {
        if (me->HasAura(57499) && (spellInfo->SpellFamilyFlags[0] & 0x5 || spellInfo->SpellFamilyFlags[1] & 0x40))
            return true;

        if (me->HasAura(51713) && (spellInfo->SpellFamilyFlags[0] & 0x2080000))
            return true;

        if (me->HasAura(16886) || me->HasAura(16188))
        {
            if (spellInfo->IsPositive())
                return true;
        }

        return false;
    }

    virtual bool CheckSpecialPowerRequirements(SpellInfo const* /*spellInfo*/) const { return true; }
    virtual uint8 GetTalentSpellMinLevel(uint32 /*spellId*/) const { return 0; }

    // =========================================================================
    // 全战术机制降阶与 Rank 1 保底引擎
    // =========================================================================
    uint32 GetAppropriateRank(uint32 maxRankSpellId, bool allowRankOneFallback = true) const
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(maxRankSpellId);
        if (!spellInfo)
            return 0;

        uint8 const botLevel = me->GetLevel();
        uint8 const talentMinLevel = GetTalentSpellMinLevel(maxRankSpellId);

        if (!allowRankOneFallback && talentMinLevel > 0 && botLevel < talentMinLevel)
            return 0;

        uint32 lastValidRankId = spellInfo->Id;

        while (spellInfo && spellInfo->SpellLevel > botLevel)
        {
            lastValidRankId = spellInfo->Id;
            SpellInfo const* prevRankSpell = spellInfo->GetPrevRankSpell();
            if (!prevRankSpell)
                return allowRankOneFallback ? lastValidRankId : 0;

            spellInfo = prevRankSpell;
        }

        return spellInfo ? spellInfo->Id : (allowRankOneFallback ? lastValidRankId : 0);
    }

    // =========================================================================
    // 通用施法条件仲裁 (含 verbose 诊断日志)
    // =========================================================================
    bool CanCast(Unit* target, uint32 spellId, bool checkGcd = true, bool verbose = false) const
    {
        auto LogBlock = [this, verbose, spellId](char const* reason) -> bool
        {
            if (verbose && isDebugLogging)
                LOG_INFO("scripts", "[Bot: {}] CanCast 阻断 [SpellID: {}] 原因: {}", me->GetName(), spellId, reason);
            return false;
        };

        // 休息状态为最高优先级门禁：必须在能量/射程/冷却/LoS 等一切判定之前短路，
        // 确保休息期间连瞬发技能也无法穿透释放，彻底锁死进攻意图。
        if (isResting)
            return LogBlock("随从正处于就地休息状态，锁死技能释放");

        if (!target || !target->IsInWorld() || target->GetMap() != me->GetMap() || spellId == 0)
            return LogBlock("目标空/不在世界/跨地图/法术ID为0");

        if (me->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING | UNIT_STATE_CASTING))
            return LogBlock("自身受控或正在施法/引导");

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->IsPassive())
            return LogBlock("法术元数据无效或为被动技能");

        // 避火移动优先保护：自身处于危险禁区覆盖范围内时，封锁一切需读条或引导的技能。
        // 否则 ExecuteSpell() 内部的 StopMoving() 会把正在逃跑的随从强行刹停站桩，
        // 而读条期间的 UNIT_STATE_CASTING 又反过来封锁 ManageCasterCombat 的走位通道，
        // 最终陷入「起跑 -> 刹车读条 -> 踩火暴毙」的死循环。
        // 瞬发技能（火冲、真言术：盾、回春术等）不在此列，依然允许边走边丢。
        bool const isNonInstant = (spellInfo->CalcCastTime() > 0 || spellInfo->IsChanneled());
        if (isNonInstant && IsUnderDangerThreat(2.0f))
            return LogBlock("自身处于危险区域中，封锁读条/引导以保证 APF 跑位优先");

        if (me->HasUnitState(UNIT_STATE_ROOT) && (spellInfo->HasEffect(SPELL_EFFECT_CHARGE) || spellInfo->HasEffect(SPELL_EFFECT_CHARGE_DEST)))
            return LogBlock("定身状态无法突进/冲锋");

        if (spellInfo->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING) && me->GetCurrentSpell(CURRENT_MELEE_SPELL))
            return LogBlock("平砍强化技能已在排队中");

        if (!CheckShapeshiftExemption(spellInfo))
        {
            if (spellInfo->CheckShapeshift(me->GetShapeshiftForm()) != SPELL_CAST_OK)
                return LogBlock("姿态/形态不匹配");
        }

        bool const isOffGcd = (spellInfo->StartRecoveryCategory == 0 && spellInfo->StartRecoveryTime == 0) || spellInfo->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING);
        if (checkGcd && !isOffGcd && gcdTimer > 0)
            return LogBlock("公共冷却 (GCD) 未就绪");

        if (me->HasSpellCooldown(spellId))
            return LogBlock("技能冷却中 (CD)");

        bool const isResurrect = spellInfo->HasEffect(SPELL_EFFECT_RESURRECT) || spellInfo->HasEffect(SPELL_EFFECT_RESURRECT_NEW);
        if (!isResurrect && !target->IsAlive())
            return LogBlock("非复活技能禁止对阵亡单位施放");
        if (isResurrect && target->IsAlive())
            return LogBlock("复活技能只能对阵亡单位施放");

        if (!CheckSpecialPowerRequirements(spellInfo))
            return LogBlock("未通过专精特殊资源校验 (如符文尚未冷却)");

        int32 cost = spellInfo->CalcPowerCost(me, spellInfo->GetSchoolMask());
        if (cost > 0)
        {
            if (spellInfo->PowerType == POWER_RAGE || spellInfo->PowerType == POWER_RUNIC_POWER)
                cost *= 10;

            if (me->GetPower(static_cast<Powers>(spellInfo->PowerType)) < static_cast<uint32>(cost))
                return LogBlock("当前能量/怒气/法力值不足");
        }

        if (target != me)
        {
            bool const isFriendlySpell = target->IsFriendlyTo(me) || spellInfo->IsPositive();

            if (spellInfo->GetMaxRange(false) <= 5.0f)
            {
                if (!me->IsWithinMeleeRange(target))
                    return LogBlock("超出近战攻击距离");
            }
            else
            {
                float const minRange = spellInfo->GetMinRange(isFriendlySpell);
                float const maxRange = spellInfo->GetMaxRange(isFriendlySpell);

                if (minRange > 0.0f && me->IsWithinDist(target, minRange))
                    return LogBlock("小于最小射程盲区");

                if (maxRange > 0.0f)
                {
                    if (isFriendlySpell)
                    {
                        if (!me->IsWithinDist(target, maxRange))
                            return LogBlock("超出友方几何最大射程");
                    }
                    else
                    {
                        if (!me->IsWithinCombatRange(target, maxRange))
                            return LogBlock("超出敌方战斗包围盒最大射程");
                    }
                }
                else if (!me->IsWithinMeleeRange(target))
                {
                    return LogBlock("默认射程超出近战范围");
                }
            }

            if (!me->IsWithinLOSInMap(target))
                return LogBlock("目标不在视线内 (LoS 阻挡)");
        }

        return true;
    }

    // =========================================================================
    // 通用施法执行引擎 (读条防掐断强化)
    // =========================================================================
    bool ExecuteSpell(Unit* target, uint32 spellId, bool applyGcd = true, Unit* facingTarget = nullptr)
    {
        if (!target || !target->IsInWorld() || target->GetMap() != me->GetMap())
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return false;

        if (facingTarget && facingTarget->IsInWorld() && facingTarget->GetMap() == me->GetMap())
            me->SetFacingToObject(facingTarget);
        else if (target != me && !spellInfo->IsPositive())
            me->SetFacingToObject(target);

        int32 castTime = int32(spellInfo->CalcCastTime());
        me->ModSpellCastTime(spellInfo, castTime);
        bool const isInstant = (castTime <= 0 && !spellInfo->IsChanneled());

        if (!isInstant)
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                me->GetMotionMaster()->Clear();
            if (me->isMoving())
                me->StopMoving();
        }

        SpellCastResult const result = me->CastSpell(target, spellId, false);
        if (result == SPELL_CAST_OK)
        {
            if (isDebugLogging)
                LOG_INFO("scripts", "[Bot: {}] 施法成功: [SpellID: {}] -> 目标: [{}]", me->GetName(), spellId, target->GetName());

            bool const isOffGcd = (spellInfo->StartRecoveryCategory == 0 && spellInfo->StartRecoveryTime == 0) || spellInfo->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING);
            if (applyGcd && !isOffGcd)
            {
                bool const isShortGcd = (me->getPowerType() == POWER_ENERGY || me->getPowerType() == POWER_RUNIC_POWER);
                gcdTimer = isShortGcd ? 1000 : 1500;
            }
            return true;
        }
        else
        {
            if (isDebugLogging)
                LOG_INFO("scripts", "[Bot: {}] 施法被底层拒绝: [SpellID: {}] -> 目标: [{}] | 引擎错误码: {}", me->GetName(), spellId, target->GetName(), uint32(result));
            return false;
        }
    }

    // =========================================================================
    // 阶段三：记忆化压秒打断仲裁引擎
    // =========================================================================
    /// @brief 检查指定目标是否正在读条，并根据记忆库中该技能的提前量判定是否到达打断时机。
    ///        引导类法术每跳均生效，零延时即刻抢断；读条法术严格匹配压秒窗口。
    bool ShouldInterruptTarget(Unit* target, uint32 interruptSpellId = 0)
    {
        if (!target || !target->IsAlive() || target->GetMap() != me->GetMap())
            return false;

        if (interruptSpellId != 0 && !CanCast(target, interruptSpellId))
            return false;

        if (!target->HasUnitState(UNIT_STATE_CASTING))
            return false;

        Spell* curSpell = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        bool isChanneled = false;
        if (!curSpell)
        {
            curSpell = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            isChanneled = true;
        }

        if (!curSpell)
            return false;

        SpellInfo const* spellInfo = curSpell->GetSpellInfo();
        if (!spellInfo)
            return false;

        uint32 const spellId = spellInfo->Id;

        // 通道引导类法术（暴风雪、苦修、精神鞭笞等）持续造成威胁，
        // 不存在压秒收益，必须零延时立刻抢断；此时打断技能的 CD 也天然被最大化利用。
        if (isChanneled || spellInfo->IsChanneled())
            return true;

        int32 const castTime = curSpell->GetCastTime();
        if (castTime <= 0)
            return false;

        // 常态进度由 UpdateTimers 每帧驱动，此处仅作目标切换时的兜底校准：
        // 绝不在查询路径清零 elapsed，否则打断 CD 期间积累的进度会被反复抹掉，
        // 导致技能一就绪就误判为「刚起手」而错过压秒窗口。
        if (target->GetGUID() != currentEnemyCastingGuid || spellId != currentEnemyCastingSpellId)
        {
            currentEnemyCastingGuid = target->GetGUID();
            currentEnemyCastingSpellId = spellId;
            currentEnemyCastingTotalMs = static_cast<uint32>(castTime);
            currentEnemyCastingElapsedMs = 0;
        }

        uint32 const remainingMs = (currentEnemyCastingTotalMs > currentEnemyCastingElapsedMs)
            ? (currentEnemyCastingTotalMs - currentEnemyCastingElapsedMs)
            : 0;

        // 读取该随从对该技能学到的出手余量（未录入时默认提前 350ms）
        uint32 leadTimeMs = 350;
        auto it = learnedInterruptDelays.find(spellId);
        if (it != learnedInterruptDelays.end())
            leadTimeMs = it->second;

        // 读条剩余时间进入余量窗口，判定为最佳打断时机
        return remainingMs <= leadTimeMs;
    }

    /// @brief 一键尝试打断：结合压秒时机检查、施法条件判定与法术释放
    bool TryInterrupt(Unit* target, uint32 interruptSpellId)
    {
        if (!target || interruptSpellId == 0)
            return false;

        if (ShouldInterruptTarget(target, interruptSpellId))
            return ExecuteSpell(target, interruptSpellId);

        return false;
    }

    // =========================================================================
    // 通用脱战跟随维护
    // =========================================================================
    void UpdateFollowMaster(uint32 diff)
    {
        if (me->IsInCombat())
            return;

        if (followCheckTimer > diff)
        {
            followCheckTimer -= diff;
            return;
        }
        followCheckTimer = 1000;

        Player* master = GetMaster();
        if (!master || !master->IsAlive())
            return;

        if (me->GetMap() != master->GetMap())
            return;

        // 处于就地休息或保持阵型状态下，禁止常规 6 码跟随抢占：
        // 阵型成员一旦被拉回跟随队形，三大阵型的几何外推结果会被每秒巡检整体冲刷。
        // 仅在指挥官脱离过远 (>45 码) 跨区域时保留防丢失瞬移，并顺势解除两类锁定。
        if (isResting || isHoldingFormation)
        {
            if (me->GetDistance(master) > 45.0f)
            {
                if (isDebugLogging)
                    LOG_INFO("scripts", "[Bot: {}] 休息/阵型保持中仍严重脱离 (>45码)，执行防丢失瞬移并解除锁定。", me->GetName());

                isHoldingFormation = false;
                isResting = false;

                float const x = master->GetPositionX() - 2.0f * std::cos(master->GetOrientation());
                float const y = master->GetPositionY() - 2.0f * std::sin(master->GetOrientation());
                float const z = master->GetPositionZ();
                me->NearTeleportTo(x, y, z, master->GetOrientation());
                me->GetMotionMaster()->Clear();
            }
            return;
        }

        if (me->GetPhaseMask() != master->GetPhaseMask())
            me->SetPhaseMask(master->GetPhaseMask(), true);

        float const dist = me->GetDistance(master);

        if (dist > 45.0f)
        {
            if (isDebugLogging)
                LOG_INFO("scripts", "[Bot: {}] 跟随超距卡死 (>45码)，执行防卡死瞬移对齐。", me->GetName());

            float const x = master->GetPositionX() - 2.0f * std::cos(master->GetOrientation());
            float const y = master->GetPositionY() - 2.0f * std::sin(master->GetOrientation());
            float const z = master->GetPositionZ();
            me->NearTeleportTo(x, y, z, master->GetOrientation());
            me->GetMotionMaster()->Clear();
            return;
        }

        if (dist > 6.0f && me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        {
            me->GetMotionMaster()->MoveFollow(master, 3.0f, me->GetAngle(master));
        }
    }

    void UpdateTimers(uint32 diff)
    {
        if (apfMoveUpdateTimer > diff)
            apfMoveUpdateTimer -= diff;
        else
            apfMoveUpdateTimer = 0;

        if (gcdTimer > diff)
            gcdTimer -= diff;
        else
            gcdTimer = 0;

        // 敌方施法进度常态化侦测：即使打断技能正处于冷却，也每帧主动推进读条进度，
        // 保证 CD 一转好即可立刻命中压在末尾的正确窗口，而非从 0 重新计时。
        {
            Unit* enemyTarget = me->GetVictim();
            if (!enemyTarget && !memoryPreloadTargetGuid.IsEmpty())
                enemyTarget = ObjectAccessor::GetUnit(*me, memoryPreloadTargetGuid);

            Spell* trackedSpell = nullptr;
            if (enemyTarget && enemyTarget->IsAlive() && enemyTarget->HasUnitState(UNIT_STATE_CASTING))
            {
                trackedSpell = enemyTarget->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                if (!trackedSpell)
                    trackedSpell = enemyTarget->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            }

            if (trackedSpell && trackedSpell->GetSpellInfo())
            {
                uint32 const trackedSpellId = trackedSpell->GetSpellInfo()->Id;
                if (enemyTarget->GetGUID() != currentEnemyCastingGuid || trackedSpellId != currentEnemyCastingSpellId)
                {
                    // 换目标 / 换法术：重置追踪基准
                    currentEnemyCastingGuid = enemyTarget->GetGUID();
                    currentEnemyCastingSpellId = trackedSpellId;
                    currentEnemyCastingTotalMs = static_cast<uint32>(trackedSpell->GetCastTime());
                    currentEnemyCastingElapsedMs = 0;
                }
                else
                {
                    currentEnemyCastingElapsedMs += diff;
                }
            }
            else
            {
                // 目标停止施法、阵亡或丢失：立即强制清零，
                // 杜绝上一场战斗的残留进度污染下一次压秒判定窗口。
                currentEnemyCastingGuid.Clear();
                currentEnemyCastingSpellId = 0;
                currentEnemyCastingElapsedMs = 0;
                currentEnemyCastingTotalMs = 0;
            }
        }

        if (me->getPowerType() == POWER_ENERGY)
        {
            energyRegenTimer += diff;
            if (energyRegenTimer >= 100)
            {
                uint32 const addVal = energyRegenTimer / 100;
                energyRegenTimer %= 100;
                if (me->GetPower(POWER_ENERGY) < me->GetMaxPower(POWER_ENERGY))
                    me->ModifyPower(POWER_ENERGY, addVal);
            }
        }

        if (me->getPowerType() == POWER_MANA && me->IsInCombat())
        {
            manaRegenTimer += diff;
            if (manaRegenTimer >= 2000)
            {
                manaRegenTimer %= 2000;
                uint32 const addMana = me->GetMaxPower(POWER_MANA) * 2 / 100;
                if (me->GetPower(POWER_MANA) < me->GetMaxPower(POWER_MANA))
                    me->ModifyPower(POWER_MANA, addMana);
            }
        }

        // 就地休息强力补给：每 1 秒恢复 5% 最大生命值与 5% 最大法力值（能量固定 10 点）。
        // UpdateTimers 每帧驱动，故以 restRegenTimer 按秒节流，避免按帧结算瞬间回满。
        if (isResting && !me->IsInCombat())
        {
            restRegenTimer += diff;
            if (restRegenTimer >= 1000)
            {
                restRegenTimer -= 1000;

                if (me->GetHealth() < me->GetMaxHealth())
                    me->ModifyHealth(std::max<int32>(1, static_cast<int32>(me->GetMaxHealth() * 5 / 100)));

                if (me->getPowerType() == POWER_MANA && me->GetPower(POWER_MANA) < me->GetMaxPower(POWER_MANA))
                    me->ModifyPower(POWER_MANA, static_cast<int32>(me->GetMaxPower(POWER_MANA) * 5 / 100));
                else if (me->getPowerType() == POWER_ENERGY && me->GetPower(POWER_ENERGY) < me->GetMaxPower(POWER_ENERGY))
                    me->ModifyPower(POWER_ENERGY, 10);
            }
        }
        else
        {
            restRegenTimer = 0;
        }

        // 危险禁区生命周期仅在战斗中自然消退；脱战与跑尸阶段挂起倒计时。
        // 若按真实时间持续扣减，玩家释放灵魂跑尸回本动辄 1~2 分钟，
        // 远长于 15 秒默认留存时间，重新开怪时学到的火圈已被全部清空。
        if (me->IsInCombat() && !activeDangerZones.empty())
        {
            for (auto it = activeDangerZones.begin(); it != activeDangerZones.end(); )
            {
                if (it->durationMs <= diff)
                    it = activeDangerZones.erase(it);
                else
                {
                    it->durationMs -= diff;
                    ++it;
                }
            }
        }

        if (wasInCombat && !me->IsInCombat())
        {
            wasInCombat = false;
            OnCombatEnded(me->IsAlive());
        }
        else if (!wasInCombat && me->IsInCombat())
        {
            wasInCombat = true;
            combatTimerMs = 0;
            tankSampleTimer = 0;
            otCheckTimer = 0;
            combatEventBuffer.Clear();
        }

        // 战时采样：前 10 秒窗口内做 OT 检测与主坦仇恨速率 (TPS) 沉淀
        if (me->IsInCombat())
        {
            combatTimerMs += diff;

            if (combatTimerMs <= 10000)
            {
                // 独立累加器取代 combatTimerMs % 1000 取模判定：
                // diff 波动（卡帧 / 批量结算）会让取模判定跳帧或同一秒重复命中，
                // 造成 OT 采样密度不稳定。
                otCheckTimer += diff;
                if (!IsTankBot() && otCheckTimer >= 1000)
                {
                    otCheckTimer -= 1000;
                    Unit* attacker = me->getAttackerForHelper();
                    if (attacker && attacker->GetVictim() == me)
                    {
                        BotCombatEvent ev;
                        ev.combatTimeMs = combatTimerMs;
                        ev.eventType = BotCombatEventType::THREAT_OT_WARNING;
                        ev.sourceGuid = attacker->GetGUID();
                        combatEventBuffer.Push(ev);
                    }
                }

                tankSampleTimer += diff;
                if (tankSampleTimer >= 1000)
                {
                    tankSampleTimer -= 1000;

                    Unit* groupTank = GetGroupTank();
                    Unit* currentVictim = me->GetVictim();
                    if (groupTank && groupTank != me && currentVictim && currentVictim->IsInWorld() && groupTank->GetMap() == me->GetMap())
                    {
                        float const currentThreat = currentVictim->GetThreatMgr().GetThreat(groupTank);
                        if (currentThreat > 0.0f)
                        {
                            BotCombatEvent ev;
                            ev.combatTimeMs = combatTimerMs;
                            ev.eventType = BotCombatEventType::TANK_THREAT_SAMPLE;
                            ev.amount = static_cast<uint32>(currentThreat);
                            ev.sourceGuid = groupTank->GetGUID();
                            combatEventBuffer.Push(ev);
                        }
                    }
                }
            }
        }

        if (!me->IsInCombat())
        {
            if (levelSyncTimer <= diff)
            {
                levelSyncTimer = 2000;
                SyncLevelWithMaster();
            }
            else
            {
                levelSyncTimer -= diff;
            }

            // 脱战保活轮询：3 秒一次低频巡检，覆盖「护卫被脚本强制移除 /
            // 跨图传送丢失 / 实体被清理」等无法通过战斗结算感知的异常场景。
            if (guardianCheckTimer <= diff)
            {
                guardianCheckTimer = 3000;
                EnsureGuardianAlive();
            }
            else
            {
                guardianCheckTimer -= diff;
            }
        }
    }
};

template <typename T_AI>
class AdaptiveBotScript : public CreatureScript
{
public:
    explicit AdaptiveBotScript(char const* name) : CreatureScript(name) { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new T_AI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!creature->IsAlive())
            return false;

        auto* botAI = dynamic_cast<AdaptiveBotAI*>(creature->AI());
        if (!botAI)
            return false;

        Player* master = botAI->GetMaster();

        if (!master)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "跟随我，协助我作战！", GOSSIP_SENDER_MAIN, 1);
        }
        else if (master == player)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "解散并原地待命。", GOSSIP_SENDER_MAIN, 2);
            AddGossipItemFor(player, GOSSIP_ICON_DOT, botAI->isDebugLogging ? "【调试】关闭调试日志" : "【调试】开启调试日志", GOSSIP_SENDER_MAIN, 3);
        }
        else
        {
            ChatHandler(player->GetSession()).SendNotification("该随从正在协助其他指挥官。");
            return true;
        }

        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        ClearGossipMenuFor(player);

        auto* botAI = dynamic_cast<AdaptiveBotAI*>(creature->AI());
        if (!botAI)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        if (action == 1)
        {
            if (!botAI->GetMaster())
            {
                botAI->SetMaster(player);
                creature->Say("遵命，我将协助您作战。", LANG_UNIVERSAL);
            }
            CloseGossipMenuFor(player);
        }
        else if (action == 2)
        {
            if (botAI->GetMaster() == player)
            {
                botAI->UnregisterFromMaster();
                botAI->masterGuid.Clear();
                creature->CombatStop(true);
                creature->GetMotionMaster()->MoveIdle();
                creature->RestoreFaction();
                creature->Say("我在此待命。", LANG_UNIVERSAL);

                // 恢复为数据库 creature_template 中定义的初始最低等级并补满血量
                creature->SetLevel(creature->GetCreatureTemplate()->minlevel);
                creature->SetHealth(creature->GetMaxHealth());
            }
            CloseGossipMenuFor(player);
        }
        else if (action == 3)
        {
            botAI->isDebugLogging = !botAI->isDebugLogging;
            creature->Whisper(botAI->isDebugLogging ? "调试日志已【开启】。" : "调试日志已【关闭】。", LANG_UNIVERSAL, player);
            CloseGossipMenuFor(player);
        }

        return true;
    }
};
