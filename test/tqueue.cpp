//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2020
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/db/binlog/Binlog.h"
#include "td/db/binlog/BinlogEvent.h"
#include "td/db/binlog/BinlogHelper.h"
#include "td/db/TQueue.h"

#include "td/utils/int_types.h"
#include "td/utils/misc.h"
#include "td/utils/port/path.h"
#include "td/utils/Slice.h"
#include "td/utils/Span.h"
#include "td/utils/Status.h"
#include "td/utils/tests.h"
#include "td/utils/VectorQueue.h"

#include <map>
#include <set>

TEST(TQueue, hands) {
  td::TQueue::Event events[100];
  auto events_span = td::MutableSpan<td::TQueue::Event>(events, 100);

  auto tqueue = td::TQueue::create();
  auto qid = 12;
  ASSERT_EQ(true, tqueue->get_head(qid).empty());
  ASSERT_EQ(true, tqueue->get_tail(qid).empty());
  tqueue->push(qid, "hello", 0, 0, td::Auto());
  auto head = tqueue->get_head(qid);
  ASSERT_EQ(head.next().ok(), tqueue->get_tail(qid));
  ASSERT_EQ(1u, tqueue->get(qid, head, true, 0, events_span).move_as_ok());
}

class TestTQueue {
 public:
  using EventId = td::TQueue::EventId;

  td::CSlice binlog_path() {
    return "test_binlog";
  }
  TestTQueue() {
    baseline_ = td::TQueue::create();
    memory_ = td::TQueue::create();

    auto memory_storage = td::make_unique<td::TQueueMemoryStorage>();
    memory_storage_ = memory_storage.get();
    memory_->set_callback(std::move(memory_storage));

    binlog_ = td::TQueue::create();
    auto tqueue_binlog = td::make_unique<td::TQueueBinlog<td::Binlog>>();
    td::Binlog::destroy(binlog_path()).ensure();
    auto binlog = std::make_shared<td::Binlog>();
    binlog->init(binlog_path().str(), [&](const td::BinlogEvent &event) { UNREACHABLE(); }).ensure();
    tqueue_binlog->set_binlog(binlog);
    binlog_->set_callback(std::move(tqueue_binlog));
  }

  void restart(td::Random::Xorshift128plus &rnd, double now) {
    if (rnd.fast(0, 10) == 0) {
      baseline_->run_gc(now);
    }

    memory_->extract_callback().release();
    auto memory_storage = td::unique_ptr<td::TQueueMemoryStorage>(memory_storage_);
    memory_ = td::TQueue::create();
    memory_storage->replay(*memory_);
    memory_->set_callback(std::move(memory_storage));
    if (rnd.fast(0, 10) == 0) {
      memory_->run_gc(now);
    }

    if (rnd.fast(0, 100) != 0) {
      return;
    }

    LOG(INFO) << "Restart binlog";
    binlog_ = td::TQueue::create();
    auto tqueue_binlog = td::make_unique<td::TQueueBinlog<td::Binlog>>();
    auto binlog = std::make_shared<td::Binlog>();
    binlog->init(binlog_path().str(), [&](const td::BinlogEvent &event) { tqueue_binlog->replay(event, *binlog_); })
        .ensure();
    tqueue_binlog->set_binlog(binlog);
    binlog_->set_callback(std::move(tqueue_binlog));
    if (rnd.fast(0, 10) == 0) {
      binlog_->run_gc(now);
    }
  }

  EventId push(td::TQueue::QueueId queue_id, td::string data, double expires_at, EventId new_id = EventId()) {
    auto a_id = baseline_->push(queue_id, data, expires_at, 0, new_id).move_as_ok();
    auto b_id = memory_->push(queue_id, data, expires_at, 0, new_id).move_as_ok();
    auto c_id = binlog_->push(queue_id, data, expires_at, 0, new_id).move_as_ok();
    ASSERT_EQ(a_id, b_id);
    ASSERT_EQ(a_id, c_id);
    return a_id;
  }

  void check_head_tail(td::TQueue::QueueId qid, double now) {
    //ASSERT_EQ(baseline_->get_head(qid), memory_->get_head(qid));
    //ASSERT_EQ(baseline_->get_head(qid), binlog_->get_head(qid));
    ASSERT_EQ(baseline_->get_tail(qid), memory_->get_tail(qid));
    ASSERT_EQ(baseline_->get_tail(qid), binlog_->get_tail(qid));
  }

  void check_get(td::TQueue::QueueId qid, td::Random::Xorshift128plus &rnd, double now) {
    td::TQueue::Event a[10];
    td::MutableSpan<td::TQueue::Event> a_span(a, 10);
    td::TQueue::Event b[10];
    td::MutableSpan<td::TQueue::Event> b_span(b, 10);
    td::TQueue::Event c[10];
    td::MutableSpan<td::TQueue::Event> c_span(c, 10);

    auto a_from = baseline_->get_head(qid);
    //auto b_from = memory_->get_head(qid);
    //auto c_from = binlog_->get_head(qid);
    //ASSERT_EQ(a_from, b_from);
    //ASSERT_EQ(a_from, c_from);

    auto tmp = a_from.advance(rnd.fast(-10, 10));
    if (tmp.is_ok()) {
      a_from = tmp.move_as_ok();
    }
    baseline_->get(qid, a_from, true, now, a_span).move_as_ok();
    memory_->get(qid, a_from, true, now, b_span).move_as_ok();
    binlog_->get(qid, a_from, true, now, c_span).move_as_ok();
    ASSERT_EQ(a_span.size(), b_span.size());
    ASSERT_EQ(a_span.size(), c_span.size());
    for (size_t i = 0; i < a_span.size(); i++) {
      ASSERT_EQ(a_span[i].id, b_span[i].id);
      ASSERT_EQ(a_span[i].id, c_span[i].id);
      ASSERT_EQ(a_span[i].data, b_span[i].data);
      ASSERT_EQ(a_span[i].data, c_span[i].data);
    }
  }

 private:
  td::unique_ptr<td::TQueue> baseline_;
  td::unique_ptr<td::TQueue> memory_;
  td::unique_ptr<td::TQueue> binlog_;
  td::TQueueMemoryStorage *memory_storage_{nullptr};
};

TEST(TQueue, random) {
  using EventId = td::TQueue::EventId;
  td::Random::Xorshift128plus rnd(123);
  auto next_qid = [&] {
    return rnd.fast(1, 10);
  };
  auto next_first_id = [&] {
    return EventId::from_int32(EventId::MAX_ID - 20).move_as_ok();
    //if (rnd.fast(0, 3) == 0) {
    //return EventId::from_int32(EventId::MAX_ID - 20).move_as_ok();
    //}
    //return EventId::from_int32(rnd.fast(1000000000, 1500000000)).move_as_ok();
  };
  TestTQueue q;
  double now = 0;
  auto push_event = [&] {
    auto data = PSTRING() << rnd();
    q.push(next_qid(), data, now + rnd.fast(-10, 10) * 10 + 5, next_first_id());
  };
  auto inc_now = [&] {
    now += 10;
  };
  auto check_head_tail = [&] {
    q.check_head_tail(next_qid(), now);
  };
  auto restart = [&] {
    q.restart(rnd, now);
  };
  auto get = [&] {
    q.check_get(next_qid(), rnd, now);
  };
  td::RandomSteps steps({{push_event, 100}, {check_head_tail, 10}, {get, 40}, {inc_now, 5}, {restart, 1}});
  for (int i = 0; i < 100000; i++) {
    steps.step(rnd);
  }
}
