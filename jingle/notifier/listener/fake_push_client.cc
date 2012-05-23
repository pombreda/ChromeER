// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "jingle/notifier/listener/fake_push_client.h"

#include "jingle/notifier/listener/push_client_observer.h"

namespace notifier {

FakePushClient::FakePushClient() {}

FakePushClient::~FakePushClient() {}

void FakePushClient::AddObserver(PushClientObserver* observer) {
  observers_.AddObserver(observer);
}

void FakePushClient::RemoveObserver(PushClientObserver* observer) {
  observers_.RemoveObserver(observer);
}

void FakePushClient::UpdateSubscriptions(
    const SubscriptionList& subscriptions) {
  subscriptions_ = subscriptions;
}

void FakePushClient::UpdateCredentials(
    const std::string& email, const std::string& token) {
  email_ = email;
  token_ = token;
}

void FakePushClient::SendNotification(const Notification& notification) {
  sent_notifications_.push_back(notification);
}

void FakePushClient::SimulateNotificationStateChange(
    bool notifications_enabled) {
  FOR_EACH_OBSERVER(PushClientObserver, observers_,
                    OnNotificationStateChange(notifications_enabled));
}

void FakePushClient::SimulateIncomingNotification(
    const Notification& notification) {
  FOR_EACH_OBSERVER(PushClientObserver, observers_,
                    OnIncomingNotification(notification));
}

const SubscriptionList& FakePushClient::subscriptions() const {
  return subscriptions_;
}

const std::string& FakePushClient::email() const {
  return email_;
}

const std::string& FakePushClient::token() const {
  return token_;
}

const std::vector<Notification>& FakePushClient::sent_notifications() const {
  return sent_notifications_;
}

}  // namespace notifier

