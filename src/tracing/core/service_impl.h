/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACING_CORE_SERVICE_IMPL_H_
#define SRC_TRACING_CORE_SERVICE_IMPL_H_

#include <functional>
#include <map>
#include <memory>
#include <set>

#include "perfetto/base/page_allocator.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/service.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/trace_config.h"
#include "src/tracing/core/id_allocator.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

class Consumer;
class DataSourceConfig;
class Producer;
class SharedMemory;
class TraceConfig;

// The tracing service business logic.
class ServiceImpl : public Service {
 public:
  // The implementation behind the service endpoint exposed to each producer.
  class ProducerEndpointImpl : public Service::ProducerEndpoint {
   public:
    ProducerEndpointImpl(ProducerID,
                         ServiceImpl*,
                         base::TaskRunner*,
                         Producer*,
                         std::unique_ptr<SharedMemory>);
    ~ProducerEndpointImpl() override;

    Producer* producer() const { return producer_; }
    ProducerID id() const { return id_; }

    // Service::ProducerEndpoint implementation.
    void RegisterDataSource(const DataSourceDescriptor&,
                            RegisterDataSourceCallback) override;
    void UnregisterDataSource(DataSourceID) override;
    void NotifySharedMemoryUpdate(
        const std::vector<uint32_t>& changed_pages) override;
    std::unique_ptr<TraceWriter> CreateTraceWriter(BufferID) override;
    SharedMemory* shared_memory() const override;

   private:
    ProducerEndpointImpl(const ProducerEndpointImpl&) = delete;
    ProducerEndpointImpl& operator=(const ProducerEndpointImpl&) = delete;

    ProducerID const id_;
    ServiceImpl* const service_;
    base::TaskRunner* const task_runner_;
    Producer* producer_;
    std::unique_ptr<SharedMemory> shared_memory_;
    SharedMemoryABI shmem_abi_;
    DataSourceID last_data_source_id_ = 0;
  };

  // The implementation behind the service endpoint exposed to each consumer.
  class ConsumerEndpointImpl : public Service::ConsumerEndpoint {
   public:
    ConsumerEndpointImpl(ServiceImpl*, base::TaskRunner*, Consumer*);
    ~ConsumerEndpointImpl() override;

    Consumer* consumer() const { return consumer_; }
    base::WeakPtr<ConsumerEndpointImpl> GetWeakPtr();

    // Service::ConsumerEndpoint implementation.
    void EnableTracing(const TraceConfig&) override;
    void DisableTracing() override;
    void ReadBuffers() override;
    void FreeBuffers() override;

   private:
    ConsumerEndpointImpl(const ConsumerEndpointImpl&) = delete;
    ConsumerEndpointImpl& operator=(const ConsumerEndpointImpl&) = delete;

    ServiceImpl* const service_;
    Consumer* const consumer_;
    base::WeakPtrFactory<ConsumerEndpointImpl> weak_ptr_factory_;
  };

  explicit ServiceImpl(std::unique_ptr<SharedMemory::Factory>,
                       base::TaskRunner*);
  ~ServiceImpl() override;

  // Called by ProducerEndpointImpl.
  void DisconnectProducer(ProducerID);
  void RegisterDataSource(ProducerID,
                          DataSourceID,
                          const DataSourceDescriptor&);
  void CopyProducerPageIntoLogBuffer(ProducerID,
                                     BufferID,
                                     const uint8_t*,
                                     size_t);

  // Called by ConsumerEndpointImpl.
  void DisconnectConsumer(ConsumerEndpointImpl*);
  void EnableTracing(ConsumerEndpointImpl*, const TraceConfig&);
  void DisableTracing(ConsumerEndpointImpl*);
  void ReadBuffers(ConsumerEndpointImpl*);
  void FreeBuffers(ConsumerEndpointImpl*);

  // Service implementation.
  std::unique_ptr<Service::ProducerEndpoint> ConnectProducer(
      Producer*,
      size_t shared_buffer_size_hint_bytes = 0) override;

  std::unique_ptr<Service::ConsumerEndpoint> ConnectConsumer(
      Consumer*) override;

  // Exposed mainly for testing.
  size_t num_producers() const { return producers_.size(); }
  ProducerEndpointImpl* GetProducer(ProducerID) const;

 private:
  struct RegisteredDataSource {
    ProducerID producer_id;
    DataSourceID data_source_id;
    DataSourceDescriptor descriptor;
  };

  struct TraceBuffer {
    // TODO(primiano): make this configurable.
    static constexpr size_t kBufferPageSize = 4096;
    explicit TraceBuffer(size_t size);
    ~TraceBuffer();
    TraceBuffer(TraceBuffer&&) noexcept;
    TraceBuffer& operator=(TraceBuffer&&);

    bool is_valid() const { return !!data; }
    size_t num_pages() const { return size / kBufferPageSize; }

    uint8_t* get_page(size_t page) {
      PERFETTO_DCHECK(page < num_pages());
      return reinterpret_cast<uint8_t*>(data.get()) + page * kBufferPageSize;
    }

    uint8_t* get_next_page() {
      size_t cur = cur_page;
      cur_page = cur_page == num_pages() - 1 ? 0 : cur_page + 1;
      return get_page(cur);
    }

    size_t size;
    size_t cur_page = 0;  // Write pointer in the ring buffer.
    base::PageAllocator::UniquePtr data;

    // TODO(primiano): The TraceBuffer is not shared and there is no reason to
    // use the SharedMemoryABI. This is just a a temporary workaround to reuse
    // the convenience of SharedMemoryABI for bookkeeping of the buffer when
    // implementing ReadBuffers().
    std::unique_ptr<SharedMemoryABI> abi;
  };

  // Holds the state of a tracing session. A tracing session is uniquely bound
  // a specific Consumer. Each Consumer can own one or more sessions.
  struct TracingSession {
    TracingSession(const TraceConfig& new_config) : config(new_config) {}

    // List of data source instances that have been enabled on the various
    // producers for this tracing session.
    std::multimap<ProducerID, DataSourceInstanceID> data_source_instances;

    // The key of this map matches the |target_buffer| in the
    // SharedMemoryABI::ChunkHeader.
    std::map<BufferID, TraceBuffer> trace_buffers;

    TraceConfig config;
  };

  void CreateDataSourceInstanceForProducer(
      const TraceConfig::DataSource& cfg_data_source,
      ProducerEndpointImpl* producer,
      TracingSession* tracing_session);

  ServiceImpl(const ServiceImpl&) = delete;
  ServiceImpl& operator=(const ServiceImpl&) = delete;

  base::TaskRunner* const task_runner_;
  std::unique_ptr<SharedMemory::Factory> shm_factory_;
  ProducerID last_producer_id_ = 0;
  DataSourceInstanceID last_data_source_instance_id_ = 0;

  // Buffer IDs are global across all consumers (because a Producer can produce
  // data for more than one trace session, hence more than one consumer).
  IdAllocator buffer_ids_;

  std::multimap<std::string /*name*/, RegisteredDataSource> data_sources_;

  // TODO(primiano): There doesn't seem to be any good reason why |producers_|
  // is a map indexed by ID and not just a set<ProducerEndpointImpl*>.
  std::map<ProducerID, ProducerEndpointImpl*> producers_;

  std::set<ConsumerEndpointImpl*> consumers_;
  std::map<ConsumerEndpointImpl*, TracingSession> tracing_sessions_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_CORE_SERVICE_IMPL_H_