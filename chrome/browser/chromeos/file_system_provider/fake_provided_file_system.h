// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_FILE_SYSTEM_PROVIDER_FAKE_PROVIDED_FILE_SYSTEM_H_
#define CHROME_BROWSER_CHROMEOS_FILE_SYSTEM_PROVIDER_FAKE_PROVIDED_FILE_SYSTEM_H_

#include <map>
#include <string>
#include <vector>

#include "base/callback.h"
#include "base/files/file.h"
#include "base/memory/linked_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/task/cancelable_task_tracker.h"
#include "chrome/browser/chromeos/file_system_provider/provided_file_system_info.h"
#include "chrome/browser/chromeos/file_system_provider/provided_file_system_interface.h"
#include "chrome/browser/chromeos/file_system_provider/provided_file_system_observer.h"
#include "chrome/browser/chromeos/file_system_provider/watcher.h"
#include "storage/browser/fileapi/async_file_util.h"
#include "storage/browser/fileapi/watcher_manager.h"
#include "url/gurl.h"

class Profile;

namespace base {
class Time;
}  // namespace base

namespace net {
class IOBuffer;
}  // namespace net

namespace chromeos {
namespace file_system_provider {

class RequestManager;

// Path of a sample fake file, which is added to the fake file system by
// default.
extern const base::FilePath::CharType kFakeFilePath[];

// Represents a file or a directory on a fake file system.
struct FakeEntry {
  FakeEntry();
  FakeEntry(scoped_ptr<EntryMetadata> metadata, const std::string& contents);
  ~FakeEntry();

  scoped_ptr<EntryMetadata> metadata;
  std::string contents;

 private:
  DISALLOW_COPY_AND_ASSIGN(FakeEntry);
};

// Fake provided file system implementation. Does not communicate with target
// extensions. Used for unit tests.
class FakeProvidedFileSystem : public ProvidedFileSystemInterface {
 public:
  explicit FakeProvidedFileSystem(
      const ProvidedFileSystemInfo& file_system_info);
  virtual ~FakeProvidedFileSystem();

  // Adds a fake entry to the fake file system.
  void AddEntry(const base::FilePath& entry_path,
                bool is_directory,
                const std::string& name,
                int64 size,
                base::Time modification_time,
                std::string mime_type,
                std::string contents);

  // Fetches a pointer to a fake entry registered in the fake file system. If
  // not found, then returns NULL. The returned pointes is owned by
  // FakeProvidedFileSystem.
  const FakeEntry* GetEntry(const base::FilePath& entry_path) const;

  // ProvidedFileSystemInterface overrides.
  virtual AbortCallback RequestUnmount(
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual AbortCallback GetMetadata(
      const base::FilePath& entry_path,
      ProvidedFileSystemInterface::MetadataFieldMask fields,
      const ProvidedFileSystemInterface::GetMetadataCallback& callback)
      override;
  virtual AbortCallback ReadDirectory(
      const base::FilePath& directory_path,
      const storage::AsyncFileUtil::ReadDirectoryCallback& callback) override;
  virtual AbortCallback OpenFile(const base::FilePath& file_path,
                                 OpenFileMode mode,
                                 const OpenFileCallback& callback) override;
  virtual AbortCallback CloseFile(
      int file_handle,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual AbortCallback ReadFile(
      int file_handle,
      net::IOBuffer* buffer,
      int64 offset,
      int length,
      const ReadChunkReceivedCallback& callback) override;
  virtual AbortCallback CreateDirectory(
      const base::FilePath& directory_path,
      bool recursive,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual AbortCallback DeleteEntry(
      const base::FilePath& entry_path,
      bool recursive,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual AbortCallback CreateFile(
      const base::FilePath& file_path,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual AbortCallback CopyEntry(
      const base::FilePath& source_path,
      const base::FilePath& target_path,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual AbortCallback MoveEntry(
      const base::FilePath& source_path,
      const base::FilePath& target_path,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual AbortCallback Truncate(
      const base::FilePath& file_path,
      int64 length,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual AbortCallback WriteFile(
      int file_handle,
      net::IOBuffer* buffer,
      int64 offset,
      int length,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual AbortCallback AddWatcher(
      const GURL& origin,
      const base::FilePath& entry_path,
      bool recursive,
      bool persistent,
      const storage::AsyncFileUtil::StatusCallback& callback,
      const storage::WatcherManager::NotificationCallback&
          notification_callback) override;
  virtual void RemoveWatcher(
      const GURL& origin,
      const base::FilePath& entry_path,
      bool recursive,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual const ProvidedFileSystemInfo& GetFileSystemInfo() const override;
  virtual RequestManager* GetRequestManager() override;
  virtual Watchers* GetWatchers() override;
  virtual void AddObserver(ProvidedFileSystemObserver* observer) override;
  virtual void RemoveObserver(ProvidedFileSystemObserver* observer) override;
  virtual void Notify(
      const base::FilePath& entry_path,
      bool recursive,
      storage::WatcherManager::ChangeType change_type,
      scoped_ptr<ProvidedFileSystemObserver::Changes> changes,
      const std::string& tag,
      const storage::AsyncFileUtil::StatusCallback& callback) override;
  virtual base::WeakPtr<ProvidedFileSystemInterface> GetWeakPtr() override;

  // Factory callback, to be used in Service::SetFileSystemFactory(). The
  // |event_router| argument can be NULL.
  static ProvidedFileSystemInterface* Create(
      Profile* profile,
      const ProvidedFileSystemInfo& file_system_info);

 private:
  typedef std::map<base::FilePath, linked_ptr<FakeEntry> > Entries;
  typedef std::map<int, base::FilePath> OpenedFilesMap;

  // Utility function for posting a task which can be aborted by calling the
  // returned callback.
  AbortCallback PostAbortableTask(const base::Closure& callback);

  // Aborts a request. |task_id| refers to a posted callback returning a
  // response for the operation, which will be cancelled, hence not called.
  void Abort(int task_id,
             const storage::AsyncFileUtil::StatusCallback& callback);

  // Aborts a request. |task_ids| refers to a vector of posted callbacks
  // returning a response for the operation, which will be cancelled, hence not
  // called.
  void AbortMany(const std::vector<int>& task_ids,
                 const storage::AsyncFileUtil::StatusCallback& callback);

  ProvidedFileSystemInfo file_system_info_;
  Entries entries_;
  OpenedFilesMap opened_files_;
  int last_file_handle_;
  base::CancelableTaskTracker tracker_;
  ObserverList<ProvidedFileSystemObserver> observers_;
  Watchers watchers_;

  base::WeakPtrFactory<FakeProvidedFileSystem> weak_ptr_factory_;
  DISALLOW_COPY_AND_ASSIGN(FakeProvidedFileSystem);
};

}  // namespace file_system_provider
}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_FILE_SYSTEM_PROVIDER_FAKE_PROVIDED_FILE_SYSTEM_H_
