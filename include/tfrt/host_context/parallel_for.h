/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- parallel_for.h -------------------------------------------*- C++ -*-===//
//
// Parallel algorithms for the HostContext.
//
//===----------------------------------------------------------------------===//

#ifndef TFRT_SUPPORT_PARALLEL_FOR_H_
#define TFRT_SUPPORT_PARALLEL_FOR_H_

#include <cstddef>

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/STLExtras.h"
#include "tfrt/host_context/host_context.h"
#include "tfrt/support/forward_decls.h"
#include "tfrt/support/mutex.h"
#include "tfrt/support/thread_annotations.h"

namespace tfrt {
class HostContext;

class ParallelFor {
 public:
  explicit ParallelFor(HostContext* host) : host_(host) {}

  //===--------------------------------------------------------------------===//
  // BlockSizes configures how a range is split into parallely executed blocks.
  //===--------------------------------------------------------------------===//
  class BlockSizes {
   public:
    // Splits a range into a fixed size blocks.
    static BlockSizes Fixed(size_t n);
    // Splits range into a block sizes not smaller than `min`.
    static BlockSizes Min(size_t min);

   private:
    friend class ParallelFor;

    explicit BlockSizes(llvm::unique_function<size_t(size_t)> impl)
        : impl_(std::move(impl)) {}

    // Returns a parallel block size for a range of `total_size` and the
    // specified number of worker threads.
    size_t GetBlockSize(size_t num_worker_threads, size_t total_size) const;

    // Block sizes computation internally represented as a function from the
    // parallel for parameters to the block size. This is an internal detail,
    // a contract between ParallelFor and BlockSizes. Users of ParallelFor
    // must rely only on public static methods to choose block sizes policy.
    mutable llvm::unique_function<size_t(size_t)> impl_;
  };

  //===--------------------------------------------------------------------===//
  // Parallel for algorithms.
  //===--------------------------------------------------------------------===//

  // Executes `compute` in parallel for non-overlapping subranges [start, end)
  // in the [0, total_size) range. When all subtasks have completed, the
  // `on_done` callback will be called. Uses `block_sizes` to compute the
  // parallel block size.
  void Execute(size_t total_size, const BlockSizes& block_sizes,
               llvm::unique_function<void(size_t, size_t)> compute,
               llvm::unique_function<void()> on_done) const;

  // Execute implementation with a support for asynchronous compute function
  // completion. When all async values returned from subtasks are available,
  // calls `on_done` with their results.
  //
  // If the compute task submits any new subtasks to the same work queue, it is
  // unsafe to do a blocking wait (e.g. using latch) for their completion,
  // because it might lead to a thread pool exhaustion and dead locks. All tasks
  // completions must be communicated with async values.
  template <typename T, typename R>
  AsyncValueRef<R> Execute(
      size_t total_size, const BlockSizes& block_sizes,
      llvm::unique_function<AsyncValueRef<T>(size_t, size_t)> compute,
      llvm::unique_function<R(ArrayRef<AsyncValueRef<T>>)> on_done) const;

 private:
  HostContext* host_;  // must outlive all parallel operations in flight
};

template <typename T, typename R>
AsyncValueRef<R> ParallelFor::Execute(
    size_t total_size, const BlockSizes& block_sizes,
    llvm::unique_function<AsyncValueRef<T>(size_t, size_t)> compute,
    llvm::unique_function<R(ArrayRef<AsyncValueRef<T>>)> on_done) const {
  // Immediately return the result of `on_done` if nothing to execute.
  if (total_size == 0) return host_->MakeAvailableAsyncValueRef<R>(on_done({}));

  AsyncValueRef<R> result = host_->MakeUnconstructedAsyncValueRef<R>();

  using ComputeFn = llvm::unique_function<AsyncValueRef<T>(size_t, size_t)>;
  using DoneFn = llvm::unique_function<R(ArrayRef<AsyncValueRef<T>>)>;

  // Move functions passed in as arguments to the heap, together with
  // buffered block compute results.
  struct ExecuteContext {
    ExecuteContext(ComputeFn compute, DoneFn on_done, AsyncValueRef<R> result)
        : compute(std::move(compute)),
          on_done(std::move(on_done)),
          result(std::move(result)) {}

    llvm::SmallVector<AsyncValue*, 32> BlockResults() const {
      auto mapped = llvm::map_range(block_results, [](auto& async_value) {
        return async_value.GetAsyncValue();
      });
      return {mapped.begin(), mapped.end()};
    }

    ComputeFn compute;
    DoneFn on_done;
    AsyncValueRef<R> result;
    llvm::SmallVector<AsyncValueRef<T>, 32> block_results;
  };

  // ExecuteContext will be moved into the final call back, that will be
  // executed by RunWhenReady, and will be destroyed after the final value will
  // be emplaced into the `result`.
  auto ctx = std::make_unique<ExecuteContext>(
      std::move(compute), std::move(on_done), result.CopyRef());
  ExecuteContext* ctx_ptr = ctx.get();

  Execute(
      total_size, block_sizes,
      // -------------------------------------------------------------------- //
      // Launch block compute tasks.
      [ctx = ctx_ptr, mu = std::make_unique<mutex>()](size_t begin,
                                                      size_t end) -> void {
        AsyncValueRef<T> block_result = ctx->compute(begin, end);
        mutex_lock lock(*mu);
        ctx->block_results.emplace_back(std::move(block_result));
      },
      // -------------------------------------------------------------------- //
      // At this point all block compute tasks are launched, but not all of
      // their asynchronous results are completed. When all block results are
      // ready, call `on_done` function to compute a value for `result`.
      [host = host_, ctx = std::move(ctx)]() mutable -> void {
        host->RunWhenReady(ctx->BlockResults(), [host, ctx = std::move(ctx)]() {
          R result = ctx->on_done(ctx->block_results);
          ctx->result.emplace(std::move(result));
        });
      });

  return result;
}

}  // namespace tfrt

#endif  // TFRT_SUPPORT_PARALLEL_FOR_H_
