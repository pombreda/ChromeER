// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/shell/browser/shell_prefs.h"

#include "base/prefs/json_pref_store.h"
#include "base/prefs/pref_filter.h"
#include "base/prefs/pref_registry_simple.h"
#include "base/prefs/pref_service.h"
#include "base/prefs/pref_service_factory.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/user_prefs/user_prefs.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/browser/extension_prefs.h"

#if defined(OS_CHROMEOS)
#include "chromeos/audio/audio_devices_pref_handler_impl.h"
#endif

using base::FilePath;
using user_prefs::PrefRegistrySyncable;

namespace extensions {
namespace {

void RegisterLocalStatePrefs(PrefRegistrySimple* registry) {
#if defined(OS_CHROMEOS)
  chromeos::AudioDevicesPrefHandlerImpl::RegisterPrefs(registry, "");
#endif
}

// Creates a JsonPrefStore from a file at |filepath| and synchronously loads
// the preferences.
scoped_refptr<JsonPrefStore> CreateAndLoadPrefStore(const FilePath& filepath) {
  scoped_refptr<base::SequencedTaskRunner> task_runner =
      JsonPrefStore::GetTaskRunnerForFile(
          filepath, content::BrowserThread::GetBlockingPool());
  scoped_refptr<JsonPrefStore> pref_store =
      new JsonPrefStore(filepath, task_runner, scoped_ptr<PrefFilter>());
  pref_store->ReadPrefs();  // Synchronous.
  return pref_store;
}

}  // namespace

namespace shell_prefs {

scoped_ptr<PrefService> CreateLocalState(const FilePath& data_dir) {
  FilePath filepath = data_dir.AppendASCII("local_state.json");
  scoped_refptr<JsonPrefStore> pref_store = CreateAndLoadPrefStore(filepath);

  // Local state is considered "user prefs" from the factory's perspective.
  base::PrefServiceFactory factory;
  factory.set_user_prefs(pref_store);

  // Local state preferences are not syncable.
  PrefRegistrySimple* registry = new PrefRegistrySimple;
  RegisterLocalStatePrefs(registry);

  return factory.Create(registry);
}

scoped_ptr<PrefService> CreateUserPrefService(
    content::BrowserContext* browser_context) {
  FilePath filepath = browser_context->GetPath().AppendASCII("user_prefs.json");
  scoped_refptr<JsonPrefStore> pref_store = CreateAndLoadPrefStore(filepath);

  base::PrefServiceFactory factory;
  factory.set_user_prefs(pref_store);

  // TODO(jamescook): If we want to support prefs that are set by extensions
  // via ChromeSettings properties (e.g. chrome.accessibilityFeatures or
  // chrome.proxy) then this should create an ExtensionPrefStore and attach it
  // with PrefServiceFactory::set_extension_prefs().
  // See https://developer.chrome.com/extensions/types#ChromeSetting

  // Prefs should be registered before the PrefService is created.
  PrefRegistrySyncable* pref_registry = new PrefRegistrySyncable;
  ExtensionPrefs::RegisterProfilePrefs(pref_registry);

  scoped_ptr<PrefService> pref_service = factory.Create(pref_registry);
  user_prefs::UserPrefs::Set(browser_context, pref_service.get());
  return pref_service;
}

}  // namespace shell_prefs

}  // namespace extensions
