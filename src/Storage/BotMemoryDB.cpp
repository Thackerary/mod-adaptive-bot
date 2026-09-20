/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license
 */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#endif

#include "Storage/BotMemoryDB.h"

#include "Log.h"

// 内嵌编译模式：sqlite3 官方 amalgamation（sqlite3.c / sqlite3.h）随模块源码一同编译，
// 使用引号包含以优先命中本模块 src/Storage 目录，彻底摆脱系统级 SQLite 环境依赖。
#include "sqlite3.h"

#include <algorithm>
#include <ctime>
#include <utility>

namespace
{
    constexpr char const* BOT_MEMORY_LOG_FILTER = "sql";

    constexpr int    BOT_MEMORY_BUSY_TIMEOUT_MS        = 5000;
    constexpr double BOT_MEMORY_HAZARD_INIT_CONFIDENCE = 0.5;
    constexpr double BOT_MEMORY_HAZARD_CONFIDENCE_STEP = 0.25;
    constexpr uint32 BOT_MEMORY_INTERRUPT_STEP_MS      = 150;
    constexpr uint32 BOT_MEMORY_INTERRUPT_MAX_MS       = 1200;
    constexpr uint32 BOT_MEMORY_INTERRUPT_MAX_PRIORITY = 10;

    /// @brief 轻量 RAII 语句守卫。
    ///        保证任意提前 return 路径都能 sqlite3_finalize，杜绝后台线程
    ///        长期运行下的 stmt 句柄泄漏。
    class ScopedStatement
    {
    public:
        ScopedStatement(sqlite3* db, char const* sql)
        {
            _valid = (sqlite3_prepare_v2(db, sql, -1, &_stmt, nullptr) == SQLITE_OK);
        }

        ~ScopedStatement()
        {
            if (_stmt)
                sqlite3_finalize(_stmt);
        }

        ScopedStatement(ScopedStatement const&) = delete;
        ScopedStatement& operator=(ScopedStatement const&) = delete;

        [[nodiscard]] bool IsValid() const { return _valid && _stmt != nullptr; }
        [[nodiscard]] sqlite3_stmt* Get() const { return _stmt; }

        /// @return 原始 sqlite3_step 返回码，供调用方严格区分 ROW / DONE / 错误。
        int StepRaw() { return sqlite3_step(_stmt); }

        bool Execute() { return sqlite3_step(_stmt) == SQLITE_DONE; }

    private:
        sqlite3_stmt* _stmt{ nullptr };
        bool _valid{ false };
    };

    bool ExecSimple(sqlite3* db, char const* sql)
    {
        if (!db || !sql)
            return false;

        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK)
        {
            LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] SQL 执行失败 [{}]: {}", sql, errMsg ? errMsg : "未知错误");
            if (errMsg)
                sqlite3_free(errMsg);
            return false;
        }

        return true;
    }

    bool BindKey(ScopedStatement& stmt, uint32 spawnId, uint32 bossEntry, int firstIndex = 1)
    {
        return sqlite3_bind_int64(stmt.Get(), firstIndex, static_cast<sqlite3_int64>(spawnId)) == SQLITE_OK
            && sqlite3_bind_int64(stmt.Get(), firstIndex + 1, static_cast<sqlite3_int64>(bossEntry)) == SQLITE_OK;
    }

    uint32 CurrentUnixTime()
    {
        return static_cast<uint32>(std::time(nullptr));
    }

    /// @brief 落盘单个任务：战绩累加 -> 熟练度重算 -> 危险禁区置信刷新 -> 打断余量学习。
    ///        调用方必须已开启事务，本函数不负责 BEGIN/COMMIT。
    bool ApplyPersistTask(sqlite3* db, AttributionPersistTask const& task)
    {
        AttributionReport const& report = task.report;
        bool const     isWin      = !report.isWipe;
        uint32 const   wipeDelta  = isWin ? 0u : 1u;
        uint32 const   killDelta  = isWin ? 1u : 0u;
        double const   tps        = static_cast<double>(report.tankFirst10sTps);
        uint32 const   nowUnix    = CurrentUnixTime();

        // ---- 1. 个体战绩档案：先保证行存在，再原子累加 ----
        {
            ScopedStatement stmt(db,
                "INSERT OR IGNORE INTO bot_individual_profile "
                "(spawn_id, boss_entry, wipe_count, kill_count, experienced_tps, proficiency_level, last_updated) "
                "VALUES (?, ?, 0, 0, 0.0, 1, ?);");

            if (!stmt.IsValid())
            {
                LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 战绩档案 INSERT 准备失败: {}", sqlite3_errmsg(db));
                return false;
            }

            if (!BindKey(stmt, task.spawnId, task.bossEntry))
                return false;
            sqlite3_bind_int64(stmt.Get(), 3, static_cast<sqlite3_int64>(nowUnix));

            if (!stmt.Execute())
            {
                LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 战绩档案 INSERT 失败: {}", sqlite3_errmsg(db));
                return false;
            }
        }

        {
            // experienced_tps 采用滑动平均（权重 50%）：首次直接落库，此后平滑融合，
            // 避免单次异常战斗（开局秒躺 / 坦克掉线）把长期测得的仇恨曲线彻底带偏。
            ScopedStatement stmt(db,
                "UPDATE bot_individual_profile SET "
                "  wipe_count = wipe_count + ?,"
                "  kill_count = kill_count + ?,"
                "  experienced_tps = CASE WHEN ? > 0.0 THEN "
                "    CASE WHEN experienced_tps > 0.0 THEN (experienced_tps + ?) / 2.0 ELSE ? END "
                "    ELSE experienced_tps END,"
                "  last_updated = ? "
                "WHERE spawn_id = ? AND boss_entry = ?;");

            if (!stmt.IsValid())
            {
                LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 战绩档案 UPDATE 准备失败: {}", sqlite3_errmsg(db));
                return false;
            }

            sqlite3_bind_int64(stmt.Get(), 1, static_cast<sqlite3_int64>(wipeDelta));
            sqlite3_bind_int64(stmt.Get(), 2, static_cast<sqlite3_int64>(killDelta));
            sqlite3_bind_double(stmt.Get(), 3, tps);
            sqlite3_bind_double(stmt.Get(), 4, tps);
            sqlite3_bind_double(stmt.Get(), 5, tps);
            sqlite3_bind_int64(stmt.Get(), 6, static_cast<sqlite3_int64>(nowUnix));
            sqlite3_bind_int64(stmt.Get(), 7, static_cast<sqlite3_int64>(task.spawnId));
            sqlite3_bind_int64(stmt.Get(), 8, static_cast<sqlite3_int64>(task.bossEntry));

            if (!stmt.Execute())
            {
                LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 战绩档案 UPDATE 失败: {}", sqlite3_errmsg(db));
                return false;
            }
        }

        // ---- 2. 熟练度重算 ----
        // 双门槛：既要交手次数够多，也必须真的赢过，杜绝靠反复送人头刷成"老兵"。
        {
            ScopedStatement stmt(db,
                "UPDATE bot_individual_profile SET proficiency_level = CASE "
                "  WHEN (wipe_count + kill_count) >= 8 AND kill_count >= 2 THEN 3 "
                "  WHEN (wipe_count + kill_count) >= 3 THEN 2 "
                "  ELSE 1 END "
                "WHERE spawn_id = ? AND boss_entry = ?;");

            if (!stmt.IsValid())
            {
                LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 熟练度 UPDATE 准备失败: {}", sqlite3_errmsg(db));
                return false;
            }

            if (!BindKey(stmt, task.spawnId, task.bossEntry))
                return false;

            if (!stmt.Execute())
            {
                LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 熟练度 UPDATE 失败: {}", sqlite3_errmsg(db));
                return false;
            }
        }

        // ---- 3. 危险禁区：同源坐标刷新 + 置信度递增 ----
        // 刻意采用 UPDATE-then-INSERT 而非 UPSERT：前者兼容全版本 SQLite，
        // 且能在命中已有记录时保留并累加 confidence_score（INSERT OR REPLACE 会重置）。
        for (auto const& zone : report.derivedDangerZones)
        {
            if (zone.spellId == 0)
                continue;

            {
                // hazard_type 采用 MAX 单调升级：DangerZoneType 枚举序为
                // CIRCLE = 0 < FRONTAL_CONE = 1，归因器提炼的圆形伤害源不得把
                // 已识别的顺劈/吐息锥形区错误降级回圆形。
                ScopedStatement stmt(db,
                    "UPDATE bot_individual_hazard SET "
                    "  hazard_type = MAX(hazard_type, ?), x = ?, y = ?, z = ?, radius = ?, "
                    "  confidence_score = MIN(1.0, confidence_score + ?) "
                    "WHERE spawn_id = ? AND boss_entry = ? AND spell_id = ?;");

                if (!stmt.IsValid())
                {
                    LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 危险禁区 UPDATE 准备失败: {}", sqlite3_errmsg(db));
                    return false;
                }

                sqlite3_bind_int(stmt.Get(), 1, static_cast<int>(zone.type));
                sqlite3_bind_double(stmt.Get(), 2, static_cast<double>(zone.x));
                sqlite3_bind_double(stmt.Get(), 3, static_cast<double>(zone.y));
                sqlite3_bind_double(stmt.Get(), 4, static_cast<double>(zone.z));
                sqlite3_bind_double(stmt.Get(), 5, static_cast<double>(zone.radius));
                sqlite3_bind_double(stmt.Get(), 6, BOT_MEMORY_HAZARD_CONFIDENCE_STEP);
                sqlite3_bind_int64(stmt.Get(), 7, static_cast<sqlite3_int64>(task.spawnId));
                sqlite3_bind_int64(stmt.Get(), 8, static_cast<sqlite3_int64>(task.bossEntry));
                sqlite3_bind_int64(stmt.Get(), 9, static_cast<sqlite3_int64>(zone.spellId));

                if (!stmt.Execute())
                {
                    LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 危险禁区 UPDATE 失败: {}", sqlite3_errmsg(db));
                    return false;
                }
            }

            // 单写线程独占连接，sqlite3_changes 可安全用于判定 UPDATE 是否命中已有行。
            if (sqlite3_changes(db) > 0)
                continue;

            {
                ScopedStatement stmt(db,
                    "INSERT INTO bot_individual_hazard "
                    "(spawn_id, boss_entry, spell_id, hazard_type, x, y, z, radius, confidence_score) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);");

                if (!stmt.IsValid())
                {
                    LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 危险禁区 INSERT 准备失败: {}", sqlite3_errmsg(db));
                    return false;
                }

                sqlite3_bind_int64(stmt.Get(), 1, static_cast<sqlite3_int64>(task.spawnId));
                sqlite3_bind_int64(stmt.Get(), 2, static_cast<sqlite3_int64>(task.bossEntry));
                sqlite3_bind_int64(stmt.Get(), 3, static_cast<sqlite3_int64>(zone.spellId));
                sqlite3_bind_int(stmt.Get(), 4, static_cast<int>(zone.type));
                sqlite3_bind_double(stmt.Get(), 5, static_cast<double>(zone.x));
                sqlite3_bind_double(stmt.Get(), 6, static_cast<double>(zone.y));
                sqlite3_bind_double(stmt.Get(), 7, static_cast<double>(zone.z));
                sqlite3_bind_double(stmt.Get(), 8, static_cast<double>(zone.radius));
                sqlite3_bind_double(stmt.Get(), 9, BOT_MEMORY_HAZARD_INIT_CONFIDENCE);

                if (!stmt.Execute())
                {
                    LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 危险禁区 INSERT 失败: {}", sqlite3_errmsg(db));
                    return false;
                }
            }
        }

        // ---- 4. 压秒打断余量学习 ----
        // reaction_delay_ms 语义：相对敌方读条结束的「提前出手余量」。
        //   首见初始值必须换算为真实毫秒 (漏断次数 * 步长)，上限 1200ms；
        //   后续每次漏断继续累加 150ms，杜绝初始仅写入 1~2ms 虚假延时的缺陷。
        // priority_level 首次置 1 并同步累加，供维度 A 打断仲裁排序使用。
        if (report.lastMissedSpellId != 0 && report.missedInterruptsCount > 0)
        {
            uint32 const initDelayMs = std::min<uint32>(
                BOT_MEMORY_INTERRUPT_MAX_MS,
                report.missedInterruptsCount * BOT_MEMORY_INTERRUPT_STEP_MS);

            ScopedStatement stmt(db,
                "INSERT INTO bot_individual_interrupt "
                "(spawn_id, boss_entry, spell_id, reaction_delay_ms, priority_level) "
                "VALUES (?, ?, ?, ?, 1) "
                "ON CONFLICT(spawn_id, boss_entry, spell_id) DO UPDATE SET "
                "  reaction_delay_ms = MIN(?, reaction_delay_ms + ?),"
                "  priority_level = MIN(?, priority_level + 1);");

            if (!stmt.IsValid())
            {
                LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 打断档案 INSERT 准备失败: {}", sqlite3_errmsg(db));
                return false;
            }

            sqlite3_bind_int64(stmt.Get(), 1, static_cast<sqlite3_int64>(task.spawnId));
            sqlite3_bind_int64(stmt.Get(), 2, static_cast<sqlite3_int64>(task.bossEntry));
            sqlite3_bind_int64(stmt.Get(), 3, static_cast<sqlite3_int64>(report.lastMissedSpellId));
            sqlite3_bind_int64(stmt.Get(), 4, static_cast<sqlite3_int64>(initDelayMs));
            sqlite3_bind_int64(stmt.Get(), 5, static_cast<sqlite3_int64>(BOT_MEMORY_INTERRUPT_MAX_MS));
            sqlite3_bind_int64(stmt.Get(), 6, static_cast<sqlite3_int64>(BOT_MEMORY_INTERRUPT_STEP_MS));
            sqlite3_bind_int64(stmt.Get(), 7, static_cast<sqlite3_int64>(BOT_MEMORY_INTERRUPT_MAX_PRIORITY));

            if (!stmt.Execute())
            {
                LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 打断档案写入失败: {}", sqlite3_errmsg(db));
                return false;
            }
        }

        return true;
    }

    /// @brief 单事务批量落盘。任何一条失败即整体回滚，杜绝半写脏档案。
    void ProcessTaskBatch(sqlite3* db, std::vector<AttributionPersistTask>& batch)
    {
        if (!db || batch.empty())
            return;

        // BEGIN IMMEDIATE：WAL 下立即抢占写锁，避免与其他连接的长读事务反复争抢。
        if (!ExecSimple(db, "BEGIN IMMEDIATE TRANSACTION;"))
            return;

        for (auto const& task : batch)
        {
            if (!ApplyPersistTask(db, task))
            {
                LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 批次落盘失败，已回滚本批 {} 条任务。", batch.size());
                ExecSimple(db, "ROLLBACK;");
                return;
            }
        }

        ExecSimple(db, "COMMIT;");
    }
}

BotMemoryDB* BotMemoryDB::Instance()
{
    static BotMemoryDB instance;
    return &instance;
}

BotMemoryDB::~BotMemoryDB()
{
    Close();
}

bool BotMemoryDB::Initialize(std::string const& dbPath)
{
    if (_readDb && _writeDb)
        return true;

    if (dbPath.empty())
    {
        LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 数据库路径为空，认知记忆库初始化中止。");
        return false;
    }

    _dbPath = dbPath;

    // -------------------------------------------------------------------------
    // 1. 写入连接：仅后台落盘线程使用，负责建表与全部写事务。
    // -------------------------------------------------------------------------
    int const writeOpenRes = sqlite3_open_v2(_dbPath.c_str(), &_writeDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);

    if (writeOpenRes != SQLITE_OK)
    {
        LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 无法打开写入连接 [{}]: {}",
            _dbPath, _writeDb ? sqlite3_errmsg(_writeDb) : "未知错误");

        if (_writeDb)
        {
            sqlite3_close(_writeDb);
            _writeDb = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout(_writeDb, BOT_MEMORY_BUSY_TIMEOUT_MS);

    // WAL + NORMAL：写入不阻塞读取，且不必每个事务都 fsync，
    // 在「随机崩溃可接受、绝不卡世界心跳」的随从记忆场景下是最优取舍。
    // 该 pragma 会立即创建 -wal / -shm 共享文件，为后续只读连接铺路。
    ExecSimple(_writeDb, "PRAGMA journal_mode = WAL;");
    ExecSimple(_writeDb, "PRAGMA synchronous = NORMAL;");
    ExecSimple(_writeDb, "PRAGMA temp_store = MEMORY;");

    InitSchema();

    // -------------------------------------------------------------------------
    // 2. 只读连接：仅游戏主线程使用，实现真正的 WAL 无锁并发读取。
    //    必须在写入连接切到 WAL 之后再打开，确保 -shm 共享索引已存在。
    // -------------------------------------------------------------------------
    int const readOpenRes = sqlite3_open_v2(_dbPath.c_str(), &_readDb,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);

    if (readOpenRes != SQLITE_OK)
    {
        LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 无法打开只读连接 [{}]: {}",
            _dbPath, _readDb ? sqlite3_errmsg(_readDb) : "未知错误");

        if (_readDb)
        {
            sqlite3_close(_readDb);
            _readDb = nullptr;
        }

        sqlite3_close(_writeDb);
        _writeDb = nullptr;
        return false;
    }

    sqlite3_busy_timeout(_readDb, BOT_MEMORY_BUSY_TIMEOUT_MS);

    _running = true;
    _workerThread = std::thread(&BotMemoryDB::WorkerLoop, this);

    LOG_INFO(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 个体认知记忆库已就绪于 [{}] (WAL 读写分离 / 异步批量落盘)。", _dbPath);
    return true;
}

void BotMemoryDB::Close()
{
    if (_running.exchange(false))
        _cv.notify_all();

    if (_workerThread.joinable())
        _workerThread.join();

    // 进入 Close 时工作线程已 join，两个连接均不再被任何线程持有，可安全关闭。
    if (_readDb)
    {
        sqlite3_close(_readDb);
        _readDb = nullptr;
    }

    if (_writeDb)
    {
        sqlite3_close(_writeDb);
        _writeDb = nullptr;
    }

    LOG_INFO(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 个体认知记忆库已安全关闭。");
}

void BotMemoryDB::InitSchema()
{
    if (!_writeDb)
        return;

    // 表 1：个体战绩档案（熟练度与坦克仇恨速率）
    ExecSimple(_writeDb,
        "CREATE TABLE IF NOT EXISTS bot_individual_profile ("
        "  spawn_id INTEGER NOT NULL,"
        "  boss_entry INTEGER NOT NULL,"
        "  wipe_count INTEGER NOT NULL DEFAULT 0,"
        "  kill_count INTEGER NOT NULL DEFAULT 0,"
        "  experienced_tps REAL NOT NULL DEFAULT 0.0,"
        "  proficiency_level INTEGER NOT NULL DEFAULT 1,"
        "  last_updated INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (spawn_id, boss_entry)"
        ");");

    // 表 2：个体已学到的危险禁区（UNIQUE 约束自带查询索引）
    ExecSimple(_writeDb,
        "CREATE TABLE IF NOT EXISTS bot_individual_hazard ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  spawn_id INTEGER NOT NULL,"
        "  boss_entry INTEGER NOT NULL,"
        "  spell_id INTEGER NOT NULL,"
        "  hazard_type INTEGER NOT NULL DEFAULT 0,"
        "  x REAL NOT NULL DEFAULT 0.0,"
        "  y REAL NOT NULL DEFAULT 0.0,"
        "  z REAL NOT NULL DEFAULT 0.0,"
        "  radius REAL NOT NULL DEFAULT 5.0,"
        "  confidence_score REAL NOT NULL DEFAULT 0.0,"
        "  UNIQUE (spawn_id, boss_entry, spell_id)"
        ");");

    // 表 3：个体压秒打断档案
    ExecSimple(_writeDb,
        "CREATE TABLE IF NOT EXISTS bot_individual_interrupt ("
        "  spawn_id INTEGER NOT NULL,"
        "  boss_entry INTEGER NOT NULL,"
        "  spell_id INTEGER NOT NULL,"
        "  reaction_delay_ms INTEGER NOT NULL DEFAULT 0,"
        "  priority_level INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (spawn_id, boss_entry, spell_id)"
        ");");
}

bool BotMemoryDB::LoadBotKnowledge(uint32 spawnId, uint32 bossEntry, BotCognitionRecord& outRecord)
{
    outRecord = BotCognitionRecord();

    if (!_readDb || spawnId == 0 || bossEntry == 0)
        return false;

    outRecord.bossEntry = bossEntry;

    // ---- 1. 个体战绩档案：主键索引单行命中，无档案即首次遭遇该首领 ----
    // 走独立只读连接，绝不与后台 BEGIN IMMEDIATE 写事务争抢同一句柄的互斥锁。
    {
        ScopedStatement stmt(_readDb,
            "SELECT wipe_count, kill_count, experienced_tps, proficiency_level "
            "FROM bot_individual_profile WHERE spawn_id = ? AND boss_entry = ?;");

        if (!stmt.IsValid())
        {
            LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 战绩档案查询准备失败: {}", sqlite3_errmsg(_readDb));
            return false;
        }

        if (!BindKey(stmt, spawnId, bossEntry))
            return false;

        int const rc = stmt.StepRaw();
        if (rc != SQLITE_ROW)
            return false; // 尚无认知档案：首次面对该首领，走纯 baseline

        outRecord.wipeCount = static_cast<uint32>(sqlite3_column_int64(stmt.Get(), 0));
        outRecord.killCount = static_cast<uint32>(sqlite3_column_int64(stmt.Get(), 1));
        outRecord.experiencedTps = static_cast<float>(sqlite3_column_double(stmt.Get(), 2));
        outRecord.proficiencyLevel = static_cast<uint8>(
            std::clamp(sqlite3_column_int(stmt.Get(), 3), 1, 3));
    }

    // ---- 2. 已学到的危险禁区 ----
    {
        ScopedStatement stmt(_readDb,
            "SELECT spell_id, hazard_type, x, y, z, radius "
            "FROM bot_individual_hazard WHERE spawn_id = ? AND boss_entry = ?;");

        if (!stmt.IsValid())
        {
            LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 危险禁区查询准备失败: {}", sqlite3_errmsg(_readDb));
            return true; // 档案主体已读到，局部失败不阻断预热
        }

        if (!BindKey(stmt, spawnId, bossEntry))
            return true;

        int rc = SQLITE_ROW;
        while ((rc = stmt.StepRaw()) == SQLITE_ROW)
        {
            DangerZone zone;
            zone.spellId = static_cast<uint32>(sqlite3_column_int64(stmt.Get(), 0));

            int const hazardType = sqlite3_column_int(stmt.Get(), 1);
            zone.type = (hazardType == static_cast<int>(DangerZoneType::FRONTAL_CONE))
                ? DangerZoneType::FRONTAL_CONE
                : DangerZoneType::CIRCLE;

            // 地图归属由调用方在合并瞬间盖章为当前 MapId：数据库不持久化地图，
            // 否则跨副本复用时 mapId 为 0 会被 IsUnderDangerThreat() 视作全地图生效。
            zone.mapId = 0;
            zone.x = static_cast<float>(sqlite3_column_double(stmt.Get(), 2));
            zone.y = static_cast<float>(sqlite3_column_double(stmt.Get(), 3));
            zone.z = static_cast<float>(sqlite3_column_double(stmt.Get(), 4));
            zone.radius = static_cast<float>(sqlite3_column_double(stmt.Get(), 5));
            zone.durationMs = 600000; // 与运行时长效记忆口径保持一致

            outRecord.learnedHazards.push_back(zone);
        }

        if (rc != SQLITE_DONE)
            LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 危险禁区遍历中断: {}", sqlite3_errmsg(_readDb));
    }

    // ---- 3. 压秒打断档案 ----
    {
        ScopedStatement stmt(_readDb,
            "SELECT spell_id, reaction_delay_ms "
            "FROM bot_individual_interrupt WHERE spawn_id = ? AND boss_entry = ?;");

        if (!stmt.IsValid())
        {
            LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 打断档案查询准备失败: {}", sqlite3_errmsg(_readDb));
            return true;
        }

        if (!BindKey(stmt, spawnId, bossEntry))
            return true;

        int rc = SQLITE_ROW;
        while ((rc = stmt.StepRaw()) == SQLITE_ROW)
        {
            uint32 const spellId = static_cast<uint32>(sqlite3_column_int64(stmt.Get(), 0));
            uint32 const delay = static_cast<uint32>(sqlite3_column_int64(stmt.Get(), 1));
            if (spellId != 0)
                outRecord.interruptDelays[spellId] = delay;
        }

        if (rc != SQLITE_DONE)
            LOG_ERROR(BOT_MEMORY_LOG_FILTER, "[BotMemoryDB] 打断档案遍历中断: {}", sqlite3_errmsg(_readDb));
    }

    return true;
}

void BotMemoryDB::EnqueuePersistTask(uint32 spawnId, uint32 bossEntry, uint32 mapId, AttributionReport const& report)
{
    if (!_running || !_writeDb || spawnId == 0 || bossEntry == 0)
        return;

    AttributionPersistTask task;
    task.spawnId = spawnId;
    task.bossEntry = bossEntry;
    task.currentMapId = mapId;
    task.report = report; // 深拷贝：仅复制定长数值与两个小 vector，全程无磁盘 IO

    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _taskQueue.push(std::move(task));
    }
    _cv.notify_one();
}

void BotMemoryDB::WorkerLoop()
{
    std::vector<AttributionPersistTask> batch;
    batch.reserve(32);

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            _cv.wait(lock, [this] { return !_running || !_taskQueue.empty(); });

            // 停机条件：已置位 _running=false 且队列彻底排空。
            if (!_running && _taskQueue.empty())
                break;

            batch.clear();
            while (!_taskQueue.empty())
            {
                batch.push_back(std::move(_taskQueue.front()));
                _taskQueue.pop();
            }
        }

        if (!batch.empty())
            ProcessTaskBatch(_writeDb, batch);
    }
}
