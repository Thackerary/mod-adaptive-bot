/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "ScriptMgr.h"
#include "Common.h"
#include "ObjectGuid.h"
#include <array>
#include <mutex>
#include <unordered_map>

class Player;
class Creature;

// 十大冒险者公会枚举
enum AdventureGuildType : uint8
{
    GUILD_NONE                 = 0,
    GUILD_EXPLORERS_LEAGUE     = 1, // 铁炉堡探险者协会 (联盟)
    GUILD_SI7_MERCENARIES      = 2, // 暴风城军情七处 (联盟)
    GUILD_SILVER_COVENANT      = 3, // 达拉然银色盟约 (联盟)
    GUILD_WARSONG_OFFENSIVE    = 4, // 战歌远征突击队 (部落)
    GUILD_SUNREAVERS           = 5, // 夺日者议会 (部落)
    GUILD_DEATHSTALKERS        = 6, // 幽暗城死亡猎手狂怒社 (部落)
    GUILD_ARGENT_CRUSADE       = 7, // 银色北伐军先锋营 (中立)
    GUILD_UNDERBELLY_SYNDICATE = 8, // 达拉然下水道黑市行会 (中立)
    GUILD_CENARION_EXPEDITION  = 9, // 塞纳里奥议会/远征队 (中立)
    GUILD_STEAMWHEEDLE_CARTEL  = 10 // 热砂财阀雇佣行 (中立跨阵营)
};

// 结算触发源枚举
enum BillingReason : uint8
{
    BILLING_REASON_PERIODIC,    // 20 分钟周期轮询静默划扣
    BILLING_REASON_MAP_CHANGE,  // 进出副本切图强制结算
    BILLING_REASON_DISBAND,     // 队伍主动解散最终清算
    BILLING_REASON_MANUAL       // 玩家手动结算
};

// 单指挥官的内存实时契约记账单
struct BotHireContract
{
    ObjectGuid masterGuid;
    uint32 pendingCopper{ 0 };          // 累积未结清佣金 (铜)
    uint32 totalKilledCount{ 0 };       // 本轮周期击杀统计
    uint32 periodicTimer{ 0 };          // 20 分钟心跳累加器 (ms)
    uint32 lastMapId{ 0xFFFFFFFF };     // 切图结算锚点：上一次观测到的地图 ID
    bool inGracePeriod{ false };        // 是否处于扣费失败宽限期
    uint32 gracePeriodTimer{ 0 };       // 宽限期倒计时 (ms，默认 1 小时)
    uint32 graceRecoveryCheckTimer{ 0 };// 宽限期资金自愈检测节流器 (2000ms)
    bool reminded30Min{ false };        // 30 分钟催缴预警已发送标记
    bool reminded10Min{ false };        // 10 分钟紧急催缴已发送标记
};

class BotGuildEscrowMgr
{
public:
    static constexpr uint32 PERIODIC_BILLING_INTERVAL_MS = 20 * 60 * 1000; // 20 分钟周期轮询
    static constexpr uint32 GRACE_PERIOD_MS              = 60 * 60 * 1000; // 1 小时欠费宽限 (60分钟)
    static constexpr uint32 CONTRACT_PROBE_INTERVAL_MS   = 1000;           // 契约自愈探针周期
    static constexpr uint32 GRACE_RECOVERY_PROBE_MS      = 2000;           // 宽限期资金自愈检测周期
    static constexpr uint32 GRACE_WARN_30MIN_MS          = 30 * 60 * 1000; // 30 分钟催缴预警阶梯
    static constexpr uint32 GRACE_WARN_10MIN_MS          = 10 * 60 * 1000; // 10 分钟紧急催缴阶梯
    static constexpr float  BREACH_PENALTY_RATE          = 1.15f;          // 违约滞纳金 15%

    static BotGuildEscrowMgr* Instance();

    // 内存契约生命周期
    void StartContract(Player* player);
    void RemoveContract(ObjectGuid const& playerGuid);
    bool HasActiveContract(ObjectGuid const& playerGuid);

    // 核心算费与结算
    void AccumulateKillFee(Player* player, Creature* killed);
    bool SettleCurrentBill(Player* player, BillingReason reason);
    void Update(Player* player, uint32 diff);

    // 信用黑名单审计
    bool IsBankrupt(ObjectGuid const& playerGuid);
    void ClearBankruptcy(Player* player);
    void LoadBankruptcyFromDB();

    // 费率折扣计算
    static float CalculateGuildDiscount(uint8 playerGuildId, uint8 botGuildId, bool isCrossFaction);

private:
    BotGuildEscrowMgr() = default;
    ~BotGuildEscrowMgr() = default;

    std::mutex _lock;
    std::unordered_map<ObjectGuid, BotHireContract> _activeContracts;
    std::unordered_map<ObjectGuid, uint32> _contractProbeTimers; // 开户探针节流器
    std::unordered_map<ObjectGuid, uint32> _bankruptDebts;       // GUID -> 欠款金额(铜)

    // 击杀全局定长去重环：PlayerScript 侧与随从 AI 侧是两条独立上报通道，
    // 同一次击杀会被回调两次。若不设闸，任何非治疗阵容都会被双重计费。
    // 定长环形缓冲零堆分配，最近 16 次击杀 GUID 的窗口足以覆盖同帧双路回调。
    std::array<ObjectGuid, 16> _recentKilledGuids{};
    uint8 _recentKilledIdx{ 0 };
    bool IsDuplicateKill(ObjectGuid const& guid);
};

#define sBotGuildEscrowMgr BotGuildEscrowMgr::Instance()

// 挂载玩家事件监听器
class BotGuildEscrowPlayerScript : public PlayerScript
{
public:
    BotGuildEscrowPlayerScript();

    void OnPlayerLogin(Player* player) override;
    void OnPlayerLogout(Player* player) override;
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override;
    void OnPlayerUpdate(Player* player, uint32 p_time) override;
};

// 数据库就绪后才载入黑名单：脚本注册阶段（AddSC_*）数据库尚未连通
class BotGuildEscrowWorldScript : public WorldScript
{
public:
    BotGuildEscrowWorldScript();

    void OnStartup() override;
};

void AddSC_BotGuildEscrowMgr();
