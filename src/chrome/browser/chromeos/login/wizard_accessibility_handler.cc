// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/wizard_accessibility_handler.h"

#include "chrome/browser/accessibility_events.h"
#include "chrome/browser/chromeos/cros/cros_library.h"
#include "chrome/browser/chromeos/cros/speech_synthesis_library.h"
#include "chrome/common/notification_service.h"

void WizardAccessibilityHandler::Observe(
    NotificationType type,
    const NotificationSource& source,
    const NotificationDetails& details) {
  const AccessibilityControlInfo *info = NULL;
  info = Details<const AccessibilityControlInfo>(details).ptr();
  switch (type.value) {
    case NotificationType::ACCESSIBILITY_CONTROL_FOCUSED:
      Speak(info->name().c_str());
      break;
    case NotificationType::ACCESSIBILITY_CONTROL_ACTION:
      break;
    case NotificationType::ACCESSIBILITY_TEXT_CHANGED:
      break;
    case NotificationType::ACCESSIBILITY_MENU_OPENED:
      break;
    case NotificationType::ACCESSIBILITY_MENU_CLOSED:
      break;
    default:
      NOTREACHED() << "Cannot recognize notification type: " << type.value;
      break;
  }
}

void WizardAccessibilityHandler::Speak(const char* speak_str) {
  if (chromeos::CrosLibrary::Get()->EnsureLoaded()) {
    chromeos::CrosLibrary::Get()->GetSpeechSynthesisLibrary()->
        StopSpeaking();
    chromeos::CrosLibrary::Get()->GetSpeechSynthesisLibrary()->
        Speak(speak_str);
  }
}
