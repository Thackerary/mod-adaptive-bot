/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */

#pragma once

#include "Define.h"
#include "Analytics/CombatAnalyzer.h"
#include "Movement/DangerZones.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// @brief 单个随从针对单个首领的个体认知档案。
///        所有字段均以 (spawn_id, boss_entry) 复合主键物理隔离，
///        随从之间绝不共享经验。
struct BotCognitionRecord
{
    uint32 bossEntry{ 0 };
    uint32 wipeCount{ 0 };
    uint32 killCount{ 0 };
    float experiencedTps{ 0.0f };
    uint8 proficiencyLevel{ 1 }; // 1: 新手, 2: 熟练, 3: 老兵
    std::vector<DangerZone> learnedHazards;
    std::unordered_map<uint32, uint32> interruptDelays; // spellId -> reactionDelayMs
};

/// @brief 战后落盘任务：主线程只做一次深拷贝，随后完全交给后台线程。
struct AttributionPersistTask
{
    uint32 spawnId{ 0 };
    uint32 bossEntry{ 0 };
    uint32 currentMapId{ 0 };
    AttributionReport report;
};

/// @brief 顶层持久化机制记忆库（SQLite / WAL / 独立落盘线程）。
///
/// 线程契约：
///   - 游戏主线程：只在开怪瞬间调用 LoadBotKnowledge()，只在战斗终结瞬间调用
///     EnqueuePersistTask()（深拷贝入队，< 1 微秒，绝不触碰磁盘）。
///   - 后台工作线程：独占消费队列，单事务批量 INSERT/UPDATE，绝不阻塞世界心跳。
class BotMemoryDB
{
public:
    static BotMemoryDB* Instance();

    /// @brief 打开数据库、开启 WAL、建表并拉起后台落盘线程。
    /// @return true 表示数据库与工作线程均已就绪。
    bool Initialize(std::string const& dbPath);

    /// @brief 优雅停机：置位 _running、唤醒并 join 工作线程、排空队列、关闭句柄。
    ///        幂等，重复调用安全。
    void Close();

    /// @brief 数据库句柄是否已就绪（供上层决定是否走记忆化分支）。
    [[nodiscard]] bool IsReady() const { return _db != nullptr; }

    // 战前查询 (主线程调用, 主键索引 O(1) 单行命中)
    bool LoadBotKnowledge(uint32 spawnId, uint32 bossEntry, BotCognitionRecord& outRecord);

    // 战后提交 (主线程非阻塞入队)
    void EnqueuePersistTask(uint32 spawnId, uint32 bossEntry, uint32 mapId, AttributionReport const& report);

private:
    BotMemoryDB() = default;
    ~BotMemoryDB();

    BotMemoryDB(BotMemoryDB const&) = delete;
    BotMemoryDB& operator=(BotMemoryDB const&) = delete;

    void WorkerLoop();
    void InitSchema();

    struct sqlite3* _db{ nullptr };
    std::string _dbPath;

    std::queue<AttributionPersistTask> _taskQueue;
    std::mutex _queueMutex;
    std::condition_variable _cv;
    std::thread _workerThread;
    std::atomic<bool> _running{ false };
};

#define sBotMemory BotMemoryDB::Instance()
