// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <deque>
#include <string>

#include "base/basictypes.h"
#include "base/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "chrome/browser/sync_file_system/drive_backend/sync_task_manager.h"
#include "chrome/browser/sync_file_system/drive_backend/sync_task_token.h"
#include "chrome/browser/sync_file_system/sync_file_system_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "webkit/common/fileapi/file_system_util.h"

#define MAKE_PATH(path)                                       \
  base::FilePath(fileapi::VirtualPath::GetNormalizedFilePath( \
      base::FilePath(FILE_PATH_LITERAL(path))))

namespace sync_file_system {
namespace drive_backend {

namespace {

void DumbTask(SyncStatusCode status,
              const SyncStatusCallback& callback) {
  base::MessageLoop::current()->PostTask(
      FROM_HERE, base::Bind(callback, status));
}

void IncrementAndAssign(int expected_before_counter,
                        int* counter,
                        SyncStatusCode* status_out,
                        SyncStatusCode status) {
  EXPECT_EQ(expected_before_counter, *counter);
  ++(*counter);
  *status_out = status;
}

template <typename T>
void IncrementAndAssignWithOwnedPointer(T* object,
                                        int* counter,
                                        SyncStatusCode* status_out,
                                        SyncStatusCode status) {
  ++(*counter);
  *status_out = status;
}

class TaskManagerClient
    : public SyncTaskManager::Client,
      public base::SupportsWeakPtr<TaskManagerClient> {
 public:
  explicit TaskManagerClient(int64 maximum_background_task)
      : maybe_schedule_next_task_count_(0),
        task_scheduled_count_(0),
        idle_task_scheduled_count_(0),
        last_operation_status_(SYNC_STATUS_OK) {
    task_manager_.reset(new SyncTaskManager(
        AsWeakPtr(), maximum_background_task));
    task_manager_->Initialize(SYNC_STATUS_OK);
    maybe_schedule_next_task_count_ = 0;
  }
  virtual ~TaskManagerClient() {}

  // DriveFileSyncManager::Client overrides.
  virtual void MaybeScheduleNextTask() OVERRIDE {
    ++maybe_schedule_next_task_count_;
  }
  virtual void NotifyLastOperationStatus(
      SyncStatusCode last_operation_status,
      bool last_operation_used_network) OVERRIDE {
    last_operation_status_ = last_operation_status;
  }

  void ScheduleTask(SyncStatusCode status_to_return,
                    const SyncStatusCallback& callback) {
    task_manager_->ScheduleTask(
        FROM_HERE,
        base::Bind(&TaskManagerClient::DoTask, AsWeakPtr(),
                   status_to_return, false /* idle */),
        SyncTaskManager::PRIORITY_MED,
        callback);
  }

  void ScheduleTaskIfIdle(SyncStatusCode status_to_return) {
    task_manager_->ScheduleTaskIfIdle(
        FROM_HERE,
        base::Bind(&TaskManagerClient::DoTask, AsWeakPtr(),
                   status_to_return, true /* idle */),
        SyncStatusCallback());
  }

  int maybe_schedule_next_task_count() const {
    return maybe_schedule_next_task_count_;
  }
  int task_scheduled_count() const { return task_scheduled_count_; }
  int idle_task_scheduled_count() const { return idle_task_scheduled_count_; }
  SyncStatusCode last_operation_status() const {
    return last_operation_status_;
  }

 private:
  void DoTask(SyncStatusCode status_to_return,
              bool is_idle_task,
              const SyncStatusCallback& callback) {
    ++task_scheduled_count_;
    if (is_idle_task)
      ++idle_task_scheduled_count_;
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(callback, status_to_return));
  }

  scoped_ptr<SyncTaskManager> task_manager_;

  int maybe_schedule_next_task_count_;
  int task_scheduled_count_;
  int idle_task_scheduled_count_;

  SyncStatusCode last_operation_status_;

  DISALLOW_COPY_AND_ASSIGN(TaskManagerClient);
};

class MultihopSyncTask : public ExclusiveTask {
 public:
  MultihopSyncTask(bool* task_started,
                   bool* task_completed)
      : task_started_(task_started),
        task_completed_(task_completed),
        weak_ptr_factory_(this) {
    DCHECK(task_started_);
    DCHECK(task_completed_);
  }

  virtual ~MultihopSyncTask() {}

  virtual void RunExclusive(const SyncStatusCallback& callback) OVERRIDE {
    DCHECK(!*task_started_);
    *task_started_ = true;
    base::MessageLoop::current()->PostTask(
        FROM_HERE, base::Bind(&MultihopSyncTask::CompleteTask,
                              weak_ptr_factory_.GetWeakPtr(), callback));
  }

 private:
  void CompleteTask(const SyncStatusCallback& callback) {
    DCHECK(*task_started_);
    DCHECK(!*task_completed_);
    *task_completed_ = true;
    callback.Run(SYNC_STATUS_OK);
  }

  bool* task_started_;
  bool* task_completed_;
  base::WeakPtrFactory<MultihopSyncTask> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(MultihopSyncTask);
};

class BackgroundTask : public SyncTask {
 public:
  struct Stats {
    int64 running_background_task;
    int64 finished_task;
    int64 max_parallel_task;

    Stats()
        : running_background_task(0),
          finished_task(0),
          max_parallel_task(0) {}
  };

  BackgroundTask(const std::string& app_id,
                 const base::FilePath& path,
                 Stats* stats)
      : app_id_(app_id),
        path_(path),
        stats_(stats),
        weak_ptr_factory_(this) {
  }

  virtual ~BackgroundTask() {
  }

  virtual void RunPreflight(scoped_ptr<SyncTaskToken> token) OVERRIDE {
    scoped_ptr<BlockingFactor> blocking_factor(new BlockingFactor);
    blocking_factor->app_id = app_id_;
    blocking_factor->paths.push_back(path_);

    SyncTaskManager::UpdateBlockingFactor(
        token.Pass(), blocking_factor.Pass(),
        base::Bind(&BackgroundTask::RunAsBackgroundTask,
                   weak_ptr_factory_.GetWeakPtr()));
  }

 private:
  void RunAsBackgroundTask(scoped_ptr<SyncTaskToken> token) {
    ++(stats_->running_background_task);
    if (stats_->max_parallel_task < stats_->running_background_task)
      stats_->max_parallel_task = stats_->running_background_task;

    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(&BackgroundTask::CompleteTask,
                   weak_ptr_factory_.GetWeakPtr(),
                   base::Passed(&token)));
  }

  void CompleteTask(scoped_ptr<SyncTaskToken> token) {
    ++(stats_->finished_task);
    --(stats_->running_background_task);
    SyncTaskManager::NotifyTaskDone(token.Pass(), SYNC_STATUS_OK);
  }

  std::string app_id_;
  base::FilePath path_;
  Stats* stats_;

  base::WeakPtrFactory<BackgroundTask> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(BackgroundTask);
};

class BlockerUpdateTestHelper : public SyncTask {
 public:
  typedef std::vector<std::string> Log;

  BlockerUpdateTestHelper(const std::string& name,
                          const std::string& app_id,
                          const std::vector<std::string>& paths,
                          Log* log)
      : name_(name),
        app_id_(app_id),
        paths_(paths.begin(), paths.end()),
        log_(log),
        weak_ptr_factory_(this) {
  }

  virtual ~BlockerUpdateTestHelper() {
  }

  virtual void RunPreflight(scoped_ptr<SyncTaskToken> token) OVERRIDE {
    UpdateBlocker(token.Pass());
  }

 private:
  void UpdateBlocker(scoped_ptr<SyncTaskToken> token) {
    if (paths_.empty()) {
      log_->push_back(name_ + ": finished");
      SyncTaskManager::NotifyTaskDone(token.Pass(), SYNC_STATUS_OK);
      return;
    }

    std::string updating_to = paths_.front();
    paths_.pop_front();

    log_->push_back(name_ + ": updating to " + updating_to);

    scoped_ptr<BlockingFactor> blocking_factor(new BlockingFactor);
    blocking_factor->app_id = app_id_;
    blocking_factor->paths.push_back(
        base::FilePath(fileapi::VirtualPath::GetNormalizedFilePath(
            base::FilePath::FromUTF8Unsafe(updating_to))));

    SyncTaskManager::UpdateBlockingFactor(
        token.Pass(), blocking_factor.Pass(),
        base::Bind(&BlockerUpdateTestHelper::UpdateBlockerSoon,
                   weak_ptr_factory_.GetWeakPtr(),
                   updating_to));
  }

  void UpdateBlockerSoon(const std::string& updated_to,
                         scoped_ptr<SyncTaskToken> token) {
    log_->push_back(name_ + ": updated to " + updated_to);
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(&BlockerUpdateTestHelper::UpdateBlocker,
                   weak_ptr_factory_.GetWeakPtr(), base::Passed(&token)));
  }

  std::string name_;
  std::string app_id_;
  std::deque<std::string> paths_;
  Log* log_;

  base::WeakPtrFactory<BlockerUpdateTestHelper> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(BlockerUpdateTestHelper);
};

// Arbitrary non-default status values for testing.
const SyncStatusCode kStatus1 = static_cast<SyncStatusCode>(-1);
const SyncStatusCode kStatus2 = static_cast<SyncStatusCode>(-2);
const SyncStatusCode kStatus3 = static_cast<SyncStatusCode>(-3);
const SyncStatusCode kStatus4 = static_cast<SyncStatusCode>(-4);
const SyncStatusCode kStatus5 = static_cast<SyncStatusCode>(-5);

}  // namespace

TEST(SyncTaskManagerTest, ScheduleTask) {
  base::MessageLoop message_loop;
  TaskManagerClient client(0 /* maximum_background_task */);
  int callback_count = 0;
  SyncStatusCode callback_status = SYNC_STATUS_OK;

  client.ScheduleTask(kStatus1, base::Bind(&IncrementAndAssign, 0,
                                           &callback_count,
                                           &callback_status));
  message_loop.RunUntilIdle();

  EXPECT_EQ(kStatus1, callback_status);
  EXPECT_EQ(kStatus1, client.last_operation_status());

  EXPECT_EQ(1, callback_count);
  EXPECT_EQ(1, client.maybe_schedule_next_task_count());
  EXPECT_EQ(1, client.task_scheduled_count());
  EXPECT_EQ(0, client.idle_task_scheduled_count());
}

TEST(SyncTaskManagerTest, ScheduleTwoTasks) {
  base::MessageLoop message_loop;
  TaskManagerClient client(0 /* maximum_background_task */);
  int callback_count = 0;
  SyncStatusCode callback_status = SYNC_STATUS_OK;

  client.ScheduleTask(kStatus1, base::Bind(&IncrementAndAssign, 0,
                                           &callback_count,
                                           &callback_status));
  client.ScheduleTask(kStatus2, base::Bind(&IncrementAndAssign, 1,
                                           &callback_count,
                                           &callback_status));
  message_loop.RunUntilIdle();

  EXPECT_EQ(kStatus2, callback_status);
  EXPECT_EQ(kStatus2, client.last_operation_status());

  EXPECT_EQ(2, callback_count);
  EXPECT_EQ(1, client.maybe_schedule_next_task_count());
  EXPECT_EQ(2, client.task_scheduled_count());
  EXPECT_EQ(0, client.idle_task_scheduled_count());
}

TEST(SyncTaskManagerTest, ScheduleIdleTask) {
  base::MessageLoop message_loop;
  TaskManagerClient client(0 /* maximum_background_task */);

  client.ScheduleTaskIfIdle(kStatus1);
  message_loop.RunUntilIdle();

  EXPECT_EQ(kStatus1, client.last_operation_status());

  EXPECT_EQ(1, client.maybe_schedule_next_task_count());
  EXPECT_EQ(1, client.task_scheduled_count());
  EXPECT_EQ(1, client.idle_task_scheduled_count());
}

TEST(SyncTaskManagerTest, ScheduleIdleTaskWhileNotIdle) {
  base::MessageLoop message_loop;
  TaskManagerClient client(0 /* maximum_background_task */);
  int callback_count = 0;
  SyncStatusCode callback_status = SYNC_STATUS_OK;

  client.ScheduleTask(kStatus1, base::Bind(&IncrementAndAssign, 0,
                                           &callback_count,
                                           &callback_status));
  client.ScheduleTaskIfIdle(kStatus2);
  message_loop.RunUntilIdle();

  // Idle task must not have run.
  EXPECT_EQ(kStatus1, callback_status);
  EXPECT_EQ(kStatus1, client.last_operation_status());

  EXPECT_EQ(1, callback_count);
  EXPECT_EQ(1, client.maybe_schedule_next_task_count());
  EXPECT_EQ(1, client.task_scheduled_count());
  EXPECT_EQ(0, client.idle_task_scheduled_count());
}

TEST(SyncTaskManagerTest, ScheduleAndCancelSyncTask) {
  base::MessageLoop message_loop;

  int callback_count = 0;
  SyncStatusCode status = SYNC_STATUS_UNKNOWN;

  bool task_started = false;
  bool task_completed = false;

  {
    SyncTaskManager task_manager(base::WeakPtr<SyncTaskManager::Client>(),
                                 0 /* maximum_background_task */);
    task_manager.Initialize(SYNC_STATUS_OK);
    task_manager.ScheduleSyncTask(
        FROM_HERE,
        scoped_ptr<SyncTask>(new MultihopSyncTask(
            &task_started, &task_completed)),
        SyncTaskManager::PRIORITY_MED,
        base::Bind(&IncrementAndAssign, 0, &callback_count, &status));
  }

  message_loop.RunUntilIdle();
  EXPECT_EQ(0, callback_count);
  EXPECT_EQ(SYNC_STATUS_UNKNOWN, status);
  EXPECT_TRUE(task_started);
  EXPECT_FALSE(task_completed);
}

TEST(SyncTaskManagerTest, ScheduleTaskAtPriority) {
  base::MessageLoop message_loop;
  SyncTaskManager task_manager(base::WeakPtr<SyncTaskManager::Client>(),
                               0 /* maximum_background_task */);
  task_manager.Initialize(SYNC_STATUS_OK);

  int callback_count = 0;
  SyncStatusCode callback_status1 = SYNC_STATUS_OK;
  SyncStatusCode callback_status2 = SYNC_STATUS_OK;
  SyncStatusCode callback_status3 = SYNC_STATUS_OK;
  SyncStatusCode callback_status4 = SYNC_STATUS_OK;
  SyncStatusCode callback_status5 = SYNC_STATUS_OK;

  // This will run first even if its priority is low, since there're no
  // pending tasks.
  task_manager.ScheduleTask(
      FROM_HERE,
      base::Bind(&DumbTask, kStatus1),
      SyncTaskManager::PRIORITY_LOW,
      base::Bind(&IncrementAndAssign, 0, &callback_count, &callback_status1));

  // This runs last (expected counter == 4).
  task_manager.ScheduleTask(
      FROM_HERE,
      base::Bind(&DumbTask, kStatus2),
      SyncTaskManager::PRIORITY_LOW,
      base::Bind(&IncrementAndAssign, 4, &callback_count, &callback_status2));

  // This runs second (expected counter == 1).
  task_manager.ScheduleTask(
      FROM_HERE,
      base::Bind(&DumbTask, kStatus3),
      SyncTaskManager::PRIORITY_HIGH,
      base::Bind(&IncrementAndAssign, 1, &callback_count, &callback_status3));

  // This runs fourth (expected counter == 3).
  task_manager.ScheduleTask(
      FROM_HERE,
      base::Bind(&DumbTask, kStatus4),
      SyncTaskManager::PRIORITY_MED,
      base::Bind(&IncrementAndAssign, 3, &callback_count, &callback_status4));

  // This runs third (expected counter == 2).
  task_manager.ScheduleTask(
      FROM_HERE,
      base::Bind(&DumbTask, kStatus5),
      SyncTaskManager::PRIORITY_HIGH,
      base::Bind(&IncrementAndAssign, 2, &callback_count, &callback_status5));

  message_loop.RunUntilIdle();

  EXPECT_EQ(kStatus1, callback_status1);
  EXPECT_EQ(kStatus2, callback_status2);
  EXPECT_EQ(kStatus3, callback_status3);
  EXPECT_EQ(kStatus4, callback_status4);
  EXPECT_EQ(kStatus5, callback_status5);
  EXPECT_EQ(5, callback_count);
}

TEST(SyncTaskManagerTest, BackgroundTask_Sequential) {
  base::MessageLoop message_loop;
  SyncTaskManager task_manager(base::WeakPtr<SyncTaskManager::Client>(),
                               10 /* maximum_background_task */);
  task_manager.Initialize(SYNC_STATUS_OK);

  SyncStatusCode status = SYNC_STATUS_FAILED;
  BackgroundTask::Stats stats;
  task_manager.ScheduleSyncTask(
      FROM_HERE,
      scoped_ptr<SyncTask>(new BackgroundTask(
          "app_id", MAKE_PATH("/hoge/fuga"),
          &stats)),
      SyncTaskManager::PRIORITY_MED,
      CreateResultReceiver(&status));

  task_manager.ScheduleSyncTask(
      FROM_HERE,
      scoped_ptr<SyncTask>(new BackgroundTask(
          "app_id", MAKE_PATH("/hoge"),
          &stats)),
      SyncTaskManager::PRIORITY_MED,
      CreateResultReceiver(&status));

  task_manager.ScheduleSyncTask(
      FROM_HERE,
      scoped_ptr<SyncTask>(new BackgroundTask(
          "app_id", MAKE_PATH("/hoge/fuga/piyo"),
          &stats)),
      SyncTaskManager::PRIORITY_MED,
      CreateResultReceiver(&status));

  message_loop.RunUntilIdle();

  EXPECT_EQ(SYNC_STATUS_OK, status);
  EXPECT_EQ(0, stats.running_background_task);
  EXPECT_EQ(3, stats.finished_task);
  EXPECT_EQ(1, stats.max_parallel_task);
}

TEST(SyncTaskManagerTest, BackgroundTask_Parallel) {
  base::MessageLoop message_loop;
  SyncTaskManager task_manager(base::WeakPtr<SyncTaskManager::Client>(),
                               10 /* maximum_background_task */);
  task_manager.Initialize(SYNC_STATUS_OK);

  SyncStatusCode status = SYNC_STATUS_FAILED;
  BackgroundTask::Stats stats;
  task_manager.ScheduleSyncTask(
      FROM_HERE,
      scoped_ptr<SyncTask>(new BackgroundTask(
          "app_id", MAKE_PATH("/hoge"),
          &stats)),
      SyncTaskManager::PRIORITY_MED,
      CreateResultReceiver(&status));

  task_manager.ScheduleSyncTask(
      FROM_HERE,
      scoped_ptr<SyncTask>(new BackgroundTask(
          "app_id", MAKE_PATH("/fuga"),
          &stats)),
      SyncTaskManager::PRIORITY_MED,
      CreateResultReceiver(&status));

  task_manager.ScheduleSyncTask(
      FROM_HERE,
      scoped_ptr<SyncTask>(new BackgroundTask(
          "app_id", MAKE_PATH("/piyo"),
          &stats)),
      SyncTaskManager::PRIORITY_MED,
      CreateResultReceiver(&status));

  message_loop.RunUntilIdle();

  EXPECT_EQ(SYNC_STATUS_OK, status);
  EXPECT_EQ(0, stats.running_background_task);
  EXPECT_EQ(3, stats.finished_task);
  EXPECT_EQ(3, stats.max_parallel_task);
}

TEST(SyncTaskManagerTest, BackgroundTask_Throttled) {
  base::MessageLoop message_loop;
  SyncTaskManager task_manager(base::WeakPtr<SyncTaskManager::Client>(),
                               2 /* maximum_background_task */);
  task_manager.Initialize(SYNC_STATUS_OK);

  SyncStatusCode status = SYNC_STATUS_FAILED;
  BackgroundTask::Stats stats;
  task_manager.ScheduleSyncTask(
      FROM_HERE,
      scoped_ptr<SyncTask>(new BackgroundTask(
          "app_id", MAKE_PATH("/hoge"),
          &stats)),
      SyncTaskManager::PRIORITY_MED,
      CreateResultReceiver(&status));

  task_manager.ScheduleSyncTask(
      FROM_HERE,
      scoped_ptr<SyncTask>(new BackgroundTask(
          "app_id", MAKE_PATH("/fuga"),
          &stats)),
      SyncTaskManager::PRIORITY_MED,
      CreateResultReceiver(&status));

  task_manager.ScheduleSyncTask(
      FROM_HERE,
      scoped_ptr<SyncTask>(new BackgroundTask(
          "app_id", MAKE_PATH("/piyo"),
          &stats)),
      SyncTaskManager::PRIORITY_MED,
      CreateResultReceiver(&status));

  message_loop.RunUntilIdle();

  EXPECT_EQ(SYNC_STATUS_OK, status);
  EXPECT_EQ(0, stats.running_background_task);
  EXPECT_EQ(3, stats.finished_task);
  EXPECT_EQ(2, stats.max_parallel_task);
}

TEST(SyncTaskManagerTest, UpdateBlockingFactor) {
  base::MessageLoop message_loop;
  SyncTaskManager task_manager(base::WeakPtr<SyncTaskManager::Client>(),
                               10 /* maximum_background_task */);
  task_manager.Initialize(SYNC_STATUS_OK);

  SyncStatusCode status = SYNC_STATUS_FAILED;
  BlockerUpdateTestHelper::Log log;

  {
    std::vector<std::string> paths;
    paths.push_back("/foo/bar");
    paths.push_back("/foo");
    paths.push_back("/hoge/fuga/piyo");
    task_manager.ScheduleSyncTask(
        FROM_HERE,
        scoped_ptr<SyncTask>(new BlockerUpdateTestHelper(
            "task1", "app_id", paths, &log)),
        SyncTaskManager::PRIORITY_MED,
        CreateResultReceiver(&status));
  }

  {
    std::vector<std::string> paths;
    paths.push_back("/foo");
    paths.push_back("/foo/bar");
    paths.push_back("/hoge/fuga/piyo");
    task_manager.ScheduleSyncTask(
        FROM_HERE,
        scoped_ptr<SyncTask>(new BlockerUpdateTestHelper(
            "task2", "app_id", paths, &log)),
        SyncTaskManager::PRIORITY_MED,
        CreateResultReceiver(&status));
  }

  message_loop.RunUntilIdle();

  EXPECT_EQ(SYNC_STATUS_OK, status);

  ASSERT_EQ(14u, log.size());
  int i = 0;

  // task1 takes "/foo/bar" first.
  EXPECT_EQ("task1: updating to /foo/bar", log[i++]);
  EXPECT_EQ("task1: updated to /foo/bar", log[i++]);

  // task1 blocks task2. task2's update should be pending until task1 update.
  EXPECT_EQ("task2: updating to /foo", log[i++]);

  // task1 releases "/foo/bar" and tries to take "/foo". Then, pending task2
  // takes "/foo" and blocks task1.
  EXPECT_EQ("task1: updating to /foo", log[i++]);
  EXPECT_EQ("task2: updated to /foo", log[i++]);

  // task2 releases "/foo".
  EXPECT_EQ("task2: updating to /foo/bar", log[i++]);
  EXPECT_EQ("task1: updated to /foo", log[i++]);

  // task1 releases "/foo".
  EXPECT_EQ("task1: updating to /hoge/fuga/piyo", log[i++]);
  EXPECT_EQ("task1: updated to /hoge/fuga/piyo", log[i++]);
  EXPECT_EQ("task2: updated to /foo/bar", log[i++]);

  EXPECT_EQ("task1: finished", log[i++]);

  EXPECT_EQ("task2: updating to /hoge/fuga/piyo", log[i++]);
  EXPECT_EQ("task2: updated to /hoge/fuga/piyo", log[i++]);
  EXPECT_EQ("task2: finished", log[i++]);
}

}  // namespace drive_backend
}  // namespace sync_file_system
