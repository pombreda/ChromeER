// Copyright 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/scheduler/scheduler.h"

#include <string>
#include <vector>

#include "base/debug/trace_event.h"
#include "base/logging.h"
#include "base/memory/scoped_vector.h"
#include "base/message_loop/message_loop.h"
#include "base/power_monitor/power_monitor.h"
#include "base/power_monitor/power_monitor_source.h"
#include "base/run_loop.h"
#include "base/time/time.h"
#include "cc/test/begin_frame_args_test.h"
#include "cc/test/ordered_simple_task_runner.h"
#include "cc/test/scheduler_test_common.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

#define EXPECT_ACTION(action, client, action_index, expected_num_actions) \
  do {                                                                    \
    EXPECT_EQ(expected_num_actions, client.num_actions_());               \
    if (action_index >= 0) {                                              \
      ASSERT_LT(action_index, client.num_actions_()) << scheduler;        \
      EXPECT_STREQ(action, client.Action(action_index));                  \
    }                                                                     \
    for (int i = expected_num_actions; i < client.num_actions_(); ++i)    \
      ADD_FAILURE() << "Unexpected action: " << client.Action(i)          \
                    << " with state:\n" << client.StateForAction(i);      \
  } while (false)

#define EXPECT_NO_ACTION(client) EXPECT_ACTION("", client, -1, 0)

#define EXPECT_SINGLE_ACTION(action, client) \
  EXPECT_ACTION(action, client, 0, 1)

#define EXPECT_SCOPED(statements) \
  {                               \
    SCOPED_TRACE("");             \
    statements;                   \
  }

#define CREATE_SCHEDULER_AND_INIT_SURFACE(settings)            \
  TestScheduler* scheduler = client.CreateScheduler(settings); \
  EXPECT_SCOPED(client.InitializeOutputSurfaceAndFirstCommit(scheduler));

namespace cc {
namespace {

class FakeSchedulerClient;

class FakeSchedulerClient : public SchedulerClient {
 public:
  class FakeExternalBeginFrameSource : public BeginFrameSourceMixIn {
   public:
    explicit FakeExternalBeginFrameSource(FakeSchedulerClient* client)
        : client_(client) {}
    ~FakeExternalBeginFrameSource() override {}

    void OnNeedsBeginFramesChange(bool needs_begin_frames) override {
      if (needs_begin_frames) {
        client_->PushAction("SetNeedsBeginFrames(true)");
      } else {
        client_->PushAction("SetNeedsBeginFrames(false)");
      }
      client_->states_.push_back(client_->scheduler_->AsValue());
    }

    void TestOnBeginFrame(const BeginFrameArgs& args) {
      return CallOnBeginFrame(args);
    }

   private:
    FakeSchedulerClient* client_;
  };

  class FakePowerMonitorSource : public base::PowerMonitorSource {
   public:
    FakePowerMonitorSource() {}
    ~FakePowerMonitorSource() override {}
    void GeneratePowerStateEvent(bool on_battery_power) {
      on_battery_power_impl_ = on_battery_power;
      ProcessPowerEvent(POWER_STATE_EVENT);
      base::MessageLoop::current()->RunUntilIdle();
    }
    bool IsOnBatteryPowerImpl() override { return on_battery_power_impl_; }

   private:
    bool on_battery_power_impl_;
  };

  FakeSchedulerClient()
      : automatic_swap_ack_(true),
        begin_frame_is_sent_to_children_(false),
        now_src_(TestNowSource::Create()),
        task_runner_(new OrderedSimpleTaskRunner(now_src_, true)),
        fake_external_begin_frame_source_(nullptr),
        fake_power_monitor_source_(new FakePowerMonitorSource),
        power_monitor_(make_scoped_ptr<base::PowerMonitorSource>(
            fake_power_monitor_source_)),
        scheduler_(nullptr) {
    // A bunch of tests require Now() to be > BeginFrameArgs::DefaultInterval()
    now_src_->AdvanceNow(base::TimeDelta::FromMilliseconds(100));
    // Fail if we need to run 100 tasks in a row.
    task_runner_->SetRunTaskLimit(100);
    Reset();
  }

  void Reset() {
    actions_.clear();
    states_.clear();
    draw_will_happen_ = true;
    swap_will_happen_if_draw_happens_ = true;
    num_draws_ = 0;
    log_anticipated_draw_time_change_ = false;
    begin_frame_is_sent_to_children_ = false;
  }

  TestScheduler* CreateScheduler(const SchedulerSettings& settings) {
    scoped_ptr<FakeExternalBeginFrameSource> fake_external_begin_frame_source;
    if (settings.use_external_begin_frame_source &&
        settings.throttle_frame_production) {
      fake_external_begin_frame_source.reset(
          new FakeExternalBeginFrameSource(this));
      fake_external_begin_frame_source_ =
          fake_external_begin_frame_source.get();
    }
    scheduler_ = TestScheduler::Create(now_src_,
                                       this,
                                       settings,
                                       0,
                                       task_runner_,
                                       &power_monitor_,
                                       fake_external_begin_frame_source.Pass());
    DCHECK(scheduler_);
    return scheduler_.get();
  }

  // Most tests don't care about DidAnticipatedDrawTimeChange, so only record it
  // for tests that do.
  void set_log_anticipated_draw_time_change(bool log) {
    log_anticipated_draw_time_change_ = log;
  }
  bool needs_begin_frames() {
    return scheduler_->frame_source().NeedsBeginFrames();
  }
  int num_draws() const { return num_draws_; }
  int num_actions_() const { return static_cast<int>(actions_.size()); }
  const char* Action(int i) const { return actions_[i]; }
  std::string StateForAction(int i) const { return states_[i]->ToString(); }
  base::TimeTicks posted_begin_impl_frame_deadline() const {
    return posted_begin_impl_frame_deadline_;
  }

  bool ExternalBeginFrame() {
    return scheduler_->settings().use_external_begin_frame_source &&
           scheduler_->settings().throttle_frame_production;
  }

  FakeExternalBeginFrameSource* fake_external_begin_frame_source() const {
    return fake_external_begin_frame_source_;
  }

  base::PowerMonitor* PowerMonitor() { return &power_monitor_; }

  FakePowerMonitorSource* PowerMonitorSource() {
    return fake_power_monitor_source_;
  }

  // As this function contains EXPECT macros, to allow debugging it should be
  // called inside EXPECT_SCOPED like so;
  //   EXPECT_SCOPED(client.InitializeOutputSurfaceAndFirstCommit(scheduler));
  void InitializeOutputSurfaceAndFirstCommit(TestScheduler* scheduler) {
    TRACE_EVENT0("cc",
                 "SchedulerUnitTest::InitializeOutputSurfaceAndFirstCommit");
    DCHECK(scheduler);

    // Check the client doesn't have any actions queued when calling this
    // function.
    EXPECT_NO_ACTION((*this));
    EXPECT_FALSE(needs_begin_frames());

    // Start the initial output surface creation.
    EXPECT_FALSE(scheduler->CanStart());
    scheduler->SetCanStart();
    scheduler->SetVisible(true);
    scheduler->SetCanDraw(true);
    EXPECT_SINGLE_ACTION("ScheduledActionBeginOutputSurfaceCreation", (*this));
    Reset();

    // We don't see anything happening until the first impl frame.
    scheduler->DidCreateAndInitializeOutputSurface();
    scheduler->SetNeedsCommit();
    EXPECT_TRUE(needs_begin_frames());
    EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
    Reset();

    {
      SCOPED_TRACE("Do first frame to commit after initialize.");
      AdvanceFrame();

      scheduler->NotifyBeginMainFrameStarted();
      scheduler->NotifyReadyToCommitThenActivateIfNeeded();

      // Run the posted deadline task.
      EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
      task_runner().RunTasksWhile(ImplFrameDeadlinePending(true));
      EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());

      EXPECT_FALSE(scheduler->CommitPending());
    }

    Reset();

    {
      SCOPED_TRACE(
          "Run second frame so Scheduler calls SetNeedsBeginFrame(false).");
      AdvanceFrame();

      // Run the posted deadline task.
      EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
      task_runner().RunTasksWhile(ImplFrameDeadlinePending(true));
      EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
    }

    EXPECT_FALSE(needs_begin_frames());
    Reset();
  }

  // As this function contains EXPECT macros, to allow debugging it should be
  // called inside EXPECT_SCOPED like so;
  //   EXPECT_SCOPED(client.AdvanceFrame());
  void AdvanceFrame() {
    TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("cc.debug.scheduler.frames"),
                 "FakeSchedulerClient::AdvanceFrame");
    // Consume any previous deadline first, if no deadline is currently
    // pending, ImplFrameDeadlinePending will return false straight away and we
    // will run no tasks.
    task_runner().RunTasksWhile(ImplFrameDeadlinePending(true));
    EXPECT_FALSE(scheduler_->BeginImplFrameDeadlinePending());

    // Send the next BeginFrame message if using an external source, otherwise
    // it will be already in the task queue.
    if (ExternalBeginFrame()) {
      SendNextBeginFrame();
      EXPECT_TRUE(scheduler_->BeginImplFrameDeadlinePending());
    }

    // Then run tasks until new deadline is scheduled.
    EXPECT_TRUE(task_runner().RunTasksWhile(ImplFrameDeadlinePending(false)));
    EXPECT_TRUE(scheduler_->BeginImplFrameDeadlinePending());
  }

  void SendNextBeginFrame() {
    DCHECK(ExternalBeginFrame());
    // Creep the time forward so that any BeginFrameArgs is not equal to the
    // last one otherwise we violate the BeginFrameSource contract.
    now_src_->AdvanceNow(BeginFrameArgs::DefaultInterval());
    fake_external_begin_frame_source_->TestOnBeginFrame(
        CreateBeginFrameArgsForTesting(BEGINFRAME_FROM_HERE, now_src_));
  }

  OrderedSimpleTaskRunner& task_runner() { return *task_runner_; }
  TestNowSource* now_src() { return now_src_.get(); }

  int ActionIndex(const char* action) const {
    for (size_t i = 0; i < actions_.size(); i++)
      if (!strcmp(actions_[i], action))
        return i;
    return -1;
  }

  bool HasAction(const char* action) const {
    return ActionIndex(action) >= 0;
  }

  void SetDrawWillHappen(bool draw_will_happen) {
    draw_will_happen_ = draw_will_happen;
  }
  void SetSwapWillHappenIfDrawHappens(bool swap_will_happen_if_draw_happens) {
    swap_will_happen_if_draw_happens_ = swap_will_happen_if_draw_happens;
  }
  void SetAutomaticSwapAck(bool automatic_swap_ack) {
    automatic_swap_ack_ = automatic_swap_ack;
  }
  // SchedulerClient implementation.
  void WillBeginImplFrame(const BeginFrameArgs& args) override {
    PushAction("WillBeginImplFrame");
  }
  void ScheduledActionSendBeginMainFrame() override {
    PushAction("ScheduledActionSendBeginMainFrame");
  }
  void ScheduledActionAnimate() override {
    PushAction("ScheduledActionAnimate");
  }
  DrawResult ScheduledActionDrawAndSwapIfPossible() override {
    PushAction("ScheduledActionDrawAndSwapIfPossible");
    num_draws_++;
    DrawResult result =
        draw_will_happen_ ? DRAW_SUCCESS : DRAW_ABORTED_CHECKERBOARD_ANIMATIONS;
    bool swap_will_happen =
        draw_will_happen_ && swap_will_happen_if_draw_happens_;
    if (swap_will_happen) {
      scheduler_->DidSwapBuffers();

      if (automatic_swap_ack_)
        scheduler_->DidSwapBuffersComplete();
    }
    return result;
  }
  DrawResult ScheduledActionDrawAndSwapForced() override {
    PushAction("ScheduledActionDrawAndSwapForced");
    return DRAW_SUCCESS;
  }
  void ScheduledActionCommit() override { PushAction("ScheduledActionCommit"); }
  void ScheduledActionActivateSyncTree() override {
    PushAction("ScheduledActionActivateSyncTree");
  }
  void ScheduledActionBeginOutputSurfaceCreation() override {
    PushAction("ScheduledActionBeginOutputSurfaceCreation");
  }
  void ScheduledActionPrepareTiles() override {
    PushAction("ScheduledActionPrepareTiles");
  }
  void DidAnticipatedDrawTimeChange(base::TimeTicks) override {
    if (log_anticipated_draw_time_change_)
      PushAction("DidAnticipatedDrawTimeChange");
  }
  base::TimeDelta DrawDurationEstimate() override { return base::TimeDelta(); }
  base::TimeDelta BeginMainFrameToCommitDurationEstimate() override {
    return base::TimeDelta();
  }
  base::TimeDelta CommitToActivateDurationEstimate() override {
    return base::TimeDelta();
  }

  void DidBeginImplFrameDeadline() override {}

  void SendBeginFramesToChildren(const BeginFrameArgs& args) override {
    begin_frame_is_sent_to_children_ = true;
  }

  base::Callback<bool(void)> ImplFrameDeadlinePending(bool state) {
    return base::Bind(&FakeSchedulerClient::ImplFrameDeadlinePendingCallback,
                      base::Unretained(this),
                      state);
  }

  bool begin_frame_is_sent_to_children() const {
    return begin_frame_is_sent_to_children_;
  }

 protected:
  bool ImplFrameDeadlinePendingCallback(bool state) {
    return scheduler_->BeginImplFrameDeadlinePending() == state;
  }

  void PushAction(const char* description) {
    actions_.push_back(description);
    states_.push_back(scheduler_->AsValue());
  }

  bool draw_will_happen_;
  bool swap_will_happen_if_draw_happens_;
  bool automatic_swap_ack_;
  int num_draws_;
  bool log_anticipated_draw_time_change_;
  bool begin_frame_is_sent_to_children_;
  base::TimeTicks posted_begin_impl_frame_deadline_;
  std::vector<const char*> actions_;
  std::vector<scoped_refptr<base::debug::ConvertableToTraceFormat>> states_;
  scoped_refptr<TestNowSource> now_src_;
  scoped_refptr<OrderedSimpleTaskRunner> task_runner_;
  FakeExternalBeginFrameSource* fake_external_begin_frame_source_;
  FakePowerMonitorSource* fake_power_monitor_source_;
  base::PowerMonitor power_monitor_;
  scoped_ptr<TestScheduler> scheduler_;
};

TEST(SchedulerTest, InitializeOutputSurfaceDoesNotBeginImplFrame) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;
  TestScheduler* scheduler = client.CreateScheduler(scheduler_settings);
  scheduler->SetCanStart();
  scheduler->SetVisible(true);
  scheduler->SetCanDraw(true);

  EXPECT_SINGLE_ACTION("ScheduledActionBeginOutputSurfaceCreation", client);
  client.Reset();
  scheduler->DidCreateAndInitializeOutputSurface();
  EXPECT_NO_ACTION(client);
}

TEST(SchedulerTest, SendBeginFramesToChildren) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;
  scheduler_settings.forward_begin_frames_to_children = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  EXPECT_FALSE(client.begin_frame_is_sent_to_children());
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);
  EXPECT_TRUE(client.needs_begin_frames());

  scheduler->SetChildrenNeedBeginFrames(true);

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_TRUE(client.begin_frame_is_sent_to_children());
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(client.needs_begin_frames());
}

TEST(SchedulerTest, SendBeginFramesToChildrenWithoutCommit) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;
  scheduler_settings.forward_begin_frames_to_children = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  EXPECT_FALSE(client.needs_begin_frames());
  scheduler->SetChildrenNeedBeginFrames(true);
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);
  EXPECT_TRUE(client.needs_begin_frames());

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_TRUE(client.begin_frame_is_sent_to_children());
}

TEST(SchedulerTest, RequestCommit) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame on the next BeginImplFrame.
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);
  client.Reset();

  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // If we don't swap on the deadline, we wait for the next BeginFrame.
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_NO_ACTION(client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // NotifyReadyToCommit should trigger the commit.
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // BeginImplFrame should prepare the draw.
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // BeginImplFrame deadline should draw.
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTION("ScheduledActionDrawAndSwapIfPossible", client, 0, 1);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // The following BeginImplFrame deadline should SetNeedsBeginFrame(false)
  // to avoid excessive toggles.
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_SINGLE_ACTION("WillBeginImplFrame", client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(false)", client);
  client.Reset();
}

TEST(SchedulerTest, RequestCommitAfterBeginMainFrameSent) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame.
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // Now SetNeedsCommit again. Calling here means we need a second commit.
  scheduler->SetNeedsCommit();
  EXPECT_EQ(client.num_actions_(), 0);
  client.Reset();

  // Finish the first commit.
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTION("ScheduledActionAnimate", client, 0, 2);
  EXPECT_ACTION("ScheduledActionDrawAndSwapIfPossible", client, 1, 2);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());

  // Because we just swapped, the Scheduler should also request the next
  // BeginImplFrame from the OutputSurface.
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();
  // Since another commit is needed, the next BeginImplFrame should initiate
  // the second commit.
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // Finishing the commit before the deadline should post a new deadline task
  // to trigger the deadline early.
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTION("ScheduledActionAnimate", client, 0, 2);
  EXPECT_ACTION("ScheduledActionDrawAndSwapIfPossible", client, 1, 2);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // On the next BeginImplFrame, verify we go back to a quiescent state and
  // no longer request BeginImplFrames.
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_FALSE(client.needs_begin_frames());
  client.Reset();
}

class SchedulerClientThatsetNeedsDrawInsideDraw : public FakeSchedulerClient {
 public:
  SchedulerClientThatsetNeedsDrawInsideDraw()
      : FakeSchedulerClient(), request_redraws_(false) {}

  void ScheduledActionSendBeginMainFrame() override {}

  void SetRequestRedrawsInsideDraw(bool enable) { request_redraws_ = enable; }

  DrawResult ScheduledActionDrawAndSwapIfPossible() override {
    // Only SetNeedsRedraw the first time this is called
    if (request_redraws_) {
      scheduler_->SetNeedsRedraw();
    }
    return FakeSchedulerClient::ScheduledActionDrawAndSwapIfPossible();
  }

  DrawResult ScheduledActionDrawAndSwapForced() override {
    NOTREACHED();
    return DRAW_SUCCESS;
  }

  void ScheduledActionCommit() override {}
  void DidAnticipatedDrawTimeChange(base::TimeTicks) override {}

 private:
  bool request_redraws_;
};

// Tests for two different situations:
// 1. the scheduler dropping SetNeedsRedraw requests that happen inside
//    a ScheduledActionDrawAndSwap
// 2. the scheduler drawing twice inside a single tick
TEST(SchedulerTest, RequestRedrawInsideDraw) {
  SchedulerClientThatsetNeedsDrawInsideDraw client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);
  client.SetRequestRedrawsInsideDraw(true);

  scheduler->SetNeedsRedraw();
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());
  EXPECT_EQ(0, client.num_draws());

  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());

  client.SetRequestRedrawsInsideDraw(false);

  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client.num_draws());
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());

  // We stop requesting BeginImplFrames after a BeginImplFrame where we don't
  // swap.
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client.num_draws());
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(client.needs_begin_frames());
}

// Test that requesting redraw inside a failed draw doesn't lose the request.
TEST(SchedulerTest, RequestRedrawInsideFailedDraw) {
  SchedulerClientThatsetNeedsDrawInsideDraw client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);
  client.SetRequestRedrawsInsideDraw(true);
  client.SetDrawWillHappen(false);

  scheduler->SetNeedsRedraw();
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());
  EXPECT_EQ(0, client.num_draws());

  // Fail the draw.
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());

  // We have a commit pending and the draw failed, and we didn't lose the redraw
  // request.
  EXPECT_TRUE(scheduler->CommitPending());
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());

  client.SetRequestRedrawsInsideDraw(false);

  // Fail the draw again.
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client.num_draws());
  EXPECT_TRUE(scheduler->CommitPending());
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());

  // Draw successfully.
  client.SetDrawWillHappen(true);
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(3, client.num_draws());
  EXPECT_TRUE(scheduler->CommitPending());
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());
}

class SchedulerClientThatSetNeedsCommitInsideDraw : public FakeSchedulerClient {
 public:
  SchedulerClientThatSetNeedsCommitInsideDraw()
      : set_needs_commit_on_next_draw_(false) {}

  void ScheduledActionSendBeginMainFrame() override {}
  DrawResult ScheduledActionDrawAndSwapIfPossible() override {
    // Only SetNeedsCommit the first time this is called
    if (set_needs_commit_on_next_draw_) {
      scheduler_->SetNeedsCommit();
      set_needs_commit_on_next_draw_ = false;
    }
    return FakeSchedulerClient::ScheduledActionDrawAndSwapIfPossible();
  }

  DrawResult ScheduledActionDrawAndSwapForced() override {
    NOTREACHED();
    return DRAW_SUCCESS;
  }

  void ScheduledActionCommit() override {}
  void DidAnticipatedDrawTimeChange(base::TimeTicks) override {}

  void SetNeedsCommitOnNextDraw() { set_needs_commit_on_next_draw_ = true; }

 private:
  bool set_needs_commit_on_next_draw_;
};

// Tests for the scheduler infinite-looping on SetNeedsCommit requests that
// happen inside a ScheduledActionDrawAndSwap
TEST(SchedulerTest, RequestCommitInsideDraw) {
  SchedulerClientThatSetNeedsCommitInsideDraw client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  EXPECT_FALSE(client.needs_begin_frames());
  scheduler->SetNeedsRedraw();
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_EQ(0, client.num_draws());
  EXPECT_TRUE(client.needs_begin_frames());

  client.SetNeedsCommitOnNextDraw();
  EXPECT_SCOPED(client.AdvanceFrame());
  client.SetNeedsCommitOnNextDraw();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());
  EXPECT_TRUE(scheduler->CommitPending());
  EXPECT_TRUE(client.needs_begin_frames());
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();

  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client.num_draws());

  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->CommitPending());
  EXPECT_TRUE(client.needs_begin_frames());

  // We stop requesting BeginImplFrames after a BeginImplFrame where we don't
  // swap.
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client.num_draws());
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->CommitPending());
  EXPECT_FALSE(client.needs_begin_frames());
}

// Tests that when a draw fails then the pending commit should not be dropped.
TEST(SchedulerTest, RequestCommitInsideFailedDraw) {
  SchedulerClientThatsetNeedsDrawInsideDraw client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  client.SetDrawWillHappen(false);

  scheduler->SetNeedsRedraw();
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());
  EXPECT_EQ(0, client.num_draws());

  // Fail the draw.
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());

  // We have a commit pending and the draw failed, and we didn't lose the commit
  // request.
  EXPECT_TRUE(scheduler->CommitPending());
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());

  // Fail the draw again.
  EXPECT_SCOPED(client.AdvanceFrame());

  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client.num_draws());
  EXPECT_TRUE(scheduler->CommitPending());
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());

  // Draw successfully.
  client.SetDrawWillHappen(true);
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(3, client.num_draws());
  EXPECT_TRUE(scheduler->CommitPending());
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());
}

TEST(SchedulerTest, NoSwapWhenDrawFails) {
  SchedulerClientThatSetNeedsCommitInsideDraw client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  scheduler->SetNeedsRedraw();
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());
  EXPECT_EQ(0, client.num_draws());

  // Draw successfully, this starts a new frame.
  client.SetNeedsCommitOnNextDraw();
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());

  scheduler->SetNeedsRedraw();
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(client.needs_begin_frames());

  // Fail to draw, this should not start a frame.
  client.SetDrawWillHappen(false);
  client.SetNeedsCommitOnNextDraw();
  EXPECT_SCOPED(client.AdvanceFrame());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(2, client.num_draws());
}

class SchedulerClientNeedsPrepareTilesInDraw : public FakeSchedulerClient {
 public:
  DrawResult ScheduledActionDrawAndSwapIfPossible() override {
    scheduler_->SetNeedsPrepareTiles();
    return FakeSchedulerClient::ScheduledActionDrawAndSwapIfPossible();
  }
};

// Test prepare tiles is independant of draws.
TEST(SchedulerTest, PrepareTiles) {
  SchedulerClientNeedsPrepareTilesInDraw client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // Request both draw and prepare tiles. PrepareTiles shouldn't
  // be trigged until BeginImplFrame.
  client.Reset();
  scheduler->SetNeedsPrepareTiles();
  scheduler->SetNeedsRedraw();
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_TRUE(scheduler->PrepareTilesPending());
  EXPECT_TRUE(client.needs_begin_frames());
  EXPECT_EQ(0, client.num_draws());
  EXPECT_FALSE(client.HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(client.HasAction("ScheduledActionDrawAndSwapIfPossible"));

  // We have no immediate actions to perform, so the BeginImplFrame should post
  // the deadline task.
  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  // On the deadline, he actions should have occured in the right order.
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());
  EXPECT_TRUE(client.HasAction("ScheduledActionDrawAndSwapIfPossible"));
  EXPECT_TRUE(client.HasAction("ScheduledActionPrepareTiles"));
  EXPECT_LT(client.ActionIndex("ScheduledActionDrawAndSwapIfPossible"),
            client.ActionIndex("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->PrepareTilesPending());
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());

  // Request a draw. We don't need a PrepareTiles yet.
  client.Reset();
  scheduler->SetNeedsRedraw();
  EXPECT_TRUE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->PrepareTilesPending());
  EXPECT_TRUE(client.needs_begin_frames());
  EXPECT_EQ(0, client.num_draws());

  // We have no immediate actions to perform, so the BeginImplFrame should post
  // the deadline task.
  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  // Draw. The draw will trigger SetNeedsPrepareTiles, and
  // then the PrepareTiles action will be triggered after the Draw.
  // Afterwards, neither a draw nor PrepareTiles are pending.
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());
  EXPECT_TRUE(client.HasAction("ScheduledActionDrawAndSwapIfPossible"));
  EXPECT_TRUE(client.HasAction("ScheduledActionPrepareTiles"));
  EXPECT_LT(client.ActionIndex("ScheduledActionDrawAndSwapIfPossible"),
            client.ActionIndex("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->PrepareTilesPending());
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());

  // We need a BeginImplFrame where we don't swap to go idle.
  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_SINGLE_ACTION("WillBeginImplFrame", client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(false)", client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_EQ(0, client.num_draws());

  // Now trigger a PrepareTiles outside of a draw. We will then need
  // a begin-frame for the PrepareTiles, but we don't need a draw.
  client.Reset();
  EXPECT_FALSE(client.needs_begin_frames());
  scheduler->SetNeedsPrepareTiles();
  EXPECT_TRUE(client.needs_begin_frames());
  EXPECT_TRUE(scheduler->PrepareTilesPending());
  EXPECT_FALSE(scheduler->RedrawPending());

  // BeginImplFrame. There will be no draw, only PrepareTiles.
  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_SINGLE_ACTION("WillBeginImplFrame", client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(0, client.num_draws());
  EXPECT_FALSE(client.HasAction("ScheduledActionDrawAndSwapIfPossible"));
  EXPECT_TRUE(client.HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
}

// Test that PrepareTiles only happens once per frame.  If an external caller
// initiates it, then the state machine should not PrepareTiles on that frame.
TEST(SchedulerTest, PrepareTilesOncePerFrame) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // If DidPrepareTiles during a frame, then PrepareTiles should not occur
  // again.
  scheduler->SetNeedsPrepareTiles();
  scheduler->SetNeedsRedraw();
  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  EXPECT_TRUE(scheduler->PrepareTilesPending());
  scheduler->DidPrepareTiles();  // An explicit PrepareTiles.
  EXPECT_FALSE(scheduler->PrepareTilesPending());

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());
  EXPECT_TRUE(client.HasAction("ScheduledActionDrawAndSwapIfPossible"));
  EXPECT_FALSE(client.HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->PrepareTilesPending());
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());

  // Next frame without DidPrepareTiles should PrepareTiles with draw.
  scheduler->SetNeedsPrepareTiles();
  scheduler->SetNeedsRedraw();
  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());
  EXPECT_TRUE(client.HasAction("ScheduledActionDrawAndSwapIfPossible"));
  EXPECT_TRUE(client.HasAction("ScheduledActionPrepareTiles"));
  EXPECT_LT(client.ActionIndex("ScheduledActionDrawAndSwapIfPossible"),
            client.ActionIndex("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->PrepareTilesPending());
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  scheduler->DidPrepareTiles();  // Corresponds to ScheduledActionPrepareTiles

  // If we get another DidPrepareTiles within the same frame, we should
  // not PrepareTiles on the next frame.
  scheduler->DidPrepareTiles();  // An explicit PrepareTiles.
  scheduler->SetNeedsPrepareTiles();
  scheduler->SetNeedsRedraw();
  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  EXPECT_TRUE(scheduler->PrepareTilesPending());

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());
  EXPECT_TRUE(client.HasAction("ScheduledActionDrawAndSwapIfPossible"));
  EXPECT_FALSE(client.HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());

  // If we get another DidPrepareTiles, we should not PrepareTiles on the next
  // frame. This verifies we don't alternate calling PrepareTiles once and
  // twice.
  EXPECT_TRUE(scheduler->PrepareTilesPending());
  scheduler->DidPrepareTiles();  // An explicit PrepareTiles.
  EXPECT_FALSE(scheduler->PrepareTilesPending());
  scheduler->SetNeedsPrepareTiles();
  scheduler->SetNeedsRedraw();
  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  EXPECT_TRUE(scheduler->PrepareTilesPending());

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());
  EXPECT_TRUE(client.HasAction("ScheduledActionDrawAndSwapIfPossible"));
  EXPECT_FALSE(client.HasAction("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());

  // Next frame without DidPrepareTiles should PrepareTiles with draw.
  scheduler->SetNeedsPrepareTiles();
  scheduler->SetNeedsRedraw();
  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(1, client.num_draws());
  EXPECT_TRUE(client.HasAction("ScheduledActionDrawAndSwapIfPossible"));
  EXPECT_TRUE(client.HasAction("ScheduledActionPrepareTiles"));
  EXPECT_LT(client.ActionIndex("ScheduledActionDrawAndSwapIfPossible"),
            client.ActionIndex("ScheduledActionPrepareTiles"));
  EXPECT_FALSE(scheduler->RedrawPending());
  EXPECT_FALSE(scheduler->PrepareTilesPending());
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  scheduler->DidPrepareTiles();  // Corresponds to ScheduledActionPrepareTiles
}

TEST(SchedulerTest, TriggerBeginFrameDeadlineEarly) {
  SchedulerClientNeedsPrepareTilesInDraw client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  scheduler->SetNeedsRedraw();
  EXPECT_SCOPED(client.AdvanceFrame());

  // The deadline should be zero since there is no work other than drawing
  // pending.
  EXPECT_EQ(base::TimeTicks(), client.posted_begin_impl_frame_deadline());
}

class SchedulerClientWithFixedEstimates : public FakeSchedulerClient {
 public:
  SchedulerClientWithFixedEstimates(
      base::TimeDelta draw_duration,
      base::TimeDelta begin_main_frame_to_commit_duration,
      base::TimeDelta commit_to_activate_duration)
      : draw_duration_(draw_duration),
        begin_main_frame_to_commit_duration_(
            begin_main_frame_to_commit_duration),
        commit_to_activate_duration_(commit_to_activate_duration) {}

  base::TimeDelta DrawDurationEstimate() override { return draw_duration_; }
  base::TimeDelta BeginMainFrameToCommitDurationEstimate() override {
    return begin_main_frame_to_commit_duration_;
  }
  base::TimeDelta CommitToActivateDurationEstimate() override {
    return commit_to_activate_duration_;
  }

 private:
    base::TimeDelta draw_duration_;
    base::TimeDelta begin_main_frame_to_commit_duration_;
    base::TimeDelta commit_to_activate_duration_;
};

void MainFrameInHighLatencyMode(int64 begin_main_frame_to_commit_estimate_in_ms,
                                int64 commit_to_activate_estimate_in_ms,
                                bool impl_latency_takes_priority,
                                bool should_send_begin_main_frame) {
  // Set up client with specified estimates (draw duration is set to 1).
  SchedulerClientWithFixedEstimates client(
      base::TimeDelta::FromMilliseconds(1),
      base::TimeDelta::FromMilliseconds(
          begin_main_frame_to_commit_estimate_in_ms),
      base::TimeDelta::FromMilliseconds(commit_to_activate_estimate_in_ms));
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  scheduler->SetImplLatencyTakesPriority(impl_latency_takes_priority);

  // Impl thread hits deadline before commit finishes.
  scheduler->SetNeedsCommit();
  EXPECT_FALSE(scheduler->MainThreadIsInHighLatencyMode());
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_FALSE(scheduler->MainThreadIsInHighLatencyMode());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_TRUE(scheduler->MainThreadIsInHighLatencyMode());
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_TRUE(scheduler->MainThreadIsInHighLatencyMode());
  EXPECT_TRUE(client.HasAction("ScheduledActionSendBeginMainFrame"));

  client.Reset();
  scheduler->SetNeedsCommit();
  EXPECT_TRUE(scheduler->MainThreadIsInHighLatencyMode());
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_TRUE(scheduler->MainThreadIsInHighLatencyMode());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_EQ(scheduler->MainThreadIsInHighLatencyMode(),
            should_send_begin_main_frame);
  EXPECT_EQ(client.HasAction("ScheduledActionSendBeginMainFrame"),
            should_send_begin_main_frame);
}

TEST(SchedulerTest,
    SkipMainFrameIfHighLatencyAndCanCommitAndActivateBeforeDeadline) {
  // Set up client so that estimates indicate that we can commit and activate
  // before the deadline (~8ms by default).
  MainFrameInHighLatencyMode(1, 1, false, false);
}

TEST(SchedulerTest, NotSkipMainFrameIfHighLatencyAndCanCommitTooLong) {
  // Set up client so that estimates indicate that the commit cannot finish
  // before the deadline (~8ms by default).
  MainFrameInHighLatencyMode(10, 1, false, true);
}

TEST(SchedulerTest, NotSkipMainFrameIfHighLatencyAndCanActivateTooLong) {
  // Set up client so that estimates indicate that the activate cannot finish
  // before the deadline (~8ms by default).
  MainFrameInHighLatencyMode(1, 10, false, true);
}

TEST(SchedulerTest, NotSkipMainFrameInPreferImplLatencyMode) {
  // Set up client so that estimates indicate that we can commit and activate
  // before the deadline (~8ms by default), but also enable impl latency takes
  // priority mode.
  MainFrameInHighLatencyMode(1, 1, true, true);
}

TEST(SchedulerTest, PollForCommitCompletion) {
  // Since we are simulating a long commit, set up a client with draw duration
  // estimates that prevent skipping main frames to get to low latency mode.
  SchedulerClientWithFixedEstimates client(
      base::TimeDelta::FromMilliseconds(1),
      base::TimeDelta::FromMilliseconds(32),
      base::TimeDelta::FromMilliseconds(32));
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  client.set_log_anticipated_draw_time_change(true);

  BeginFrameArgs frame_args =
      CreateBeginFrameArgsForTesting(BEGINFRAME_FROM_HERE, client.now_src());
  frame_args.interval = base::TimeDelta::FromMilliseconds(1000);

  // At this point, we've drawn a frame. Start another commit, but hold off on
  // the NotifyReadyToCommit for now.
  EXPECT_FALSE(scheduler->CommitPending());
  scheduler->SetNeedsCommit();
  client.fake_external_begin_frame_source()->TestOnBeginFrame(frame_args);
  EXPECT_TRUE(scheduler->CommitPending());

  // Draw and swap the frame, but don't ack the swap to simulate the Browser
  // blocking on the renderer.
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  scheduler->DidSwapBuffers();

  // Spin the event loop a few times and make sure we get more
  // DidAnticipateDrawTimeChange calls every time.
  int actions_so_far = client.num_actions_();

  // Does three iterations to make sure that the timer is properly repeating.
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ((frame_args.interval * 2).InMicroseconds(),
              client.task_runner().DelayToNextTaskTime().InMicroseconds())
        << scheduler->AsValue()->ToString();
    client.task_runner().RunPendingTasks();
    EXPECT_GT(client.num_actions_(), actions_so_far);
    EXPECT_STREQ(client.Action(client.num_actions_() - 1),
                 "DidAnticipatedDrawTimeChange");
    actions_so_far = client.num_actions_();
  }

  // Do the same thing after BeginMainFrame starts but still before activation.
  scheduler->NotifyBeginMainFrameStarted();
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ((frame_args.interval * 2).InMicroseconds(),
              client.task_runner().DelayToNextTaskTime().InMicroseconds())
        << scheduler->AsValue()->ToString();
    client.task_runner().RunPendingTasks();
    EXPECT_GT(client.num_actions_(), actions_so_far);
    EXPECT_STREQ(client.Action(client.num_actions_() - 1),
                 "DidAnticipatedDrawTimeChange");
    actions_so_far = client.num_actions_();
  }
}

TEST(SchedulerTest, BeginRetroFrame) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame on the next BeginImplFrame.
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);
  client.Reset();

  // Create a BeginFrame with a long deadline to avoid race conditions.
  // This is the first BeginFrame, which will be handled immediately.
  BeginFrameArgs args =
      CreateBeginFrameArgsForTesting(BEGINFRAME_FROM_HERE, client.now_src());
  args.deadline += base::TimeDelta::FromHours(1);
  client.fake_external_begin_frame_source()->TestOnBeginFrame(args);
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // Queue BeginFrames while we are still handling the previous BeginFrame.
  args.frame_time += base::TimeDelta::FromSeconds(1);
  client.fake_external_begin_frame_source()->TestOnBeginFrame(args);
  args.frame_time += base::TimeDelta::FromSeconds(1);
  client.fake_external_begin_frame_source()->TestOnBeginFrame(args);

  // If we don't swap on the deadline, we wait for the next BeginImplFrame.
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_NO_ACTION(client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // NotifyReadyToCommit should trigger the commit.
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // BeginImplFrame should prepare the draw.
  client.task_runner().RunPendingTasks();  // Run posted BeginRetroFrame.
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // BeginImplFrame deadline should draw.
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTION("ScheduledActionDrawAndSwapIfPossible", client, 0, 1);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // The following BeginImplFrame deadline should SetNeedsBeginFrame(false)
  // to avoid excessive toggles.
  client.task_runner().RunPendingTasks();  // Run posted BeginRetroFrame.
  EXPECT_SINGLE_ACTION("WillBeginImplFrame", client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(false)", client);
  client.Reset();
}

TEST(SchedulerTest, BeginRetroFrame_SwapThrottled) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);
  scheduler->SetEstimatedParentDrawTime(base::TimeDelta::FromMicroseconds(1));

  // To test swap ack throttling, this test disables automatic swap acks.
  scheduler->SetMaxSwapsPending(1);
  client.SetAutomaticSwapAck(false);

  // SetNeedsCommit should begin the frame on the next BeginImplFrame.
  client.Reset();
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);
  client.Reset();

  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // Queue BeginFrame while we are still handling the previous BeginFrame.
  client.SendNextBeginFrame();
  EXPECT_NO_ACTION(client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // NotifyReadyToCommit should trigger the pending commit and draw.
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // Swapping will put us into a swap throttled state.
  // Run posted deadline.
  client.task_runner().RunTasksWhile(client.ImplFrameDeadlinePending(true));
  EXPECT_ACTION("ScheduledActionAnimate", client, 0, 2);
  EXPECT_ACTION("ScheduledActionDrawAndSwapIfPossible", client, 1, 2);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // While swap throttled, BeginRetroFrames should trigger BeginImplFrames
  // but not a BeginMainFrame or draw.
  scheduler->SetNeedsCommit();
  scheduler->SetNeedsRedraw();
  // Run posted BeginRetroFrame.
  client.task_runner().RunTasksWhile(client.ImplFrameDeadlinePending(false));
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // Let time pass sufficiently beyond the regular deadline but not beyond the
  // late deadline.
  client.now_src()->AdvanceNow(BeginFrameArgs::DefaultInterval() -
                               base::TimeDelta::FromMicroseconds(1));
  client.task_runner().RunUntilTime(client.now_src()->Now());
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  // Take us out of a swap throttled state.
  scheduler->DidSwapBuffersComplete();
  EXPECT_SINGLE_ACTION("ScheduledActionSendBeginMainFrame", client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();

  // Verify that the deadline was rescheduled.
  client.task_runner().RunUntilTime(client.now_src()->Now());
  EXPECT_SINGLE_ACTION("ScheduledActionDrawAndSwapIfPossible", client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());
  client.Reset();
}

TEST(SchedulerTest, RetroFrameDoesNotExpireTooEarly) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  scheduler->SetNeedsCommit();
  EXPECT_TRUE(client.needs_begin_frames());
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  scheduler->NotifyBeginMainFrameStarted();

  client.Reset();
  client.SendNextBeginFrame();
  // This BeginFrame is queued up as a retro frame.
  EXPECT_NO_ACTION(client);
  // The previous deadline is still pending.
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  // This commit should schedule the (previous) deadline to trigger immediately.
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);

  client.Reset();
  // The deadline task should trigger causing a draw.
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.task_runner().RunTasksWhile(client.ImplFrameDeadlinePending(true));
  EXPECT_ACTION("ScheduledActionAnimate", client, 0, 2);
  EXPECT_ACTION("ScheduledActionDrawAndSwapIfPossible", client, 1, 2);

  // Keep animating.
  client.Reset();
  scheduler->SetNeedsAnimate();
  scheduler->SetNeedsRedraw();
  EXPECT_NO_ACTION(client);

  // Let's advance sufficiently past the next frame's deadline.
  client.now_src()->AdvanceNow(
      BeginFrameArgs::DefaultInterval() -
      BeginFrameArgs::DefaultEstimatedParentDrawTime() +
      base::TimeDelta::FromMicroseconds(1));

  // The retro frame hasn't expired yet.
  client.task_runner().RunTasksWhile(client.ImplFrameDeadlinePending(false));
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  // This is an immediate deadline case.
  client.Reset();
  client.task_runner().RunPendingTasks();
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_SINGLE_ACTION("ScheduledActionDrawAndSwapIfPossible", client);
}

TEST(SchedulerTest, RetroFrameDoesNotExpireTooLate) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  scheduler->SetNeedsCommit();
  EXPECT_TRUE(client.needs_begin_frames());
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  scheduler->NotifyBeginMainFrameStarted();

  client.Reset();
  client.SendNextBeginFrame();
  // This BeginFrame is queued up as a retro frame.
  EXPECT_NO_ACTION(client);
  // The previous deadline is still pending.
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  // This commit should schedule the (previous) deadline to trigger immediately.
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);

  client.Reset();
  // The deadline task should trigger causing a draw.
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.task_runner().RunTasksWhile(client.ImplFrameDeadlinePending(true));
  EXPECT_ACTION("ScheduledActionAnimate", client, 0, 2);
  EXPECT_ACTION("ScheduledActionDrawAndSwapIfPossible", client, 1, 2);

  // Keep animating.
  client.Reset();
  scheduler->SetNeedsAnimate();
  scheduler->SetNeedsRedraw();
  EXPECT_NO_ACTION(client);

  // Let's advance sufficiently past the next frame's deadline.
  client.now_src()->AdvanceNow(BeginFrameArgs::DefaultInterval() +
                               base::TimeDelta::FromMicroseconds(1));

  // The retro frame should've expired.
  EXPECT_NO_ACTION(client);
}

void BeginFramesNotFromClient(bool use_external_begin_frame_source,
                              bool throttle_frame_production) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source =
      use_external_begin_frame_source;
  scheduler_settings.throttle_frame_production = throttle_frame_production;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  DCHECK(!client.fake_external_begin_frame_source());

  // SetNeedsCommit should begin the frame on the next BeginImplFrame
  // without calling SetNeedsBeginFrame.
  scheduler->SetNeedsCommit();
  EXPECT_NO_ACTION(client);
  client.Reset();

  // When the client-driven BeginFrame are disabled, the scheduler posts it's
  // own BeginFrame tasks.
  client.task_runner().RunPendingTasks();  // Run posted BeginFrame.
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // If we don't swap on the deadline, we wait for the next BeginFrame.
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_NO_ACTION(client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // NotifyReadyToCommit should trigger the commit.
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  client.Reset();

  // BeginImplFrame should prepare the draw.
  client.task_runner().RunPendingTasks();  // Run posted BeginFrame.
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // BeginImplFrame deadline should draw.
  client.task_runner().RunTasksWhile(client.ImplFrameDeadlinePending(true));
  EXPECT_ACTION("ScheduledActionDrawAndSwapIfPossible", client, 0, 1);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // The following BeginImplFrame deadline should SetNeedsBeginFrame(false)
  // to avoid excessive toggles.
  client.task_runner().RunPendingTasks();  // Run posted BeginFrame.
  EXPECT_SINGLE_ACTION("WillBeginImplFrame", client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // Make sure SetNeedsBeginFrame isn't called on the client
  // when the BeginFrame is no longer needed.
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_NO_ACTION(client);
  client.Reset();
}

TEST(SchedulerTest, SyntheticBeginFrames) {
  bool use_external_begin_frame_source = false;
  bool throttle_frame_production = true;
  BeginFramesNotFromClient(use_external_begin_frame_source,
                           throttle_frame_production);
}

TEST(SchedulerTest, VSyncThrottlingDisabled) {
  bool use_external_begin_frame_source = true;
  bool throttle_frame_production = false;
  BeginFramesNotFromClient(use_external_begin_frame_source,
                           throttle_frame_production);
}

TEST(SchedulerTest, SyntheticBeginFrames_And_VSyncThrottlingDisabled) {
  bool use_external_begin_frame_source = false;
  bool throttle_frame_production = false;
  BeginFramesNotFromClient(use_external_begin_frame_source,
                           throttle_frame_production);
}

void BeginFramesNotFromClient_SwapThrottled(
    bool use_external_begin_frame_source,
    bool throttle_frame_production) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source =
      use_external_begin_frame_source;
  scheduler_settings.throttle_frame_production = throttle_frame_production;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);
  scheduler->SetEstimatedParentDrawTime(base::TimeDelta::FromMicroseconds(1));

  DCHECK(!client.fake_external_begin_frame_source());

  // To test swap ack throttling, this test disables automatic swap acks.
  scheduler->SetMaxSwapsPending(1);
  client.SetAutomaticSwapAck(false);

  // SetNeedsCommit should begin the frame on the next BeginImplFrame.
  client.Reset();
  scheduler->SetNeedsCommit();
  EXPECT_NO_ACTION(client);
  client.Reset();

  // Trigger the first BeginImplFrame and BeginMainFrame
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // NotifyReadyToCommit should trigger the pending commit and draw.
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  client.Reset();

  // Swapping will put us into a swap throttled state.
  // Run posted deadline.
  client.task_runner().RunTasksWhile(client.ImplFrameDeadlinePending(true));
  EXPECT_ACTION("ScheduledActionAnimate", client, 0, 2);
  EXPECT_ACTION("ScheduledActionDrawAndSwapIfPossible", client, 1, 2);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // While swap throttled, BeginFrames should trigger BeginImplFrames,
  // but not a BeginMainFrame or draw.
  scheduler->SetNeedsCommit();
  scheduler->SetNeedsRedraw();
  EXPECT_SCOPED(client.AdvanceFrame());  // Run posted BeginFrame.
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // Let time pass sufficiently beyond the regular deadline but not beyond the
  // late deadline.
  client.now_src()->AdvanceNow(BeginFrameArgs::DefaultInterval() -
                               base::TimeDelta::FromMicroseconds(1));
  client.task_runner().RunUntilTime(client.now_src()->Now());
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  // Take us out of a swap throttled state.
  scheduler->DidSwapBuffersComplete();
  EXPECT_SINGLE_ACTION("ScheduledActionSendBeginMainFrame", client);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();

  // Verify that the deadline was rescheduled.
  // We can't use RunUntilTime(now) here because the next frame is also
  // scheduled if throttle_frame_production = false.
  base::TimeTicks before_deadline = client.now_src()->Now();
  client.task_runner().RunTasksWhile(client.ImplFrameDeadlinePending(true));
  base::TimeTicks after_deadline = client.now_src()->Now();
  EXPECT_EQ(after_deadline, before_deadline);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  client.Reset();
}

TEST(SchedulerTest, SyntheticBeginFrames_SwapThrottled) {
  bool use_external_begin_frame_source = false;
  bool throttle_frame_production = true;
  BeginFramesNotFromClient_SwapThrottled(use_external_begin_frame_source,
                                         throttle_frame_production);
}

TEST(SchedulerTest, VSyncThrottlingDisabled_SwapThrottled) {
  bool use_external_begin_frame_source = true;
  bool throttle_frame_production = false;
  BeginFramesNotFromClient_SwapThrottled(use_external_begin_frame_source,
                                         throttle_frame_production);
}

TEST(SchedulerTest,
     SyntheticBeginFrames_And_VSyncThrottlingDisabled_SwapThrottled) {
  bool use_external_begin_frame_source = false;
  bool throttle_frame_production = false;
  BeginFramesNotFromClient_SwapThrottled(use_external_begin_frame_source,
                                         throttle_frame_production);
}

TEST(SchedulerTest, DidLoseOutputSurfaceAfterOutputSurfaceIsInitialized) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;
  TestScheduler* scheduler = client.CreateScheduler(scheduler_settings);
  scheduler->SetCanStart();
  scheduler->SetVisible(true);
  scheduler->SetCanDraw(true);

  EXPECT_SINGLE_ACTION("ScheduledActionBeginOutputSurfaceCreation", client);
  client.Reset();
  scheduler->DidCreateAndInitializeOutputSurface();
  EXPECT_NO_ACTION(client);

  scheduler->DidLoseOutputSurface();
  EXPECT_SINGLE_ACTION("ScheduledActionBeginOutputSurfaceCreation", client);
}

TEST(SchedulerTest, DidLoseOutputSurfaceAfterBeginFrameStarted) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame.
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  scheduler->DidLoseOutputSurface();
  // Do nothing when impl frame is in deadine pending state.
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(false)", client);

  client.Reset();
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_ACTION("ScheduledActionCommit", client, 0, 1);

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_SINGLE_ACTION("ScheduledActionBeginOutputSurfaceCreation", client);
}

void DidLoseOutputSurfaceAfterBeginFrameStartedWithHighLatency(
    bool impl_side_painting) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.impl_side_painting = impl_side_painting;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame.
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  scheduler->DidLoseOutputSurface();
  // Do nothing when impl frame is in deadine pending state.
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(false)", client);

  client.Reset();
  // Run posted deadline.
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.task_runner().RunTasksWhile(client.ImplFrameDeadlinePending(true));
  // OnBeginImplFrameDeadline didn't schedule any actions because main frame is
  // not yet completed.
  EXPECT_NO_ACTION(client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());

  // BeginImplFrame is not started.
  client.task_runner().RunUntilTime(client.now_src()->Now() +
                                    base::TimeDelta::FromMilliseconds(10));
  EXPECT_NO_ACTION(client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  if (impl_side_painting) {
    EXPECT_ACTION("ScheduledActionCommit", client, 0, 3);
    EXPECT_ACTION("ScheduledActionActivateSyncTree", client, 1, 3);
    EXPECT_ACTION("ScheduledActionBeginOutputSurfaceCreation", client, 2, 3);
  } else {
    EXPECT_ACTION("ScheduledActionCommit", client, 0, 2);
    EXPECT_ACTION("ScheduledActionBeginOutputSurfaceCreation", client, 1, 2);
  }
}

TEST(SchedulerTest, DidLoseOutputSurfaceAfterBeginFrameStartedWithHighLatency) {
  bool impl_side_painting = false;
  DidLoseOutputSurfaceAfterBeginFrameStartedWithHighLatency(impl_side_painting);
}

TEST(SchedulerTest,
     DidLoseOutputSurfaceAfterBeginFrameStartedWithHighLatencyWithImplPaint) {
  bool impl_side_painting = true;
  DidLoseOutputSurfaceAfterBeginFrameStartedWithHighLatency(impl_side_painting);
}

void DidLoseOutputSurfaceAfterReadyToCommit(bool impl_side_painting) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.impl_side_painting = impl_side_painting;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame.
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);

  client.Reset();
  scheduler->DidLoseOutputSurface();
  if (impl_side_painting) {
    // Sync tree should be forced to activate.
    EXPECT_ACTION("SetNeedsBeginFrames(false)", client, 0, 2);
    EXPECT_ACTION("ScheduledActionActivateSyncTree", client, 1, 2);
  } else {
    EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(false)", client);
  }

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_SINGLE_ACTION("ScheduledActionBeginOutputSurfaceCreation", client);
}

TEST(SchedulerTest, DidLoseOutputSurfaceAfterReadyToCommit) {
  DidLoseOutputSurfaceAfterReadyToCommit(false);
}

TEST(SchedulerTest, DidLoseOutputSurfaceAfterReadyToCommitWithImplPainting) {
  DidLoseOutputSurfaceAfterReadyToCommit(true);
}

TEST(SchedulerTest, DidLoseOutputSurfaceAfterSetNeedsPrepareTiles) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  scheduler->SetNeedsPrepareTiles();
  scheduler->SetNeedsRedraw();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  scheduler->DidLoseOutputSurface();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(false)", client);

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_ACTION("ScheduledActionPrepareTiles", client, 0, 2);
  EXPECT_ACTION("ScheduledActionBeginOutputSurfaceCreation", client, 1, 2);
}

TEST(SchedulerTest, DidLoseOutputSurfaceAfterBeginRetroFramePosted) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame on the next BeginImplFrame.
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  // Create a BeginFrame with a long deadline to avoid race conditions.
  // This is the first BeginFrame, which will be handled immediately.
  client.Reset();
  BeginFrameArgs args =
      CreateBeginFrameArgsForTesting(BEGINFRAME_FROM_HERE, client.now_src());
  args.deadline += base::TimeDelta::FromHours(1);
  client.fake_external_begin_frame_source()->TestOnBeginFrame(args);
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());

  // Queue BeginFrames while we are still handling the previous BeginFrame.
  args.frame_time += base::TimeDelta::FromSeconds(1);
  client.fake_external_begin_frame_source()->TestOnBeginFrame(args);
  args.frame_time += base::TimeDelta::FromSeconds(1);
  client.fake_external_begin_frame_source()->TestOnBeginFrame(args);

  // If we don't swap on the deadline, we wait for the next BeginImplFrame.
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_NO_ACTION(client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());

  // NotifyReadyToCommit should trigger the commit.
  client.Reset();
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  EXPECT_TRUE(client.needs_begin_frames());

  client.Reset();
  EXPECT_FALSE(scheduler->IsBeginRetroFrameArgsEmpty());
  scheduler->DidLoseOutputSurface();
  EXPECT_ACTION("SetNeedsBeginFrames(false)", client, 0, 2);
  EXPECT_ACTION("ScheduledActionBeginOutputSurfaceCreation", client, 1, 2);
  EXPECT_TRUE(scheduler->IsBeginRetroFrameArgsEmpty());

  // Posted BeginRetroFrame is aborted.
  client.Reset();
  client.task_runner().RunPendingTasks();
  EXPECT_NO_ACTION(client);
}

TEST(SchedulerTest, DidLoseOutputSurfaceDuringBeginRetroFrameRunning) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame on the next BeginImplFrame.
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  // Create a BeginFrame with a long deadline to avoid race conditions.
  // This is the first BeginFrame, which will be handled immediately.
  client.Reset();
  BeginFrameArgs args =
      CreateBeginFrameArgsForTesting(BEGINFRAME_FROM_HERE, client.now_src());
  args.deadline += base::TimeDelta::FromHours(1);
  client.fake_external_begin_frame_source()->TestOnBeginFrame(args);
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());

  // Queue BeginFrames while we are still handling the previous BeginFrame.
  args.frame_time += base::TimeDelta::FromSeconds(1);
  client.fake_external_begin_frame_source()->TestOnBeginFrame(args);
  args.frame_time += base::TimeDelta::FromSeconds(1);
  client.fake_external_begin_frame_source()->TestOnBeginFrame(args);

  // If we don't swap on the deadline, we wait for the next BeginImplFrame.
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_NO_ACTION(client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());

  // NotifyReadyToCommit should trigger the commit.
  client.Reset();
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  EXPECT_TRUE(client.needs_begin_frames());

  // BeginImplFrame should prepare the draw.
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted BeginRetroFrame.
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionAnimate", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(client.needs_begin_frames());

  client.Reset();
  EXPECT_FALSE(scheduler->IsBeginRetroFrameArgsEmpty());
  scheduler->DidLoseOutputSurface();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(false)", client);
  EXPECT_TRUE(scheduler->IsBeginRetroFrameArgsEmpty());

  // BeginImplFrame deadline should abort drawing.
  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_SINGLE_ACTION("ScheduledActionBeginOutputSurfaceCreation", client);
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_FALSE(client.needs_begin_frames());

  // No more BeginRetroFrame because BeginRetroFrame queue is cleared.
  client.Reset();
  client.task_runner().RunPendingTasks();
  EXPECT_NO_ACTION(client);
}

TEST(SchedulerTest,
     StopBeginFrameAfterDidLoseOutputSurfaceWithSyntheticBeginFrameSource) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame on the next BeginImplFrame.
  EXPECT_FALSE(scheduler->frame_source().NeedsBeginFrames());
  scheduler->SetNeedsCommit();
  EXPECT_TRUE(scheduler->frame_source().NeedsBeginFrames());

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted Tick.
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  EXPECT_TRUE(scheduler->frame_source().NeedsBeginFrames());

  // NotifyReadyToCommit should trigger the commit.
  client.Reset();
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);
  EXPECT_TRUE(scheduler->frame_source().NeedsBeginFrames());

  client.Reset();
  scheduler->DidLoseOutputSurface();
  EXPECT_NO_ACTION(client);
  EXPECT_FALSE(scheduler->frame_source().NeedsBeginFrames());

  client.Reset();
  client.task_runner().RunPendingTasks();  // Run posted deadline.
  EXPECT_SINGLE_ACTION("ScheduledActionBeginOutputSurfaceCreation", client);
  EXPECT_FALSE(scheduler->frame_source().NeedsBeginFrames());
}

TEST(SchedulerTest, ScheduledActionActivateAfterBecomingInvisible) {
  FakeSchedulerClient client;
  SchedulerSettings scheduler_settings;
  scheduler_settings.impl_side_painting = true;
  scheduler_settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(scheduler_settings);

  // SetNeedsCommit should begin the frame.
  scheduler->SetNeedsCommit();
  EXPECT_SINGLE_ACTION("SetNeedsBeginFrames(true)", client);

  client.Reset();
  EXPECT_SCOPED(client.AdvanceFrame());
  EXPECT_ACTION("WillBeginImplFrame", client, 0, 2);
  EXPECT_ACTION("ScheduledActionSendBeginMainFrame", client, 1, 2);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  client.Reset();
  scheduler->NotifyBeginMainFrameStarted();
  scheduler->NotifyReadyToCommit();
  EXPECT_SINGLE_ACTION("ScheduledActionCommit", client);

  client.Reset();
  scheduler->SetVisible(false);
  // Sync tree should be forced to activate.
  EXPECT_ACTION("SetNeedsBeginFrames(false)", client, 0, 2);
  EXPECT_ACTION("ScheduledActionActivateSyncTree", client, 1, 2);
}

TEST(SchedulerTest, SchedulerPowerMonitoring) {
  FakeSchedulerClient client;
  SchedulerSettings settings;
  settings.disable_hi_res_timer_tasks_on_battery = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(settings);

  base::TimeTicks before_deadline, after_deadline;

  scheduler->SetNeedsCommit();
  scheduler->SetNeedsRedraw();
  client.Reset();

  // On non-battery power
  EXPECT_FALSE(client.PowerMonitor()->IsOnBatteryPower());

  EXPECT_SCOPED(client.AdvanceFrame());
  client.Reset();

  before_deadline = client.now_src()->Now();
  EXPECT_TRUE(client.task_runner().RunTasksWhile(
      client.ImplFrameDeadlinePending(true)));
  after_deadline = client.now_src()->Now();

  // We post a non-zero deadline task when not on battery
  EXPECT_LT(before_deadline, after_deadline);

  // Switch to battery power
  client.PowerMonitorSource()->GeneratePowerStateEvent(true);
  EXPECT_TRUE(client.PowerMonitor()->IsOnBatteryPower());

  EXPECT_SCOPED(client.AdvanceFrame());
  scheduler->SetNeedsCommit();
  scheduler->SetNeedsRedraw();
  client.Reset();

  before_deadline = client.now_src()->Now();
  EXPECT_TRUE(client.task_runner().RunTasksWhile(
      client.ImplFrameDeadlinePending(true)));
  after_deadline = client.now_src()->Now();

  // We post a zero deadline task when on battery
  EXPECT_EQ(before_deadline, after_deadline);

  // Switch to non-battery power
  client.PowerMonitorSource()->GeneratePowerStateEvent(false);
  EXPECT_FALSE(client.PowerMonitor()->IsOnBatteryPower());

  EXPECT_SCOPED(client.AdvanceFrame());
  scheduler->SetNeedsCommit();
  scheduler->SetNeedsRedraw();
  client.Reset();

  // Same as before
  before_deadline = client.now_src()->Now();
  EXPECT_TRUE(client.task_runner().RunTasksWhile(
      client.ImplFrameDeadlinePending(true)));
  after_deadline = client.now_src()->Now();
}

TEST(SchedulerTest,
     SimulateWindowsLowResolutionTimerOnBattery_PrioritizeImplLatencyOff) {
  FakeSchedulerClient client;
  SchedulerSettings settings;
  settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(settings);

  // Set needs commit so that the scheduler tries to wait for the main thread
  scheduler->SetNeedsCommit();
  // Set needs redraw so that the scheduler doesn't wait too long
  scheduler->SetNeedsRedraw();
  client.Reset();

  // Switch to battery power
  client.PowerMonitorSource()->GeneratePowerStateEvent(true);
  EXPECT_TRUE(client.PowerMonitor()->IsOnBatteryPower());

  EXPECT_SCOPED(client.AdvanceFrame());
  scheduler->SetNeedsCommit();
  scheduler->SetNeedsRedraw();
  client.Reset();

  // Disable auto-advancing of now_src
  client.task_runner().SetAutoAdvanceNowToPendingTasks(false);

  // Deadline task is pending
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.task_runner().RunPendingTasks();
  // Deadline task is still pending
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());

  // Advance now by 15 ms - same as windows low res timer
  client.now_src()->AdvanceNowMicroseconds(15000);
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.task_runner().RunPendingTasks();
  // Deadline task finally completes
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
}

TEST(SchedulerTest,
     SimulateWindowsLowResolutionTimerOnBattery_PrioritizeImplLatencyOn) {
  FakeSchedulerClient client;
  SchedulerSettings settings;
  settings.disable_hi_res_timer_tasks_on_battery = true;
  settings.use_external_begin_frame_source = true;

  CREATE_SCHEDULER_AND_INIT_SURFACE(settings);

  // Set needs commit so that the scheduler tries to wait for the main thread
  scheduler->SetNeedsCommit();
  // Set needs redraw so that the scheduler doesn't wait too long
  scheduler->SetNeedsRedraw();
  client.Reset();

  // Switch to battery power
  client.PowerMonitorSource()->GeneratePowerStateEvent(true);
  EXPECT_TRUE(client.PowerMonitor()->IsOnBatteryPower());

  EXPECT_SCOPED(client.AdvanceFrame());
  scheduler->SetNeedsCommit();
  scheduler->SetNeedsRedraw();
  client.Reset();

  // Disable auto-advancing of now_src
  client.task_runner().SetAutoAdvanceNowToPendingTasks(false);

  // Deadline task is pending
  EXPECT_TRUE(scheduler->BeginImplFrameDeadlinePending());
  client.task_runner().RunPendingTasks();
  // Deadline task runs immediately
  EXPECT_FALSE(scheduler->BeginImplFrameDeadlinePending());
}

}  // namespace
}  // namespace cc
