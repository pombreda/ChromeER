/*
 * Copyright 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "build/build_config.h"

#ifndef OS_WIN
#include <unistd.h>
#endif

// Metrics report unit testing
#include <new>
#include <algorithm>
#include "gtest/gtest.h"
#include "metrics.h"

#ifdef OS_WIN
#define SLEEP_ONE_SECOND() ::Sleep(1000)
#else
#define SLEEP_ONE_SECOND() ::sleep(1)
#endif

DECLARE_METRIC_count(count);
DEFINE_METRIC_count(count);

DECLARE_METRIC_timing(timing);
DEFINE_METRIC_timing(timing);

DECLARE_METRIC_integer(integer);
DEFINE_METRIC_integer(integer);

DECLARE_METRIC_bool(bool);
DEFINE_METRIC_bool(bool);

using ::stats_report::BoolMetric;
using ::stats_report::CountMetric;
using ::stats_report::IntegerMetric;
using ::stats_report::MetricBase;
using ::stats_report::MetricCollection;
using ::stats_report::MetricCollectionBase;
using ::stats_report::MetricIterator;
using ::stats_report::TimingMetric;
using ::stats_report::TimingSample;
using ::stats_report::kBoolType;
using ::stats_report::kCountType;
using ::stats_report::kIntegerType;
using ::stats_report::kTimingType;


class MetricsTest: public testing::Test {
 protected:
  MetricCollection coll_;
};

class MetricsEnumTest: public MetricsTest {
 public:
  virtual void SetUp() {
    coll_.Initialize();
  }

  virtual void TearDown() {
    coll_.Uninitialize();
  }

 protected:
  MetricsEnumTest(): count_("count", &coll_), timing_("timing", &coll_),
                     integer_("integer", &coll_), bool_("bool", &coll_) {
  }

  CountMetric count_;
  TimingMetric timing_;
  IntegerMetric integer_;
  BoolMetric bool_;
};


// Validates that the above-declared metrics are available
// in the expected namespace
TEST_F(MetricsTest, Globals) {
  EXPECT_EQ(0u, ::metric_count.Reset());
  TimingMetric::TimingData data = ::metric_timing.Reset();
  EXPECT_EQ(0u, data.count);
  EXPECT_EQ(0u, data.maximum);
  EXPECT_EQ(0u, data.minimum);
  EXPECT_EQ(0u, data.sum);

  EXPECT_EQ(0u, ::metric_integer.value());
  EXPECT_EQ(BoolMetric::kBoolUnset, ::metric_bool.Reset());

  // Check for correct initialization
  EXPECT_STREQ("count", metric_count.name());
  EXPECT_STREQ("timing", metric_timing.name());
  EXPECT_STREQ("integer", metric_integer.name());
  EXPECT_STREQ("bool", metric_bool.name());
}


// make GUnit happy
inline std::ostream &operator << (std::ostream &str, const MetricIterator &it) {
  str << std::hex << reinterpret_cast<void*>(*it);
  return str;
}

TEST_F(MetricsTest, CollectionInitialization) {
  // The global MetricCollection is aliased to zero memory so as to ensure
  // no initialization order snafus. If an initialized MetricCollection
  // sets any of its storage to non-zero, there's a good chance that e.g. a
  // vtbl has snuck in there, which must not happen
  char buf1[sizeof(MetricCollection)] = { 0 };
  char buf2[sizeof(MetricCollection)] = { 0 };

  // Placement new a MetricCollection to one of the buffers
  new (buf1) MetricCollection();

  // and check they're still equivalent
  EXPECT_EQ(0, memcmp(buf1, buf2, sizeof(MetricCollection)));

  // MetricCollection must not extend MetricCollectionBase in size
  EXPECT_EQ(sizeof(MetricCollection), sizeof(MetricCollectionBase));
}

TEST_F(MetricsTest, Count) {
  CountMetric foo("foo", &coll_);

  EXPECT_EQ(0u, foo.Reset());
  EXPECT_EQ(kCountType, foo.type());
  ASSERT_TRUE(NULL != foo.AsCount());
  ASSERT_TRUE(NULL == foo.AsTiming());
  ASSERT_TRUE(NULL == foo.AsInteger());
  ASSERT_TRUE(NULL == foo.AsBool());

  ++foo;
  EXPECT_EQ(1u, foo.value());
  foo++;
  EXPECT_EQ(2u, foo.value());

  foo += 100;
  EXPECT_EQ(102u, foo.value());
}

TEST_F(MetricsTest, Timing) {
  TimingMetric foo("foo", &coll_);

  EXPECT_EQ(kTimingType, foo.type());
  ASSERT_TRUE(NULL == foo.AsCount());
  ASSERT_TRUE(NULL != foo.AsTiming());
  ASSERT_TRUE(NULL == foo.AsInteger());
  ASSERT_TRUE(NULL == foo.AsBool());

  foo.AddSample(100);
  foo.AddSample(50);

  EXPECT_EQ(2u, foo.count());
  EXPECT_EQ(150u, foo.sum());
  EXPECT_EQ(100u, foo.maximum());
  EXPECT_EQ(50u, foo.minimum());
  EXPECT_EQ(75u, foo.average());

  TimingMetric::TimingData data = foo.Reset();
  EXPECT_EQ(2u, data.count);
  EXPECT_EQ(150u, data.sum);
  EXPECT_EQ(100u, data.maximum);
  EXPECT_EQ(50u, data.minimum);

  EXPECT_EQ(0u, foo.count());
  EXPECT_EQ(0u, foo.sum());
  EXPECT_EQ(0u, foo.maximum());
  EXPECT_EQ(0u, foo.minimum());
  EXPECT_EQ(0u, foo.average());

  // Test counted samples
  foo.AddSamples(10u, 1000);
  foo.AddSamples(10u, 500);
  EXPECT_EQ(20u, foo.count());
  EXPECT_EQ(1500u, foo.sum());
  EXPECT_EQ(100u, foo.maximum());
  EXPECT_EQ(50u, foo.minimum());
  EXPECT_EQ(75u, foo.average());
}

TEST_F(MetricsTest, TimingSample) {
  TimingMetric foo("foo", &coll_);

  // add a sample to foo
  {
    TimingSample sample(&foo);

    SLEEP_ONE_SECOND();
  }

  TimingMetric::TimingData data = foo.Reset();

  // Should be precisely one sample in there
  EXPECT_EQ(1u, data.count);

  // again, this time with a non-unity count
  {
    TimingSample sample(&foo, 2);

    EXPECT_EQ(2u, sample.count());
    SLEEP_ONE_SECOND();
  }

  data = foo.Reset();

  // Should be precisely two samples in there
  EXPECT_EQ(2u, data.count);

  // now with zero count
  {
    TimingSample sample(&foo, 0);
  }

  data = foo.Reset();

  // Should be no samples in there
  EXPECT_EQ(0u, data.count);
}

TEST_F(MetricsTest, Integer) {
  IntegerMetric foo("foo", &coll_);

  EXPECT_EQ(kIntegerType, foo.type());
  ASSERT_TRUE(NULL == foo.AsCount());
  ASSERT_TRUE(NULL == foo.AsTiming());
  ASSERT_TRUE(NULL != foo.AsInteger());
  ASSERT_TRUE(NULL == foo.AsBool());

  EXPECT_EQ(0u, foo.value());
  foo.Set(1005);
  EXPECT_EQ(1005u, foo.value());
  foo = 1009UL;
  EXPECT_EQ(1009u, foo.value());

  foo.Set(0);

  ++foo;
  EXPECT_EQ(1u, foo.value());
  foo++;
  EXPECT_EQ(2u, foo.value());

  foo += 100;
  EXPECT_EQ(102u, foo.value());

  foo -= 100;
  EXPECT_EQ(2u, foo.value());
  foo--;
  EXPECT_EQ(1u, foo.value());
  --foo;
  EXPECT_EQ(0u, foo.value());
}

TEST_F(MetricsTest, Bool) {
  BoolMetric foo("foo", &coll_);

  EXPECT_EQ(kBoolType, foo.type());
  ASSERT_TRUE(NULL == foo.AsCount());
  ASSERT_TRUE(NULL == foo.AsTiming());
  ASSERT_TRUE(NULL == foo.AsInteger());
  ASSERT_TRUE(NULL != foo.AsBool());

  EXPECT_EQ(BoolMetric::kBoolUnset, foo.Reset());
  foo.Set(true);
  EXPECT_EQ(BoolMetric::kBoolTrue, foo.Reset());
  foo.Set(false);
  EXPECT_EQ(BoolMetric::kBoolFalse, foo.Reset());
  EXPECT_EQ(BoolMetric::kBoolUnset, foo.Reset());
}

TEST_F(MetricsEnumTest, Enumeration) {
  MetricBase *metrics[] = {
    &count_,
    &timing_,
    &integer_,
    &bool_,
  };

  int limit = sizeof(metrics) / sizeof(metrics[0]);
  for (int i = 0; i < limit; ++i) {
    MetricBase *stat = metrics[i];
    MetricBase *curr = coll_.first();

    for (; NULL != curr; curr = curr->next()) {
      if (stat == curr)
        break;
    }

    // if NULL, we didn't find our counter
    EXPECT_TRUE(NULL != curr);
  }
}

TEST_F(MetricsEnumTest, Iterator) {
  typedef MetricBase *MetricBasePtr;
  MetricBasePtr metrics[] = { &count_, &timing_, &integer_, &bool_, };
  int num_stats = arraysize(metrics);

  MetricIterator it(coll_), end;
  EXPECT_NE(it, end);

  // copy construction
  EXPECT_EQ(it, MetricIterator(it));
  EXPECT_EQ(end, MetricIterator(end));

  // # of iterations
  int i = 0;
  while (it++ != end)
    ++i;
  ASSERT_EQ(num_stats, i);
  ASSERT_EQ(end, it);

  // increment past end is idempotent
  ++it;
  ASSERT_EQ(end, it);

  // Check that we return no garbage or nonsense
  for (it = MetricIterator(coll_); it != end; ++it) {
    MetricBasePtr *stats_end = &metrics[num_stats];
    EXPECT_NE(stats_end, std::find(metrics, stats_end, *it));
  }

  // and that all metrics can be found
  int limit = sizeof(metrics) / sizeof(metrics[0]);
  for (int i = 0; i < limit; ++i) {
    MetricBase *stat = metrics[i];

    EXPECT_EQ(stat, *std::find(MetricIterator(coll_), end, stat));
  }
}

TEST_F(MetricsTest, SimpleConstruction) {
  const CountMetric c("c", 100);

  EXPECT_EQ(100u, c.value());
  EXPECT_EQ(kCountType, c.type());
  EXPECT_STREQ("c", c.name());
  EXPECT_TRUE(NULL == c.next());

  TimingMetric::TimingData data = { 10, 0, 1000, 10, 500 };
  const TimingMetric t("t", data);

  EXPECT_EQ(10u, t.count());
  EXPECT_EQ(1000u, t.sum());
  EXPECT_EQ(10u, t.minimum());
  EXPECT_EQ(500u, t.maximum());
  EXPECT_EQ(kTimingType, t.type());
  EXPECT_STREQ("t", t.name());
  EXPECT_TRUE(NULL == t.next());

  const IntegerMetric i("i", 200);

  EXPECT_EQ(200u, i.value());
  EXPECT_EQ(kIntegerType, i.type());
  EXPECT_STREQ("i", i.name());
  EXPECT_TRUE(NULL == i.next());

  const BoolMetric bool_true("bool_true", BoolMetric::kBoolTrue);

  EXPECT_EQ(BoolMetric::kBoolTrue, bool_true.value());
  EXPECT_EQ(kBoolType, bool_true.type());
  EXPECT_STREQ("bool_true", bool_true.name());
  EXPECT_TRUE(NULL == bool_true.next());

  const BoolMetric bool_false("bool_false", BoolMetric::kBoolFalse);

  EXPECT_EQ(BoolMetric::kBoolFalse, bool_false.value());
  EXPECT_EQ(kBoolType, bool_false.type());
  EXPECT_STREQ("bool_false", bool_false.name());
  EXPECT_TRUE(NULL == bool_false.next());
}
