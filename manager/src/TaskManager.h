#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include <map>
#include <vector>
#include <iostream>

#include <boost/smart_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lockable_adapter.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/atomic.hpp>

#include "Common.h"
#include "TaskQueue.h"

namespace blaze {

/**
 * Manages a task queue for one accelerator executor
 */
class TaskManager 
: public boost::basic_lockable_adapter<boost::mutex>
{

public:

  TaskManager(
    Task* (*create_func)(), 
    void (*destroy_func)(Task*),
    Platform *_platform
  ): power(true),  // TODO: 
     exeQueueLength(0),
     nextTaskId(0),
     lobbyWaitTime(0),
     doorWaitTime(0),
     deltaDelay(0),
     createTask(create_func),
     destroyTask(destroy_func),
     platform(_platform)
  {;}

  int estimateTime(Task* task);

  // create a task and return the task pointer
  Task_ptr create();

  // enqueue a task in the corresponding application queue
  void enqueue(std::string app_id, Task* task);

  // dequeue a task from the execute queue
  bool dequeue(Task* &task);

  // schedule a task from app queues to execution queue  
  void schedule();

  // execute front task in the queue
  void execute();

  // get best and worst cast wait time 
  std::pair<int, int> getWaitTime(Task* task);

  void startExecutor();
  void startScheduler();

  // start executor and scheduler threads
  void start();
  //void stop();

  // query the current execution queue length
  int getExeQueueLength();

  // experimental
  std::string getConfig(int idx, std::string key);

private:

  bool power;

  // wait time for currently enqueued tasks
  mutable boost::atomic<int> lobbyWaitTime;   
  // wait time for all tasks waiting to enqueue
  mutable boost::atomic<int> doorWaitTime;    

  mutable boost::atomic<int> nextTaskId;

  // current number of tasks in the execution queue
  mutable boost::atomic<int> exeQueueLength;

  int deltaDelay;
  
  // Task implementation loaded from user acc_impl
  Task* (*createTask)();
  void (*destroyTask)(Task*);

  Platform *platform;

  // thread function body for scheduler and executor
  void do_schedule();
  void do_execute();

  void updateDelayModel(Task* task, int estimateTime, int realTime);

  //TODO: add a pointer to GAM communicator

  // application queues mapped by application id
  std::map<std::string, TaskQueue_ptr> app_queues;

  TaskQueue execution_queue;
};

const TaskManager_ptr NULL_TASK_MANAGER;
}

#endif
