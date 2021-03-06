// Copyright (c) 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "android_webview/browser/aw_ssl_host_state_delegate.h"

#include "net/base/hash_value.h"

using content::SSLHostStateDelegate;

namespace android_webview {

namespace internal {
net::SHA256HashValue getChainFingerprint256(const net::X509Certificate& cert) {
  net::SHA256HashValue fingerprint =
      net::X509Certificate::CalculateChainFingerprint256(
          cert.os_cert_handle(), cert.GetIntermediateCertificates());
  return fingerprint;
}

CertPolicy::CertPolicy() {
}
CertPolicy::~CertPolicy() {
}

// For an allowance, we consider a given |cert| to be a match to a saved
// allowed cert if the |error| is an exact match to or subset of the errors
// in the saved CertStatus.
bool CertPolicy::Check(const net::X509Certificate& cert,
                       net::CertStatus error) const {
  net::SHA256HashValue fingerprint = getChainFingerprint256(cert);
  std::map<net::SHA256HashValue, net::CertStatus,
           net::SHA256HashValueLessThan>::const_iterator allowed_iter =
      allowed_.find(fingerprint);
  if ((allowed_iter != allowed_.end()) && (allowed_iter->second & error) &&
      ((allowed_iter->second & error) == error)) {
    return true;
  }
  return false;
}

void CertPolicy::Allow(const net::X509Certificate& cert,
                       net::CertStatus error) {
  // If this same cert had already been saved with a different error status,
  // this will replace it with the new error status.
  net::SHA256HashValue fingerprint = getChainFingerprint256(cert);
  allowed_[fingerprint] = error;
}

}  // namespace internal

AwSSLHostStateDelegate::AwSSLHostStateDelegate() {
}

AwSSLHostStateDelegate::~AwSSLHostStateDelegate() {
}

void AwSSLHostStateDelegate::HostRanInsecureContent(const std::string& host,
                                                    int pid) {
  // Intentional no-op for Android WebView.
}

bool AwSSLHostStateDelegate::DidHostRunInsecureContent(const std::string& host,
                                                       int pid) const {
  // Intentional no-op for Android WebView.
  return false;
}

void AwSSLHostStateDelegate::AllowCert(const std::string& host,
                                       const net::X509Certificate& cert,
                                       net::CertStatus error) {
  cert_policy_for_host_[host].Allow(cert, error);
}

void AwSSLHostStateDelegate::Clear() {
  cert_policy_for_host_.clear();
}

SSLHostStateDelegate::CertJudgment AwSSLHostStateDelegate::QueryPolicy(
    const std::string& host,
    const net::X509Certificate& cert,
    net::CertStatus error,
    bool* expired_previous_decision) {
  return cert_policy_for_host_[host].Check(cert, error)
             ? SSLHostStateDelegate::ALLOWED
             : SSLHostStateDelegate::DENIED;
}

}  // namespace android_webview
