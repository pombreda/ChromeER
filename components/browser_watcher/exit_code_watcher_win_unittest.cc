// Copyright (c) 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/browser_watcher/exit_code_watcher_win.h"

#include "base/command_line.h"
#include "base/process/process.h"
#include "base/strings/string16.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/multiprocess_test.h"
#include "base/test/test_reg_util_win.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/win/scoped_handle.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"

namespace browser_watcher {

namespace {

const base::char16 kRegistryPath[] = L"Software\\BrowserWatcherTest";

MULTIPROCESS_TEST_MAIN(Sleeper) {
  // Sleep forever - the test harness will kill this process to give it an
  // exit code.
  base::PlatformThread::Sleep(base::TimeDelta::FromMilliseconds(INFINITE));
  return 1;
}

class ScopedSleeperProcess {
 public:
   ScopedSleeperProcess() : is_killed_(false) {
  }

  ~ScopedSleeperProcess() {
    if (process_.IsValid()) {
      process_.Terminate(-1);
      int exit_code = 0;
      EXPECT_TRUE(process_.WaitForExit(&exit_code));
    }
  }

  void Launch() {
    ASSERT_FALSE(process_.IsValid());

    base::CommandLine cmd_line(base::GetMultiProcessTestChildBaseCommandLine());
    base::LaunchOptions options;
    options.start_hidden = true;
    process_ = base::Process(
        base::SpawnMultiProcessTestChild("Sleeper", cmd_line, options));
    ASSERT_TRUE(process_.IsValid());
  }

  void Kill(int exit_code, bool wait) {
    ASSERT_TRUE(process_.IsValid());
    ASSERT_FALSE(is_killed_);
    process_.Terminate(exit_code);
    int seen_exit_code = 0;
    EXPECT_TRUE(process_.WaitForExit(&seen_exit_code));
    EXPECT_EQ(exit_code, seen_exit_code);
    is_killed_ = true;
  }

  void GetNewHandle(base::ProcessHandle* output) {
    ASSERT_TRUE(process_.IsValid());

    ASSERT_TRUE(DuplicateHandle(::GetCurrentProcess(),
                                process_.Handle(),
                                ::GetCurrentProcess(),
                                output,
                                0,
                                FALSE,
                                DUPLICATE_SAME_ACCESS));
  }

  const base::Process& process() const { return process_; }

 private:
  base::Process process_;
  bool is_killed_;
};

class BrowserWatcherTest : public testing::Test {
 public:
  typedef testing::Test Super;

  static const int kExitCode = 0xCAFEBABE;

  BrowserWatcherTest() :
      cmd_line_(base::CommandLine::NO_PROGRAM),
      process_(base::kNullProcessHandle) {
  }

  virtual void SetUp() override {
    Super::SetUp();

    override_manager_.OverrideRegistry(HKEY_CURRENT_USER);
  }

  virtual void TearDown() override {
    if (process_ != base::kNullProcessHandle) {
      base::CloseProcessHandle(process_);
      process_ = base::kNullProcessHandle;
    }

    Super::TearDown();
  }

  void OpenSelfWithAccess(uint32 access) {
    ASSERT_EQ(base::kNullProcessHandle, process_);
    ASSERT_TRUE(base::OpenProcessHandleWithAccess(
        base::GetCurrentProcId(), access, &process_));
  }

  void VerifyWroteExitCode(base::ProcessId proc_id, int exit_code) {
    base::win::RegistryValueIterator it(
          HKEY_CURRENT_USER, kRegistryPath);

    ASSERT_EQ(1, it.ValueCount());
    base::win::RegKey key(HKEY_CURRENT_USER,
                          kRegistryPath,
                          KEY_QUERY_VALUE);

    // The value name should encode the process id at the start.
    EXPECT_TRUE(StartsWith(it.Name(),
                           base::StringPrintf(L"%d-", proc_id),
                           false));
    DWORD value = 0;
    ASSERT_EQ(ERROR_SUCCESS, key.ReadValueDW(it.Name(), &value));
    ASSERT_EQ(exit_code, value);
  }

 protected:
  base::CommandLine cmd_line_;
  base::ProcessHandle process_;
  registry_util::RegistryOverrideManager override_manager_;
};

}  // namespace

TEST_F(BrowserWatcherTest, ExitCodeWatcherInvalidCmdLineFailsInit) {
  ExitCodeWatcher watcher(kRegistryPath);

  // An empty command line should fail.
  EXPECT_FALSE(watcher.ParseArguments(cmd_line_));

  // A non-numeric parent-handle argument should fail.
  cmd_line_.AppendSwitchASCII(ExitCodeWatcher::kParenthHandleSwitch, "asdf");
  EXPECT_FALSE(watcher.ParseArguments(cmd_line_));
}

TEST_F(BrowserWatcherTest, ExitCodeWatcherInvalidHandleFailsInit) {
  ExitCodeWatcher watcher(kRegistryPath);

  // A waitable event has a non process-handle.
  base::WaitableEvent event(false, false);

  // A non-process handle should fail.
  cmd_line_.AppendSwitchASCII(ExitCodeWatcher::kParenthHandleSwitch,
                              base::StringPrintf("%d", event.handle()));
  EXPECT_FALSE(watcher.ParseArguments(cmd_line_));
}

TEST_F(BrowserWatcherTest, ExitCodeWatcherNoAccessHandleFailsInit) {
  ExitCodeWatcher watcher(kRegistryPath);

  // Open a SYNCHRONIZE-only handle to this process.
  ASSERT_NO_FATAL_FAILURE(OpenSelfWithAccess(SYNCHRONIZE));

  // A process handle with insufficient access should fail.
  cmd_line_.AppendSwitchASCII(ExitCodeWatcher::kParenthHandleSwitch,
                              base::StringPrintf("%d", process_));
  EXPECT_FALSE(watcher.ParseArguments(cmd_line_));
}

TEST_F(BrowserWatcherTest, ExitCodeWatcherSucceedsInit) {
  ExitCodeWatcher watcher(kRegistryPath);

  // Open a handle to this process with sufficient access for the watcher.
  ASSERT_NO_FATAL_FAILURE(
      OpenSelfWithAccess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION));

  // A process handle with sufficient access should succeed init.
  cmd_line_.AppendSwitchASCII(ExitCodeWatcher::kParenthHandleSwitch,
                              base::StringPrintf("%d", process_));
  EXPECT_TRUE(watcher.ParseArguments(cmd_line_));

  ASSERT_EQ(process_, watcher.process().Handle());

  // The watcher takes ownership of the handle, make sure it's not
  // double-closed.
  process_ = base::kNullProcessHandle;
}

TEST_F(BrowserWatcherTest, ExitCodeWatcherOnExitedProcess) {
  ScopedSleeperProcess sleeper;
  ASSERT_NO_FATAL_FAILURE(sleeper.Launch());

  // Create a new handle to the sleeper process. This handle will leak in
  // the case this test fails. A ScopedHandle cannot be used here, as the
  // ownership would momentarily be held by two of them, which is disallowed.
  base::ProcessHandle sleeper_handle = base::kNullProcessHandle;
  sleeper.GetNewHandle(&sleeper_handle);

  ExitCodeWatcher watcher(kRegistryPath);

  cmd_line_.AppendSwitchASCII(ExitCodeWatcher::kParenthHandleSwitch,
                              base::StringPrintf("%d", sleeper_handle));
  EXPECT_TRUE(watcher.ParseArguments(cmd_line_));
  ASSERT_EQ(sleeper_handle, watcher.process().Handle());

  // Verify that the watcher wrote a sentinel for the process.
  VerifyWroteExitCode(sleeper.process().pid(), STILL_ACTIVE);

  // Kill the sleeper, and make sure it's exited before we continue.
  ASSERT_NO_FATAL_FAILURE(sleeper.Kill(kExitCode, true));

  watcher.WaitForExit();
  EXPECT_EQ(kExitCode, watcher.exit_code());

  VerifyWroteExitCode(sleeper.process().pid(), kExitCode);
}

}  // namespace browser_watcher
