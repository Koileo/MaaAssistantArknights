#pragma once
#include "Task/AbstractTask.h"

#include <optional>

#include "MaaUtils/NoWarningCV.hpp"
#include "Vision/OCRer.h"

namespace asst
{
class BattleProcessTask;
class ProcessTask;

class MultiCopilotTaskPlugin : public AbstractTask
{
public:
    struct MultiCopilotConfig
    {
        std::filesystem::path copilot_file; // 文件名
        std::string nav_name;               // 关卡名
        bool is_raid = false;               // 是否是突袭
        int id;
    };

public:
    using AbstractTask::AbstractTask;
    virtual ~MultiCopilotTaskPlugin() override = default;

    void set_multi_copilot_config(std::vector<MultiCopilotConfig> config) { m_copilot_configs = std::move(config); }

    void set_battle_task_ptr(const std::shared_ptr<BattleProcessTask>& ptr) { m_battle_task_ptr = ptr; }
    void set_cycle_tasks(const std::vector<std::shared_ptr<AbstractTask>>& tasks);
    void set_switch_copilot_on_failure(bool value) { m_switch_copilot_on_failure = value; }

    bool has_pending_config() const { return m_index_current < static_cast<int>(m_copilot_configs.size()); }
    bool was_abandoned_for_leak() const;
    bool complete_current_battle(bool three_stars);

private:
    virtual bool _run() override;
    size_t select_visible_config();
    bool navigate_to_stage(const std::string& stage_name);
    bool enter_stage(const Rect rect, const std::string& stage_name);
    OCRer::ResultsVec find_stage(
        const cv::Mat& image,
        std::tuple<int, int, int> threshold_low,
        std::tuple<int, int, int> threshold_high);
    bool is_stage_detail_opened(const cv::Mat& image); // 检查关卡介绍是否已展开
    bool confirm_stage_name(const cv::Mat& image, const std::string& stage_name);

    std::vector<MultiCopilotConfig> m_copilot_configs;
    int m_index_current = 0; // 当前执行的索引
    int m_current_retry = 0;
    bool m_switch_copilot_on_failure = false;
    std::shared_ptr<BattleProcessTask> m_battle_task_ptr = nullptr;
    std::vector<std::weak_ptr<AbstractTask>> m_cycle_tasks;
    int m_max_retry = 20;
};

class MultiCopilotSettlementTask : public AbstractTask
{
public:
    using AbstractTask::AbstractTask;
    virtual ~MultiCopilotSettlementTask() override = default;

    void set_multi_copilot_task_ptr(const std::shared_ptr<MultiCopilotTaskPlugin>& ptr)
    {
        m_multi_copilot_task_ptr = ptr;
    }

private:
    virtual bool _run() override;

    std::shared_ptr<MultiCopilotTaskPlugin> m_multi_copilot_task_ptr = nullptr;
};
}
