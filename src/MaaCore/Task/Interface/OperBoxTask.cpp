#include "OperBoxTask.h"

#include "Task/Miscellaneous/OperBoxRecognitionTask.h"
#include "Task/Miscellaneous/ScreenshotTaskPlugin.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"

asst::OperBoxTask::OperBoxTask(const AsstCallback& callback, Assistant* inst) :
    InterfaceTask(callback, inst, TaskType)
{
    // 子任务将在 set_params 或首次运行时构建
}

bool asst::OperBoxTask::set_params(const json::value& params)
{
    LogTraceFunction;

    auto paradox_filter_opt = params.find<bool>("paradox_filter");
    if (paradox_filter_opt) {
        m_paradox_filter = *paradox_filter_opt;
        LogInfo << "OperBoxTask: paradox_filter = " << m_paradox_filter;
    }

    // 重新构建子任务列表
    build_subtasks();
    m_subtasks_built = true;

    return true;
}

bool asst::OperBoxTask::run()
{
    LogTraceFunction;

    // 如果 set_params 没有被调用，使用默认配置构建子任务
    if (!m_subtasks_built) {
        LogInfo << "OperBoxTask: building subtasks with default config";
        build_subtasks();
        m_subtasks_built = true;
    }

    return InterfaceTask::run();
}

void asst::OperBoxTask::build_subtasks()
{
    LogTraceFunction;
    LogInfo << "OperBoxTask: building subtasks, paradox_filter = " << m_paradox_filter;

    m_subtasks.clear();

    auto enter_task = std::make_shared<ProcessTask>(m_callback, m_inst, TaskType);
    enter_task->set_tasks({ "OperBoxBegin" }).set_ignore_error(true);
    enter_task->register_plugin<ScreenshotTaskPlugin>();
    m_subtasks.emplace_back(enter_task);

    auto expand_role_task = std::make_shared<ProcessTask>(m_callback, m_inst, TaskType);
    expand_role_task->set_tasks({ "BattleQuickFormationExpandRole" });
    m_subtasks.emplace_back(expand_role_task);

    auto select_all_task = std::make_shared<ProcessTask>(m_callback, m_inst, TaskType);
    select_all_task->set_tasks({ "BattleQuickFormationRole-All", "BattleQuickFormationRole-All-OCR" });
    m_subtasks.emplace_back(select_all_task);

    auto recognition_task = std::make_shared<OperBoxRecognitionTask>(m_callback, m_inst, TaskType);
    recognition_task->set_paradox_filter(m_paradox_filter);
    recognition_task->set_retry_times(0);
    m_subtasks.emplace_back(recognition_task);

    LogInfo << "OperBoxTask: built " << m_subtasks.size() << " subtasks";
}
