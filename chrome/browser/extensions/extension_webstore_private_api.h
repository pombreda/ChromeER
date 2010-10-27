// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_EXTENSIONS_EXTENSION_WEBSTORE_PRIVATE_API_H_
#define CHROME_BROWSER_EXTENSIONS_EXTENSION_WEBSTORE_PRIVATE_API_H_
#pragma once

#include "chrome/browser/browser_signin.h"
#include "chrome/browser/extensions/extension_function.h"
#include "chrome/common/net/gaia/google_service_auth_error.h"

class ProfileSyncService;

class WebstorePrivateApi {
 public:
  // Allows you to set the ProfileSyncService the function will use for
  // testing purposes.
  static void SetTestingProfileSyncService(ProfileSyncService* service);

  // Allows you to set the BrowserSignin the function will use for
  // testing purposes.
  static void SetTestingBrowserSignin(BrowserSignin* signin);
};

class InstallFunction : public SyncExtensionFunction {
 public:
  static void SetTestingInstallBaseUrl(const char* testing_install_base_url);

 protected:
  ~InstallFunction() {}
  virtual bool RunImpl();
  DECLARE_EXTENSION_FUNCTION_NAME("webstorePrivate.install");
};

class GetBrowserLoginFunction : public SyncExtensionFunction {
  virtual bool RunImpl();
  DECLARE_EXTENSION_FUNCTION_NAME("webstorePrivate.getBrowserLogin");
};

class GetStoreLoginFunction : public SyncExtensionFunction {
  virtual bool RunImpl();
  DECLARE_EXTENSION_FUNCTION_NAME("webstorePrivate.getStoreLogin");
};

class SetStoreLoginFunction : public SyncExtensionFunction {
  virtual bool RunImpl();
  DECLARE_EXTENSION_FUNCTION_NAME("webstorePrivate.setStoreLogin");
};

class PromptBrowserLoginFunction : public AsyncExtensionFunction,
                                   public BrowserSignin::SigninDelegate {
 public:
  // Implements BrowserSignin::SigninDelegate interface.
  virtual void OnLoginSuccess();
  virtual void OnLoginFailure(const GoogleServiceAuthError& error);

 protected:
  virtual ~PromptBrowserLoginFunction();
  virtual bool RunImpl();

 private:
  // Creates the message for signing in.
  virtual string16 GetLoginMessage();

  DECLARE_EXTENSION_FUNCTION_NAME("webstorePrivate.promptBrowserLogin");
};

#endif  // CHROME_BROWSER_EXTENSIONS_EXTENSION_WEBSTORE_PRIVATE_API_H_
