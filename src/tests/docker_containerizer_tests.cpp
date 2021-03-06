/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>

#include "linux/cgroups.hpp"

#include "messages/messages.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

#include "slave/containerizer/docker.hpp"
#include "slave/paths.hpp"
#include "slave/slave.hpp"
#include "slave/state.hpp"


using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave::paths;
using namespace mesos::internal::slave::state;
using namespace mesos::internal::tests;

using namespace process;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::DockerContainerizerProcess;

using process::Future;
using process::Message;
using process::PID;
using process::UPID;

using std::vector;
using std::list;
using std::string;

using testing::_;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::Invoke;
using testing::Return;


class MockDocker : public Docker
{
public:
  MockDocker(const string& path) : Docker(path)
  {
    EXPECT_CALL(*this, logs(_, _))
      .WillRepeatedly(Invoke(this, &MockDocker::_logs));

    EXPECT_CALL(*this, stop(_, _, _))
      .WillRepeatedly(Invoke(this, &MockDocker::_stop));
  }

  MOCK_CONST_METHOD2(
      logs,
      process::Future<Nothing>(
          const string&,
          const string&));

  MOCK_CONST_METHOD3(
      stop,
      process::Future<Nothing>(
          const string&,
          const Duration&, bool));

  process::Future<Nothing> _logs(
      const string& container,
      const string& directory) const
  {
    return Docker::logs(container, directory);
  }

  process::Future<Nothing> _stop(
      const string& container,
      const Duration& timeout,
      bool remove) const
  {
    return Docker::stop(container, timeout, remove);
  }
};


class DockerContainerizerTest : public MesosTest
{
public:
  static bool exists(
      const list<Docker::Container>& containers,
      const ContainerID& containerId)
  {
    string expectedName = slave::DOCKER_NAME_PREFIX + stringify(containerId);

    foreach (const Docker::Container& container, containers) {
      // Docker inspect name contains an extra slash in the beginning.
      if (strings::contains(container.name, expectedName)) {
        return true;
      }
    }

    return false;
  }


  static bool running(
      const list<Docker::Container>& containers,
      const ContainerID& containerId)
  {
    string expectedName = slave::DOCKER_NAME_PREFIX + stringify(containerId);

    foreach (const Docker::Container& container, containers) {
      // Docker inspect name contains an extra slash in the beginning.
      if (strings::contains(container.name, expectedName)) {
        return container.pid.isSome();
      }
    }

    return false;
  }


  virtual void TearDown()
  {
    Try<Docker*> docker = Docker::create(tests::flags.docker, false);
    ASSERT_SOME(docker);
    Future<list<Docker::Container>> containers =
      docker.get()->ps(true, slave::DOCKER_NAME_PREFIX);

    AWAIT_READY(containers);

    // Cleanup all mesos launched containers.
    foreach (const Docker::Container& container, containers.get()) {
      AWAIT_READY_FOR(docker.get()->rm(container.id, true), Seconds(30));
    }

    delete docker.get();
  }
};


class MockDockerContainerizer : public DockerContainerizer {
public:
  MockDockerContainerizer(
      const slave::Flags& flags,
      Shared<Docker> docker)
    : DockerContainerizer(flags, docker)
  {
    initialize();
  }

  MockDockerContainerizer(const Owned<DockerContainerizerProcess>& process)
    : DockerContainerizer(process)
  {
    initialize();
  }

  void initialize()
  {
    // NOTE: See TestContainerizer::setup for why we use
    // 'EXPECT_CALL' and 'WillRepeatedly' here instead of
    // 'ON_CALL' and 'WillByDefault'.
    EXPECT_CALL(*this, launch(_, _, _, _, _, _, _))
      .WillRepeatedly(Invoke(this, &MockDockerContainerizer::_launchExecutor));

    EXPECT_CALL(*this, launch(_, _, _, _, _, _, _, _))
      .WillRepeatedly(Invoke(this, &MockDockerContainerizer::_launch));

    EXPECT_CALL(*this, update(_, _))
      .WillRepeatedly(Invoke(this, &MockDockerContainerizer::_update));
  }

  MOCK_METHOD7(
      launch,
      process::Future<bool>(
          const ContainerID&,
          const ExecutorInfo&,
          const std::string&,
          const Option<std::string>&,
          const SlaveID&,
          const process::PID<slave::Slave>&,
          bool checkpoint));

  MOCK_METHOD8(
      launch,
      process::Future<bool>(
          const ContainerID&,
          const TaskInfo&,
          const ExecutorInfo&,
          const std::string&,
          const Option<std::string>&,
          const SlaveID&,
          const process::PID<slave::Slave>&,
          bool checkpoint));

  MOCK_METHOD2(
      update,
      process::Future<Nothing>(
          const ContainerID&,
          const Resources&));

  // Default 'launch' implementation (necessary because we can't just
  // use &DockerContainerizer::launch with 'Invoke').
  process::Future<bool> _launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint)
  {
    return DockerContainerizer::launch(
        containerId,
        taskInfo,
        executorInfo,
        directory,
        user,
        slaveId,
        slavePid,
        checkpoint);
  }

  process::Future<bool> _launchExecutor(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const string& directory,
      const Option<string>& user,
      const SlaveID& slaveId,
      const PID<Slave>& slavePid,
      bool checkpoint)
  {
    return DockerContainerizer::launch(
        containerId,
        executorInfo,
        directory,
        user,
        slaveId,
        slavePid,
        checkpoint);
  }

  process::Future<Nothing> _update(
      const ContainerID& containerId,
      const Resources& resources)
  {
    return DockerContainerizer::update(
        containerId,
        resources);
  }
};


class MockDockerContainerizerProcess : public DockerContainerizerProcess
{
public:
  MockDockerContainerizerProcess(
      const slave::Flags& flags,
      const Shared<Docker>& docker)
    : DockerContainerizerProcess(flags, docker)
  {
    EXPECT_CALL(*this, fetch(_))
      .WillRepeatedly(Invoke(this, &MockDockerContainerizerProcess::_fetch));

    EXPECT_CALL(*this, pull(_, _, _, _))
      .WillRepeatedly(Invoke(this, &MockDockerContainerizerProcess::_pull));
  }

  MOCK_METHOD1(
      fetch,
      process::Future<Nothing>(const ContainerID& containerId));

  MOCK_METHOD4(
      pull,
      process::Future<Nothing>(
          const ContainerID& containerId,
          const std::string& directory,
          const std::string& image,
          bool forcePullImage));

  process::Future<Nothing> _fetch(const ContainerID& containerId)
  {
    return DockerContainerizerProcess::fetch(containerId);
  }

  process::Future<Nothing> _pull(
      const ContainerID& containerId,
      const std::string& directory,
      const std::string& image,
      bool forcePullImage)
  {
    return DockerContainerizerProcess::pull(
        containerId,
        directory,
        image,
        forcePullImage);
  }
};


// Only enable executor launch on linux as other platforms
// requires running linux VM and need special port forwarding
// to get host networking to work.
#ifdef __linux__
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch_Executor)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  slave::Flags flags = CreateSlaveFlags();

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("tnachen/test-executor");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launchExecutor)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  Future<list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  ASSERT_TRUE(exists(containers.get(), containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  containers = docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  ASSERT_FALSE(running(containers.get(), containerId.get()));

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  Shutdown();
}


// This test verifies that a custom executor can be launched and
// registered with the slave with docker bridge network enabled.
// We're assuming that the custom executor is registering it's public
// ip instead of 0.0.0.0 or equivelent to the slave as that's the
// default behavior for libprocess.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch_Executor_Bridged)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  slave::Flags flags = CreateSlaveFlags();

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
    &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("tnachen/test-executor");
  dockerInfo.set_network(ContainerInfo::DockerInfo::BRIDGE);

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launchExecutor)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  Future<list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  ASSERT_TRUE(exists(containers.get(), containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  containers = docker->ps(true, slave::DOCKER_NAME_PREFIX);
  AWAIT_READY(containers);

  ASSERT_FALSE(running(containers.get(), containerId.get()));

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  Shutdown();
}
#endif // __linux__


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Launch)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  slave::Flags flags = CreateSlaveFlags();

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  Future<list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  ASSERT_TRUE(containers.get().size() > 0);

  ASSERT_TRUE(exists(containers.get(), containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  containers = docker->ps(true, slave::DOCKER_NAME_PREFIX);

  ASSERT_FALSE(running(containers.get(), containerId.get()));

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  Shutdown();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Kill)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  slave::Flags flags = CreateSlaveFlags();

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  Future<TaskStatus> statusKilled;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusKilled));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.killTask(task.task_id());

  AWAIT_READY(statusKilled);
  EXPECT_EQ(TASK_KILLED, statusKilled.get().state());

  AWAIT_READY(termination);

  Future<list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  ASSERT_FALSE(running(containers.get(), containerId.get()));

  driver.stop();
  driver.join();

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  Shutdown();
}


// This test tests DockerContainerizer::usage().
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Usage)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>("cpus:2;mem:1024");

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  // Run a CPU intensive command, so we can measure utime and stime later.
  command.set_value("dd if=/dev/zero of=/dev/null");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  // We ignore all update calls to prevent resizing cgroup limits.
  EXPECT_CALL(dockerContainerizer, update(_, _))
    .WillRepeatedly(Return(Nothing()));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  // Verify the usage.
  ResourceStatistics statistics;
  Duration waited = Duration::zero();
  do {
    Future<ResourceStatistics> usage =
      dockerContainerizer.usage(containerId.get());
    AWAIT_READY(usage);

    statistics = usage.get();

    if (statistics.cpus_user_time_secs() > 0 &&
        statistics.cpus_system_time_secs() > 0) {
      break;
    }

    os::sleep(Milliseconds(200));
    waited += Milliseconds(200);
  } while (waited < Seconds(3));

  EXPECT_EQ(2, statistics.cpus_limit());
  EXPECT_EQ(Gigabytes(1).bytes(), statistics.mem_limit_bytes());
  EXPECT_LT(0, statistics.cpus_user_time_secs());
  EXPECT_LT(0, statistics.cpus_system_time_secs());

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  dockerContainerizer.destroy(containerId.get());

  AWAIT_READY(termination);

  // Usage() should fail again since the container is destroyed.
  Future<ResourceStatistics> usage =
    dockerContainerizer.usage(containerId.get());

  AWAIT_FAILED(usage);

  driver.stop();
  driver.join();

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  Shutdown();
}


#ifdef __linux__
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Update)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(containerId);

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  string containerName = slave::DOCKER_NAME_PREFIX + containerId.get().value();
  Future<Docker::Container> container = docker->inspect(containerName);

  AWAIT_READY(container);

  Try<Resources> newResources = Resources::parse("cpus:1;mem:128");

  ASSERT_SOME(newResources);

  Future<Nothing> update =
    dockerContainerizer.update(containerId.get(), newResources.get());

  AWAIT_READY(update);

  Result<string> cpuHierarchy = cgroups::hierarchy("cpu");
  Result<string> memoryHierarchy = cgroups::hierarchy("memory");

  ASSERT_SOME(cpuHierarchy);
  ASSERT_SOME(memoryHierarchy);

  Option<pid_t> pid = container.get().pid;
  ASSERT_SOME(pid);

  Result<string> cpuCgroup = cgroups::cpu::cgroup(pid.get());
  ASSERT_SOME(cpuCgroup);

  Result<string> memoryCgroup = cgroups::memory::cgroup(pid.get());
  ASSERT_SOME(memoryCgroup);

  Try<uint64_t> cpu = cgroups::cpu::shares(
      cpuHierarchy.get(),
      cpuCgroup.get());

  ASSERT_SOME(cpu);

  Try<Bytes> mem = cgroups::memory::soft_limit_in_bytes(
      memoryHierarchy.get(),
      memoryCgroup.get());

  ASSERT_SOME(mem);

  EXPECT_EQ(1024u, cpu.get());
  EXPECT_EQ(128u, mem.get().megabytes());

  newResources = Resources::parse("cpus:1;mem:144");

  // Issue second update that uses the cached pid instead of inspect.
  update = dockerContainerizer.update(containerId.get(), newResources.get());

  AWAIT_READY(update);

  cpu = cgroups::cpu::shares(cpuHierarchy.get(), cpuCgroup.get());

  ASSERT_SOME(cpu);

  mem = cgroups::memory::soft_limit_in_bytes(
      memoryHierarchy.get(),
      memoryCgroup.get());

  ASSERT_SOME(mem);

  EXPECT_EQ(1024u, cpu.get());
  EXPECT_EQ(144u, mem.get().megabytes());

  driver.stop();
  driver.join();

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  Shutdown();
}
#endif //__linux__


// Disabling recover test as the docker rm in recover is async.
// Even though we wait for the container to finish, when the wait
// returns docker rm might still be in progress.
// TODO(tnachen): Re-enable test when we wait for the async kill
// to finish. One way to do this is to mock the Docker interface
// and let the mocked docker collect all the remove futures and
// at the end of the test wait for all of them before the test exits.
TEST_F(DockerContainerizerTest, DISABLED_ROOT_DOCKER_Recover)
{
  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  MockDockerContainerizer dockerContainerizer(flags, docker);

  ContainerID containerId;
  containerId.set_value("c1");
  ContainerID reapedContainerId;
  reapedContainerId.set_value("c2");

  Resources resources = Resources::parse("cpus:1;mem:512").get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_value("sleep 1000");

  Future<Nothing> d1 =
    docker->run(
        containerInfo,
        commandInfo,
        slave::DOCKER_NAME_PREFIX + stringify(containerId),
        flags.work_dir,
        flags.docker_sandbox_directory,
        resources);

  Future<Nothing> d2 =
    docker->run(
        containerInfo,
        commandInfo,
        slave::DOCKER_NAME_PREFIX + stringify(reapedContainerId),
        flags.work_dir,
        flags.docker_sandbox_directory,
        resources);

  AWAIT_READY(d1);
  AWAIT_READY(d2);

  SlaveState slaveState;
  FrameworkState frameworkState;

  ExecutorID execId;
  execId.set_value("e1");

  ExecutorState execState;
  ExecutorInfo execInfo;
  execState.info = execInfo;
  execState.latest = containerId;

  Try<process::Subprocess> wait =
    process::subprocess(
        tests::flags.docker + " wait " +
        slave::DOCKER_NAME_PREFIX +
        stringify(containerId));

  ASSERT_SOME(wait);

  Try<process::Subprocess> reaped =
    process::subprocess(
        tests::flags.docker + " wait " +
        slave::DOCKER_NAME_PREFIX +
        stringify(reapedContainerId));

  ASSERT_SOME(reaped);

  FrameworkID frameworkId;

  RunState runState;
  runState.id = containerId;
  runState.forkedPid = wait.get().pid();
  execState.runs.put(containerId, runState);
  frameworkState.executors.put(execId, execState);

  slaveState.frameworks.put(frameworkId, frameworkState);

  Future<Nothing> recover = dockerContainerizer.recover(slaveState);

  AWAIT_READY(recover);

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId);

  ASSERT_FALSE(termination.isFailed());

  AWAIT_FAILED(dockerContainerizer.wait(reapedContainerId));

  dockerContainerizer.destroy(containerId);

  AWAIT_READY(termination);

  AWAIT_READY(reaped.get().status());

  Shutdown();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_Logs)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  string uuid = UUID::random().toString();

  CommandInfo command;
  command.set_value("echo out" + uuid + " ; echo err" + uuid + " 1>&2");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  // Now check that the proper output is in stderr and stdout (which
  // might also contain other things, hence the use of a UUID).
  Try<string> read = os::read(path::join(directory.get(), "stderr"));

  ASSERT_SOME(read);
  EXPECT_TRUE(strings::contains(read.get(), "err" + uuid));
  EXPECT_FALSE(strings::contains(read.get(), "out" + uuid));

  read = os::read(path::join(directory.get(), "stdout"));

  ASSERT_SOME(read);
  EXPECT_TRUE(strings::contains(read.get(), "out" + uuid));
  EXPECT_FALSE(strings::contains(read.get(), "err" + uuid));

  driver.stop();
  driver.join();

  Shutdown();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_shell(false);

  // NOTE: By not setting CommandInfo::value we're testing that we
  // will still be able to run the container because it has a default
  // entrypoint!

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/inky");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  Try<string> read = os::read(path::join(directory.get(), "stdout"));

  ASSERT_SOME(read);

  // Since we're not passing any command value, we're expecting the
  // default entry point to be run which is 'echo' with the default
  // command from the image which is 'inky'.
  EXPECT_TRUE(strings::contains(read.get(), "inky"));

  read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);
  EXPECT_FALSE(strings::contains(read.get(), "inky"));

  driver.stop();
  driver.join();

  Shutdown();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD_Override)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  // We skip stopping the docker container because stopping  a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  string uuid = UUID::random().toString();

  CommandInfo command;
  command.set_shell(false);

  // We can set the value to just the 'uuid' since it should get
  // passed as an argument to the entrypoint, i.e., 'echo uuid'.
  command.set_value(uuid);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/inky");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  // Now check that the proper output is in stderr and stdout.
  Try<string> read = os::read(path::join(directory.get(), "stdout"));

  ASSERT_SOME(read);

  // We expect the passed in command value to override the image's
  // default command, thus we should see the value of 'uuid' in the
  // output instead of the default command which is 'inky'.
  EXPECT_TRUE(strings::contains(read.get(), uuid));
  EXPECT_FALSE(strings::contains(read.get(), "inky"));

  read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);
  EXPECT_FALSE(strings::contains(read.get(), "inky"));
  EXPECT_FALSE(strings::contains(read.get(), uuid));

  driver.stop();
  driver.join();

  Shutdown();
}


// The following test uses a Docker image (mesosphere/inky) that has
// an entrypoint "echo" and a default command "inky".
TEST_F(DockerContainerizerTest, ROOT_DOCKER_Default_CMD_Args)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  string uuid = UUID::random().toString();

  CommandInfo command;
  command.set_shell(false);

  // We should also be able to skip setting the comamnd value and just
  // set the arguments and those should also get passed through to the
  // entrypoint!
  command.add_arguments(uuid);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/inky");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  // Now check that the proper output is in stderr and stdout.
  Try<string> read = os::read(path::join(directory.get(), "stdout"));

  ASSERT_SOME(read);

  // We expect the passed in command arguments to override the image's
  // default command, thus we should see the value of 'uuid' in the
  // output instead of the default command which is 'inky'.
  EXPECT_TRUE(strings::contains(read.get(), uuid));
  EXPECT_FALSE(strings::contains(read.get(), "inky"));

  read = os::read(path::join(directory.get(), "stderr"));
  ASSERT_SOME(read);
  EXPECT_FALSE(strings::contains(read.get(), "inky"));
  EXPECT_FALSE(strings::contains(read.get(), uuid));

  driver.stop();
  driver.join();

  Shutdown();
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up we make sure the executor
// re-registers and the slave properly sends the update.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_SlaveRecoveryTaskContainer)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // Setup recovery slave flags.
  flags.checkpoint = true;
  flags.recover = "reconnect";
  flags.strict = true;

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  // We put the containerizer on the heap so we can more easily
  // control it's lifetime, i.e., when we invoke the destructor.
  MockDockerContainerizer* dockerContainerizer1 =
    new MockDockerContainerizer(flags, docker);

  Try<PID<Slave> > slave1 = StartSlave(dockerContainerizer1, flags);
  ASSERT_SOME(slave1);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  EXPECT_CALL(*dockerContainerizer1, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(dockerContainerizer1,
                           &MockDockerContainerizer::_launch)));

  // Drop the first update from the executor.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(containerId);

  // Stop the slave before the status update is received.
  AWAIT_READY(statusUpdateMessage);

  Stop(slave1.get());

  delete dockerContainerizer1;

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  MockDockerContainerizer* dockerContainerizer2 =
    new MockDockerContainerizer(flags, docker);

  Try<PID<Slave> > slave2 = StartSlave(dockerContainerizer2, flags);
  ASSERT_SOME(slave2);

  // Ensure the executor re-registers.
  AWAIT_READY(reregisterExecutorMessage);
  UPID executorPid = reregisterExecutorMessage.get().from;

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage.get().body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_RUNNING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  // Make sure the container is still running.
  Future<list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  ASSERT_TRUE(exists(containers.get(), containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer2->wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  Shutdown();

  delete dockerContainerizer2;
}


// The slave is stopped before the first update for a task is received
// from the executor. When it comes back up we make sure the executor
// re-registers and the slave properly sends the update.
//
// TODO(benh): This test is currently disabled because the executor
// inside the image mesosphere/test-executor does not properly set the
// executor PID that is uses during registration, so when the new
// slave recovers it can't reconnect and instead destroys that
// container. In particular, it uses '0' for it's IP which we properly
// parse and can even properly use for sending other messages, but the
// current implementation of 'UPID::operator bool ()' fails if the IP
// component of a PID is '0'.
TEST_F(DockerContainerizerTest,
       DISABLED_ROOT_DOCKER_SlaveRecoveryExecutorContainer)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  // Setup recovery slave flags.
  flags.checkpoint = true;
  flags.recover = "reconnect";
  flags.strict = true;

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  MockDockerContainerizer* dockerContainerizer1 =
    new MockDockerContainerizer(flags, docker);

  Try<PID<Slave> > slave1 = StartSlave(dockerContainerizer1, flags);
  ASSERT_SOME(slave1);

  // Enable checkpointing for the framework.
  FrameworkInfo frameworkInfo;
  frameworkInfo.CopyFrom(DEFAULT_FRAMEWORK_INFO);
  frameworkInfo.set_checkpoint(true);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  ExecutorInfo executorInfo;
  ExecutorID executorId;
  executorId.set_value("e1");
  executorInfo.mutable_executor_id()->CopyFrom(executorId);

  CommandInfo command;
  command.set_value("test-executor");
  executorInfo.mutable_command()->CopyFrom(command);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("mesosphere/test-executor");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);
  executorInfo.mutable_container()->CopyFrom(containerInfo);

  task.mutable_executor()->CopyFrom(executorInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  Future<SlaveID> slaveId;
  EXPECT_CALL(*dockerContainerizer1, launch(_, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<4>(&slaveId),
                    Invoke(dockerContainerizer1,
                           &MockDockerContainerizer::_launchExecutor)));

  // We need to wait until the container's pid has been been
  // checkpointed so that when the next slave recovers it won't treat
  // the executor as having gone lost! We know this has completed
  // after Containerizer::launch returns and the
  // Slave::executorLaunched gets dispatched.
  Future<Nothing> executorLaunched =
    FUTURE_DISPATCH(_, &Slave::executorLaunched);

  // The test-executor in the image immediately sends a TASK_RUNNING
  // followed by TASK_FINISHED (no sleep/delay in between) so we need
  // to drop the first TWO updates that come from the executor rather
  // than only the first update like above where we can control how
  // the length of the task.
  Future<StatusUpdateMessage> statusUpdateMessage1 =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  // Drop the first update from the executor.
  Future<StatusUpdateMessage> statusUpdateMessage2 =
    DROP_PROTOBUF(StatusUpdateMessage(), _, _);

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(containerId);
  AWAIT_READY(slaveId);

  AWAIT_READY(executorLaunched);
  AWAIT_READY(statusUpdateMessage1);
  AWAIT_READY(statusUpdateMessage2);

  Stop(slave1.get());

  delete dockerContainerizer1;

  Future<Message> reregisterExecutorMessage =
    FUTURE_MESSAGE(Eq(ReregisterExecutorMessage().GetTypeName()), _, _);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(_, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  MockDockerContainerizer* dockerContainerizer2 =
    new MockDockerContainerizer(flags, docker);

  Try<PID<Slave> > slave2 = StartSlave(dockerContainerizer2, flags);
  ASSERT_SOME(slave2);

  // Ensure the executor re-registers.
  AWAIT_READY(reregisterExecutorMessage);
  UPID executorPid = reregisterExecutorMessage.get().from;

  ReregisterExecutorMessage reregister;
  reregister.ParseFromString(reregisterExecutorMessage.get().body);

  // Executor should inform about the unacknowledged update.
  ASSERT_EQ(1, reregister.updates_size());
  const StatusUpdate& update = reregister.updates(0);
  ASSERT_EQ(task.task_id(), update.status().task_id());
  ASSERT_EQ(TASK_RUNNING, update.status().state());

  // Scheduler should receive the recovered update.
  AWAIT_READY(status);
  ASSERT_EQ(TASK_RUNNING, status.get().state());

  // Make sure the container is still running.
  Future<list<Docker::Container>> containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  ASSERT_TRUE(exists(containers.get(), containerId.get()));

  driver.stop();
  driver.join();

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  delete dockerContainerizer2;
}


// This test verifies that port mapping with bridge network is
// exposing the host port to the container port, by sending data
// to the host port and receiving it in the container by listening
// to the mapped container port.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_PortMapping)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  flags.resources = "cpus:1;mem:1024;ports:[10000-10000]";

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillOnce(FutureResult(&logs,
                           Invoke((MockDocker*) docker.get(),
                                  &MockDocker::_logs)));

  // We skip stopping the docker container because stopping a container
  // even when it terminated might not flush the logs and we end up
  // not getting stdout/stderr in our tests.
  EXPECT_CALL(*mockDocker, stop(_, _, _))
    .WillRepeatedly(Return(Nothing()));

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_shell(false);
  command.set_value("nc");
  command.add_arguments("-l");
  command.add_arguments("-p");
  command.add_arguments("1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  dockerInfo.set_network(ContainerInfo::DockerInfo::BRIDGE);

  ContainerInfo::DockerInfo::PortMapping portMapping;
  portMapping.set_host_port(10000);
  portMapping.set_container_port(1000);

  dockerInfo.add_port_mappings()->CopyFrom(portMapping);
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  Future<string> directory;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    FutureArg<3>(&directory),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY(directory);
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  string uuid = UUID::random().toString();

  // Write uuid to docker mapped host port.
  Try<process::Subprocess> s = process::subprocess(
      "echo " + uuid + " | nc localhost 10000");

  ASSERT_SOME(s);
  AWAIT_READY_FOR(s.get().status(), Seconds(60));

  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  // Now check that the proper output is in stdout.
  Try<string> read = os::read(path::join(directory.get(), "stdout"));

  ASSERT_SOME(read);

  // We expect the uuid that is sent to host port to be written
  // to stdout by the docker container running nc -l.
  EXPECT_TRUE(strings::contains(read.get(), uuid));

  driver.stop();
  driver.join();

  Shutdown();
}


// This test verifies that sandbox with ':' in the path can still
// run successfully. This a limitation of the Docker CLI where
// the volume map parameter treats colons (:) as seperators,
// and incorrectly seperates the sandbox directory.
TEST_F(DockerContainerizerTest, ROOT_DOCKER_LaunchSandboxWithColon)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // We need to capture and await on the logs process's future so that
  // we can ensure there is no child process at the end of the test.
  // The logs future is being awaited at teardown.
  Future<Nothing> logs;
  EXPECT_CALL(*mockDocker, logs(_, _))
    .WillRepeatedly(FutureResult(
        &logs, Invoke((MockDocker*)docker.get(), &MockDocker::_logs)));

  MockDockerContainerizer dockerContainerizer(flags, docker);

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("test:colon");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  Future<TaskStatus> statusRunning;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillRepeatedly(DoDefault());

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));
  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  Future<list<Docker::Container> > containers =
    docker->ps(true, slave::DOCKER_NAME_PREFIX);

  AWAIT_READY(containers);

  ASSERT_TRUE(containers.get().size() > 0);

  ASSERT_TRUE(exists(containers.get(), containerId.get()));

  Future<containerizer::Termination> termination =
    dockerContainerizer.wait(containerId.get());

  driver.stop();
  driver.join();

  AWAIT_READY(termination);

  // See above where we assign logs future for more comments.
  AWAIT_READY_FOR(logs, Seconds(30));

  Shutdown();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_DestroyWhileFetching)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(flags, docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Promise<Nothing> promise;
  Future<Nothing> fetch;

  // We want to pause the fetch call to simulate a long fetch time.
  EXPECT_CALL(*process, fetch(_))
    .WillOnce(DoAll(FutureSatisfy(&fetch),
                    Return(promise.future())));

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));

  AWAIT_READY(fetch);

  dockerContainerizer.destroy(containerId.get());

  // Resume docker launch.
  promise.set(Nothing());

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}


TEST_F(DockerContainerizerTest, ROOT_DOCKER_DestroyWhilePulling)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();

  MockDocker* mockDocker = new MockDocker(tests::flags.docker);
  Shared<Docker> docker(mockDocker);

  // The docker containerizer will free the process, so we must
  // allocate on the heap.
  MockDockerContainerizerProcess* process =
    new MockDockerContainerizerProcess(flags, docker);

  MockDockerContainerizer dockerContainerizer(
      (Owned<DockerContainerizerProcess>(process)));

  Future<Nothing> fetch;
  EXPECT_CALL(*process, fetch(_))
    .WillOnce(DoAll(FutureSatisfy(&fetch),
                    Return(Nothing())));

  Promise<Nothing> promise;

  // We want to pause the fetch call to simulate a long fetch time.
  EXPECT_CALL(*process, pull(_, _, _, _))
    .WillOnce(Return(promise.future()));

  Try<PID<Slave> > slave = StartSlave(&dockerContainerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const Offer& offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offer.slave_id());
  task.mutable_resources()->CopyFrom(offer.resources());

  CommandInfo command;
  command.set_value("sleep 1000");

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_command()->CopyFrom(command);
  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusFailed));

  Future<ContainerID> containerId;
  EXPECT_CALL(dockerContainerizer, launch(_, _, _, _, _, _, _, _))
    .WillOnce(DoAll(FutureArg<0>(&containerId),
                    Invoke(&dockerContainerizer,
                           &MockDockerContainerizer::_launch)));

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY_FOR(containerId, Seconds(60));

  // Wait until fetch is finished.
  AWAIT_READY(fetch);

  dockerContainerizer.destroy(containerId.get());

  // Resume docker launch.
  promise.set(Nothing());

  AWAIT_READY(statusFailed);

  EXPECT_EQ(TASK_FAILED, statusFailed.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}
