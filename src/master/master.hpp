// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __MASTER_HPP__
#define __MASTER_HPP__

#include <stdint.h>

#include <list>
#include <memory>
#include <string>
#include <vector>

#include <boost/circular_buffer.hpp>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <mesos/allocator/allocator.hpp>
#include <mesos/master/contender.hpp>
#include <mesos/master/detector.hpp>
#include <mesos/master/master.hpp>

#include <mesos/module/authenticator.hpp>

#include <mesos/quota/quota.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/limiter.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timer.hpp>

#include <process/metrics/counter.hpp>

#include <stout/cache.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/linkedhashmap.hpp>
#include <stout/multihashmap.hpp>
#include <stout/option.hpp>
#include <stout/recordio.hpp>
#include <stout/uuid.hpp>

#include "common/http.hpp"
#include "common/protobuf_utils.hpp"
#include "common/resources_utils.hpp"

#include "files/files.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "master/constants.hpp"
#include "master/flags.hpp"
#include "master/machine.hpp"
#include "master/metrics.hpp"
#include "master/registrar.hpp"
#include "master/validation.hpp"

#include "messages/messages.hpp"

namespace process {
class RateLimiter; // Forward declaration.
}

namespace mesos {

// Forward declarations.
class Authorizer;

namespace internal {

// Forward declarations.
namespace registry {
class Slaves;
}

class WhitelistWatcher;

namespace master {

class Master;
class SlaveObserver;

struct BoundedRateLimiter;
struct Framework;
struct Role;


struct Slave
{
  Slave(Master* const _master,
        const SlaveInfo& _info,
        const process::UPID& _pid,
        const MachineID& _machineId,
        const std::string& _version,
        const process::Time& _registeredTime,
        const Resources& _checkpointedResources,
        const std::vector<ExecutorInfo> executorInfos =
          std::vector<ExecutorInfo>(),
        const std::vector<Task> tasks =
          std::vector<Task>())
    : master(_master),
      id(_info.id()),
      info(_info),
      machineId(_machineId),
      pid(_pid),
      version(_version),
      registeredTime(_registeredTime),
      connected(true),
      active(true),
      checkpointedResources(_checkpointedResources),
      observer(nullptr)
  {
    CHECK(_info.has_id());

    Try<Resources> resources = applyCheckpointedResources(
        info.resources(),
        _checkpointedResources);

    // NOTE: This should be validated during slave recovery.
    CHECK_SOME(resources);
    totalResources = resources.get();

    foreach (const ExecutorInfo& executorInfo, executorInfos) {
      CHECK(executorInfo.has_framework_id());
      addExecutor(executorInfo.framework_id(), executorInfo);
    }

    foreach (const Task& task, tasks) {
      addTask(new Task(task));
    }
  }

  ~Slave() {}

  Task* getTask(const FrameworkID& frameworkId, const TaskID& taskId)
  {
    if (tasks.contains(frameworkId) && tasks[frameworkId].contains(taskId)) {
      return tasks[frameworkId][taskId];
    }
    return nullptr;
  }

  void addTask(Task* task);

  // Notification of task termination, for resource accounting.
  // TODO(bmahler): This is a hack for performance. We need to
  // maintain resource counters because computing task resources
  // functionally for all tasks is expensive, for now.
  void taskTerminated(Task* task)
  {
    const TaskID& taskId = task->task_id();
    const FrameworkID& frameworkId = task->framework_id();

    CHECK(protobuf::isTerminalState(task->state()));
    CHECK(tasks[frameworkId].contains(taskId))
      << "Unknown task " << taskId << " of framework " << frameworkId;

    usedResources[frameworkId] -= task->resources();
    if (!tasks.contains(frameworkId) && !executors.contains(frameworkId)) {
      usedResources.erase(frameworkId);
    }
  }

  void removeTask(Task* task)
  {
    const TaskID& taskId = task->task_id();
    const FrameworkID& frameworkId = task->framework_id();

    CHECK(tasks[frameworkId].contains(taskId))
      << "Unknown task " << taskId << " of framework " << frameworkId;

    if (!protobuf::isTerminalState(task->state())) {
      usedResources[frameworkId] -= task->resources();
      if (!tasks.contains(frameworkId) && !executors.contains(frameworkId)) {
        usedResources.erase(frameworkId);
      }
    }

    tasks[frameworkId].erase(taskId);
    if (tasks[frameworkId].empty()) {
      tasks.erase(frameworkId);
    }

    killedTasks.remove(frameworkId, taskId);
  }

  void addOffer(Offer* offer)
  {
    CHECK(!offers.contains(offer)) << "Duplicate offer " << offer->id();

    offers.insert(offer);
    offeredResources += offer->resources();
  }

  void removeOffer(Offer* offer)
  {
    CHECK(offers.contains(offer)) << "Unknown offer " << offer->id();

    offeredResources -= offer->resources();
    offers.erase(offer);
  }

  void addInverseOffer(InverseOffer* inverseOffer)
  {
    CHECK(!inverseOffers.contains(inverseOffer))
      << "Duplicate inverse offer " << inverseOffer->id();

    inverseOffers.insert(inverseOffer);
  }

  void removeInverseOffer(InverseOffer* inverseOffer)
  {
    CHECK(inverseOffers.contains(inverseOffer))
      << "Unknown inverse offer " << inverseOffer->id();

    inverseOffers.erase(inverseOffer);
  }

  bool hasExecutor(const FrameworkID& frameworkId,
                   const ExecutorID& executorId) const
  {
    return executors.contains(frameworkId) &&
      executors.get(frameworkId).get().contains(executorId);
  }

  void addExecutor(const FrameworkID& frameworkId,
                   const ExecutorInfo& executorInfo)
  {
    CHECK(!hasExecutor(frameworkId, executorInfo.executor_id()))
      << "Duplicate executor '" << executorInfo.executor_id()
      << "' of framework " << frameworkId;

    executors[frameworkId][executorInfo.executor_id()] = executorInfo;
    usedResources[frameworkId] += executorInfo.resources();
  }

  void removeExecutor(const FrameworkID& frameworkId,
                      const ExecutorID& executorId)
  {
    CHECK(hasExecutor(frameworkId, executorId))
      << "Unknown executor '" << executorId << "' of framework " << frameworkId;

    usedResources[frameworkId] -=
      executors[frameworkId][executorId].resources();

    executors[frameworkId].erase(executorId);
    if (executors[frameworkId].empty()) {
      executors.erase(frameworkId);
    }
  }

  void apply(const Offer::Operation& operation)
  {
    Try<Resources> resources = totalResources.apply(operation);
    CHECK_SOME(resources);

    totalResources = resources.get();
    checkpointedResources = totalResources.filter(needCheckpointing);
  }

  Master* const master;
  const SlaveID id;
  const SlaveInfo info;

  const MachineID machineId;

  process::UPID pid;

  // TODO(bmahler): Use stout's Version when it can parse labels, etc.
  std::string version;

  process::Time registeredTime;
  Option<process::Time> reregisteredTime;

  // Slave becomes disconnected when the socket closes.
  bool connected;

  // Slave becomes deactivated when it gets disconnected. In the
  // future this might also happen via HTTP endpoint.
  // No offers will be made for a deactivated slave.
  bool active;

  // Executors running on this slave.
  hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo>> executors;

  // Tasks that have not yet been launched because they are currently
  // being authorized. This is similar to Framework's pendingTasks but we
  // track pendingTasks per agent separately to determine if any offer
  // operation for this agent would change resources requested by these tasks.
  hashmap<FrameworkID, hashmap<TaskID, TaskInfo>> pendingTasks;

  // Tasks present on this slave.
  // TODO(bmahler): The task pointer ownership complexity arises from the fact
  // that we own the pointer here, but it's shared with the Framework struct.
  // We should find a way to eliminate this.
  hashmap<FrameworkID, hashmap<TaskID, Task*>> tasks;

  // Tasks that were asked to kill by frameworks.
  // This is used for reconciliation when the slave re-registers.
  multihashmap<FrameworkID, TaskID> killedTasks;

  // Active offers on this slave.
  hashset<Offer*> offers;

  // Active inverse offers on this slave.
  hashset<InverseOffer*> inverseOffers;

  // Resources for active task / executors. Note that we maintain multiple
  // copies of each shared resource in `usedResources` as they are used by
  // multiple tasks.
  hashmap<FrameworkID, Resources> usedResources;

  Resources offeredResources; // Offers.

  // Resources that should be checkpointed by the slave (e.g.,
  // persistent volumes, dynamic reservations, etc). These are either
  // in use by a task/executor, or are available for use and will be
  // re-offered to the framework.
  Resources checkpointedResources;

  // The current total resources of the slave. Note that this is
  // different from 'info.resources()' because this also considers
  // operations (e.g., CREATE, RESERVE) that have been applied and
  // includes revocable resources as well.
  Resources totalResources;

  SlaveObserver* observer;

private:
  Slave(const Slave&);              // No copying.
  Slave& operator=(const Slave&); // No assigning.
};


inline std::ostream& operator<<(std::ostream& stream, const Slave& slave)
{
  return stream << slave.id << " at " << slave.pid
                << " (" << slave.info.hostname() << ")";
}


// Represents the streaming HTTP connection to a framework or a client
// subscribed to the '/api/vX' endpoint.
struct HttpConnection
{
  HttpConnection(const process::http::Pipe::Writer& _writer,
                 ContentType _contentType,
                 UUID _streamId)
    : writer(_writer),
      contentType(_contentType),
      streamId(_streamId) {}

  // We need to evolve the internal old style message/unversioned event into a
  // versioned event e.g., `v1::scheduler::Event` or `v1::master::Event`.
  template <typename Message, typename Event = v1::scheduler::Event>
  bool send(const Message& message)
  {
    ::recordio::Encoder<Event> encoder (lambda::bind(
        serialize, contentType, lambda::_1));

    return writer.write(encoder.encode(evolve(message)));
  }

  bool close()
  {
    return writer.close();
  }

  process::Future<Nothing> closed() const
  {
    return writer.readerClosed();
  }

  process::http::Pipe::Writer writer;
  ContentType contentType;
  UUID streamId;
};


class Master : public ProtobufProcess<Master>
{
public:
  Master(mesos::allocator::Allocator* allocator,
         Registrar* registrar,
         Files* files,
         mesos::master::contender::MasterContender* contender,
         mesos::master::detector::MasterDetector* detector,
         const Option<Authorizer*>& authorizer,
         const Option<std::shared_ptr<process::RateLimiter>>&
           slaveRemovalLimiter,
         const Flags& flags = Flags());

  virtual ~Master();

  // Message handlers.
  void submitScheduler(
      const std::string& name);

  void registerFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo);

  void reregisterFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      bool failover);

  void unregisterFramework(
      const process::UPID& from,
      const FrameworkID& frameworkId);

  void deactivateFramework(
      const process::UPID& from,
      const FrameworkID& frameworkId);

  // TODO(vinod): Remove this once the old driver is removed.
  void resourceRequest(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);

  void launchTasks(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<TaskInfo>& tasks,
      const Filters& filters,
      const std::vector<OfferID>& offerIds);

  void reviveOffers(
      const process::UPID& from,
      const FrameworkID& frameworkId);

  void killTask(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const TaskID& taskId);

  void statusUpdateAcknowledgement(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const std::string& uuid);

  void schedulerMessage(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void executorMessage(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);

  void registerSlave(
      const process::UPID& from,
      const SlaveInfo& slaveInfo,
      const std::vector<Resource>& checkpointedResources,
      const std::string& version);

  void reregisterSlave(
      const process::UPID& from,
      const SlaveInfo& slaveInfo,
      const std::vector<Resource>& checkpointedResources,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<FrameworkInfo>& frameworks,
      const std::vector<Archive::Framework>& completedFrameworks,
      const std::string& version);

  void unregisterSlave(
      const process::UPID& from,
      const SlaveID& slaveId);

  void statusUpdate(
      StatusUpdate update,
      const process::UPID& pid);

  void reconcileTasks(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<TaskStatus>& statuses);

  void exitedExecutor(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      int32_t status);

  void updateSlave(
      const SlaveID& slaveId,
      const Resources& oversubscribedResources);

  void updateUnavailability(
      const MachineID& machineId,
      const Option<Unavailability>& unavailability);

  void markUnreachable(const SlaveID& slaveId);

  void authenticate(
      const process::UPID& from,
      const process::UPID& pid);

  // TODO(bmahler): It would be preferred to use a unique libprocess
  // Process identifier (PID is not sufficient) for identifying the
  // framework instance, rather than relying on re-registration time.
  void frameworkFailoverTimeout(
      const FrameworkID& frameworkId,
      const process::Time& reregisteredTime);

  void offer(
      const FrameworkID& framework,
      const hashmap<SlaveID, Resources>& resources);

  void inverseOffer(
      const FrameworkID& framework,
      const hashmap<SlaveID, UnavailableResources>& resources);

  // Invoked when there is a newly elected leading master.
  // Made public for testing purposes.
  void detected(const process::Future<Option<MasterInfo>>& pid);

  // Invoked when the contender has lost the candidacy.
  // Made public for testing purposes.
  void lostCandidacy(const process::Future<Nothing>& lost);

  // Continuation of recover().
  // Made public for testing purposes.
  process::Future<Nothing> _recover(const Registry& registry);

  MasterInfo info() const
  {
    return info_;
  }

protected:
  virtual void initialize();
  virtual void finalize();

  virtual void visit(const process::MessageEvent& event);
  virtual void visit(const process::ExitedEvent& event);

  virtual void exited(const process::UPID& pid);
  void exited(const FrameworkID& frameworkId, const HttpConnection& http);
  void _exited(Framework* framework);

  // Invoked upon noticing a subscriber disconnection.
  void exited(const UUID& id);

  // Invoked when the message is ready to be executed after
  // being throttled.
  // 'principal' being None indicates it is throttled by
  // 'defaultLimiter'.
  void throttled(
      const process::MessageEvent& event,
      const Option<std::string>& principal);

  // Continuations of visit().
  void _visit(const process::MessageEvent& event);
  void _visit(const process::ExitedEvent& event);

  // Helper method invoked when the capacity for a framework
  // principal is exceeded.
  void exceededCapacity(
      const process::MessageEvent& event,
      const Option<std::string>& principal,
      uint64_t capacity);

  // Recovers state from the registrar.
  process::Future<Nothing> recover();
  void recoveredSlavesTimeout(const Registry& registry);

  void _registerSlave(
      const SlaveInfo& slaveInfo,
      const process::UPID& pid,
      const std::vector<Resource>& checkpointedResources,
      const std::string& version,
      const process::Future<bool>& admit);

  void _reregisterSlave(
      const SlaveInfo& slaveInfo,
      const process::UPID& pid,
      const std::vector<Resource>& checkpointedResources,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<FrameworkInfo>& frameworks,
      const std::vector<Archive::Framework>& completedFrameworks,
      const std::string& version,
      const process::Future<bool>& readmit);

  void __reregisterSlave(
      Slave* slave,
      const std::vector<Task>& tasks,
      const std::vector<FrameworkInfo>& frameworks);

  // 'authenticate' is the future returned by the authenticator.
  void _authenticate(
      const process::UPID& pid,
      const process::Future<Option<std::string>>& authenticate);

  void authenticationTimeout(process::Future<Option<std::string>> future);

  void fileAttached(const process::Future<Nothing>& result,
                    const std::string& path);

  // Invoked when the contender has entered the contest.
  void contended(const process::Future<process::Future<Nothing>>& candidacy);

  // Task reconciliation, split from the message handler
  // to allow re-use.
  void _reconcileTasks(
      Framework* framework,
      const std::vector<TaskStatus>& statuses);

  // When a slave that is known to the master re-registers, we need to
  // reconcile the master's view of the slave's tasks and executors.
  // This function also sends the `ReregisterSlaveMessage`.
  void reconcileKnownSlave(
      Slave* slave,
      const std::vector<ExecutorInfo>& executors,
      const std::vector<Task>& tasks);

  // Add a framework.
  void addFramework(Framework* framework);

  // Replace the scheduler for a framework with a new process ID, in
  // the event of a scheduler failover.
  void failoverFramework(Framework* framework, const process::UPID& newPid);

  // Replace the scheduler for a framework with a new HTTP connection,
  // in the event of a scheduler failover.
  void failoverFramework(Framework* framework, const HttpConnection& http);

  void _failoverFramework(Framework* framework);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule offers that were assigned to this framework.
  void removeFramework(Framework* framework);

  // Remove a framework from the slave, i.e., remove its tasks and
  // executors and recover the resources.
  void removeFramework(Slave* slave, Framework* framework);

  void disconnect(Framework* framework);
  void deactivate(Framework* framework);

  void disconnect(Slave* slave);
  void deactivate(Slave* slave);

  // Add a slave.
  void addSlave(
      Slave* slave,
      const std::vector<Archive::Framework>& completedFrameworks =
        std::vector<Archive::Framework>());

  void _markUnreachable(
      Slave* slave,
      TimeInfo unreachableTime,
      const process::Future<bool>& registrarResult);

  // Mark a slave as unreachable in the registry. Called when the slave
  // does not re-register in time after a master failover.
  Nothing markUnreachableAfterFailover(const Registry::Slave& slave);

  void _markUnreachableAfterFailover(
      const SlaveInfo& slaveInfo,
      const process::Future<bool>& registrarResult);

  void sendSlaveLost(const SlaveInfo& slaveInfo);

  // Remove the slave from the registrar and from the master's state.
  //
  // TODO(bmahler): 'reason' is optional until MESOS-2317 is resolved.
  void removeSlave(
      Slave* slave,
      const std::string& message,
      Option<process::metrics::Counter> reason = None());

  void _removeSlave(
      Slave* slave,
      const process::Future<bool>& registrarResult,
      const std::string& removalCause,
      Option<process::metrics::Counter> reason = None());

  // Validates that the framework is authenticated, if required.
  Option<Error> validateFrameworkAuthentication(
      const FrameworkInfo& frameworkInfo,
      const process::UPID& from);

  // Returns whether the framework is authorized.
  // Returns failure for transient authorization failures.
  process::Future<bool> authorizeFramework(
      const FrameworkInfo& frameworkInfo);

  // Returns whether the task is authorized.
  // Returns failure for transient authorization failures.
  process::Future<bool> authorizeTask(
      const TaskInfo& task,
      Framework* framework);

  /**
   * Authorizes a `RESERVE` offer operation.
   *
   * Returns whether the Reserve operation is authorized with the
   * provided principal. This function is used for authorization of
   * operations originating from both frameworks and operators. Note
   * that operations may be validated AFTER authorization, so it's
   * possible that `reserve` could be malformed.
   *
   * @param reserve The `RESERVE` operation to be performed.
   * @param principal An `Option` containing the principal attempting
   *     this operation.
   *
   * @return A `Future` containing a boolean value representing the
   *     success or failure of this authorization. A failed `Future`
   *     implies that validation of the operation did not succeed.
   */
  process::Future<bool> authorizeReserveResources(
      const Offer::Operation::Reserve& reserve,
      const Option<std::string>& principal);

  /**
   * Authorizes an `UNRESERVE` offer operation.
   *
   * Returns whether the Unreserve operation is authorized with the
   * provided principal. This function is used for authorization of
   * operations originating both from frameworks and operators. Note
   * that operations may be validated AFTER authorization, so it's
   * possible that `unreserve` could be malformed.
   *
   * @param unreserve The `UNRESERVE` operation to be performed.
   * @param principal An `Option` containing the principal attempting
   *     this operation.
   *
   * @return A `Future` containing a boolean value representing the
   *     success or failure of this authorization. A failed `Future`
   *     implies that validation of the operation did not succeed.
   */
  process::Future<bool> authorizeUnreserveResources(
      const Offer::Operation::Unreserve& unreserve,
      const Option<std::string>& principal);

  /**
   * Authorizes a `CREATE` offer operation.
   *
   * Returns whether the Create operation is authorized with the provided
   * principal. This function is used for authorization of operations
   * originating both from frameworks and operators. Note that operations may be
   * validated AFTER authorization, so it's possible that `create` could be
   * malformed.
   *
   * @param create The `CREATE` operation to be performed.
   * @param principal An `Option` containing the principal attempting this
   *     operation.
   *
   * @return A `Future` containing a boolean value representing the success or
   *     failure of this authorization. A failed `Future` implies that
   *     validation of the operation did not succeed.
   */
  process::Future<bool> authorizeCreateVolume(
      const Offer::Operation::Create& create,
      const Option<std::string>& principal);

  /**
   * Authorizes a `DESTROY` offer operation.
   *
   * Returns whether the Destroy operation is authorized with the provided
   * principal. This function is used for authorization of operations
   * originating both from frameworks and operators. Note that operations may be
   * validated AFTER authorization, so it's possible that `destroy` could be
   * malformed.
   *
   * @param destroy The `DESTROY` operation to be performed.
   * @param principal An `Option` containing the principal attempting this
   *     operation.
   *
   * @return A `Future` containing a boolean value representing the success or
   *     failure of this authorization. A failed `Future` implies that
   *     validation of the operation did not succeed.
   */
  process::Future<bool> authorizeDestroyVolume(
      const Offer::Operation::Destroy& destroy,
      const Option<std::string>& principal);

  // Add the task and its executor (if not already running) to the
  // framework and slave. Returns the resources consumed as a result,
  // which includes resources for the task and its executor
  // (if not already running).
  Resources addTask(const TaskInfo& task, Framework* framework, Slave* slave);

  // Transitions the task, and recovers resources if the task becomes
  // terminal.
  void updateTask(Task* task, const StatusUpdate& update);

  // Removes the task.
  void removeTask(Task* task);

  // Remove an executor and recover its resources.
  void removeExecutor(
      Slave* slave,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Attempts to update the allocator by applying the given operation.
  // If successful, updates the slave's resources, sends a
  // 'CheckpointResourcesMessage' to the slave with the updated
  // checkpointed resources, and returns a 'Future' with 'Nothing'.
  // Otherwise, no action is taken and returns a failed 'Future'.
  process::Future<Nothing> apply(
      Slave* slave,
      const Offer::Operation& operation);

  // Forwards the update to the framework.
  void forward(
      const StatusUpdate& update,
      const process::UPID& acknowledgee,
      Framework* framework);

  // Remove an offer after specified timeout
  void offerTimeout(const OfferID& offerId);

  // Remove an offer and optionally rescind the offer as well.
  void removeOffer(Offer* offer, bool rescind = false);

  // Remove an inverse offer after specified timeout
  void inverseOfferTimeout(const OfferID& inverseOfferId);

  // Remove an inverse offer and optionally rescind it as well.
  void removeInverseOffer(InverseOffer* inverseOffer, bool rescind = false);

  Framework* getFramework(const FrameworkID& frameworkId);
  Offer* getOffer(const OfferID& offerId);
  InverseOffer* getInverseOffer(const OfferID& inverseOfferId);

  FrameworkID newFrameworkId();
  OfferID newOfferId();
  SlaveID newSlaveId();

private:
  // Updates the slave's resources by applying the given operation.
  // It also sends a 'CheckpointResourcesMessage' to the slave with
  // the updated checkpointed resources.
  void _apply(Slave* slave, const Offer::Operation& operation);

  void drop(
      const process::UPID& from,
      const scheduler::Call& call,
      const std::string& message);

  void drop(
      Framework* framework,
      const Offer::Operation& operation,
      const std::string& message);

  // Call handlers.
  void receive(
      const process::UPID& from,
      const scheduler::Call& call);

  void subscribe(
      HttpConnection http,
      const scheduler::Call::Subscribe& subscribe);

  void _subscribe(
      HttpConnection http,
      const FrameworkInfo& frameworkInfo,
      bool force,
      const process::Future<bool>& authorized);

  void subscribe(
      const process::UPID& from,
      const scheduler::Call::Subscribe& subscribe);

  void _subscribe(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      bool force,
      const process::Future<bool>& authorized);

  // Subscribes a client to the 'api/vX' endpoint.
  void subscribe(HttpConnection http);

  void teardown(Framework* framework);

  void accept(
      Framework* framework,
      scheduler::Call::Accept accept);

  void _accept(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const Resources& offeredResources,
      const scheduler::Call::Accept& accept,
      const process::Future<std::list<process::Future<bool>>>& authorizations);

  void acceptInverseOffers(
      Framework* framework,
      const scheduler::Call::AcceptInverseOffers& accept);

  void decline(
      Framework* framework,
      const scheduler::Call::Decline& decline);

  void declineInverseOffers(
      Framework* framework,
      const scheduler::Call::DeclineInverseOffers& decline);

  void revive(Framework* framework);

  void kill(
      Framework* framework,
      const scheduler::Call::Kill& kill);

  void shutdown(
      Framework* framework,
      const scheduler::Call::Shutdown& shutdown);

  void acknowledge(
      Framework* framework,
      const scheduler::Call::Acknowledge& acknowledge);

  void reconcile(
      Framework* framework,
      const scheduler::Call::Reconcile& reconcile);

  void message(
      Framework* framework,
      const scheduler::Call::Message& message);

  void request(
      Framework* framework,
      const scheduler::Call::Request& request);

  void suppress(Framework* framework);

  bool elected() const
  {
    return leader.isSome() && leader.get() == info_;
  }

  void scheduleRegistryGc();

  void doRegistryGc();

  void _doRegistryGc(
      const hashset<SlaveID>& toRemove,
      const process::Future<bool>& registrarResult);

  process::Future<bool> authorizeLogAccess(
      const Option<std::string>& principal);

  /**
   * Returns whether the given role is on the whitelist.
   *
   * When using explicit roles, this consults the configured (static)
   * role whitelist. When using implicit roles, any role is allowed
   * (and access control is done via ACLs).
   */
  bool isWhitelistedRole(const std::string& name);

  /**
   * Inner class used to namespace the handling of quota requests.
   *
   * It operates inside the Master actor. It is responsible for validating
   * and persisting quota requests, and exposing quota status.
   * @see master/quota_handler.cpp for implementations.
   */
  class QuotaHandler
  {
  public:
    explicit QuotaHandler(Master* _master) : master(_master)
    {
      CHECK_NOTNULL(master);
    }

    // Returns a list of set quotas.
    process::Future<process::http::Response> status(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> status(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> set(
        const mesos::master::Call& call,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> set(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> remove(
        const mesos::master::Call& call,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> remove(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

  private:
    // Heuristically tries to determine whether a quota request could
    // reasonably be satisfied given the current cluster capacity. The
    // goal is to determine whether a user may accidentally request an
    // amount of resources that would prevent frameworks without quota
    // from getting any offers. A force flag will allow users to bypass
    // this check.
    //
    // The heuristic tests whether the total quota, including the new
    // request, does not exceed the sum of non-static cluster resources,
    // i.e. the following inequality holds:
    //   total - statically reserved >= total quota + quota request
    //
    // Please be advised that:
    //   * It is up to an allocator how to satisfy quota (for example,
    //     what resources to account towards quota, as well as which
    //     resources to consider allocatable for quota).
    //   * Even if there are enough resources at the moment of this check,
    //     agents may terminate at any time, rendering the cluster under
    //     quota.
    Option<Error> capacityHeuristic(
        const mesos::quota::QuotaInfo& request) const;

    // We always want to rescind offers after the capacity heuristic. The
    // reason for this is the race between the allocator and the master:
    // it can happen that there are not enough free resources at the
    // allocator's disposal when it is notified about the quota request,
    // but at this point it's too late to rescind.
    //
    // While rescinding, we adhere to the following rules:
    //   * Rescind at least as many resources as there are in the quota request.
    //   * Rescind all offers from an agent in order to make the potential
    //     offer bigger, which increases the chances that a quota'ed framework
    //     will be able to use the offer.
    //   * Rescind offers from at least `numF` agents to make it possible
    //     (but not guaranteed, due to fair sharing) that each framework in
    //     the role for which quota is set gets an offer (`numF` is the
    //     number of frameworks in the quota'ed role). Though this is not
    //     strictly necessary, we think this will increase the debugability
    //     and will improve user experience.
    //
    // TODO(alexr): Consider removing this function once offer management
    // (including rescinding) is moved to allocator.
    void rescindOffers(const mesos::quota::QuotaInfo& request) const;

    process::Future<bool> authorizeGetQuota(
        const Option<std::string>& principal,
        const std::string& role) const;

    // TODO(mpark): The following functions `authorizeSetQuota` and
    // `authorizeRemoveQuota` should be replaced with `authorizeUpdateQuota` at
    // the end of deprecation cycle which started with 1.0.

    process::Future<bool> authorizeSetQuota(
        const Option<std::string>& principal,
        const mesos::quota::QuotaInfo& quotaInfo) const;

    process::Future<bool> authorizeRemoveQuota(
        const Option<std::string>& principal,
        const mesos::quota::QuotaInfo& quotaInfo) const;

    process::Future<mesos::quota::QuotaStatus> _status(
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> _set(
        const mesos::quota::QuotaRequest& quotaRequest,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> __set(
        const mesos::quota::QuotaInfo& quotaInfo,
        bool forced) const;

    process::Future<process::http::Response> _remove(
        const std::string& role,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> __remove(
        const std::string& role) const;

    // To perform actions related to quota management, we require access to the
    // master data structures. No synchronization primitives are needed here
    // since `QuotaHandler`'s functions are invoked in the Master's actor.
    Master* master;
  };

  /**
   * Inner class used to namespace the handling of /weights requests.
   *
   * It operates inside the Master actor. It is responsible for validating
   * and persisting /weights requests.
   * @see master/weights_handler.cpp for implementations.
   */
  class WeightsHandler
  {
  public:
    explicit WeightsHandler(Master* _master) : master(_master)
    {
      CHECK_NOTNULL(master);
    }

    process::Future<process::http::Response> get(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> get(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> update(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> update(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

  private:
    process::Future<bool> authorizeGetWeight(
        const Option<std::string>& principal,
        const std::string& role) const;

    process::Future<bool> authorizeUpdateWeights(
        const Option<std::string>& principal,
        const std::vector<std::string>& roles) const;

    process::Future<std::vector<WeightInfo>> _filterWeights(
        const std::vector<WeightInfo>& weightInfos,
        const std::list<bool>& authorized) const;

    process::Future<std::vector<WeightInfo>> _getWeights(
        const Option<std::string>& principal) const;

    process::Future<process::http::Response>_updateWeights(
        const Option<std::string>& principal,
        const google::protobuf::RepeatedPtrField<WeightInfo>& weightInfos)
            const;

    process::Future<process::http::Response> __updateWeights(
        const std::vector<WeightInfo>& updateWeightInfos) const;

    // Rescind all outstanding offers if any of the 'weightInfos' roles has
    // an active framework.
    void rescindOffers(const std::vector<WeightInfo>& weightInfos) const;

    Master* master;
  };

  // Inner class used to namespace HTTP route handlers (see
  // master/http.cpp for implementations).
  class Http
  {
  public:
    explicit Http(Master* _master) : master(_master),
                                     quotaHandler(_master),
                                     weightsHandler(_master) {}

    // Logs the request, route handlers can compose this with the
    // desired request handler to get consistent request logging.
    static void log(const process::http::Request& request);

    // /api/v1
    process::Future<process::http::Response> api(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /api/v1/scheduler
    process::Future<process::http::Response> scheduler(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/create-volumes
    process::Future<process::http::Response> createVolumes(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/destroy-volumes
    process::Future<process::http::Response> destroyVolumes(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/flags
    process::Future<process::http::Response> flags(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/frameworks
    process::Future<process::http::Response> frameworks(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/health
    process::Future<process::http::Response> health(
        const process::http::Request& request) const;

    // /master/redirect
    process::Future<process::http::Response> redirect(
        const process::http::Request& request) const;

    // /master/reserve
    process::Future<process::http::Response> reserve(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/roles
    process::Future<process::http::Response> roles(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/teardown
    process::Future<process::http::Response> teardown(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/slaves
    process::Future<process::http::Response> slaves(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/state
    process::Future<process::http::Response> state(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/state-summary
    process::Future<process::http::Response> stateSummary(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/tasks
    process::Future<process::http::Response> tasks(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/maintenance/schedule
    process::Future<process::http::Response> maintenanceSchedule(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/maintenance/status
    process::Future<process::http::Response> maintenanceStatus(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/machine/down
    process::Future<process::http::Response> machineDown(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/machine/up
    process::Future<process::http::Response> machineUp(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/unreserve
    process::Future<process::http::Response> unreserve(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/quota
    process::Future<process::http::Response> quota(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    // /master/weights
    process::Future<process::http::Response> weights(
        const process::http::Request& request,
        const Option<std::string>& principal) const;

    static std::string API_HELP();
    static std::string SCHEDULER_HELP();
    static std::string FLAGS_HELP();
    static std::string FRAMEWORKS_HELP();
    static std::string HEALTH_HELP();
    static std::string REDIRECT_HELP();
    static std::string ROLES_HELP();
    static std::string TEARDOWN_HELP();
    static std::string SLAVES_HELP();
    static std::string STATE_HELP();
    static std::string STATESUMMARY_HELP();
    static std::string TASKS_HELP();
    static std::string MAINTENANCE_SCHEDULE_HELP();
    static std::string MAINTENANCE_STATUS_HELP();
    static std::string MACHINE_DOWN_HELP();
    static std::string MACHINE_UP_HELP();
    static std::string CREATE_VOLUMES_HELP();
    static std::string DESTROY_VOLUMES_HELP();
    static std::string RESERVE_HELP();
    static std::string UNRESERVE_HELP();
    static std::string QUOTA_HELP();
    static std::string WEIGHTS_HELP();

  private:
    JSON::Object __flags() const;

    class FlagsError; // Forward declaration.

    process::Future<Try<JSON::Object, FlagsError>> _flags(
        const Option<std::string>& principal) const;

    process::Future<std::vector<const Task*>> _tasks(
        const size_t limit,
        const size_t offset,
        const std::string& order,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> _teardown(
        const FrameworkID& id) const;

    process::Future<process::http::Response> _updateMaintenanceSchedule(
        const mesos::maintenance::Schedule& schedule) const;

    mesos::maintenance::Schedule _getMaintenanceSchedule() const;

    process::Future<mesos::maintenance::ClusterStatus>
      _getMaintenanceStatus() const;

    process::Future<process::http::Response> _startMaintenance(
        const google::protobuf::RepeatedPtrField<MachineID>& machineIds) const;

    process::Future<process::http::Response> _stopMaintenance(
        const google::protobuf::RepeatedPtrField<MachineID>& machineIds) const;

    process::Future<process::http::Response> _reserve(
        const SlaveID& slaveId,
        const Resources& resources,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> _unreserve(
        const SlaveID& slaveId,
        const Resources& resources,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> _createVolumes(
        const SlaveID& slaveId,
        const google::protobuf::RepeatedPtrField<Resource>& volumes,
        const Option<std::string>& principal) const;

    process::Future<process::http::Response> _destroyVolumes(
        const SlaveID& slaveId,
        const google::protobuf::RepeatedPtrField<Resource>& volumes,
        const Option<std::string>& principal) const;

    /**
     * Continuation for operations: /reserve, /unreserve,
     * /create-volumes and /destroy-volumes. First tries to recover
     * 'required' amount of resources by rescinding outstanding
     * offers, then tries to apply the operation by calling
     * 'master->apply' and propagates the 'Future<Nothing>' as
     * 'Future<Response>' where 'Nothing' -> 'OK' and Failed ->
     * 'Conflict'.
     *
     * @param slaveId The ID of the slave that the operation is
     *     updating.
     * @param required The resources needed to satisfy the operation.
     *     This is used for an optimization where we try to only
     *     rescind offers that would contribute to satisfying the
     *     operation.
     * @param operation The operation to be performed.
     *
     * @return Returns 'OK' if successful, 'Conflict' otherwise.
     */
    process::Future<process::http::Response> _operation(
        const SlaveID& slaveId,
        Resources required,
        const Offer::Operation& operation) const;

    process::Future<std::vector<std::string>> _roles(
        const Option<std::string>& principal) const;

    // Master API handlers.

    process::Future<process::http::Response> getAgents(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    mesos::master::Response::GetAgents _getAgents() const;

    process::Future<process::http::Response> getFlags(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getHealth(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getVersion(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getRoles(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getMetrics(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getLoggingLevel(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> setLoggingLevel(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> listFiles(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getMaster(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> updateMaintenanceSchedule(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getMaintenanceSchedule(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getMaintenanceStatus(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> startMaintenance(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> stopMaintenance(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getTasks(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    mesos::master::Response::GetTasks _getTasks(
        const process::Owned<ObjectApprover>& frameworksApprover,
        const process::Owned<ObjectApprover>& tasksApprover) const;

    process::Future<process::http::Response> createVolumes(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> destroyVolumes(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> reserveResources(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> unreserveResources(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> getFrameworks(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    mesos::master::Response::GetFrameworks _getFrameworks(
        const process::Owned<ObjectApprover>& frameworksApprover) const;

    process::Future<process::http::Response> getExecutors(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    mesos::master::Response::GetExecutors _getExecutors(
        const process::Owned<ObjectApprover>& frameworksApprover,
        const process::Owned<ObjectApprover>& executorsApprover) const;

    process::Future<process::http::Response> getState(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    mesos::master::Response::GetState _getState(
        const process::Owned<ObjectApprover>& frameworksApprover,
        const process::Owned<ObjectApprover>& taskApprover,
        const process::Owned<ObjectApprover>& executorsApprover) const;

    process::Future<process::http::Response> subscribe(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    process::Future<process::http::Response> readFile(
        const mesos::master::Call& call,
        const Option<std::string>& principal,
        ContentType contentType) const;

    Master* master;

    // NOTE: The quota specific pieces of the Operator API are factored
    // out into this separate class.
    QuotaHandler quotaHandler;

    // NOTE: The weights specific pieces of the Operator API are factored
    // out into this separate class.
    WeightsHandler weightsHandler;
  };

  Master(const Master&);              // No copying.
  Master& operator=(const Master&); // No assigning.

  friend struct Framework;
  friend struct Metrics;
  friend struct Slave;

  // NOTE: Since 'getOffer', 'getInverseOffer' and 'slaves' are
  // protected, we need to make the following functions friends.
  friend Offer* validation::offer::getOffer(
      Master* master, const OfferID& offerId);

  friend InverseOffer* validation::offer::getInverseOffer(
      Master* master, const OfferID& offerId);

  friend Slave* validation::offer::getSlave(
      Master* master, const SlaveID& slaveId);

  const Flags flags;

  Http http;

  Option<MasterInfo> leader; // Current leading master.

  mesos::allocator::Allocator* allocator;
  WhitelistWatcher* whitelistWatcher;
  Registrar* registrar;
  Files* files;

  mesos::master::contender::MasterContender* contender;
  mesos::master::detector::MasterDetector* detector;

  const Option<Authorizer*> authorizer;

  MasterInfo info_;

  // Holds some info which affects how a machine behaves, as well as state that
  // represent the master's view of this machine. See the `MachineInfo` protobuf
  // and `Machine` struct for more information.
  hashmap<MachineID, Machine> machines;

  struct Maintenance
  {
    // Holds the maintenance schedule, as given by the operator.
    std::list<mesos::maintenance::Schedule> schedules;
  } maintenance;

  // Indicates when recovery is complete. Recovery begins once the
  // master is elected as a leader.
  Option<process::Future<Nothing>> recovered;

  // If this is the leading master, we periodically check whether we
  // should GC some information from the registry.
  Option<process::Timer> registryGcTimer;

  struct Slaves
  {
    Slaves() : removed(MAX_REMOVED_SLAVES) {}

    // Imposes a time limit for slaves that we recover from the
    // registry to re-register with the master.
    Option<process::Timer> recoveredTimer;

    // Slaves that have been recovered from the registrar after master
    // failover. Slaves are removed from this collection when they
    // either re-register with the master or are marked unreachable
    // because they do not re-register before `recoveredTimer` fires.
    hashset<SlaveID> recovered;

    // Slaves that are in the process of registering.
    hashset<process::UPID> registering;

    // Only those slaves that are re-registering for the first time
    // with this master. We must not answer questions related to
    // these slaves until the registrar determines their fate.
    hashset<SlaveID> reregistering;

    // Registered slaves are indexed by SlaveID and UPID. Note that
    // iteration is supported but is exposed as iteration over a
    // hashmap<SlaveID, Slave*> since it is tedious to convert
    // the map's key/value iterator into a value iterator.
    //
    // TODO(bmahler): Consider pulling in boost's multi_index,
    // or creating a simpler indexing abstraction in stout.
    struct
    {
      bool contains(const SlaveID& slaveId) const
      {
        return ids.contains(slaveId);
      }

      bool contains(const process::UPID& pid) const
      {
        return pids.contains(pid);
      }

      Slave* get(const SlaveID& slaveId) const
      {
        return ids.get(slaveId).getOrElse(nullptr);
      }

      Slave* get(const process::UPID& pid) const
      {
        return pids.get(pid).getOrElse(nullptr);
      }

      void put(Slave* slave)
      {
        CHECK_NOTNULL(slave);
        ids[slave->id] = slave;
        pids[slave->pid] = slave;
      }

      void remove(Slave* slave)
      {
        CHECK_NOTNULL(slave);
        ids.erase(slave->id);
        pids.erase(slave->pid);
      }

      void clear()
      {
        ids.clear();
        pids.clear();
      }

      size_t size() const { return ids.size(); }

      typedef hashmap<SlaveID, Slave*>::iterator iterator;
      typedef hashmap<SlaveID, Slave*>::const_iterator const_iterator;

      iterator begin() { return ids.begin(); }
      iterator end()   { return ids.end();   }

      const_iterator begin() const { return ids.begin(); }
      const_iterator end()   const { return ids.end();   }

    private:
      hashmap<SlaveID, Slave*> ids;
      hashmap<process::UPID, Slave*> pids;
    } registered;

    // Slaves that are in the process of being removed from the
    // registrar. Think of these as being partially removed: we must
    // not answer questions related to these until they are removed
    // from the registry.
    hashset<SlaveID> removing;

    // Slaves that are in the process of being marked unreachable.
    hashset<SlaveID> markingUnreachable;

    // This collection includes agents that have gracefully shutdown,
    // as well as those that have been marked unreachable. We keep a
    // cache here to prevent this from growing in an unbounded manner.
    // TODO(bmahler): Ideally we could use a cache with set semantics.
    Cache<SlaveID, Nothing> removed;

    // Slaves that have been marked unreachable. We recover this from
    // the registry, so it includes slaves marked as unreachable by
    // other instances of the master. Note that we use a linkedhashmap
    // to ensure the order of elements here matches the order in the
    // registry's unreachable list, which matches the order in which
    // agents are marked unreachable.
    LinkedHashMap<SlaveID, TimeInfo> unreachable;

    // This rate limiter is used to limit the removal of slaves failing
    // health checks.
    // NOTE: Using a 'shared_ptr' here is OK because 'RateLimiter' is
    // a wrapper around libprocess process which is thread safe.
    Option<std::shared_ptr<process::RateLimiter>> limiter;

    bool transitioning(const Option<SlaveID>& slaveId)
    {
      if (slaveId.isSome()) {
        return recovered.contains(slaveId.get());
      } else {
        return !recovered.empty();
      }
    }
  } slaves;

  struct Frameworks
  {
    Frameworks(const Flags& masterFlags)
      : completed(masterFlags.max_completed_frameworks) {}

    hashmap<FrameworkID, Framework*> registered;

    // `recovered` contains 'FrameworkInfo's for frameworks that have
    // not yet re-registered after master failover.
    hashmap<FrameworkID, FrameworkInfo> recovered;

    boost::circular_buffer<std::shared_ptr<Framework>> completed;

    // Principals of frameworks keyed by PID.
    // NOTE: Multiple PIDs can map to the same principal. The
    // principal is None when the framework doesn't specify it.
    // The differences between this map and 'authenticated' are:
    // 1) This map only includes *registered* frameworks. The mapping
    //    is added when a framework (re-)registers.
    // 2) This map includes unauthenticated frameworks (when Master
    //    allows them) if they have principals specified in
    //    FrameworkInfo.
    hashmap<process::UPID, Option<std::string>> principals;

    // BoundedRateLimiters keyed by the framework principal.
    // Like Metrics::Frameworks, all frameworks of the same principal
    // are throttled together at a common rate limit.
    hashmap<std::string, Option<process::Owned<BoundedRateLimiter>>> limiters;

    // The default limiter is for frameworks not specified in
    // 'flags.rate_limits'.
    Option<process::Owned<BoundedRateLimiter>> defaultLimiter;
  } frameworks;

  struct Subscribers
  {
    // Represents a client subscribed to the 'api/vX' endpoint.
    //
    // TODO(anand): Add support for filtering. Some subscribers
    // might only be interested in a subset of events.
    struct Subscriber
    {
      Subscriber(const HttpConnection& _http)
        : http(_http) {}

      // Not copyable, not assignable.
      Subscriber(const Subscriber&) = delete;
      Subscriber& operator=(const Subscriber&) = delete;

      ~Subscriber()
      {
        // TODO(anand): Refactor `HttpConnection` to being a RAII class instead.
        // It is possible that a caller might accidently invoke `close()` after
        // passing ownership to the `Subscriber` object. See MESOS-5843 for more
        // details.
        http.close();
      }

      HttpConnection http;
    };

    // Sends the event to all subscribers connected to the 'api/vX' endpoint.
    void send(const mesos::master::Event& event);

    // Active subscribers to the 'api/vX' endpoint keyed by the stream
    // identifier.
    hashmap<UUID, process::Owned<Subscriber>> subscribed;
  } subscribers;

  hashmap<OfferID, Offer*> offers;
  hashmap<OfferID, process::Timer> offerTimers;

  hashmap<OfferID, InverseOffer*> inverseOffers;
  hashmap<OfferID, process::Timer> inverseOfferTimers;

  // Roles with > 0 frameworks currently registered.
  hashmap<std::string, Role*> activeRoles;

  // Configured role whitelist if using the (deprecated) "explicit
  // roles" feature. If this is `None`, any role is allowed.
  Option<hashset<std::string>> roleWhitelist;

  // Configured weight for each role, if any. If a role does not
  // appear here, it has the default weight of 1.
  hashmap<std::string, double> weights;

  // Configured quota for each role, if any. We store quotas by role
  // because we set them at the role level.
  hashmap<std::string, Quota> quotas;

  // Authenticator names as supplied via flags.
  std::vector<std::string> authenticatorNames;

  Option<Authenticator*> authenticator;

  // Frameworks/slaves that are currently in the process of authentication.
  // 'authenticating' future is completed when authenticator
  // completes authentication.
  // The future is removed from the map when master completes authentication.
  hashmap<process::UPID, process::Future<Option<std::string>>> authenticating;

  // Principals of authenticated frameworks/slaves keyed by PID.
  hashmap<process::UPID, std::string> authenticated;

  int64_t nextFrameworkId; // Used to give each framework a unique ID.
  int64_t nextOfferId;     // Used to give each slot offer a unique ID.
  int64_t nextSlaveId;     // Used to give each slave a unique ID.

  // NOTE: It is safe to use a 'shared_ptr' because 'Metrics' is
  // thread safe.
  // TODO(dhamon): This does not need to be a shared_ptr. Metrics contains
  // copyable metric types only.
  std::shared_ptr<Metrics> metrics;

  // Gauge handlers.
  double _uptime_secs()
  {
    return (process::Clock::now() - startTime).secs();
  }

  double _elected()
  {
    return elected() ? 1 : 0;
  }

  double _slaves_connected();
  double _slaves_disconnected();
  double _slaves_active();
  double _slaves_inactive();

  double _frameworks_connected();
  double _frameworks_disconnected();
  double _frameworks_active();
  double _frameworks_inactive();

  double _outstanding_offers()
  {
    return offers.size();
  }

  double _event_queue_messages()
  {
    return static_cast<double>(eventCount<process::MessageEvent>());
  }

  double _event_queue_dispatches()
  {
    return static_cast<double>(eventCount<process::DispatchEvent>());
  }

  double _event_queue_http_requests()
  {
    return static_cast<double>(eventCount<process::HttpEvent>());
  }

  double _tasks_staging();
  double _tasks_starting();
  double _tasks_running();
  double _tasks_killing();

  double _resources_total(const std::string& name);
  double _resources_used(const std::string& name);
  double _resources_percent(const std::string& name);

  double _resources_revocable_total(const std::string& name);
  double _resources_revocable_used(const std::string& name);
  double _resources_revocable_percent(const std::string& name);

  process::Time startTime; // Start time used to calculate uptime.

  Option<process::Time> electedTime; // Time when this master is elected.

  // Validates the framework including authorization.
  // Returns None if the framework is valid.
  // Returns Error if the framework is invalid.
  // Returns Failure if authorization returns 'Failure'.
  process::Future<Option<Error>> validate(
      const FrameworkInfo& frameworkInfo,
      const process::UPID& from);
};


// Add a new slave to the list of admitted slaves.
class AdmitSlave : public Operation
{
public:
  explicit AdmitSlave(const SlaveInfo& _info) : info(_info)
  {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs)
  {
    // Check if this slave is currently admitted. This should only
    // happen if there is a slaveID collision, but that is extremely
    // unlikely in practice: slaveIDs are prefixed with the master ID,
    // which is a randomly generated UUID.
    if (slaveIDs->contains(info.id())) {
      return Error("Agent already admitted");
    }

    Registry::Slave* slave = registry->mutable_slaves()->add_slaves();
    slave->mutable_info()->CopyFrom(info);
    slaveIDs->insert(info.id());
    return true; // Mutation.
  }

private:
  const SlaveInfo info;
};


// Move a slave from the list of admitted slaves to the list of
// unreachable slaves.
class MarkSlaveUnreachable : public Operation
{
public:
  MarkSlaveUnreachable(const SlaveInfo& _info, TimeInfo _unreachableTime)
    : info(_info), unreachableTime(_unreachableTime) {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs)
  {
    // As currently implemented, this should not be possible: the
    // master will only mark slaves unreachable that are currently
    // admitted.
    if (!slaveIDs->contains(info.id())) {
      return Error("Agent not yet admitted");
    }

    for (int i = 0; i < registry->slaves().slaves().size(); i++) {
      const Registry::Slave& slave = registry->slaves().slaves(i);

      if (slave.info().id() == info.id()) {
        registry->mutable_slaves()->mutable_slaves()->DeleteSubrange(i, 1);
        slaveIDs->erase(info.id());

        Registry::UnreachableSlave* unreachable =
          registry->mutable_unreachable()->add_slaves();

        unreachable->mutable_id()->CopyFrom(info.id());
        unreachable->mutable_timestamp()->CopyFrom(unreachableTime);

        return true; // Mutation.
      }
    }

    // Should not happen.
    return Error("Failed to find agent " + stringify(info.id()));
  }

private:
  const SlaveInfo info;
  const TimeInfo unreachableTime;
};


// Add a slave back to the list of admitted slaves. The slave will
// typically be in the "unreachable" list; if so, it is removed from
// that list. The slave might also be in the "admitted" list already.
// Finally, the slave might be in neither the "unreachable" or
// "admitted" lists, if its metadata has been garbage collected from
// the registry.
class MarkSlaveReachable : public Operation
{
public:
  explicit MarkSlaveReachable(const SlaveInfo& _info) : info(_info) {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs)
  {
    // A slave might try to reregister that appears in the list of
    // admitted slaves. This can occur when the master fails over:
    // agents will usually attempt to reregister with the new master
    // before they are marked unreachable. In this situation, the
    // registry is already in the correct state, so no changes are
    // needed.
    if (slaveIDs->contains(info.id())) {
      return false; // No mutation.
    }

    // Check whether the slave is in the unreachable list.
    // TODO(neilc): Optimize this to avoid linear scan.
    bool found = false;
    for (int i = 0; i < registry->unreachable().slaves().size(); i++) {
      const Registry::UnreachableSlave& slave =
        registry->unreachable().slaves(i);

      if (slave.id() == info.id()) {
        registry->mutable_unreachable()->mutable_slaves()->DeleteSubrange(i, 1);
        found = true;
        break;
      }
    }

    if (!found) {
      LOG(WARNING) << "Allowing UNKNOWN agent to reregister: " << info;
    }

    // Add the slave to the admitted list, even if we didn't find it
    // in the unreachable list. This accounts for when the slave was
    // unreachable for a long time, was GC'd from the unreachable
    // list, but then eventually reregistered.
    Registry::Slave* slave = registry->mutable_slaves()->add_slaves();
    slave->mutable_info()->CopyFrom(info);
    slaveIDs->insert(info.id());

    return true; // Mutation.
  }

private:
  const SlaveInfo info;
};


class PruneUnreachable : public Operation
{
public:
  explicit PruneUnreachable(const hashset<SlaveID>& _toRemove)
    : toRemove(_toRemove) {}

protected:
  virtual Try<bool> perform(Registry* registry, hashset<SlaveID>* /*slaveIDs*/)
  {
    // Attempt to remove the SlaveIDs in `toRemove` from the
    // unreachable list. Some SlaveIDs in `toRemove` might not appear
    // in the registry; this is possible if there was a concurrent
    // registry operation.
    //
    // TODO(neilc): This has quadratic worst-case behavior, because
    // `DeleteSubrange` for a `repeated` object takes linear time.
    bool mutate = false;
    int i = 0;
    while (i < registry->unreachable().slaves().size()) {
      const Registry::UnreachableSlave& slave =
        registry->unreachable().slaves(i);

      if (toRemove.contains(slave.id())) {
        Registry::UnreachableSlaves* unreachable =
          registry->mutable_unreachable();

        unreachable->mutable_slaves()->DeleteSubrange(i, i+1);
        mutate = true;
        continue;
      }

      i++;
    }

    return mutate;
  }

private:
  const hashset<SlaveID> toRemove;
};


// Implementation of slave removal Registrar operation.
class RemoveSlave : public Operation
{
public:
  explicit RemoveSlave(const SlaveInfo& _info) : info(_info)
  {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(Registry* registry, hashset<SlaveID>* slaveIDs)
  {
    for (int i = 0; i < registry->slaves().slaves().size(); i++) {
      const Registry::Slave& slave = registry->slaves().slaves(i);
      if (slave.info().id() == info.id()) {
        registry->mutable_slaves()->mutable_slaves()->DeleteSubrange(i, 1);
        slaveIDs->erase(info.id());
        return true; // Mutation.
      }
    }

    // Should not happen: the master will only try to remove agents
    // that are currently admitted.
    return Error("Agent not yet admitted");
  }

private:
  const SlaveInfo info;
};


inline std::ostream& operator<<(
    std::ostream& stream,
    const Framework& framework);


// This process periodically sends heartbeats to a scheduler on the
// given HTTP connection.
class Heartbeater : public process::Process<Heartbeater>
{
public:
  Heartbeater(const FrameworkID& _frameworkId,
              const HttpConnection& _http,
              const Duration& _interval)
    : process::ProcessBase(process::ID::generate("heartbeater")),
      frameworkId(_frameworkId),
      http(_http),
      interval(_interval) {}

protected:
  virtual void initialize() override
  {
    heartbeat();
  }

private:
  void heartbeat()
  {
    // Only send a heartbeat if the connection is not closed.
    if (http.closed().isPending()) {
      VLOG(1) << "Sending heartbeat to " << frameworkId;

      scheduler::Event event;
      event.set_type(scheduler::Event::HEARTBEAT);

      http.send(event);
    }

    process::delay(interval, self(), &Self::heartbeat);
  }

  const FrameworkID frameworkId;
  HttpConnection http;
  const Duration interval;
};


// Information about a connected or completed framework.
// TODO(bmahler): Keeping the task and executor information in sync
// across the Slave and Framework structs is error prone!
struct Framework
{
  Framework(Master* const _master,
            const Flags& masterFlags,
            const FrameworkInfo& _info,
            const process::UPID& _pid,
            const process::Time& time = process::Clock::now())
    : master(_master),
      info(_info),
      pid(_pid),
      connected(true),
      active(true),
      registeredTime(time),
      reregisteredTime(time),
      completedTasks(masterFlags.max_completed_tasks_per_framework) {}

  Framework(Master* const _master,
            const Flags& masterFlags,
            const FrameworkInfo& _info,
            const HttpConnection& _http,
            const process::Time& time = process::Clock::now())
    : master(_master),
      info(_info),
      http(_http),
      connected(true),
      active(true),
      registeredTime(time),
      reregisteredTime(time),
      completedTasks(masterFlags.max_completed_tasks_per_framework) {}

  ~Framework()
  {
    if (http.isSome()) {
      closeHttpConnection();
    }
  }

  Task* getTask(const TaskID& taskId)
  {
    if (tasks.count(taskId) > 0) {
      return tasks[taskId];
    }

    return nullptr;
  }

  void addTask(Task* task)
  {
    CHECK(!tasks.contains(task->task_id()))
      << "Duplicate task " << task->task_id()
      << " of framework " << task->framework_id();

    tasks[task->task_id()] = task;

    if (!protobuf::isTerminalState(task->state())) {
      totalUsedResources += task->resources();
      usedResources[task->slave_id()] += task->resources();
    }
  }

  // Notification of task termination, for resource accounting.
  // TODO(bmahler): This is a hack for performance. We need to
  // maintain resource counters because computing task resources
  // functionally for all tasks is expensive, for now.
  void taskTerminated(Task* task)
  {
    CHECK(protobuf::isTerminalState(task->state()));
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    totalUsedResources -= task->resources();
    usedResources[task->slave_id()] -= task->resources();
    if (usedResources[task->slave_id()].empty()) {
      usedResources.erase(task->slave_id());
    }
  }

  // Sends a message to the connected framework.
  template <typename Message>
  void send(const Message& message)
  {
    if (!connected) {
      LOG(WARNING) << "Master attempted to send message to disconnected"
                   << " framework " << *this;
    }

    if (http.isSome()) {
      if (!http.get().send(message)) {
        LOG(WARNING) << "Unable to send event to framework " << *this << ":"
                     << " connection closed";
      }
    } else {
      CHECK_SOME(pid);
      master->send(pid.get(), message);
    }
  }

  void addCompletedTask(const Task& task)
  {
    // TODO(adam-mesos): Check if completed task already exists.
    completedTasks.push_back(std::shared_ptr<Task>(new Task(task)));
  }

  void removeTask(Task* task)
  {
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    if (!protobuf::isTerminalState(task->state())) {
      totalUsedResources -= task->resources();
      usedResources[task->slave_id()] -= task->resources();
      if (usedResources[task->slave_id()].empty()) {
        usedResources.erase(task->slave_id());
      }
    }

    addCompletedTask(*task);

    tasks.erase(task->task_id());
  }

  void addOffer(Offer* offer)
  {
    CHECK(!offers.contains(offer)) << "Duplicate offer " << offer->id();
    offers.insert(offer);
    totalOfferedResources += offer->resources();
    offeredResources[offer->slave_id()] += offer->resources();
  }

  void removeOffer(Offer* offer)
  {
    CHECK(offers.find(offer) != offers.end())
      << "Unknown offer " << offer->id();

    totalOfferedResources -= offer->resources();
    offeredResources[offer->slave_id()] -= offer->resources();
    if (offeredResources[offer->slave_id()].empty()) {
      offeredResources.erase(offer->slave_id());
    }

    offers.erase(offer);
  }

  void addInverseOffer(InverseOffer* inverseOffer)
  {
    CHECK(!inverseOffers.contains(inverseOffer))
      << "Duplicate inverse offer " << inverseOffer->id();
    inverseOffers.insert(inverseOffer);
  }

  void removeInverseOffer(InverseOffer* inverseOffer)
  {
    CHECK(inverseOffers.contains(inverseOffer))
      << "Unknown inverse offer " << inverseOffer->id();

    inverseOffers.erase(inverseOffer);
  }

  bool hasExecutor(const SlaveID& slaveId,
                   const ExecutorID& executorId)
  {
    return executors.contains(slaveId) &&
      executors[slaveId].contains(executorId);
  }

  void addExecutor(const SlaveID& slaveId,
                   const ExecutorInfo& executorInfo)
  {
    CHECK(!hasExecutor(slaveId, executorInfo.executor_id()))
      << "Duplicate executor '" << executorInfo.executor_id()
      << "' on agent " << slaveId;

    executors[slaveId][executorInfo.executor_id()] = executorInfo;
    totalUsedResources += executorInfo.resources();
    usedResources[slaveId] += executorInfo.resources();
  }

  void removeExecutor(const SlaveID& slaveId,
                      const ExecutorID& executorId)
  {
    CHECK(hasExecutor(slaveId, executorId))
      << "Unknown executor '" << executorId
      << "' of framework " << id()
      << " of agent " << slaveId;

    totalUsedResources -= executors[slaveId][executorId].resources();
    usedResources[slaveId] -= executors[slaveId][executorId].resources();
    if (usedResources[slaveId].empty()) {
      usedResources.erase(slaveId);
    }

    executors[slaveId].erase(executorId);
    if (executors[slaveId].empty()) {
      executors.erase(slaveId);
    }
  }

  const FrameworkID id() const { return info.id(); }

  // Update fields in 'info' using those in 'source'. Currently this
  // only updates 'name', 'failover_timeout', 'hostname', 'webui_url',
  // 'capabilities', and 'labels'.
  void updateFrameworkInfo(const FrameworkInfo& source)
  {
    // TODO(jmlvanre): We can't check 'FrameworkInfo.id' yet because
    // of MESOS-2559. Once this is fixed we can 'CHECK' that we only
    // merge 'info' from the same framework 'id'.

    // TODO(jmlvanre): Merge other fields as per design doc in
    // MESOS-703.

    if (source.user() != info.user()) {
      LOG(WARNING) << "Cannot update FrameworkInfo.user to '" << info.user()
                   << "' for framework " << id() << ". Check MESOS-703";
    }

    info.set_name(source.name());

    if (source.has_failover_timeout()) {
      info.set_failover_timeout(source.failover_timeout());
    } else {
      info.clear_failover_timeout();
    }

    if (source.checkpoint() != info.checkpoint()) {
      LOG(WARNING) << "Cannot update FrameworkInfo.checkpoint to '"
                   << stringify(info.checkpoint()) << "' for framework " << id()
                   << ". Check MESOS-703";
    }

    if (source.role() != info.role()) {
      LOG(WARNING) << "Cannot update FrameworkInfo.role to '" << info.role()
                   << "' for framework " << id() << ". Check MESOS-703";
    }

    if (source.has_hostname()) {
      info.set_hostname(source.hostname());
    } else {
      info.clear_hostname();
    }

    if (source.principal() != info.principal()) {
      LOG(WARNING) << "Cannot update FrameworkInfo.principal to '"
                   << info.principal() << "' for framework " << id()
                   << ". Check MESOS-703";
    }

    if (source.has_webui_url()) {
      info.set_webui_url(source.webui_url());
    } else {
      info.clear_webui_url();
    }

    if (source.capabilities_size() > 0) {
      info.mutable_capabilities()->CopyFrom(source.capabilities());
    } else {
      info.clear_capabilities();
    }

    if (source.has_labels()) {
      info.mutable_labels()->CopyFrom(source.labels());
    } else {
      info.clear_labels();
    }
  }

  void updateConnection(const process::UPID& newPid)
  {
    // Cleanup the HTTP connnection if this is a downgrade from HTTP
    // to PID. Note that the connection may already be closed.
    if (http.isSome()) {
      closeHttpConnection();
    }

    // TODO(benh): unlink(oldPid);
    pid = newPid;
  }

  void updateConnection(const HttpConnection& newHttp)
  {
    if (pid.isSome()) {
      // Wipe the PID if this is an upgrade from PID to HTTP.
      // TODO(benh): unlink(oldPid);
      pid = None();
    } else {
      // Cleanup the old HTTP connection.
      // Note that master creates a new HTTP connection for every
      // subscribe request, so 'newHttp' should always be different
      // from 'http'.
      closeHttpConnection();
    }

    CHECK_NONE(http);

    http = newHttp;
  }

  // Closes the HTTP connection and stops the heartbeat.
  //
  // TODO(vinod): Currently 'connected' variable is set separately
  // from this method. We need to make sure these are in sync.
  void closeHttpConnection()
  {
    CHECK_SOME(http);

    if (connected && !http.get().close()) {
      LOG(WARNING) << "Failed to close HTTP pipe for " << *this;
    }

    http = None();

    CHECK_SOME(heartbeater);

    terminate(heartbeater.get().get());
    wait(heartbeater.get().get());

    heartbeater = None();
  }

  void heartbeat()
  {
    CHECK_NONE(heartbeater);
    CHECK_SOME(http);

    // TODO(vinod): Make heartbeat interval configurable and include
    // this information in the SUBSCRIBED response.
    heartbeater =
      new Heartbeater(info.id(), http.get(), DEFAULT_HEARTBEAT_INTERVAL);

    process::spawn(heartbeater.get().get());
  }

  Master* const master;

  FrameworkInfo info;

  // Frameworks can either be connected via HTTP or by message
  // passing (scheduler driver). Exactly one of 'http' and 'pid'
  // will be set according to the last connection made by the
  // framework.
  Option<HttpConnection> http;
  Option<process::UPID> pid;

  // Framework becomes disconnected when the socket closes.
  bool connected;

  // Framework becomes deactivated when it is disconnected or
  // the master receives a DeactivateFrameworkMessage.
  // No offers will be made to a deactivated framework.
  bool active;

  process::Time registeredTime;
  process::Time reregisteredTime;
  process::Time unregisteredTime;

  // Tasks that have not yet been launched because they are currently
  // being authorized.
  hashmap<TaskID, TaskInfo> pendingTasks;

  hashmap<TaskID, Task*> tasks;

  // NOTE: We use a shared pointer for Task because clang doesn't like
  // Boost's implementation of circular_buffer with Task (Boost
  // attempts to do some memset's which are unsafe).
  boost::circular_buffer<std::shared_ptr<Task>> completedTasks;

  hashset<Offer*> offers; // Active offers for framework.

  hashset<InverseOffer*> inverseOffers; // Active inverse offers for framework.

  hashmap<SlaveID, hashmap<ExecutorID, ExecutorInfo>> executors;

  // NOTE: For the used and offered resources below, we keep the
  // total as well as partitioned by SlaveID.
  // We expose the total resources via the HTTP endpoint, and we
  // keep a running total of the resources because looping over the
  // slaves to sum the resources has led to perf issues (MESOS-1862).
  // We keep the resources partitioned by SlaveID because non-scalar
  // resources can be lost when summing them up across multiple
  // slaves (MESOS-2373).
  //
  // Also note that keeping the totals is safe even though it yields
  // incorrect results for non-scalar resources.
  //   (1) For overlapping set items / ranges across slaves, these
  //       will get added N times but only represented once.
  //   (2) When an initial subtraction occurs (N-1), the resource is
  //       no longer represented. (This is the source of the bug).
  //   (3) When any further subtractions occur (N-(1+M)), the
  //       Resources simply ignores the subtraction since there's
  //       nothing to remove, so this is safe for now.

  // TODO(mpark): Strip the non-scalar resources out of the totals
  // in order to avoid reporting incorrect statistics (MESOS-2623).

  // Active task / executor resources.
  Resources totalUsedResources;

  // Note that we maintain multiple copies of each shared resource in
  // `usedResources` as they are used by multiple tasks.
  hashmap<SlaveID, Resources> usedResources;

  // Offered resources.
  Resources totalOfferedResources;
  hashmap<SlaveID, Resources> offeredResources;

  // This is only set for HTTP frameworks.
  Option<process::Owned<Heartbeater>> heartbeater;

private:
  Framework(const Framework&);              // No copying.
  Framework& operator=(const Framework&); // No assigning.
};


  inline std::ostream& operator<<(
    std::ostream& stream,
    const Framework& framework)
{
  // TODO(vinod): Also log the hostname once FrameworkInfo is properly
  // updated on framework failover (MESOS-1784).
  stream << framework.id() << " (" << framework.info.name() << ")";

  if (framework.pid.isSome()) {
    stream << " at " << framework.pid.get();
  }

  return stream;
}


// Information about an active role.
struct Role
{
  void addFramework(Framework* framework)
  {
    frameworks[framework->id()] = framework;
  }

  void removeFramework(Framework* framework)
  {
    frameworks.erase(framework->id());
  }

  Resources resources() const
  {
    Resources resources;
    foreachvalue (Framework* framework, frameworks) {
      resources += framework->totalUsedResources;
      resources += framework->totalOfferedResources;
    }

    return resources;
  }

  // NOTE: The dynamic role/quota relation is stored in and administrated
  // by the master. There is no direct representation of quota information
  // here to avoid duplication and to support that an operator can associate
  // quota with a role before the role is created. Such ordering of operator
  // requests prevents a race of premature unbounded allocation that setting
  // quota first is intended to contain.

  hashmap<FrameworkID, Framework*> frameworks;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_HPP__
