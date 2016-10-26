/*
 * Copyright (C) 2015-2016 Antmicro
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

#include <unistd.h>
#include <assert.h>

#include "Workers.h"

namespace android {

Workers gWorkers;

/******************************************************************************\
                                    Workers
\******************************************************************************/

/**
 * \class Workers
 *
 * Worker threads implementation
 *
 * When started, waits for one or more generic tasks to be queued and executes
 * them in multiple threads.
 *
 * Implementation note:
 * There is no support for OpenMP nor C++11 Threads. libutil's Thread class
 * is more suitable for implementing specific threads than thread pools.
 * Lets go with pthread.
 */

Workers::Workers()
    : mRunning(false)
    , mExitRequest(false) {
}

/**
 * Starts threads.
 */
bool Workers::start() {
    if(mRunning)
        return false;

    const unsigned cpuThreadsCount = (unsigned)sysconf(_SC_NPROCESSORS_ONLN);
    mThreads.resize(cpuThreadsCount);

    int id = 0;
    for(auto it = mThreads.begin(); it != mThreads.end(); ++it) {
        *it = Thread(id++, this);
        it->run();
    }

    mRunning = true;

    return true;
}

/**
 * Stops all threads.
 *
 * No new task is picked, but the ones already in processing will finish.
 */
void Workers::stop() {
    if(!mRunning)
        return;

    {
        Mutex::Autolock lock(mMutex);

        mExitRequest = true;
        mCond.broadcast();
    }
    for(auto it = mThreads.begin(); it != mThreads.end(); ++it) {
        it->join();
    }

    mThreads.clear();
    mRunning = false;
    mExitRequest = false;
}

/**
 * Queues task and returns without waiting for it to be processed.
 */
void Workers::queueTask(Workers::Task *task) {
    Mutex::Autolock lock(mMutex);
    if(!mRunning)
        start();

    mTasks.push_back(task);
    mCond.signal();
}

/******************************************************************************\
                                 Workers::Thread
\******************************************************************************/

/**
 * \class Workers::Thread
 *
 * Interal thread representation.
 */

/**
 * Thread main loop
 */
void *Workers::Thread::threadLoop(void *t) {
    Thread *thread = static_cast<Thread *>(t);
    assert(thread != NULL);
    Workers *workers = thread->mParent;
    assert(workers != NULL);

    for(;;) {
        Workers::Task *task = NULL;

        {
            Mutex::Autolock lock(workers->mMutex);

            while(workers->mTasks.empty() && !workers->mExitRequest)
                workers->mCond.wait(workers->mMutex);

            if(workers->mExitRequest)
                break;

            /* pop task from queue */
            task = *workers->mTasks.begin();
            workers->mTasks.erase(workers->mTasks.begin());
        }

        /* process task */
        task->execute();
    }

    pthread_exit(NULL);
}

}; /* namespace android */
