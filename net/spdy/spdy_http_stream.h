// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SPDY_SPDY_HTTP_STREAM_H_
#define NET_SPDY_SPDY_HTTP_STREAM_H_
#pragma once

#include <list>
#include <string>

#include "base/basictypes.h"
#include "base/ref_counted.h"
#include "base/task.h"
#include "net/base/completion_callback.h"
#include "net/base/net_log.h"
#include "net/http/http_request_info.h"
#include "net/http/http_stream.h"
#include "net/spdy/spdy_protocol.h"
#include "net/spdy/spdy_session.h"
#include "net/spdy/spdy_stream.h"

namespace net {

class HttpResponseInfo;
class IOBuffer;
class SpdySession;
class UploadData;
class UploadDataStream;

// The SpdyHttpStream is a HTTP-specific type of stream known to a SpdySession.
class SpdyHttpStream : public SpdyStream::Delegate, public HttpStream {
 public:
  // SpdyHttpStream constructor
  SpdyHttpStream(SpdySession* spdy_session, bool direct);
  virtual ~SpdyHttpStream();

  SpdyStream* stream() { return stream_.get(); }

  // Cancels any callbacks from being invoked and deletes the stream.
  void Cancel();

  // HttpStream methods:
  virtual int InitializeStream(const HttpRequestInfo* request_info,
                               const BoundNetLog& net_log,
                               CompletionCallback* callback);
  virtual int SendRequest(const std::string& headers,
                          UploadDataStream* request_body,
                          HttpResponseInfo* response,
                          CompletionCallback* callback);
  virtual uint64 GetUploadProgress() const;
  virtual int ReadResponseHeaders(CompletionCallback* callback);
  virtual const HttpResponseInfo* GetResponseInfo() const;
  virtual int ReadResponseBody(IOBuffer* buf,
                               int buf_len,
                               CompletionCallback* callback);
  virtual void Close(bool not_reusable);
  virtual HttpStream* RenewStreamForAuth() { return NULL; }
  virtual bool IsResponseBodyComplete() const {
    if (!stream_)
      return false;
    return stream_->closed();
  }
  virtual bool CanFindEndOfResponse() const { return true; }
  virtual bool IsMoreDataBuffered() const { return false; }
  virtual bool IsConnectionReused() const {
    return spdy_session_->IsReused();
  }
  virtual void SetConnectionReused() {
    // SPDY doesn't need an indicator here.
  }
  virtual ClientSocketHandle* DetachConnection();

  virtual void GetSSLInfo(SSLInfo* ssl_info);
  virtual void GetSSLCertRequestInfo(SSLCertRequestInfo* cert_request_info);

  // SpdyStream::Delegate methods:
  virtual bool OnSendHeadersComplete(int status);
  virtual int OnSendBody();
  virtual bool OnSendBodyComplete(int status);
  virtual int OnResponseReceived(const spdy::SpdyHeaderBlock& response,
                                 base::Time response_time,
                                 int status);
  virtual void OnDataReceived(const char* buffer, int bytes);
  virtual void OnDataSent(int length);
  virtual void OnClose(int status);

 private:
  FRIEND_TEST_ALL_PREFIXES(SpdyNetworkTransactionTest, FlowControlStallResume);

  // Call the user callback.
  void DoCallback(int rv);

  void ScheduleBufferedReadCallback();

  // Returns true if the callback is invoked.
  bool DoBufferedReadCallback();
  bool ShouldWaitForMoreBufferedData() const;

  ScopedRunnableMethodFactory<SpdyHttpStream> read_callback_factory_;
  scoped_refptr<SpdyStream> stream_;
  scoped_refptr<SpdySession> spdy_session_;

  // The request to send.
  const HttpRequestInfo* request_info_;

  scoped_ptr<UploadDataStream> request_body_stream_;

  // |response_info_| is the HTTP response data object which is filled in
  // when a SYN_REPLY comes in for the stream.
  // It is not owned by this stream object, or point to |push_response_info_|.
  HttpResponseInfo* response_info_;

  scoped_ptr<HttpResponseInfo> push_response_info_;

  bool download_finished_;
  bool response_headers_received_;  // Indicates waiting for more HEADERS.

  // We buffer the response body as it arrives asynchronously from the stream.
  // TODO(mbelshe):  is this infinite buffering?
  std::list<scoped_refptr<IOBufferWithSize> > response_body_;

  CompletionCallback* user_callback_;

  // User provided buffer for the ReadResponseBody() response.
  scoped_refptr<IOBuffer> user_buffer_;
  int user_buffer_len_;

  // Is there a scheduled read callback pending.
  bool buffered_read_callback_pending_;
  // Has more data been received from the network during the wait for the
  // scheduled read callback.
  bool more_read_data_pending_;

  // Is this spdy stream direct to the origin server (or to a proxy).
  bool direct_;

  DISALLOW_COPY_AND_ASSIGN(SpdyHttpStream);
};

}  // namespace net

#endif  // NET_SPDY_SPDY_HTTP_STREAM_H_
