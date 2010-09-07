// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CONTENT_SETTING_COMBO_MODEL_H_
#define CHROME_BROWSER_CONTENT_SETTING_COMBO_MODEL_H_
#pragma once

#include "app/combobox_model.h"

#include "base/basictypes.h"
#include "base/string16.h"
#include "chrome/common/content_settings.h"

class ContentSettingComboModel : public ComboboxModel {
 public:
  explicit ContentSettingComboModel(bool show_session);
  virtual ~ContentSettingComboModel();

  virtual int GetItemCount();
  virtual string16 GetItemAt(int index);

  ContentSetting SettingForIndex(int index);

  int IndexForSetting(ContentSetting);

 private:
  const bool show_session_;

  const bool disable_cookie_prompt_;

  DISALLOW_COPY_AND_ASSIGN(ContentSettingComboModel);
};

#endif  // CHROME_BROWSER_CONTENT_SETTING_COMBO_MODEL_H_
