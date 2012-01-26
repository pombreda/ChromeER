// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "jingle/notifier/listener/push_notifications_send_update_task.h"

#include <string>

#include "base/base64.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "jingle/notifier/listener/notification_constants.h"
#include "jingle/notifier/listener/xml_element_util.h"
#include "talk/xmllite/qname.h"
#include "talk/xmllite/xmlelement.h"
#include "talk/xmpp/constants.h"
#include "talk/xmpp/jid.h"
#include "talk/xmpp/xmppclient.h"

namespace notifier {

PushNotificationsSendUpdateTask::PushNotificationsSendUpdateTask(
    buzz::XmppTaskParentInterface* parent, const Notification& notification)
    : XmppTask(parent), notification_(notification) {}

PushNotificationsSendUpdateTask::~PushNotificationsSendUpdateTask() {}

int PushNotificationsSendUpdateTask::ProcessStart() {
  scoped_ptr<buzz::XmlElement> stanza(
      MakeUpdateMessage(notification_,
                        GetClient()->jid().BareJid()));
  VLOG(1) << "Sending notification " << notification_.ToString()
          << " as stanza " << XmlElementToString(*stanza);
  if (SendStanza(stanza.get()) != buzz::XMPP_RETURN_OK) {
    LOG(WARNING) << "Could not send stanza " << XmlElementToString(*stanza);
  }
  return STATE_DONE;
}

buzz::XmlElement* PushNotificationsSendUpdateTask::MakeUpdateMessage(
    const Notification& notification,
    const buzz::Jid& to_jid_bare) {
  DCHECK(to_jid_bare.IsBare());
  const buzz::QName kQnPush(kPushNotificationsNamespace, "push");
  const buzz::QName kQnChannel(buzz::STR_EMPTY, "channel");
  const buzz::QName kQnData(kPushNotificationsNamespace, "data");
  const buzz::QName kQnRecipient(kPushNotificationsNamespace, "recipient");

  // Create our update stanza. The message is constructed as:
  // <message from='{full jid}' to='{bare jid}' type='headline'>
  //   <push xmlns='google:push' channel='{channel}'>
  //     [<recipient to='{bare jid}'>{base-64 encoded data}</data>]*
  //     <data>{base-64 encoded data}</data>
  //   </push>
  // </message>

  buzz::XmlElement* message = new buzz::XmlElement(buzz::QN_MESSAGE);
  message->AddAttr(buzz::QN_TO, to_jid_bare.Str());
  message->AddAttr(buzz::QN_TYPE, "headline");

  buzz::XmlElement* push = new buzz::XmlElement(kQnPush, true);
  push->AddAttr(kQnChannel, notification.channel);
  message->AddElement(push);

  const RecipientList& recipients = notification.recipients;
  for (size_t i = 0; i < recipients.size(); ++i) {
    const Recipient& recipient = recipients[i];
    buzz::XmlElement* recipient_element =
        new buzz::XmlElement(kQnRecipient, true);
    push->AddElement(recipient_element);
    recipient_element->AddAttr(buzz::QN_TO, recipient.to);
    if (!recipient.user_specific_data.empty()) {
      std::string base64_data;
      if (!base::Base64Encode(recipient.user_specific_data, &base64_data)) {
        LOG(WARNING) << "Could not encode data "
                     << recipient.user_specific_data;
      } else {
        recipient_element->SetBodyText(base64_data);
      }
    }
  }

  buzz::XmlElement* data = new buzz::XmlElement(kQnData, true);
  std::string base64_data;
  if (!base::Base64Encode(notification.data, &base64_data)) {
    LOG(WARNING) << "Could not encode data " << notification.data;
  }
  data->SetBodyText(base64_data);
  push->AddElement(data);

  return message;
}

}  // namespace notifier
