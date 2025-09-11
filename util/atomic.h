//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <atomic>

#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

// Background:
// std::atomic is somewhat easy to misuse:
// * Implicit conversion to T using std::memory_order_seq_cst, along with
// memory order parameter defaults, make it easy to accidentally mix sequential
// consistency ordering with acquire/release memory ordering. See
// "The single total order might not be consistent with happens-before" at
// https://en.cppreference.com/w/cpp/atomic/memory_order
// * It's easy to use nonsensical (UB) combinations like store with
// std::memory_order_acquire.
// * It is unlikely that anything in RocksDB will need std::memory_order_seq_cst
// because sequential consistency for the user, potentially writing from
// multiple threads, is provided by explicit versioning with sequence numbers.
// If threads A & B update separate atomics, it's typically OK if threads C & D
// see those updates in different orders.
//
// For such reasons, we provide wrappers below to make safe usage easier.

// Wrapper around std::atomic to avoid certain bugs (see Background above).
//
// This relaxed-only wrapper is intended for atomics that do not need
// ordering constraints with other data reads/writes aside from those
// necessary for computing data values or given by other happens-before
// relationships. For example, a cross-thread counter that never returns
// the same result can be a RelaxedAtomic.
// RelaxedAtomic和AcqReqlAtomic这两个类是Rocksdb对C++标准库std::atomic的封装
// 它们的主要作用是明确地指定原子操作的内存序（Memory Order），从而在保证线程安全的同事，尽可能优化性能
// 
// 封装了std::atomic<T>, 并强制所有操作都使用std::memory_order_relaxed内存序
// 这是最弱的内存序，只保证原子操作本身的原子性（即读写一个变量是不可分割的），但不保证任何其他内存操作的顺序
// 这意味着编译器和处理器可以自由地重排RelaxedAtomic操作前后的非原子内存访问
// 使用场景：如果你只需要保证一个变量的读写是原子的，而不需要关心这个变量的读写与其他变量的读写顺序时
// 可以使用RelaxedAtomic，例如简单的计数器、统计信息，或者在某个线程中设置一个标志，而其他线程只是周期性检查
// 这个标志，且不依赖于标志设置前其他内存操作的可见性，它提供了最高的性能，因为对编译器和处理器的限制最少。
//
// c++11所规定的这6种模式，其实并不是限制（或者规定）两个线程该怎样同步执行，而是在规定一个线程内的指令应该怎样执行
// 更为准确地说，是在讨论单线程内的指令执行顺序对于多线程的影响的问题
// 
// 1. 什么是原子操作：原子操作就是对于一个内存上变量的读取-变更-存储(load-add-store)作为一个整体一次完成
// atom本身就是一种锁，它自己就已经完成了线程间同步的问题，这里并没有那6个memory order什么事情
// 问题在于以这个原子操作为中心，其前后的代码，这些代码并不一定是需要
//
//
//  rocksdb封装主要是因为以下几个原因：
// 1. 明确意图和可行性：通过RelaxedAtomic和AcqRelAtomic，可以明确地表达出代码的意图和可行性
// 2. 避免误用：std::atomic在使用时容易出现误用，例如使用不恰当的内存序，或者使用不恰当的操作
// 3. 提高可读性：通过RelaxedAtomic和AcqRelAtomic，可以提高代码的可读性，使得代码更加清晰易懂
// 4. 便于维护：通过RelaxedAtomic和AcqRelAtomic，可以便于代码的维护，使得代码更加易于维护
template <typename T>
class RelaxedAtomic {
 public:
  explicit RelaxedAtomic(T initial = {}) : v_(initial) {}
  void StoreRelaxed(T desired) { v_.store(desired, std::memory_order_relaxed); }
  T LoadRelaxed() const { return v_.load(std::memory_order_relaxed); }
  bool CasWeakRelaxed(T& expected, T desired) {
    return v_.compare_exchange_weak(expected, desired,
                                    std::memory_order_relaxed);
  }
  bool CasStrongRelaxed(T& expected, T desired) {
    return v_.compare_exchange_strong(expected, desired,
                                      std::memory_order_relaxed);
  }
  T ExchangeRelaxed(T desired) {
    return v_.exchange(desired, std::memory_order_relaxed);
  }
  T FetchAddRelaxed(T operand) {
    return v_.fetch_add(operand, std::memory_order_relaxed);
  }
  T FetchSubRelaxed(T operand) {
    return v_.fetch_sub(operand, std::memory_order_relaxed);
  }
  T FetchAndRelaxed(T operand) {
    return v_.fetch_and(operand, std::memory_order_relaxed);
  }
  T FetchOrRelaxed(T operand) {
    return v_.fetch_or(operand, std::memory_order_relaxed);
  }
  T FetchXorRelaxed(T operand) {
    return v_.fetch_xor(operand, std::memory_order_relaxed);
  }

 protected:
  std::atomic<T> v_;
};
// compare_exchange_strong和compare_exchange_weak都是c++ std::atomic类型提供的原子性
// 比较并交换（Compare-And-Swap CAS）操作，核心功能都是相同的：尝试将原子变量的当前值与一个期望值
// 进行比较，如果相等，则原子性将原子变量的值更新为新值
//
// 主要区别在于是否允许“虚假失败”（Spurious Failure）
// 
// compare_exchange_strong
// 保证：如果原子变量的当前值确实等于你提供的expected值，那么compare_exchange_strong保证会成功（返回true），
// 除非有其他线程在同一时刻修改了该原子变量
// 无虚假失败：它不会在当前值等于期望值的情况下，无缘无故返回false
// 开销：在某些硬件架构上，为了避免虚假失败，_strong的实现可能需要额外的开销（例如，内部的自旋或重试机制）
// 使用场景：当希望CAS操作在条件满足时尽可能成功，并且不希望处理虚假失败时，例如，在非循环的单次CAS操作中，
// 或者在对性能要求不是极致敏感的场景。
//
// compare_exchange_weak
// * 允许虚假失败
// * 虚假失败通常发生在多处理器系统上，由于底层硬件或操作系统调度的一些细微时序问题，导致CAS操作在逻辑上应该成功
// 但实际上失败了，例如在比较和交换之前，另一个处理器可能短暂访问了内存位置，即使它没有改变值
//
// 通常在某些硬件架构上，实现_weak的开销更低，因为它不需要额外的机制来避免虚假失败
// 使用场景：主要用于循环中，当在循环中反复尝试CAS操作时，即使发生虚假失败，循环也会再次尝试，最终会成功
/*
do {
    expected_value = atomic_var.load);
} while (!atomic_var.compare_exchange_weak(expected_value, new_value));
*/
// 在这种循环模式下，虚假失败只是导致循环多执行一次，而不会影响正确性，由于其潜在的性能优势，在高性能的无锁算法中，
// _weak是首选。
/*
bool ThreadLocalPtr::StaticMeta::CompareAndSwap(uint32_t id, void* ptr, void*& expected) {
    return tls->entries[id].ptr.compare_exchange_strong(
      expected, ptr, std::memory_order_release, std::memory_order_relaxed);
}
*/
// 这里使用 _strong 可能是因为这个CompareAndSwap方法本身可能不是在一个紧密的自旋循环中调用的
// 或者开发者认为在这种特定场景下， _strong 的额外保证是值得的，或者在目标架构上性能差异不显著


// Wrapper around std::atomic to avoid certain bugs (see Background above).
//
// Except for some unusual cases requiring sequential consistency, this is
// a general-purpose atomic. Relaxed operations can be mixed in as appropriate.
template <typename T>
class AcqRelAtomic : public RelaxedAtomic<T> {
 public:
  explicit AcqRelAtomic(T initial = {}) : RelaxedAtomic<T>(initial) {}
  void Store(T desired) {
    RelaxedAtomic<T>::v_.store(desired, std::memory_order_release);
  }
  T Load() const {
    return RelaxedAtomic<T>::v_.load(std::memory_order_acquire);
  }
  bool CasWeak(T& expected, T desired) {
    return RelaxedAtomic<T>::v_.compare_exchange_weak(
        expected, desired, std::memory_order_acq_rel);
  }
  bool CasStrong(T& expected, T desired) {
    return RelaxedAtomic<T>::v_.compare_exchange_strong(
        expected, desired, std::memory_order_acq_rel);
  }
  T Exchange(T desired) {
    return RelaxedAtomic<T>::v_.exchange(desired, std::memory_order_acq_rel);
  }
  T FetchAdd(T operand) {
    return RelaxedAtomic<T>::v_.fetch_add(operand, std::memory_order_acq_rel);
  }
  T FetchSub(T operand) {
    return RelaxedAtomic<T>::v_.fetch_sub(operand, std::memory_order_acq_rel);
  }
  T FetchAnd(T operand) {
    return RelaxedAtomic<T>::v_.fetch_and(operand, std::memory_order_acq_rel);
  }
  T FetchOr(T operand) {
    return RelaxedAtomic<T>::v_.fetch_or(operand, std::memory_order_acq_rel);
  }
  T FetchXor(T operand) {
    return RelaxedAtomic<T>::v_.fetch_xor(operand, std::memory_order_acq_rel);
  }
};

}  // namespace ROCKSDB_NAMESPACE
