// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_NET_NET_ERROR_HELPER_CORE_H_
#define CHROME_RENDERER_NET_NET_ERROR_HELPER_CORE_H_

#include <string>

#include "base/callback.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/common/localized_error.h"
#include "chrome/common/net/net_error_info.h"
#include "url/gurl.h"

namespace base {
class ListValue;
}

namespace blink {
struct WebURLError;
}

// Class that contains the logic for how the NetErrorHelper.  This allows for
// testing the logic without a RenderView or WebFrame, which are difficult to
// mock, and for testing races which are impossible to reliably reproduce
// with real RenderViews or WebFrames.
class NetErrorHelperCore {
 public:
  enum FrameType {
    MAIN_FRAME,
    SUB_FRAME,
  };

  enum PageType {
    NON_ERROR_PAGE,
    ERROR_PAGE,
  };

  // The Delegate handles all interaction with the RenderView, WebFrame, and
  // the network, as well as the generation of error pages.
  class Delegate {
   public:
    // Generates an error page's HTML for the given error.
    virtual void GenerateLocalizedErrorPage(
        const blink::WebURLError& error,
        bool is_failed_post,
        scoped_ptr<LocalizedError::ErrorPageParams> params,
        std::string* html) const = 0;

    // Loads the given HTML in the main frame for use as an error page.
    virtual void LoadErrorPageInMainFrame(const std::string& html,
                                          const GURL& failed_url) = 0;

    // Create extra Javascript bindings in the error page.
    virtual void EnableStaleLoadBindings(const GURL& page_url) = 0;

    // Updates the currently displayed error page with a new error code.  The
    // currently displayed error page must have finished loading, and must have
    // been generated by a call to GenerateLocalizedErrorPage.
    virtual void UpdateErrorPage(const blink::WebURLError& error,
                                 bool is_failed_post) = 0;

    // Fetches an error page and calls into OnErrorPageFetched when done.  Any
    // previous fetch must either be canceled or finished before calling.  Can't
    // be called synchronously after a previous fetch completes.
    virtual void FetchNavigationCorrections(
        const GURL& navigation_correction_url,
        const std::string& navigation_correction_request_body) = 0;

    // Cancels fetching navigation corrections.  Does nothing if no fetch is
    // ongoing.
    virtual void CancelFetchNavigationCorrections() = 0;

    // Starts a reload of the page in the observed frame.
    virtual void ReloadPage() = 0;

   protected:
    virtual ~Delegate() {}
  };

  explicit NetErrorHelperCore(Delegate* delegate);
  ~NetErrorHelperCore();

  // Examines |frame| and |error| to see if this is an error worthy of a DNS
  // probe.  If it is, initializes |error_strings| based on |error|,
  // |is_failed_post|, and |locale| with suitable strings and returns true.
  // If not, returns false, in which case the caller should look up error
  // strings directly using LocalizedError::GetNavigationErrorStrings.
  //
  // Updates the NetErrorHelper with the assumption the page will be loaded
  // immediately.
  void GetErrorHTML(FrameType frame_type,
                    const blink::WebURLError& error,
                    bool is_failed_post,
                    std::string* error_html);

  // These methods handle tracking the actual state of the page.
  void OnStartLoad(FrameType frame_type, PageType page_type);
  void OnCommitLoad(FrameType frame_type);
  void OnFinishLoad(FrameType frame_type);
  void OnStop();

  void CancelPendingFetches();

  // Called when an error page have has been retrieved over the network.  |html|
  // must be an empty string on error.
  void OnNavigationCorrectionsFetched(const std::string& corrections,
                                      const std::string& accept_languages,
                                      bool is_rtl);

  // Notifies |this| that network error information from the browser process
  // has been received.
  void OnNetErrorInfo(chrome_common_net::DnsProbeStatus status);

  void OnSetNavigationCorrectionInfo(const GURL& navigation_correction_url,
                                     const std::string& language,
                                     const std::string& country_code,
                                     const std::string& api_key,
                                     const GURL& search_url);
  // Notifies |this| that the network's online status changed.
  // Handler for NetworkStateChanged notification from the browser process. If
  // the network state changes to online, this method is responsible for
  // starting the auto-reload process.
  //
  // Warning: if there are many tabs sitting at an error page, this handler will
  // be run at the same time for each of their top-level renderframes, which can
  // cause many requests to be started at the same time. There's no current
  // protection against this kind of "reload storm".
  //
  // TODO(rdsmith): prevent the reload storm.
  void NetworkStateChanged(bool online);

  void set_auto_reload_enabled(bool auto_reload_enabled) {
    auto_reload_enabled_ = auto_reload_enabled;
  }

  int auto_reload_count() const { return auto_reload_count_; }

  bool ShouldSuppressErrorPage(FrameType frame_type, const GURL& url);

  void set_timer_for_testing(scoped_ptr<base::Timer> timer) {
    auto_reload_timer_.reset(timer.release());
  }

 private:
  struct ErrorPageInfo;

  // Updates the currently displayed error page with a new error based on the
  // most recently received DNS probe result.  The page must have finished
  // loading before this is called.
  void UpdateErrorPage();

  void GenerateLocalErrorPage(
      FrameType frame_type,
      const blink::WebURLError& error,
      bool is_failed_post,
      scoped_ptr<LocalizedError::ErrorPageParams> params,
      std::string* error_html);

  blink::WebURLError GetUpdatedError(const blink::WebURLError& error) const;

  void Reload();
  bool MaybeStartAutoReloadTimer();
  void StartAutoReloadTimer();

  static bool IsReloadableError(const ErrorPageInfo& info);

  Delegate* delegate_;

  // The last DnsProbeStatus received from the browser.
  chrome_common_net::DnsProbeStatus last_probe_status_;

  // Information for the provisional / "pre-provisional" error page.  NULL when
  // there's no page pending, or the pending page is not an error page.
  scoped_ptr<ErrorPageInfo> pending_error_page_info_;

  // Information for the committed error page.  NULL when the committed page is
  // not an error page.
  scoped_ptr<ErrorPageInfo> committed_error_page_info_;

  GURL navigation_correction_url_;
  std::string language_;
  std::string country_code_;
  std::string api_key_;
  GURL search_url_;

  bool auto_reload_enabled_;
  scoped_ptr<base::Timer> auto_reload_timer_;

  // Is the browser online?
  bool online_;

  int auto_reload_count_;
  bool can_auto_reload_page_;
};

#endif  // CHROME_RENDERER_NET_NET_ERROR_HELPER_CORE_H_
