// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/host_port_pair.h"
#include "base/string_util.h"

namespace net {

HostPortPair::HostPortPair() : port_(0) {}
HostPortPair::HostPortPair(const std::string& in_host, uint16 in_port)
    : host_(in_host), port_(in_port) {}

std::string HostPortPair::ToString() const {
  // Check to see if the host is an IPv6 address.  If so, added brackets.
  if (host_.find(':') != std::string::npos) {
    DCHECK_NE(host_[0], '[');
    return StringPrintf("[%s]:%u", host_.c_str(), port_);
  }
  return StringPrintf("%s:%u", host_.c_str(), port_);
}

}  // namespace net
