// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/webdriver/commands/create_session.h"

#include <sstream>
#include <string>

#include "base/values.h"
#include "chrome/app/chrome_dll_resource.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_switches.h"

namespace webdriver {

void CreateSession::ExecutePost(Response* const response) {
  SessionManager* session_manager = Singleton<SessionManager>::get();
  std::string session_id;
  LOG(INFO) << "Creating new session";
  if (session_manager->Create(&session_id)) {
    LOG(INFO) << "Created session " << session_id;
    std::ostringstream stream;
    stream << "http://" << session_manager->GetIPAddress() << "/session/"
    << session_id;
    response->set_status(kSeeOther);
    response->set_value(Value::CreateStringValue(stream.str()));
  } else {
    response->set_status(kUnknownError);
    response->set_value(Value::CreateStringValue("Failed to create session"));
  }
}
}  // namespace webdriver

