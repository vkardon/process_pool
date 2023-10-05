//
// ProcessQueue.hpp
//
#ifndef _PROCESS_QUEUE_HPP_
#define _PROCESS_QUEUE_HPP_

#include <assert.h>         // assert()
#include <unistd.h>         // usleep()
#include <iostream>         // std::cout
#include <sys/mman.h>       // mmap()
#include <time.h>           // time()
#include "ProcessPool.hpp"

//
// Utility class to create queue of worker processes
//
template<class ARGS>
class ProcessQueue : public ProcessPool
{
    // Helper class to lock/unlock QueueLock
    class QueueLock
    {
    public:
        QueueLock(unsigned char& lock, int waitMilliseconds=5000 /*5 sec*/) : mLock(lock)
        {
            int waitUseconds = waitMilliseconds * 1000;
            while(waitUseconds > 0)
            {
                if(__sync_fetch_and_or(&mLock, (unsigned char)0xff) == 0)
                    break;

                // Use a simple Ethernet-style delay algorithm to avoid collisions.
                int delay = (random() & 0x3) * 1000; // 0-3 ms delay
                usleep(delay);
                waitUseconds -= delay;
            }
            mHasLock = (waitUseconds > 0);
        }
        ~QueueLock()
        {
            if(mHasLock)
                mLock = 0;
        }
        operator bool() const { return mHasLock; }

        // Omit the copy constructor and assignment operator
        QueueLock(const QueueLock&) = delete;
        QueueLock& operator=(const QueueLock&) = delete;

    private:
        unsigned char& mLock;
        bool mHasLock{false};
    };

public:
    // Note: maxRequestCount represents the worst case scenario
    // when processing is slow and all requests must be stored
    // in Request Queue while waiting for being processed.
    ProcessQueue(unsigned int maxRequestCount = 1000000)
    {
        mWaitForAll = false;
        mRequestQueueSize = (maxRequestCount ? sizeof(Node) * maxRequestCount + sizeof(RequestQueue) : 0);
    }
    virtual ~ProcessQueue() { Destroy(); }

    // Omit implementation of the copy constructor and assignment operator
    ProcessQueue(const ProcessQueue&) = delete;
    ProcessQueue& operator=(const ProcessQueue&) = delete;

    // Fork procCount number of child processes and DON'T wait for them to complete.
    bool Create(int procCount, void (*fptr)(const ARGS&));

    // Add request to RequestQueue
    bool Post(const ARGS& args);

    // Wait for Request Queue became empty
    bool WaitForCompletion();

    // Destroy Request Queue and terminate all child processes
    void Destroy();

private:
    struct Node : public ARGS
    {
        Node* next{nullptr};
    };

    Node* GetNextRequest();
    void FreeRequest(Node* node);
    bool CreateRequestQueue();
    void DeleteRequestQueue();
    bool HasCrashedChildren();

    // Class data
    struct RequestQueue
    {
        unsigned char lock{0};
        unsigned char* fillPtr{nullptr};
        Node* head{nullptr};
        Node* tail{nullptr};
        Node* free{nullptr};
        bool stop{false};
    };

    RequestQueue* mRequestQueue{nullptr};
    size_t mRequestQueueSize{0};
    size_t mCrashTestTimer{0};
    const unsigned int CRASH_TEST_INTERVAL{1};   // How often to check for crashed children
};

// Fork procCount number of child processes and DON'T wait for them to complete.
template<class ARGS>
bool ProcessQueue<ARGS>::Create(int procCount, void (*fptr)(const ARGS&))
{
    if(!CreateRequestQueue())
        return false;

    // Create process pool with procCount number of children processes
    // but don't wait for them to complete.
    if(!ProcessPool::Create(procCount, procCount))
    {
        DeleteRequestQueue();
        return false;
    }

    // If we are parent then we are done.
    if(IsParent())
    {
        mCrashTestTimer = time(nullptr);
        return true;
    }

    // Running as a child
    const int SLEEP_USEC = 10000; // 10 ms
    while(!mRequestQueue->stop)
    {
        // Process next request it we have any
        Node* node = GetNextRequest();
        if(node)
        {
            (*fptr)(*node); // Process request
            FreeRequest(node);
        }
        else
        {
            usleep(SLEEP_USEC); // sleep SLEEP_USEC milliseconds and check again
        }
    }

    // Exit child process
    Exit(true);

    return true;
}

template<class ARGS>
bool ProcessQueue<ARGS>::Post(const ARGS& args)
{
    assert(IsParent());

    // Check for any crash children
    if(HasCrashedChildren())
    {
        // TODO: What should we do if we have a crashed child?
    }

    QueueLock lock(mRequestQueue->lock);
    if(!lock)
    {
        ERRORMSG("Failed to obtain Request Queue lock");
        return false;
    }

    Node* node = nullptr;

    // Check if we have any free nodes that we can use.
    // Otherwise, allocate new node.
    if(mRequestQueue->free)
    {
        node = mRequestQueue->free;
        mRequestQueue->free = node->next;
    }
    else
    {
        size_t availableSize = mRequestQueueSize - (mRequestQueue->fillPtr - (unsigned char*)mRequestQueue);
        if(availableSize < sizeof(Node))
        {
            ERRORMSG("Request Queue is out of memory");
            return false;
        }

        node = new (mRequestQueue->fillPtr) Node;
        mRequestQueue->fillPtr = mRequestQueue->fillPtr + sizeof(Node);
    }

    // Copy input request
    (ARGS&)(*node) = args;

    // Append new node to the tail
    Node* tail = mRequestQueue->tail;
    if(!tail)
    {
        // Very first node
        assert(!mRequestQueue->head);
        mRequestQueue->head = node;
    }
    else
    {
        tail->next = node;
    }
    mRequestQueue->tail = node;
    node->next = nullptr;

    return true;
}

template<class ARGS>
typename ProcessQueue<ARGS>::Node* ProcessQueue<ARGS>::GetNextRequest()
{
    assert(IsChild());

    QueueLock lock(mRequestQueue->lock);
    if(!lock)
    {
        ERRORMSG("Failed to obtain Request Queue lock");
        return nullptr;
    }

    // Detach and return head request
    Node* node = mRequestQueue->head;
    if(node)
    {
        mRequestQueue->head = node->next;

        // If this very last node, then update tail as well
        if(!mRequestQueue->head)
            mRequestQueue->tail = nullptr;
    }

    return node;
}

template<class ARGS>
void ProcessQueue<ARGS>::FreeRequest(ProcessQueue::Node* node)
{
    assert(IsChild());

    if(!node)
        return;

    QueueLock lock(mRequestQueue->lock);
    if(!lock)
    {
        ERRORMSG("Failed to obtain Request Queue lock");
        return;
    }

    // Add request node to the free chain to be reused
    node->next = mRequestQueue->free;
    mRequestQueue->free = node;
}

template<class ARGS>
bool ProcessQueue<ARGS>::CreateRequestQueue()
{
    assert(IsParent());

    // Clean up first
    DeleteRequestQueue();
    assert(!mRequestQueue);

    if(mRequestQueueSize == 0)
    {
        ERRORMSG("Invalid (0) Request Queue size");
        return false;
    }

    // Open the shared memory.
    unsigned char* addr = (unsigned char*)::mmap(NULL, mRequestQueueSize, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    if(addr == MAP_FAILED)
    {
        std::string errmsg = strerror(errno);
        ERRORMSG("mmap for " << mRequestQueueSize << " bytes failed with error \"" << errmsg << "\"");
        return false;
    }

    // Create Request Queue in shared memory
    mRequestQueue = new (addr) RequestQueue;
    assert((void*)mRequestQueue == (void*)addr);

    // Set next available address for a new allocation
    mRequestQueue->fillPtr = addr + sizeof(RequestQueue);
    return true;
}

template<class ARGS>
void ProcessQueue<ARGS>::DeleteRequestQueue()
{
    assert(IsParent());

    if(mRequestQueue)
    {
        if(::munmap(mRequestQueue, mRequestQueueSize) < 0)
        {
            std::string errmsg = strerror(errno);
            ERRORMSG("munmap failed with error \"" << errmsg << "\"");
        }
    }

    mRequestQueue = nullptr;
}

template<class ARGS>
bool ProcessQueue<ARGS>::WaitForCompletion()
{
    assert(IsParent());

    // Loop until no request left in Request Queue
    for(useconds_t delay = 10000 /*10 ms*/; ; usleep(delay))
    {
        // Check for any crash children
        if(HasCrashedChildren())
        {
            // TODO: What should we do if we have a crashed child?
        }

        QueueLock lock(mRequestQueue->lock);
        if(!lock)
        {
            ERRORMSG("Failed to obtain Request Queue lock");
            return false;
        }

        if(!mRequestQueue->head)
            return true;
    }
}

template<class ARGS>
void ProcessQueue<ARGS>::Destroy()
{
    if(IsParent() && mRequestQueue)
    {
        mRequestQueue->stop = true;
        WaitForAll();
        DeleteRequestQueue();
    }
}

template<class ARGS>
bool ProcessQueue<ARGS>::HasCrashedChildren()
{
    // Check for crash children every CRASH_TEST_INTERVAL seconds
    if(time(nullptr) - mCrashTestTimer < CRASH_TEST_INTERVAL)
        return false; // Not a good time to check

    // Reset crash timer
    mCrashTestTimer = time(nullptr);

    size_t childrenCount = mChildrenPIDs.size();
    pid_t childPID = 0;

    for(size_t childIndex = 0; childIndex < childrenCount; childIndex++)
    {
        if(mChildrenPIDs[childIndex].status != CHILD_STATUS::RUNNING)
            continue; // Skip the child that is not running or done

        // Check if the child is alive
        childPID = mChildrenPIDs[childIndex].pid;
        if(IsProcessAlive(childPID))
        {
            childPID = 0; // The child process is alive
            continue;
        }

        // The child has crashed
        ERRORMSG("Child " << childIndex << " (" << childPID << ") has crashed");

        // TODO: Should we have a different CHILD_STATUS for a crashed child?
        mChildrenPIDs[childIndex].status = CHILD_STATUS::DONE;
        break;
    }

    return (childPID != 0);
}


#endif // _PROCESS_QUEUE_HPP_
