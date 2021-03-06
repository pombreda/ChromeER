// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/host/native_messaging/native_messaging_pipe.h"

#include "base/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"

namespace remoting {

NativeMessagingPipe::NativeMessagingPipe() {
}

NativeMessagingPipe::~NativeMessagingPipe() {
}

void NativeMessagingPipe::Start(
    scoped_ptr<extensions::NativeMessageHost> host,
    scoped_ptr<extensions::NativeMessagingChannel> channel,
    const base::Closure& quit_closure) {
  host_ = host.Pass();
  channel_ = channel.Pass();
  quit_closure_ = quit_closure;
  channel_->Start(this);
}

void NativeMessagingPipe::OnMessage(scoped_ptr<base::Value> message) {
  std::string message_json;
  base::JSONWriter::Write(message.get(), &message_json);
  host_->OnMessage(message_json);
}

void NativeMessagingPipe::OnDisconnect() {
  if (!quit_closure_.is_null())
    base::ResetAndReturn(&quit_closure_).Run();
}

void NativeMessagingPipe::PostMessageFromNativeHost(
    const std::string& message) {
  scoped_ptr<base::Value> json(base::JSONReader::Read(message));
  channel_->SendMessage(json.Pass());
}

void NativeMessagingPipe::CloseChannel(const std::string& error_message) {
  if (!quit_closure_.is_null())
    base::ResetAndReturn(&quit_closure_).Run();
}

}  // namespace remoting
