#pragma once

#include "Task/AbstractTask.h"
#include "Vision/Oper/OperBoxImageAnalyzer.h"

namespace asst
{
// Shared navigation for scanning the list and locating a copilot's operator.
class ParadoxListTask : public AbstractTask
{
public:
    using AbstractTask::AbstractTask;

    void set_target(std::string name) { m_target = std::move(name); }

    const auto& get_result() const noexcept { return m_result; }

    bool found_target() const noexcept { return m_found; }

    bool target_completed() const noexcept { return m_completed; }

private:
    bool _run() override;
    bool prepare();
    bool rewind();
    bool return_to_list();
    std::string detail_name();
    bool same_page(const cv::Mat& before, const cv::Mat& after) const;

    std::string m_target;
    std::vector<OperBoxInfo> m_result;
    bool m_found = false;
    bool m_completed = false;
};
}
