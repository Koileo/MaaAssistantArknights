#include "MultiCopilotTaskPlugin.h"

#include <ranges>
#include <regex>

#include "Config/GeneralConfig.h"
#include "Config/Miscellaneous/CopilotConfig.h"
#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/Miscellaneous/BattleProcessTask.h"
#include "Task/ProcessTask.h"
#include "Task/StageNavigationHelper.h"
#include "Utils/Logger.hpp"
#include "Utils/Platform.hpp"
#include "Vision/Matcher.h"
#include "Vision/Miscellaneous/PipelineAnalyzer.h"

bool asst::MultiCopilotTaskPlugin::_run()
{
    LogTraceFunction;
    if (!has_pending_config()) {
        Log.info("MultiCopilot all stages completed");
        return true;
    }
    if (m_copilot_configs.size() <= static_cast<size_t>(m_index_current)) {
        LogError << __FUNCTION__ << "configs size:" << m_copilot_configs.size() << ", current index:" << m_index_current
                 << ", out of range";
        return false;
    }

    // A failed stage must be retried immediately, even if another queued stage
    // is also visible on a branched map.
    const auto selected = m_current_retry > 0 ? static_cast<size_t>(m_index_current) : select_visible_config();
    if (selected != static_cast<size_t>(m_index_current)) {
        Log.info(
            "MultiCopilot select visible stage",
            m_copilot_configs[selected].nav_name,
            "instead of",
            m_copilot_configs[m_index_current].nav_name);
        std::swap(m_copilot_configs[m_index_current], m_copilot_configs[selected]);
    }
    const auto& config = m_copilot_configs[m_index_current];

    std::string file_name;
    if (!Copilot.load(config.copilot_file)) {
        Log.error("CopilotConfig parse failed");
        return false;
    }
    file_name = utils::path_to_utf8_string(config.copilot_file);

    const auto& stage_name = Copilot.get_stage_name();
    if (!m_battle_task_ptr->set_stage_name(stage_name)) {
        Log.error("Not support stage");
        return false;
    }

    json::value info = basic_info_with_what("CopilotListLoadTaskFileSuccess");
    info["details"]["stage_name"] = Copilot.get_stage_name();
    info["details"]["file_name"] = std::move(file_name);
    info["details"]["id"] = config.id;
    callback(AsstMsg::SubTaskExtraInfo, info);

    bool ret = false;
    for (int i = 0; i < m_max_retry; ++i) {
        const auto image = ctrler()->get_image();
        ret = navigate_to_visible_stage(image, config.nav_name);
        if (!ret) {
            ret = navigate_to_resource_if_needed(config.nav_name);
            if (ret) {
                ret = is_stage_detail_opened(ctrler()->get_image());
            }
        }
        if (!ret) {
            ret = navigate_to_chapter_if_needed(config.nav_name);
            if (ret) {
                const auto chapter_image = ctrler()->get_image();
                ret = navigate_to_visible_stage(chapter_image, config.nav_name) || navigate_to_stage(config.nav_name);
            }
        }
        sleep(Config.get_options().task_delay);
        if (ret) {
            break;
        }
        if (need_exit()) {
            return false;
        }
    }

    ProcessTask(*this, { "NotUsePrts" }).set_ignore_error(true).set_retry_times(0).run();
    if (config.is_raid) {
        // 选择突袭模式
        ret = ret && ProcessTask(*this, { "RaidConfirm", "ChangeToRaidDifficulty" }).set_retry_times(20).run();
    }

    return ret;
}

bool asst::MultiCopilotTaskPlugin::navigate_to_resource_if_needed(const std::string& stage_name)
{
    static const std::regex resource_stage(R"(^(CE|LS|CA|AP|SK|PR-[A-D])-\d$)");
    std::smatch match;
    if (!std::regex_match(stage_name, match, resource_stage)) {
        return false;
    }

    const std::string line = match[1].str();
    if (m_current_resource_line == line) {
        return false;
    }

    Log.info("MultiCopilot enter resource line", line, "for stage", stage_name);
    const std::string task = stage_name == "CE-5" || stage_name == "LS-5"
                                 ? "CopilotResource@" + stage_name
                                 : stage_name;
    if (!ProcessTask(*this, { task }).set_retry_times(m_max_retry).run()) {
        return false;
    }
    m_current_resource_line = line;
    return true;
}

bool asst::MultiCopilotTaskPlugin::navigate_to_chapter_if_needed(const std::string& stage_name)
{
    static const std::regex main_stage(R"(^([A-Za-z]{0,3})(\d{1,2})-\d{1,2}(?:-?(\w+))*$)");
    std::smatch match;
    int chapter = -1;
    if (std::regex_match(stage_name, match, main_stage)) {
        static const std::unordered_set<std::string_view> valid_prefixes = { "", "S", "H", "R", "M", "JT" };
        std::string prefix = match[1].str();
        std::ranges::transform(prefix, prefix.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (valid_prefixes.contains(prefix)) {
            chapter = std::stoi(match[2].str());
        }
        else {
            return true;
        }
    }
    else {
        return true;
    }

    if (m_current_chapter == chapter) {
        return true;
    }

    const std::string task = "Episode" + std::to_string(chapter);
    if (!Task.get(task)) {
        Log.error("MultiCopilot chapter task not found", task);
        return false;
    }
    Log.info("MultiCopilot enter chapter", chapter, "for stage", stage_name);
    if (!ProcessTask(*this, { task }).set_retry_times(m_max_retry).run()) {
        auto [threshold_low, threshold_high] = get_stage_thresholds(stage_name);
        const auto episode = chapter < 10 ? "0" + std::to_string(chapter) : std::to_string(chapter);
        const auto episodes = find_stage(ctrler()->get_image(), threshold_low, threshold_high);
        const auto target = std::ranges::find_if(episodes, [&](const OcrPack::Result& result) {
            return result.text == episode && result.rect.x > 900 && result.rect.y > 600;
        });
        if (target == episodes.end()) {
            return false;
        }

        if (target->rect.x >= 1020 && target->rect.x <= 1140) {
            Log.info("MultiCopilot already in visible episode", episode);
            m_current_chapter = chapter;
            return true;
        }

        Log.info("MultiCopilot click visible episode button", episode, target->rect);
        ctrler()->click(target->rect);
        sleep(Config.get_options().task_delay);
        // The caller verifies the target stage after switching episodes.
        return true;
    }
    m_current_chapter = chapter;
    return true;
}

void asst::MultiCopilotTaskPlugin::set_cycle_tasks(const std::vector<std::shared_ptr<AbstractTask>>& tasks)
{
    m_cycle_tasks.assign(tasks.begin(), tasks.end());
}

bool asst::MultiCopilotTaskPlugin::was_abandoned_for_leak() const
{
    return m_battle_task_ptr && m_battle_task_ptr->was_abandoned_for_leak();
}

const std::string& asst::MultiCopilotTaskPlugin::current_stage_name() const
{
    return m_copilot_configs.at(m_index_current).nav_name;
}

bool asst::MultiCopilotTaskPlugin::complete_current_battle(bool three_stars)
{
    if (!has_pending_config()) {
        return true;
    }

    const auto& config = m_copilot_configs[m_index_current];
    if (three_stars) {
        Log.info("MultiCopilot stage completed with three stars", config.nav_name);
        ++m_index_current;
        if (m_switch_copilot_on_failure) {
            while (has_pending_config() && m_copilot_configs[m_index_current].nav_name == config.nav_name) {
                Log.info("MultiCopilot skip unused fallback", m_copilot_configs[m_index_current].copilot_file);
                ++m_index_current;
            }
        }
        m_current_retry = 0;
        if (!has_pending_config()) {
            for (const auto& task : m_cycle_tasks) {
                if (const auto ptr = task.lock()) {
                    ptr->set_enable(false);
                }
            }
        }
        return true;
    }

    json::value info = basic_info_with_what("CopilotListTaskFileFailed");
    info["details"]["stage_name"] = config.nav_name;
    info["details"]["file_name"] = utils::path_to_utf8_string(config.copilot_file);
    info["details"]["id"] = config.id;
    callback(AsstMsg::SubTaskExtraInfo, info);

    if (m_switch_copilot_on_failure) {
        const auto next = static_cast<size_t>(m_index_current + 1);
        if (next < m_copilot_configs.size() && m_copilot_configs[next].nav_name == config.nav_name) {
            Log.warn(
                "MultiCopilot stage failed; switch to fallback",
                config.nav_name,
                m_copilot_configs[next].copilot_file);
            ++m_index_current;
            m_current_retry = 0;
            return true;
        }
        Log.error("MultiCopilot exhausted all fallbacks for stage", config.nav_name);
        return false;
    }

    if (m_current_retry == 0) {
        Log.warn("MultiCopilot stage did not receive three stars; retry once", config.nav_name);
        ++m_current_retry;
        return true;
    }

    Log.error("MultiCopilot stage did not receive three stars after retry; stop queue", config.nav_name);
    return false;
}

bool asst::MultiCopilotSettlementTask::_run()
{
    if (!m_multi_copilot_task_ptr || !m_multi_copilot_task_ptr->has_pending_config()) {
        return true;
    }

    const bool abandoned_for_leak = m_multi_copilot_task_ptr->was_abandoned_for_leak();
    Task.get<OcrTaskInfo>("Copilot@FightMissionFailedStageMapFlag-Retry")->text = {
        m_multi_copilot_task_ptr->current_stage_name(),
    };

    ProcessTask settlement(*this, { "Copilot@WaitUntilEndOfAction-Retry" });
    settlement.set_retry_times(0);
    if (!settlement.run()) {
        if (abandoned_for_leak) {
            Log.warn("MultiCopilot leak settlement was not recognized; continue with fallback");
            ProcessTask(*this, { "Copilot@ClickCornerUntilStartButton" }).set_retry_times(20).run();
            return m_multi_copilot_task_ptr->complete_current_battle(false);
        }
        return false;
    }

    const auto& result = settlement.get_last_task_name();
    const bool three_stars = !abandoned_for_leak &&
                             (result == "Copilot@StageDrops-Retry-Stars-3" ||
                              result == "Copilot@StageDrops-Retry-Stars-Adverse");
    const bool failed = result == "Copilot@FightMissionFailedStageFlag-Retry" ||
                        result == "Copilot@FightMissionFailedStageMapFlag-Retry";
    const bool recognized_result = three_stars || result == "Copilot@StageDrops-Retry-Stars-2" || failed;
    if (!recognized_result) {
        Log.error("MultiCopilot settlement result is unknown", result);
        if (abandoned_for_leak) {
            ProcessTask(*this, { "Copilot@ClickCornerUntilStartButton" }).set_retry_times(20).run();
            return m_multi_copilot_task_ptr->complete_current_battle(false);
        }
        return false;
    }

    if (abandoned_for_leak) {
        if (!failed &&
            !ProcessTask(*this, { "Copilot@ClickCornerUntilStartButton" }).set_retry_times(20).run()) {
            Log.warn("MultiCopilot could not leave leak settlement; continue with fallback");
        }
        return m_multi_copilot_task_ptr->complete_current_battle(false);
    }

    if (!failed && !ProcessTask(*this, { "Copilot@ClickCornerUntilStartButton" }).set_retry_times(20).run()) {
        return false;
    }
    return m_multi_copilot_task_ptr->complete_current_battle(three_stars);
}

size_t asst::MultiCopilotTaskPlugin::select_visible_config()
{
    const auto current = static_cast<size_t>(m_index_current);
    if (current + 1 >= m_copilot_configs.size()) {
        return current;
    }

    const auto image = ctrler()->get_image();
    if (is_stage_detail_opened(image)) {
        return current;
    }

    auto [threshold_low, threshold_high] = get_stage_thresholds(m_copilot_configs[current].nav_name);
    const auto stages = find_stage(image, threshold_low, threshold_high);

    for (size_t i = current; i < m_copilot_configs.size(); ++i) {
        const auto& target = m_copilot_configs[i].nav_name;
        if (std::ranges::any_of(stages, [&](const OcrPack::Result& result) { return result.text == target; })) {
            return i;
        }
    }
    return current;
}

bool asst::MultiCopilotTaskPlugin::navigate_to_stage(const std::string& stage_name)
{
    // 优先检查是否存在对应活动关卡名的模板资源，如果存在则走模板匹配
    std::string templ_path = StageNavigationHelper::get_stage_template_path(stage_name);
    if (!templ_path.empty()) {
        Log.info("Stage template found, using template matching for", stage_name, ", templ:", templ_path);
        // 动态注入模板路径到 MatchTaskInfo（需带 .png 后缀）
        Task.get<MatchTaskInfo>(stage_name + "@Copilot@ClickStageByTemplate")->templ_names = { templ_path + ".png" };
        Task.get<OcrTaskInfo>(stage_name + "@Copilot@ClickedCorrectStage")->text = { stage_name };
        return ProcessTask(*this, { stage_name + "@Copilot@StageNavigationByTemplateMatchBegin" })
            .set_retry_times(20)
            .run();
    }

    // 模板不存在，使用基于图像分析的 OCR 方案
    Log.info("No stage template available, using image-based OCR for", stage_name);

    auto [threshold_low, threshold_high] = get_stage_thresholds(stage_name);
    auto image = ctrler()->get_image();
    auto stages = find_stage(image, threshold_low, threshold_high);
    auto it = std::ranges::find_if(stages, [&](const OcrPack::Result& r) { return r.text == stage_name; });
    if (it != stages.end()) {
        if (enter_stage(it->rect, stage_name)) {
            return true;
        }
    }

    ProcessTask(*this, { "Copilot@FullStageNavigation" }).set_retry_times(20).run();
    sleep(Config.get_options().task_delay);
    image = ctrler()->get_image();
    stages = find_stage(image, threshold_low, threshold_high);
    it = std::ranges::find_if(stages, [&](const OcrPack::Result& r) { return r.text == stage_name; });
    if (it != stages.end()) {
        if (enter_stage(it->rect, stage_name)) {
            return true;
        }
    }

    for (int i = 0; i < m_max_retry; ++i) {
        if (need_exit()) {
            return false;
        }
        ProcessTask(*this, { "Copilot@StageNavigationSlowlySwipeLeft" }).set_retry_times(20).run();
        sleep(Config.get_options().task_delay);
        image = ctrler()->get_image();
        stages = find_stage(image, threshold_low, threshold_high);
        it = std::ranges::find_if(stages, [&](const OcrPack::Result& r) { return r.text == stage_name; });
        if (it != stages.end()) {
            if (enter_stage(it->rect, stage_name)) {
                return true;
            }
        }
    }

    // 划 10 次到最右，然后扫有无初见剧情
    auto plot_task = ProcessTask(*this, { "Copilot@ChapterSwipeToTheRightAndPlot" });
    if (need_exit()) {
        return false;
    }
    if (plot_task.run()) {
        sleep(Config.get_options().task_delay);
        image = ctrler()->get_image();
        stages = find_stage(image, threshold_low, threshold_high);
        it = std::ranges::find_if(stages, [&](const OcrPack::Result& r) { return r.text == stage_name; });
        if (it != stages.end()) {
            if (enter_stage(it->rect, stage_name)) {
                return true;
            }
        }
    }

    return false;
}

bool asst::MultiCopilotTaskPlugin::navigate_to_visible_stage(const cv::Mat& image, const std::string& stage_name)
{
    if (is_stage_detail_opened(image) && confirm_stage_name(image, stage_name)) {
        Log.info("MultiCopilot already at target stage", stage_name);
        return true;
    }

    auto [threshold_low, threshold_high] = get_stage_thresholds(stage_name);
    const auto stages = find_stage(image, threshold_low, threshold_high);
    const auto it = std::ranges::find_if(stages, [&](const OcrPack::Result& result) { return result.text == stage_name; });
    if (it == stages.end()) {
        return false;
    }

    Log.info("MultiCopilot target stage is visible", stage_name);
    return enter_stage(it->rect, stage_name);
}

bool asst::MultiCopilotTaskPlugin::enter_stage(const Rect rect, const std::string& stage_name)
{
    ctrler()->click(rect);
    sleep(Config.get_options().task_delay);
    auto image_entered = ctrler()->get_image();
    if (is_stage_detail_opened(image_entered)) { // 关卡介绍已展开
        sleep(Config.get_options().task_delay);
        return confirm_stage_name(image_entered, stage_name);
    }

    return false;
}

asst::OCRer::ResultsVec asst::MultiCopilotTaskPlugin::find_stage(
    const cv::Mat& image,
    std::tuple<int, int, int> threshold_low,
    std::tuple<int, int, int> threshold_high)
{
    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2HSV);
    auto [l1, l2, l3] = threshold_low;
    auto [h1, h2, h3] = threshold_high;
    cv::inRange(gray, cv::Scalar(l1, l2, l3), cv::Scalar(h1, h2, h3), gray);
    cv::dilate(gray, gray, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(15, 8)), cv::Point(-1, -1), 1);
    std::vector<cv::Mat> channels = { gray, gray, gray };
    cv::Mat gray3;
    cv::merge(channels, gray3);
    cv::bitwise_and(image, gray3, gray3);
    OCRer ocr(gray3);
    ocr.set_task_info("ClickStageName");
    if (!ocr.analyze()) {
        return {};
    }
    auto result = ocr.get_result();
    std::erase_if(result, [](const OcrPack::Result& r) { return r.text.size() == 1 || r.score < 0.5; });
    LogInfo << __FUNCTION__ << "stage results:" << result;
    return result;
}

std::pair<std::tuple<int, int, int>, std::tuple<int, int, int>> asst::MultiCopilotTaskPlugin::get_stage_thresholds(
    const std::string& stage_name)
{
    const auto stage_task = Task.get<OcrTaskInfo>(stage_name + "@ClickStageName");
    if (stage_task && stage_task->special_params.size() >= 6) {
        return {
            { stage_task->special_params[0], stage_task->special_params[1], stage_task->special_params[2] },
            { stage_task->special_params[3], stage_task->special_params[4], stage_task->special_params[5] }
        };
    }
    const auto base_task = Task.get<OcrTaskInfo>("ClickStageName");
    if (base_task && base_task->special_params.size() >= 6) {
        return {
            { base_task->special_params[0], base_task->special_params[1], base_task->special_params[2] },
            { base_task->special_params[3], base_task->special_params[4], base_task->special_params[5] }
        };
    }
    return { { 0, 0, 160 }, { 180, 30, 255 } };
}

bool asst::MultiCopilotTaskPlugin::is_stage_detail_opened(const cv::Mat& image)
{
    PipelineAnalyzer match(image);
    match.set_tasks({ "StartButton1" });
    return match.analyze().has_value();
}

bool asst::MultiCopilotTaskPlugin::confirm_stage_name(const cv::Mat& image, const std::string& stage_name)
{
    const auto ocr_check = [&](const OCRer::ResultsVecOpt& ret_opt) {
        return ret_opt.has_value() &&
               std::ranges::any_of(ret_opt.value(), [&](const OcrPack::Result& r) { return r.text == stage_name; });
    };
    OCRer ocr(image);
    ocr.set_task_info("ClickedCorrectStage");
    if (ocr_check(ocr.analyze())) {
        return true;
    }

    for (int i = 0; i < 3; ++i) {
        sleep(Config.get_options().task_delay);
        OCRer re_OCR(ctrler()->get_image());
        re_OCR.set_task_info("ClickedCorrectStage");
        if (ocr_check(re_OCR.analyze())) {
            return true;
        }
    }
    LogError << __FUNCTION__ << "confirm stage name failed after retrying 3 times, stage name:" << stage_name;
    return false;
}
