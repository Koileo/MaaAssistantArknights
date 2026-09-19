#pragma once
#include "Common/AsstBattleDef.h"
#include "Task/AbstractTask.h"
#include "Vision/Oper/OperBoxImageAnalyzer.h"

namespace asst
{
class OperBoxRecognitionTask : public AbstractTask
{
public:
    using AbstractTask::AbstractTask;
    virtual ~OperBoxRecognitionTask() override = default;

    void set_paradox_filter(bool enabled) noexcept { m_paradox_filter = enabled; }

protected:
    virtual bool _run() override;
    bool swipe_to_beginning();
    void swipe_page();
    void callback_analyze_result(bool done);
    bool swipe_and_analyze();

    std::unordered_map<std::string, OperBoxInfo> m_own_opers;
    bool m_paradox_filter = false;
};
}
