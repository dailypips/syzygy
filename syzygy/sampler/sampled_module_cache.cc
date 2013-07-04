// Copyright 2013 Google Inc. All Rights Reserved.
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

#include "syzygy/sampler/sampled_module_cache.h"

#include <psapi.h>

#include "base/stringprintf.h"
#include "sawbuck/common/com_utils.h"
#include "syzygy/common/align.h"
#include "syzygy/pe/pe_file.h"

namespace sampler {

namespace {

typedef SampledModuleCache::Process::ModuleMap ModuleMap;

}  // namespace

SampledModuleCache::SampledModuleCache(size_t log2_bucket_size)
      : log2_bucket_size_(log2_bucket_size) {
  DCHECK_LE(2u, log2_bucket_size);
  DCHECK_GE(31u, log2_bucket_size);
}

SampledModuleCache::~SampledModuleCache() {
  // Force a clean up of all modules (and consequently all processes).
  MarkAllModulesDead();
  RemoveDeadModules();
}

bool SampledModuleCache::AddModule(HANDLE process, HMODULE module) {
  DCHECK(process != INVALID_HANDLE_VALUE);

  // Create or find the process object. We don't actually insert it into the
  // map until everything has succeeded, saving us the cleanup on failure.
  DWORD pid = ::GetProcessId(process);
  scoped_ptr<Process> scoped_proc;
  Process* proc = NULL;
  ProcessMap::iterator proc_it = processes_.find(pid);
  if (proc_it == processes_.end()) {
    HANDLE temp_handle = INVALID_HANDLE_VALUE;
    if (!::DuplicateHandle(::GetCurrentProcess(), process,
                           ::GetCurrentProcess(), &temp_handle,
                           0, FALSE, DUPLICATE_SAME_ACCESS) ||
        temp_handle == INVALID_HANDLE_VALUE) {
      DWORD error = ::GetLastError();
      LOG(ERROR) << "Failed to duplicate handle to process " << pid << ": "
                 << com::LogWe(error);
      return false;
    }

    scoped_proc.reset(new Process(temp_handle, pid));
    proc = scoped_proc.get();
  } else {
    proc = proc_it->second;
  }
  DCHECK(proc != NULL);

  if (!proc->AddModule(module, log2_bucket_size_))
    return false;

  if (scoped_proc.get() != NULL) {
    // Initialization was successful so we can safely insert the newly created
    // process into the map.
    processes_.insert(std::make_pair(pid, proc));
    scoped_proc.release();
  }

  return true;
}

void SampledModuleCache::MarkAllModulesDead() {
  for (ProcessMap::iterator proc_it = processes_.begin();
       proc_it != processes_.end(); ++proc_it) {
    proc_it->second->MarkDead();
  }
}

void SampledModuleCache::RemoveDeadModules() {
  if (processes_.empty())
    return;

  ProcessMap::iterator proc_it = processes_.begin();
  ProcessMap::iterator proc_it_next = proc_it;
  ++proc_it_next;

  while (proc_it != processes_.end()) {
    // Remove any dead modules from the process.
    proc_it->second->RemoveDeadModules(dead_module_callback_);

    // If the process itself is dead (contains no more profiling modules) then
    // remove it.
    if (!proc_it->second->alive()) {
      Process* proc = proc_it->second;
      delete proc;
      processes_.erase(proc_it);
    }

    proc_it = proc_it_next;
    if (proc_it_next != processes_.end())
      ++proc_it_next;
  }
}

SampledModuleCache::Process::Process(HANDLE process, DWORD pid)
    : process_(process), pid_(pid), alive_(true) {
  DCHECK(process != INVALID_HANDLE_VALUE);
}

SampledModuleCache::Process::~Process() {
  MarkDead();
  RemoveDeadModules(DeadModuleCallback());
}

bool SampledModuleCache::Process::AddModule(HMODULE module,
                                            size_t log2_bucket_size) {
  DCHECK(module != INVALID_HANDLE_VALUE);
  DCHECK_LE(2u, log2_bucket_size);
  DCHECK_GE(31u, log2_bucket_size);

  ModuleMap::iterator mod_it = modules_.find(module);
  if (mod_it != modules_.end()) {
    // The module is already being profiled. Simply mark it as being alive.
    mod_it->second->MarkAlive();

    // And mark ourselves as being alive while we're at it.
    MarkAlive();

    return true;
  }

  // Create a new module object. We don't actually insert it into the map until
  // everything has succeeded, saving us the cleanup on failure.
  scoped_ptr<Module> mod(new Module(this, module, log2_bucket_size));

  if (!mod->Init())
    return false;

  if (!mod->Start())
    return false;

  // Initialization was successful so we can safely insert the initialized
  // (and currently profiling) module into the map.
  modules_.insert(std::make_pair(module, mod.release()));

  return true;
}

void SampledModuleCache::Process::MarkDead() {
  // Mark all of our children as dead, and ourselves.
  alive_ = false;
  for (ModuleMap::iterator it = modules_.begin(); it != modules_.end(); ++it)
    it->second->MarkDead();
}

void SampledModuleCache::Process::RemoveDeadModules(
    DeadModuleCallback callback) {
  if (modules_.empty())
    return;

  ModuleMap::iterator mod_it = modules_.begin();
  ModuleMap::iterator mod_it_next = mod_it;
  ++mod_it_next;

  while (mod_it != modules_.end()) {
    DCHECK(mod_it->second != NULL);

    if (!mod_it->second->alive()) {
      // Stop profiling.
      mod_it->second->Stop();

      // Return the results to the callback if one has been provided.
      if (!callback.is_null())
        callback.Run(mod_it->second);

      // And clean things up.
      Module* mod = mod_it->second;
      delete mod;
      modules_.erase(mod_it);
    }

    mod_it = mod_it_next;
    if (mod_it_next != modules_.end())
      ++mod_it_next;
  }
}

SampledModuleCache::Module::Module(Process* process,
                                   HMODULE module,
                                   size_t log2_bucket_size)
    : process_(process),
      module_(module),
      module_size_(0),
      module_checksum_(0),
      module_time_date_stamp_(0),
      buckets_begin_(NULL),
      buckets_end_(NULL),
      log2_bucket_size_(log2_bucket_size),
      profiling_start_time_(0),
      alive_(true) {
  DCHECK(process != NULL);
  DCHECK(module_ != INVALID_HANDLE_VALUE);
  DCHECK_LE(2u, log2_bucket_size);
  DCHECK_GE(31u, log2_bucket_size);
}

bool SampledModuleCache::Module::Init() {
  // Read the headers.
  char headers[4096] = {};
  size_t net_bytes_read = 0;
  size_t empty_reads = 0;
  while (net_bytes_read < sizeof(headers)) {
    SIZE_T bytes_read = 0;
    if (::ReadProcessMemory(process_->process(),
                            module_,
                            headers + net_bytes_read,
                            sizeof(headers) - net_bytes_read,
                            &bytes_read) == FALSE) {
      DWORD error = ::GetLastError();
      LOG(ERROR) << "ReadProcessMemory failed for module at address "
                 << base::StringPrintf("0x%08X", module_)
                 << " of process " << process_->pid() << ": "
                 << com::LogWe(error);
      return false;
    }
    if (bytes_read == 0) {
      if (++empty_reads == 3) {
        LOG(ERROR) << "ReadProcessMemory unable to read headers for module at "
                   << "address " << base::StringPrintf("0x%08X", module_)
                   << " of process " << process_->pid() << ".";
        return false;
      }
    } else {
      net_bytes_read += bytes_read;
      empty_reads = 0;
    }
  }

  const IMAGE_DOS_HEADER* dos_header =
      reinterpret_cast<const IMAGE_DOS_HEADER*>(headers);
  COMPILE_ASSERT(sizeof(IMAGE_DOS_HEADER) <= sizeof(headers),
                 headers_must_be_big_enough_for_DOS_headers);

  // Get the NT headers and make sure they're fully contained in the block we
  // read.
  if (dos_header->e_lfanew > sizeof(headers))
    return false;
  const IMAGE_NT_HEADERS* nt_headers =
      reinterpret_cast<const IMAGE_NT_HEADERS*>(headers + dos_header->e_lfanew);
  if (reinterpret_cast<const char*>(nt_headers + 1) - headers > sizeof(headers))
    return false;

  // Get the section headers and make sure they're fully contained in the
  // block we read.
  size_t section_count = nt_headers->FileHeader.NumberOfSections;
  const IMAGE_SECTION_HEADER* section_headers =
      reinterpret_cast<const IMAGE_SECTION_HEADER*>(nt_headers + 1);
  if (reinterpret_cast<const char*>(section_headers + section_count) - headers >
      sizeof(headers)) {
    return false;
  }

  module_size_ = nt_headers->OptionalHeader.SizeOfImage;
  module_checksum_ = nt_headers->OptionalHeader.CheckSum;
  module_time_date_stamp_ = nt_headers->FileHeader.TimeDateStamp;

  // Find the RVA range associated with any text segments in the module.
  DWORD text_begin = ~0;
  DWORD text_end = 0;
  for (size_t i = 0; i < section_count; ++i) {
    const IMAGE_SECTION_HEADER& sh = section_headers[i];
    static const DWORD kExecFlags = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
    if ((sh.Characteristics & kExecFlags) == 0)
      continue;

    DWORD sec_begin = sh.VirtualAddress;
    DWORD sec_end = sec_begin + sh.Misc.VirtualSize;
    if (sec_begin < text_begin)
      text_begin = sec_begin;
    if (sec_end > text_end)
      text_end = sec_end;
  }

  // Adjust the address range for the bucket size.
  DWORD bucket_size = 1 << log2_bucket_size_;
  text_begin = (text_begin / bucket_size) * bucket_size;
  text_end = ((text_end + bucket_size - 1) / bucket_size) * bucket_size;

  // Calculate the number of buckets.
  DCHECK_EQ(0u, (text_end - text_begin) % bucket_size);
  DWORD bucket_count = (text_end - text_begin) / bucket_size;

  // Calculate the bucket range in the remote address space.
  buckets_begin_ = reinterpret_cast<const void*>(
      reinterpret_cast<const char*>(module_) + text_begin);
  buckets_end_ = reinterpret_cast<const void*>(
      reinterpret_cast<const char*>(module_) + text_end);

  // Initialize the profiler.
  if (!profiler_.Initialize(process_->process(),
                            const_cast<void*>(buckets_begin_),
                            text_end - text_begin,
                            log2_bucket_size_)) {
    LOG(ERROR) << "Failed to initialize profiler for address range "
               << base::StringPrintf("0x%08X - 0x%08X",
                                     buckets_begin_,
                                     buckets_end_)
               << " of process " << process_->pid() << ".";
    return false;
  }
  DCHECK_EQ(bucket_count, profiler_.buckets().size());

  return true;
}

}  // namespace sampler
