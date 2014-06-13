// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_TEST_MOCK_OCCLUSION_TRACKER_H_
#define CC_TEST_MOCK_OCCLUSION_TRACKER_H_

#include "cc/trees/occlusion_tracker.h"

namespace cc {

template <typename LayerType>
class MockOcclusionTracker : public OcclusionTracker<LayerType> {
  // This class is used for testing only. It fakes just enough information to
  // calculate unoccluded content rect and unoccluded contributing surface
  // content rect. It calls the helper function of occlusion tracker to avoid
  // DCHECKs since testing environment won't be set up properly to pass those.
 public:
  explicit MockOcclusionTracker(const gfx::Rect& screen_scissor_rect)
      : OcclusionTracker<LayerType>(screen_scissor_rect) {
    typename OcclusionTracker<LayerType>::StackObject stack_obj;
    OcclusionTracker<LayerType>::stack_.push_back(stack_obj);
    OcclusionTracker<LayerType>::stack_.push_back(stack_obj);
  }

  void set_occluded_target_rect(const gfx::Rect& occluded) {
    OcclusionTracker<LayerType>::stack_.back().occlusion_from_inside_target =
        occluded;
  }

  void set_occluded_target_rect_for_contributing_surface(
      const gfx::Rect& occluded) {
    OcclusionTracker<
        LayerType>::stack_[OcclusionTracker<LayerType>::stack_.size() - 2]
        .occlusion_from_inside_target = occluded;
  }
};

}  // namespace cc

#endif  // CC_TEST_MOCK_OCCLUSION_TRACKER_H_
