#ifndef WORKERS_H
#define WORKERS_H

#include <pthread.h>
#include <utils/Mutex.h>
#include <utils/Condition.h>
#include <utils/Vector.h>
#include <utils/List.h>

namespace android {

class Workers {
public:
    class Task {

    public:
        typedef void (*Function)(void *);

        Task(Function fn, void *data): mFn(fn), mData(data), mCompleted(false) {}
        Task(): Task(NULL, NULL) {}
        Task& operator=(Task &&other) {
            mFn         = other.mFn;
            mData       = other.mData;
            mCompleted  = other.mCompleted;
            return *this;
        }

        void waitForCompletion() {
            mMutex.lock();
            if(!mCompleted) {
                mCond.wait(mMutex);
            }
            mMutex.unlock();
        }

        void execute() {
            Mutex::Autolock lock(mMutex);
            mFn(mData);
            mCompleted = true;
            mCond.signal();
        }

    private:
        Mutex       mMutex;
        Condition   mCond;
        Function    mFn;
        void       *mData;
        bool        mCompleted;
    };

    Workers();
    ~Workers() {}

    bool start();
    void stop();
    bool isRunning() const { return mRunning; }

    unsigned threadsNum() { return (unsigned)mThreads.size(); }

    void queueTask(Task *task);

private:
    class Thread {
    public:
        Thread(int id, Workers *parent): mId(id), mParent(parent) {}
        Thread(): mId(-1), mParent(NULL) {}

        void run() { pthread_create(&mThread, NULL, threadLoop, this); }
        void join() { pthread_join(mThread, NULL); }

    private:
        int         mId;
        pthread_t   mThread;
        Workers    *mParent;

        static void * threadLoop(void *t);
    };
    friend class Thread;

    bool            mRunning;
    bool            mExitRequest;

    Mutex           mMutex;
    Condition       mCond;
    List<Task *>    mTasks;
    Vector<Thread>  mThreads;
};

extern Workers gWorkers;

}; /* namespace android */

#endif // WORKERS_H
