//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "monitoring/instrumented_mutex.h"

#include "monitoring/perf_context_imp.h"
#include "monitoring/thread_status_util.h"
#include "rocksdb/system_clock.h"
#include "test_util/sync_point.h"

namespace ROCKSDB_NAMESPACE {
namespace {
#ifndef NPERF_CONTEXT
Statistics* stats_for_report(SystemClock* clock, Statistics* stats) {
  if (clock != nullptr && stats != nullptr &&
      stats->get_stats_level() > kExceptTimeForMutex) {
    return stats;
  } else {
    return nullptr;
  }
}
#endif  // NPERF_CONTEXT
}  // namespace

void InstrumentedMutex::Lock() {
  // 这个宏会创建一个临时的、RAII风格的计时器对象，当Lock函数被调用，这个计时器对象被创建，会记录下当前的精确时间start_time
  // 当LockInternal返回后，计时器对象因为生命周期结束而被销毁，析构函数会被调用，会再次获取当前时间end_time
  // 计算出时间差，这个时间差就是等待锁所花费的时间
  PERF_CONDITIONAL_TIMER_FOR_MUTEX_GUARD(
      db_mutex_lock_nanos, stats_code_ == DB_MUTEX_WAIT_MICROS,
      stats_for_report(clock_, stats_), stats_code_);
  LockInternal();
}

void InstrumentedMutex::LockInternal() {
#ifndef NDEBUG
  ThreadStatusUtil::TEST_StateDelay(ThreadStatus::STATE_MUTEX_WAIT);
#endif
// 这是一个内部测试和调试的，在并发代码的关键路径上，人为制造最坏情况，以暴露潜在的并发bug
// 在多线程编程中，很多bug（如死锁、竞态条件、优先级反转等）都和线程的执行时序Timing和交错Interleaving有关
// 在普通的测试环境下， 由于机器负载不高，线程可能很快获取到锁，或者总是以一种happy的顺序执行，使得bug极其难被复现
// 
// COERCE_CONTEXT_SWITCH就是为了解决这个问题，通过在获取锁的关键点上人为地、随机地引入线程休眠或主动放弃CPU,
// 来模拟一个极度繁忙、调度混乱的系统环境，是一种压力测试和混沌工程的手段，通过在关键代码路径上随机
// 人为创建出极端、恶劣的并发条件，来提高发现和复现并发bug的概率
#ifdef COERCE_CONTEXT_SWITCH
  if (stats_code_ == DB_MUTEX_WAIT_MICROS) {
    thread_local Random rnd(301);
    if (rnd.OneIn(2)) {
      if (bg_cv_) {
        bg_cv_->SignalAll();
      }
      sched_yield();
    } else {
      uint32_t sleep_us = rnd.Uniform(11) * 1000;
      if (bg_cv_) {
        bg_cv_->SignalAll();
      }
      SystemClock::Default()->SleepForMicroseconds(sleep_us);
    }
  }
#endif
  mutex_.Lock();
}

void InstrumentedCondVar::Wait() {
  PERF_CONDITIONAL_TIMER_FOR_MUTEX_GUARD(
      db_condition_wait_nanos, stats_code_ == DB_MUTEX_WAIT_MICROS,
      stats_for_report(clock_, stats_), stats_code_);
  WaitInternal();
}

void InstrumentedCondVar::WaitInternal() {
#ifndef NDEBUG
  ThreadStatusUtil::TEST_StateDelay(ThreadStatus::STATE_MUTEX_WAIT);
#endif
  cond_.Wait();
}

bool InstrumentedCondVar::TimedWait(uint64_t abs_time_us) {
  PERF_CONDITIONAL_TIMER_FOR_MUTEX_GUARD(
      db_condition_wait_nanos, stats_code_ == DB_MUTEX_WAIT_MICROS,
      stats_for_report(clock_, stats_), stats_code_);
  return TimedWaitInternal(abs_time_us);
}

bool InstrumentedCondVar::TimedWaitInternal(uint64_t abs_time_us) {
#ifndef NDEBUG
  ThreadStatusUtil::TEST_StateDelay(ThreadStatus::STATE_MUTEX_WAIT);
#endif

  TEST_SYNC_POINT_CALLBACK("InstrumentedCondVar::TimedWaitInternal",
                           &abs_time_us);

  return cond_.TimedWait(abs_time_us);
}

}  // namespace ROCKSDB_NAMESPACE
