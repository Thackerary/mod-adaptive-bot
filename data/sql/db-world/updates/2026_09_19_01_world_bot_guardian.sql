-- 伴随型战斗护卫统一载体模板 (Entry: 990001)
-- 由 mod-adaptive-bot 运行时动态接管外观、阵营、属性与机制光环

-- 伴随型战斗护卫统一载体模板 (Entry: 990001) - 适配最新 AC 表结构
DELETE FROM `creature_template` WHERE `entry` = 990001;
DELETE FROM `creature_template_model` WHERE `CreatureID` = 990001;

INSERT INTO `creature_template` (
    `entry`, `name`, `subname`, `IconName`, 
    `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, 
    `speed_walk`, `speed_run`, `rank`, `unit_class`, `unit_flags`, 
    `type`, `type_flags`, `RegenHealth`, `flags_extra`, `ScriptName`, `VerifiedBuild`
) VALUES (
    990001, '战斗随从', '守护者', '', 
    1, 80, 0, 390, 0, 
    1.0, 1.14286, 0, 1, 0, 
    1, 0, 1, 0, 'BotGuardianAI', 0
);

-- 默认占位模型为狼 (DisplayID: 165)，运行时由 C++ 动态覆写
INSERT INTO `creature_template_model` 
(`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) 
VALUES (990001, 0, 165, 1.0, 1.0, 0);