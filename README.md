# Avionics RTS Synchronization and Priority Inversion Demonstration

# Project Overview
  This project has the theme of Avionics and is a FreeRTOS application running on the simulated ESP32-S3 in Wowki.This project demonstrates how real time tasks communicate, share limited resources, and protect high priority tasks and data using a binary semaphore, counting semaphores, and mutexes.

  # How the Avionics theme is implemented
    - A UAV Beacon interrupt wakes the responder task
    - Four telem processing tasks then compete for the three different decoder resources
    - Navigation and guidance tasks safely update the shared tracking data
    - The high, medium, and low-priority tasks demonstrate priority inversion
    - The fault injection mode disables priority inheritance to show how the high priority task can experience a longer delay than when priority inheritance is enabled. 

  # Links for this Project
WOWKI SIM: https://wokwi.com/projects/471071858928177153
Demo Videos: 

# Components present 
A pushbutton is connected to the GPIO 18 pin that represent a UAV beacon pulse. Pressing this triggers the GPIO ISR. This ISR gives the "beacon_sig_sem" a binary semaphore using "xSemaphoreGiveFromISR(beacon_sig_sem, &woken);"

# Telemetru Decoder and Counting Semaphore
  The telemetry processing part contain four decoder tasks in which only three are available. The counting semaphore is created with "decoder_pool_sem = xSemaphoreCreateCounting(3, 3);"
  Here, each decoder has to successfully take the semaphore before it can use the decoder slot. Three tasks can then process the telemetry at the same time, then when all three slots are occupied the fourth task will wait until another task releases its slot. 
  The sempahore has a size of 3 becuase the avionics system has three decoder resoruces. If i chose to use a size of two, this would only let two tasks to process data and would cause the delay to increase. Using a size of four would let all tasks to enter immediately and wouldnt show resource contention.

# Share Tracking Data / Mutex Protection
  The nav and guidance tasks both update the variable "tracking_update_counter". this variable is protected by "tracking_shared_mux" mutex. Each task has to aquire the mutex before it can read and update the counter "xSemaphoreTake(tracking_shared_mux, portMAX_DELAY);"
    The Mutex prevents the two tasks from modifying the counter at the same time. This is called mutual exclusion and without it both of the tasks could read the same old value and overwrite one anothers update. 
    From this we see that the mutex is better to use than a binary semaphore because it provides task ownership and protects the critical sections.

# Task Table
Task                  | Role of Avionics                  | Priorty   | Stack | Syncronization
----------------------|-----------------------------------|-----------| ------| -----------------
Beacon responder      | Responds to UAV beacon Interrupt  | 12        | 4096  | Binary Semaphore
----------------------|-----------------------------------|-----------|-------|------------------
Telem Decoder 1-4     | Processes simulated telemetry     | 5         | 4096  | Counting Semaphore
----------------------|-----------------------------------|-----------|-------|------------------
Navigation writer     | Updates shared tracking data      | 8         | 4096  | Mutex
----------------------|-----------------------------------|-----------|-------|------------------
Guidance writer       | Updates shared tracking data      | 8         | 4096  | Mutex
----------------------|-----------------------------------|-----------|-------|------------------
High-priority task H  | Time critical task                | 15        | 4096  | Priority Inv. Lock
----------------------|-----------------------------------|-----------|-------|------------------
Medium-priority task M| CPU-intensive background task     | 10        | 4096  | No Lock
----------------------|-----------------------------------|-----------|-------|------------------
Low-priority task L   | Low-priority shared-resource owner| 5         | 4096  | Priority Inv. Lock

# Priority Inversion Exp.
This app contains high, medium, and low-priority tasks of 15, 10, and 5 respectively that are all executed on the same CPU Core. 
Mode 1: Priority inheritance on "PIP-on Mode"
  a) Quoted `[PI][H] ... waited` number:
      W (12340) app4: [PI][H] ACQUIRED @ 12362329 us — waited 12225568 us (~12225 ms) [lock=MUTEX (priority inheritance ON)]

  b) H/M/L timeline step by step using the logged timestamps
    1. L gets the mutex at 86,763us.
    2. H requests it at 136,761us and blocked.
    3. L inherits H's priority, M prevented from preempting L
    4. L releases mutex at 12,366,110us
    5. H waits 12,366,152us
    7. M becomes ready after, at 12,369,484us. 
    The measured output was: [PI][H] ACQUIRED @ 12366152 us waited 12229391 us (~12229 ms) [lock=MUTEX (priority inheritance ON)]
    
 Mode 2: Prioirty inheritance off "PIP-off mode"
  a) Quoted `[PI][H] ... waited` number:
      W (37340) app4: [PI][H] ACQUIRED @ 37367395 us — waited 37230634 us (~37230 ms) [lock=BINARY SEM (no inheritance)]

  b) H/M/L timeline step by step using the logged timestamps:
    1. L gets the binary sem lock at 86,762us
    2. H requests the lock at 136,761us
    3. M is ready at 186,761us
    4. M preempts L due to L not inheriting Hs priority
    5. M finished at 24,775,222us
    6. L resumes/releases lock at 37,347,571us
    7. H aquires lock at 37,347,611us
    8. H waits total of 37,210,850us

# Answers to: "Engineering analysis" From App 4
1. **Mutex vs binary semaphore as lock** — why use the mutex? Tie this to your two measured H-wait numbers.
      The mutex is used as a lock instead because it provides ownership and priority inheritance while a binary semaphore does not do this. This prevents the medium priority tasks from delaying lower priority tasks while high priorty taks wait. 
      This reduces Hs wait time from 37.2s to 12.2s with a mutex.

2. **Counting semaphore size** — why 3 (or whatever you picked)? What if 2? 4?
      3 was chosen for the counting semaphore size becuase it was modeled as having 3 telemetry dfecoder resources.Four decoder tasks are attempting to use the pool but only 3 can hold the resourc at the same time, 
      this causes the fourth task to wait for another to realease a decoder resoprce. If we had the sem size at 2 then only teo decoder tasks could run at the same time, casuing more delays. If we used 4 for the sem 
      size then there wouldnt be any waiting which would make it hard to see the respource-pool behavior.

# Hazard Analysis 
This system is mainly impacted by priority inversion, corrupted shared tracking data, telemetry decoder resource exhaustion, and excessive processing within an interrupt. In order to compensate for priority inversion delaying time-critical avionics tasks, a priority-inheritance mutex was used in which the wait time for the lock was recorded. In the case that the navigation and guidance update the counter at the same time, corrupted shared tracking data was avoided with the counter being mutexed. Three occupied slots can lead to resource exhaustion of the telemetry decoder, so a counting semaphore was used to regulate the slots and force the remainder of the tasks to wait. In order to respect the time constraint and maintain a short ISR for the beacon, a binary semaphore was used to signal the responder task. Thus, the semaphore imposes a wait for the ISR while an interrupt is executed for the purpose of processing.
