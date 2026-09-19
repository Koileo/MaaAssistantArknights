#include "ParadoxListTask.h"

#include <unordered_set>

#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Controller/Controller.h"
#include "MaaUtils/NoWarningCV.hpp"
#include "Task/ProcessTask.h"
#include "Vision/OCRer.h"

bool asst::ParadoxListTask::return_to_list()
{
    if (!ProcessTask(*this, { "ParadoxReturnOperListFlag" }).set_retry_times(0).run() &&
        !ProcessTask(*this, { "ParadoxReturnUntilOperList" }).set_retry_times(3).run()) {
        return false;
    }
    return ProcessTask(*this, { "BattleQuickFormationExpandRole" }).set_retry_times(3).run();
}

bool asst::ParadoxListTask::prepare()
{
    if (!ProcessTask(*this, { "ParadoxReturnOperListFlag" }).set_retry_times(0).run()) {
        if (!ProcessTask(*this, { "OperBoxBegin" }).set_retry_times(1).run() && !return_to_list()) {
            return false;
        }
    }
    if (!ProcessTask(*this, { "BattleQuickFormationExpandRole" }).set_retry_times(3).run() ||
        !ProcessTask(*this, { "BattleQuickFormationRole-All", "BattleQuickFormationRole-All-OCR" }).run()) {
        return false;
    }
    // Checking the selected tab avoids toggling its sorting direction.
    if (!ProcessTask(*this, { "OperBoxParadoxSelected" }).set_retry_times(0).run() &&
        !ProcessTask(*this, { "OperBoxOpenParadoxMenu", "OperBoxSelectParadox" }).set_retry_times(3).run()) {
        return false;
    }
    return rewind();
}

bool asst::ParadoxListTask::same_page(const cv::Mat& before, const cv::Mat& after) const
{
    if (before.empty() || after.empty() || before.size() != after.size()) {
        return false;
    }
    // Exclude the menu and the scrollbar; compare the actual operator cards.
    const cv::Rect grid(20, 80, 1100, 620);
    cv::Mat difference;
    cv::absdiff(before(grid), after(grid), difference);
    const auto mean = cv::mean(difference);
    return (mean[0] + mean[1] + mean[2]) / 3 < 1.0;
}

bool asst::ParadoxListTask::rewind()
{
    int unchanged = 0;
    for (int page = 0; page < 100 && !need_exit(); ++page) {
        const auto before = ctrler()->get_image();
        if (!ProcessTask(*this, { "OperBoxSwipeToTheLeft" }).run() || !sleep(500)) {
            return false;
        }
        unchanged = same_page(before, ctrler()->get_image()) ? unchanged + 1 : 0;
        if (unchanged >= 2) {
            return true;
        }
    }
    return false;
}

std::string asst::ParadoxListTask::detail_name()
{
    for (int retry = 0; retry < 3 && !need_exit(); ++retry) {
        if (!sleep(500)) {
            return {};
        }
        OCRer ocr(ctrler()->get_image());
        ocr.set_task_info("ParadoxDetailOperatorNameOCR");
        // The ROI contains several text lines. Detection must stay enabled.
        ocr.set_required({});
        ocr.set_without_det(false);
        std::unordered_set<std::string> names;
        if (const auto result = ocr.analyze()) {
            for (const auto& item : *result) {
                if (const auto oper = BattleData.find_first_oper(battle::Role::Unknown, item.text);
                    oper && oper->role != battle::Role::Drone) {
                    names.emplace(oper->name);
                }
            }
        }
        if (names.size() == 1) {
            return *names.begin();
        }
    }
    save_img(utils::path("debug") / utils::path("paradox"));
    return {};
}

bool asst::ParadoxListTask::_run()
{
    m_result.clear();
    m_found = false;
    m_completed = false;
    if (!prepare()) {
        return false;
    }
    std::unordered_set<std::string> seen;
    int unchanged = 0;
    for (int page = 0; page < 100 && !need_exit(); ++page) {
        OperBoxImageAnalyzer analyzer(ctrler()->get_image());
        analyzer.set_paradox_filter(true);
        if (!analyzer.analyze()) {
            // Unreadable cards are a failed scan, never an empty completed list.
            return false;
        }
        for (auto card : analyzer.get_result()) {
            if (need_exit()) {
                return false;
            }
            if (!card.paradox_unlocked || (m_target.empty() && card.paradox_completed)) {
                continue;
            }
            if (!ctrler()->click(card.rect.move(Rect(20, 50, 70, 100)))) {
                return false;
            }
            card.name = detail_name();
            if (card.name.empty()) {
                return_to_list();
                return false;
            }
            const auto oper = BattleData.find_first_oper(battle::Role::Unknown, card.name);
            if (!oper) {
                return_to_list();
                return false;
            }
            card.id = oper->id;
            card.rarity = oper->rarity;
            if (!m_target.empty() && card.name == m_target) {
                m_found = true;
                m_completed = card.paradox_completed;
                // Leave the verified operator detail page open for skill selection.
                return true;
            }
            if (!return_to_list()) {
                return false;
            }
            if (seen.emplace(card.name).second) {
                m_result.emplace_back(std::move(card));
            }
        }
        const auto before = ctrler()->get_image();
        if (!ProcessTask(*this, { "OperBoxSlowlySwipeToTheRight" }).run() || !sleep(500)) {
            return false;
        }
        unchanged = same_page(before, ctrler()->get_image()) ? unchanged + 1 : 0;
        if (unchanged >= 2) {
            return true;
        }
    }
    return false;
}
