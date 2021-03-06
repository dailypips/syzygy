// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SYZYGY_AGENT_ASAN_STACK_CAPTURE_CACHE_H_
#define SYZYGY_AGENT_ASAN_STACK_CAPTURE_CACHE_H_

#include <unordered_map>

#include "base/observer_list.h"
#include "base/synchronization/lock.h"
#include "syzygy/agent/asan/shadow.h"
#include "syzygy/agent/common/stack_capture.h"
#include "syzygy/common/asan_parameters.h"

namespace agent {
namespace asan {

// Forward declarations.
class AsanLogger;
class MemoryNotifierInterface;

// A class which manages a thread-safe cache of unique stack traces, by ID.
class StackCaptureCache {
 public:
  // The size of a page of stack captures, in bytes. This should be in the
  // hundreds of KB or low MBs so that we have an efficient pooled allocator
  // that can store hundreds to thousands of stack captures, yet whose
  // incremental growth is not too large.
  static const size_t kCachePageSize = 1024 * 1024;

  // The type used to uniquely identify a stack.
  typedef common::StackCapture::StackId StackId;

  // Forward declaration.
  class CachePage;

  // TODO(chrisha): Plumb a command-line parameter through to control the
  //     max depth of stack traces in the StackCaptureCache. This should get us
  //     significant memory savings in the stack trace cache.

  // Initializes a new stack capture cache.
  // @param logger The logger to use.
  // @param memory_notifier The memory notifier to use.
  // @param max_num_frames The maximum number of frames to be used by the
  //     StackCapture objects in this cache.
  StackCaptureCache(AsanLogger* logger,
                    MemoryNotifierInterface* memory_notifier);
  StackCaptureCache(AsanLogger* logger,
                    MemoryNotifierInterface* memory_notifier,
                    size_t max_num_frames);

  // Destroys a stack capture cache.
  ~StackCaptureCache();

  // Static initialisation of StackCaptureCache context.
  static void Init();

  // @returns the current maximum number of frames supported by saved stack
  //     traces.
  size_t max_num_frames() const { return max_num_frames_; }

  // Sets the current maximum number of frames supported by saved stack traces.
  // @param max_num_frames The maximum number of frames to set.
  void set_max_num_frames(size_t max_num_frames) {
    max_num_frames_ = max_num_frames;
  }

  // @returns the default compression reporting period value.
  static size_t GetDefaultCompressionReportingPeriod() {
    return ::common::kDefaultReportingPeriod;
  }

  // Sets a new (global) compression reporting period value. Note that this
  // method is not thread safe. It is expected to be called once at startup,
  // or not at all.
  static void set_compression_reporting_period(size_t period) {
    compression_reporting_period_ = period;
  }

  // @returns the current (global) compression reporting period value. It is
  //     expected that this value is a constant after initialization.
  static size_t compression_reporting_period() {
    return compression_reporting_period_;
  }

  // Save (or retrieve) the stack capture into the cache using its
  // absolute_stack_id as the key.
  // @param stack_capture The initialized stack capture to save.
  // @returns a pointer to the saved stack capture.
  const common::StackCapture* SaveStackTrace(
      const common::StackCapture& stack_capture);

  // Releases a previously referenced stack trace. This decrements the reference
  // count and potentially cleans up the stack trace.
  // @param stack_capture The stack capture to be released.
  void ReleaseStackTrace(const common::StackCapture* stack_capture);

  // Logs the current stack capture cache statistics. This method is thread
  // safe.
  void LogStatistics();

  // Checks if a StackCapture pointer seems to be valid. This only ensure that
  // it point into a CachePage.
  // @param stack_capture The pointer that we want to check.
  // @returns true if the pointer is valid, false otherwise.
  bool StackCapturePointerIsValid(const common::StackCapture* stack_capture);

  // Observer that is notified when a new stack is saved.
  class Observer {
   public:
    virtual void OnNewStack(common::StackCapture* new_stack) = 0;
  };
  // Adds an observer for this class. An observer should not be added more
  // than once. The caller retains the ownership of the observer object.
  // @param obs the observer to add.
  void AddObserver(Observer* obs);
  // Removes an observer.
  // @param obs the observer to remove.
  void RemoveObserver(Observer* obs);

 protected:
  // The container type in which we store the cached stacks. This enforces
  // uniqueness based on their hash value, nothing more.
  typedef std::unordered_map<StackId, common::StackCapture*> StackMap;

  // Used for shuttling around statistics about this cache.
  struct Statistics {
    // The total number of stacks currently in the cache.
    size_t cached;
    // The current total size of the stack cache, in bytes.
    size_t size;
    // The total number of reference-saturated stack captures. These will never
    // be able to be removed from the cache.
    size_t saturated;
    // The number of currently unreferenced stack captures. These are pending
    // cleanup.
    size_t unreferenced;

    // We use 64-bit integers for the following because they can overflow a
    // 32-bit value for long running processes.

    // These count information about stack captures.
    // @{
    // The total number of stacks requested over the lifetime of the stack
    // cache.
    uint64_t requested;
    // The total number of stacks that have had to be allocated. This is not
    // necessarily the same as |cached| as the stack cache can reclaim
    // unreferenced stacks.
    uint64_t allocated;
    // The total number of active references to stack captures.
    uint64_t references;
    // @}

    // These count information about individual frames.
    // @{
    // The total number of frames across all active stack captures. This is used
    // for calculating our compression ratio. This double counts actually stored
    // frames by the number of times they are referenced.
    uint64_t frames_stored;
    // The total number of frames that are physically stored across all active
    // stack captures. This does not double count multiply-referenced captures.
    uint64_t frames_alive;
    // The total number of frames in unreferenced stack captures. This is used
    // to figure out how much of our cache is actually dead.
    uint64_t frames_dead;
    // @}
  };

  // Allocates a CachePage.
  void AllocateCachePage();

  // Gets the current cache statistics. This must be called under lock_.
  // @param statistics Will be populated with current cache statistics.
  void GetStatisticsUnlocked(Statistics* statistics) const;

  // Implementation function for logging statistics.
  // @param report The statistics to be reported.
  void LogStatisticsImpl(const Statistics& statistics) const;

  // Grabs a temporary StackCapture from reclaimed_ or the current CachePage.
  // Must be called under lock_. Takes care of updating frames_dead.
  // @param num_frames The minimum number of frames that are required.
  common::StackCapture* GetStackCapture(size_t num_frames);

  // Links a stack capture into the reclaimed_ list. Meant to be called by
  // ReturnStackCapture only. Must be called under lock_. Takes care of
  // updating frames_dead (on behalf of ReturnStackCapture).
  // @param stack_capture The stack capture to be linked into reclaimed_.
  void AddStackCaptureToReclaimedList(common::StackCapture* stack_capture);

  // The default number of known stacks sets that we keep.
  static const size_t kKnownStacksSharding = 16;

  // The number of allocations between reports of the stack trace cache
  // compression ratio. Zero (0) means do not report. Values like 1 million
  // seem to be pretty good with Chrome.
  static size_t compression_reporting_period_;

  // Logger instance to which to report the compression ratio.
  AsanLogger* const logger_;

  // The memory notifier that is informed of allocations made by the cache.
  MemoryNotifierInterface* memory_notifier_;

  // Locks to protect the known stacks sets from concurrent access.
  mutable base::Lock known_stacks_locks_[kKnownStacksSharding];

  // The max depth of the stack traces to allocate. This can change, but it
  // doesn't really make sense to do so.
  size_t max_num_frames_;

  // The maps of known stacks. Accessed under known_stacks_locks_.
  StackMap known_stacks_[kKnownStacksSharding];

  // A lock protecting access to current_page_.
  base::Lock current_page_lock_;

  // The current page from which new stack captures are allocated.
  // Accessed under current_page_lock_.
  CachePage* current_page_;

  // A lock protecting access to statistics_.
  mutable base::Lock stats_lock_;

  // Aggregate statistics about the cache. Accessed under stats_lock_.
  Statistics statistics_;

  // Locks to protect each reclaimed list from concurrent access.
  base::Lock reclaimed_locks_[common::StackCapture::kMaxNumFrames + 1];

  // StackCaptures that have been reclaimed for reuse are stored in a link list
  // according to their length. We reuse the first frame in the stack capture
  // as a pointer to the next StackCapture of that size, if there is one.
  // Accessed under reclaimed_locks_.
  common::StackCapture* reclaimed_[common::StackCapture::kMaxNumFrames + 1];

  // The list of observers.
  base::ObserverList<Observer> observer_list_;

 private:
  DISALLOW_COPY_AND_ASSIGN(StackCaptureCache);
};

// A page of preallocated stack trace capture objects to be populated
// and stored in the known stacks cache set.
class StackCaptureCache::CachePage {
 public:
  // Placement-new factory. This is strictly a "bring your own memory"
  // class.
  // @param alloc The allocation to use. Must be the appropriate size.
  static CachePage* CreateInPlace(void* alloc, CachePage* link);

  // Allocates a stack capture from this cache page if possible.
  // @param max_num_frames The maximum number of frames the object needs to be
  //     able to store.
  // @param metadata_size The number of bytes to reserve for metadata. These
  //     bytes will be reserved *after* the StackCapture object and zero
  //     initialized. Defaults to zero if not provided.
  // @returns a new StackCapture, or nullptr if the page is full.
  common::StackCapture* GetNextStackCapture(size_t max_num_frames,
                                            size_t metadata_size);
  common::StackCapture* GetNextStackCapture(size_t max_num_frames);

  // Returns the most recently allocated stack capture back to the page.
  // @param stack_capture The stack capture to return.
  // @param metadata_size The number of bytes of metadata that was also
  //     allocated.
  // @returns false if the provided stack capture was not the most recently
  //     allocated one, true otherwise.
  bool ReturnStackCapture(common::StackCapture* stack_capture,
                          size_t metadata_size);
  bool ReturnStackCapture(common::StackCapture* stack_capture);

  // @returns the number of bytes used in this page. This is mainly a hook
  //     for unittesting.
  size_t bytes_used() const { return bytes_used_; }

  // @returns the number of bytes left in this page.
  size_t bytes_left() const { return kDataSize - bytes_used_; }

  // @returns a pointer to the beginning of the stack captures.
  uint8_t* data() { return data_; }

  // @returns the size of the data.
  size_t data_size() { return kDataSize; }

 protected:
  // These are protected so that we can't accidentally allocate pages directly.
  // These are meant to be placement-new initialized with allocations served
  // directly from VirtualAlloc.
  explicit CachePage(CachePage* link) : next_page_(link), bytes_used_(0) {}
  ~CachePage();

  // The parent StackCaptureCache is responsible for cleaning up the linked list
  // of cache pages, thus needs access to our internals.
  friend StackCaptureCache;

  // The cache pages from a linked list, which allows for easy cleanup
  // when the cache is destroyed.
  CachePage* next_page_;

  // The number of bytes used, also equal to the byte offset of the next
  // StackCapture object to be allocated.
  size_t bytes_used_;

  // A page's worth of data, which will be allocated as StackCapture objects.
  // NOTE: Using offsetof would be ideal, but we can't do that on an incomplete
  //       type. Thus, this needs to be maintained.
  static const size_t kDataSize = kCachePageSize - sizeof(CachePage*)
      - sizeof(size_t);
  static_assert(kDataSize < kCachePageSize,
                "kCachePageSize must be big enough for CachePage header.");
  uint8_t data_[kDataSize];

 private:
  DISALLOW_COPY_AND_ASSIGN(CachePage);
};
static_assert(sizeof(StackCaptureCache::CachePage) ==
                  StackCaptureCache::kCachePageSize,
              "kDataSize calculation needs to be updated.");
static_assert(StackCaptureCache::kCachePageSize % 4096 == 0,
              "kCachePageSize should be a multiple of the page size.");

}  // namespace asan
}  // namespace agent

#endif  // SYZYGY_AGENT_ASAN_STACK_CAPTURE_CACHE_H_
