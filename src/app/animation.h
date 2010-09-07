// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APP_ANIMATION_H_
#define APP_ANIMATION_H_
#pragma once

#include "app/animation_container.h"
#include "base/ref_counted.h"
#include "base/time.h"

namespace gfx {
class Rect;
}

class Animation;

// AnimationDelegate
//
//  Implement this interface when you want to receive notifications about the
//  state of an animation.
class AnimationDelegate {
 public:
  // Called when an animation has completed.
  virtual void AnimationEnded(const Animation* animation) {
  }

  // Called when an animation has progressed.
  virtual void AnimationProgressed(const Animation* animation) {
  }

  // Called when an animation has been canceled.
  virtual void AnimationCanceled(const Animation* animation) {
  }

 protected:
  virtual ~AnimationDelegate() {}
};

// Base class used in implementing animations. You only need use this class if
// you're implementing a new animation type, otherwise you'll likely want one of
// LinearAnimation, SlideAnimation, ThrobAnimation or MultiAnimation.
//
// To subclass override Step, which is invoked as the animation progresses and
// GetCurrentValue() to return the value appropriate to the animation.
class Animation : public AnimationContainer::Element {
 public:
  explicit Animation(base::TimeDelta timer_interval);
  virtual ~Animation();

  // Starts the animation. Does nothing if the animation is already running.
  void Start();

  // Stops the animation. Does nothing if the animation is not running.
  void Stop();

  // Gets the value for the current state, according to the animation
  // curve in use.
  virtual double GetCurrentValue() const = 0;

  // Convenience for returning a value between |start| and |target| based on
  // the current value. This is (target - start) * GetCurrentValue() + start.
  double CurrentValueBetween(double start, double target) const;
  int CurrentValueBetween(int start, int target) const;
  gfx::Rect CurrentValueBetween(const gfx::Rect& start_bounds,
                                const gfx::Rect& target_bounds) const;

  // Sets the delegate.
  void set_delegate(AnimationDelegate* delegate) { delegate_ = delegate; }

  // Sets the container used to manage the timer. A value of NULL results in
  // creating a new AnimationContainer.
  void SetContainer(AnimationContainer* container);

  bool is_animating() const { return is_animating_; }

  base::TimeDelta timer_interval() const { return timer_interval_; }

  // Returns true if rich animations should be rendered.
  // Looks at session type (e.g. remote desktop) and accessibility settings
  // to give guidance for heavy animations such as "start download" arrow.
  static bool ShouldRenderRichAnimation();

 protected:
  // Invoked from Start to allow subclasses to prepare for the animation.
  virtual void AnimationStarted() {}

  // Invoked from Stop after we're removed from the container but before the
  // delegate has been invoked.
  virtual void AnimationStopped() {}

  // Invoked from stop to determine if cancel should be invoked. If this returns
  // true the delegate is notified the animation was canceled, otherwise the
  // delegate is notified the animation stopped.
  virtual bool ShouldSendCanceledFromStop() { return false; }

  AnimationContainer* container() { return container_.get(); }
  base::TimeTicks start_time() const { return start_time_; }
  AnimationDelegate* delegate() { return delegate_; }

  // AnimationContainer::Element overrides
  virtual void SetStartTime(base::TimeTicks start_time);
  virtual void Step(base::TimeTicks time_now) = 0;
  virtual base::TimeDelta GetTimerInterval() const { return timer_interval_; }

 private:
  // Interval for the animation.
  const base::TimeDelta timer_interval_;

  // If true we're running.
  bool is_animating_;

  // Our delegate; may be null.
  AnimationDelegate* delegate_;

  // Container we're in. If non-null we're animating.
  scoped_refptr<AnimationContainer> container_;

  // Time we started at.
  base::TimeTicks start_time_;

  DISALLOW_COPY_AND_ASSIGN(Animation);
};

#endif  // APP_ANIMATION_H_
