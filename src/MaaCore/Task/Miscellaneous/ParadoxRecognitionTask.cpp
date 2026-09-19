#include "ParadoxRecognitionTask.h"

#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/Miscellaneous/CopilotConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/Miscellaneous/BattleProcessTask.h"
#include "Task/Miscellaneous/ParadoxListTask.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/OCRer.h"

bool asst::ParadoxRecognitionTask::_run()
{
    LogTraceFunction;
    if (m_paradox_files.empty()) {
        LogError << __FUNCTION__ << "no paradox oper set";
        return false;
    }

    const auto& [id, raw_path] = m_paradox_files.front();
    const auto& path = utils::path(raw_path);
    std::string file_name;
    if (!Copilot.load(path)) {
        Log.error("CopilotConfig parse failed");
        return false;
    }
    file_name = utils::path_to_utf8_string(path);
    const auto& stage_name = Copilot.get_stage_name();
    if (!m_battle_task_ptr->set_stage_name(stage_name)) {
        Log.error("Not support stage");
        return false;
    }

    json::value info = basic_info_with_what("CopilotListLoadTaskFileSuccess");
    info["details"]["stage_name"] = Copilot.get_stage_name();
    info["details"]["file_name"] = file_name;
    info["details"]["id"] = id;
    callback(AsstMsg::SubTaskExtraInfo, info);

    m_navigate_name = standardize_name(stage_name);
    LogInfo << __FUNCTION__ << "navigate name:" << m_navigate_name;
    m_paradox_files.erase(m_paradox_files.begin());

    const auto& all_oper_names = BattleData.get_all_chars();
    // BattleData also contains traps/tokens (for example
    // `trap_279_pirene`, 喷泉水池).  Their ids can end with the same
    // operator suffix as a paradox stage (`mem_irene_1` -> `irene`).
    // Only real operators may be selected for paradox simulation; otherwise
    // the trap's low rarity makes us skip skill selection and leaves the game
    // stuck on the skill page.
    const auto& it = std::find_if(all_oper_names.begin(), all_oper_names.end(), [&](const auto& pair) {
        return pair.second && pair.second->role != battle::Role::Drone && pair.first.ends_with(m_navigate_name);
    });

    if (it == all_oper_names.end()) {
        report_status("ParadoxOperatorNotFound");
        return false;
    }
    m_oper_name = {
        it->second->role,    it->second->rarity,  it->second->name,    it->second->name_en,
        it->second->name_jp, it->second->name_kr, it->second->name_tw,
    };

    // 设置技能
    m_skill_num = 1;
    auto* groups = &Copilot.get_data().groups;
    for (const auto& [_, __, ___, opers_vec] : *groups) {
        if (opers_vec.empty()) {
            continue;
        }
        for (const auto& oper : opers_vec) {
            if (match_oper(oper.name)) {
                // Copilot skill 0 means “no explicit skill”.  The paradox
                // page still requires selecting one for 3+ star operators;
                // use skill 1 as the safe default and never construct an
                // invalid ParadoxChooseSkill0 task.
                if (oper.skill >= 1 && oper.skill <= 3) {
                    m_skill_num = oper.skill;
                }
            }
        }
    }

    LogInfo << __FUNCTION__ << "operator:" << m_oper_name.name << " rarity:" << m_oper_name.rarity
            << " skill:" << m_skill_num;

    ParadoxListTask locate(m_callback, m_inst, m_task_chain);
    locate.set_target(m_oper_name.name);
    locate.set_retry_times(0);
    if (!locate.run()) {
        report_status("ParadoxRecognitionFailed");
        return false;
    }
    if (!locate.found_target()) {
        report_status("ParadoxOperatorNotFound");
        return false;
    }
    if (locate.target_completed()) {
        report_status("ParadoxAlreadyCompleted");
        return_to_oper_list();
        return false;
    }
    return enter_paradox(m_skill_num, m_oper_name.rarity);
}

std::string asst::ParadoxRecognitionTask::standardize_name(const std::string& navigate_name)
{
    size_t length = navigate_name.length();
    return navigate_name.substr(4, length - 6);
}

bool asst::ParadoxRecognitionTask::enter_paradox(const int skill_num, const int rarity)
{
    // ParadoxListTask leaves the verified detail page open.
    if (ProcessTask(*this, { "ParadoxAlreadyCompleted" }).set_retry_times(0).run()) {
        report_status("ParadoxAlreadyCompleted");
        return_to_oper_list();
        return false;
    }
    if (!ProcessTask(*this, { "ParadoxStartSimulation" }).set_retry_times(1).run()) {
        if (!ProcessTask(*this, { "OperParadoxBegin" }).set_retry_times(3).run()) {
            report_status("ParadoxRecognitionFailed");
            return_to_oper_list();
            return false;
        }
        if (ProcessTask(*this, { "ParadoxAlreadyCompleted" }).set_retry_times(0).run()) {
            report_status("ParadoxAlreadyCompleted");
            return_to_oper_list();
            return false;
        }
        if (!ProcessTask(*this, { "OperOpenParadoxChooseSkill" }).set_retry_times(3).run()) {
            report_status("ParadoxSkillSelectFailed");
            return_to_oper_list();
            return false;
        }
    }
    if (rarity > 2) {
        if (!ProcessTask(*this, { "ParadoxChooseSkill" + std::to_string(skill_num) }).set_retry_times(3).run()) {
            report_status("ParadoxSkillSelectFailed");
            return_to_oper_list();
            return false;
        }
        sleep(500);
    }
    report_status("ParadoxReady");
    return true;
}

void asst::ParadoxRecognitionTask::return_to_oper_list() const
{
    if (!ProcessTask(*this, { "ParadoxReturnOperListFlag" }).set_retry_times(0).run()) {
        ProcessTask(*this, { "ParadoxReturnUntilOperList" }).set_retry_times(3).run();
    }
    ProcessTask(*this, { "BattleQuickFormationExpandRole" }).set_retry_times(3).run();
}

void asst::ParadoxRecognitionTask::report_status(const std::string& status)
{
    json::value info = basic_info_with_what(status);
    info["details"]["stage_name"] = Copilot.get_stage_name();
    info["details"]["operator"] = m_oper_name.name;
    callback(AsstMsg::SubTaskExtraInfo, info);
}

void asst::ParadoxRecognitionTask::add_file(int id, const std::string& navigate_name)
{
    m_paradox_files.emplace_back(id, navigate_name);
}

bool asst::ParadoxRecognitionTask::match_oper(const std::string& name) const
{
    return m_oper_name.name == name || m_oper_name.name_en == name || m_oper_name.name_jp == name ||
           m_oper_name.name_kr == name || m_oper_name.name_tw == name;
}
