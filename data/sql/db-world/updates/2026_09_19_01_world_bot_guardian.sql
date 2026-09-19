-- 伴随型战斗护卫统一载体模板 (Entry: 990001)
-- 由 mod-adaptive-bot 运行时动态接管外观、阵营、属性与机制光环

DELETE FROM `creature_template` WHERE `entry` = 990001;

INSERT INTO `creature_template` (
    `entry`, 
    `difficulty_entry_1`, `difficulty_entry_2`, `difficulty_entry_3`, 
    `KillCredit1`, `KillCredit2`, 
    `modelid1`, `modelid2`, `modelid3`, `modelid4`, 
    `name`, `subname`, `IconName`, 
    `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, 
    `faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, 
    `dmgschool`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, 
    `unit_class`, `unit_flags`, `unit_flags2`, `dynamicflags`, `family`, `trainer_type`, 
    `trainer_spell`, `trainer_class`, `trainer_race`, `type`, `type_flags`, `lootid`, 
    `pickpocketLoot`, `skinloot`, `resistance1`, `resistance2`, `resistance3`, `resistance4`, 
    `resistance5`, `resistance6`, `spell1`, `spell2`, `spell3`, `spell4`, `spell5`, `spell6`, 
    `spell7`, `spell8`, `PetSpellDataId`, `VehicleId`, `mingold`, `maxgold`, `AIName`, 
    `ScriptName`, `VerifiedBuild`
) VALUES (
    990001, 
    0, 0, 0, 
    0, 0, 
    165, 0, 0, 0,                      -- 默认占位模型为狼 (运行时由 C++ 随机覆写)
    '战斗随从', '守护者', '', 
    0, 1, 80, 0, 
    35, 0, 1.0, 1.14286, 1.0, 0,       -- 默认友好中立阵营
    0, 2000, 2000, 1.0, 1.0, 
    1, 0, 0, 0, 0, 0, 
    0, 0, 0, 1, 0, 0,                  -- type = 1 (野兽/通用)，无掉落
    0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, '', 
    'BotGuardianAI', 12340             -- 绑定核心 C++ 脚本名
);