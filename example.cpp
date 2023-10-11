//
// main.cpp
//
#include <iostream>
#include <string.h>
#include <unistd.h>
#include "ProcessPool.hpp"
#include "ProcessQueue.hpp"

void TestProcessPool()
{
    // Note: Create() is blocked for a parent process, it doesn't 
    // return until all child processes stopped.
    ProcessPool procPool;
    if(!procPool.Create(4)) // 4 processes
    {
        std::cout << ">>> " << __func__ << ": ProcessPool::Create() failed" << std::endl;
        return;
    }

    // Are we a child?
    if(procPool.IsChild())
    {
        // Do something here...
        for(int i = 0; i < 20; i++)
        {
            usleep((random() % 5) * 1000); // Add a random 0-4 ms delay
            std::cout << "[" << procPool.GetChildIndex() << "][pid=" << getpid() << "]"
                    << " Do something... " << i << std::endl;
        }

        // Exit child process
        procPool.Exit(true);
    }

    // If we are here then we must be a parent process and all child processes 
    // completed (either exited or crushed).
    std::cout << ">>> " << __func__ << ": End Of TestProcessPool" << std::endl;
}

void TestProcessQueue()
{
    // Arguments to be send to the routine that will be executed
    // by child processes. 
    // Note: Arguments will be copied to a shared memory in order
    // to be accessable by all processes. Don't include complex 
    // types that internally allocates memory since that allocation(s)
    // might be inaccessible in child processes.
    struct Args
    {
        Args() = default;
        Args(int numberIn) : number(numberIn) {}
        int number{0};
    };

    // This it routine that will be executed by child processes
    void (*fptr)(const Args&) = [](const Args& args)
    {
        // Do something here...
        usleep((random() % 5) * 1000); // Add a random 0-4 ms delay
        std::cout << "[pid=" << getpid() << "] Got request: " << args.number << std::endl;
    };

    // Create process queue
    ProcessQueue<Args> procQueue;
    if(!procQueue.Create(4, fptr))  // 4 processes
    {
        std::cout << ">>> " << __func__ << ": ProcessQueue::Create() failed" << std::endl;
        return;
    }
    // At this point we have a queue of child processes that are waiting for requests

    // Post requests to process queue
    for(int i = 0; i < 20; i++)
    {
        Args args(i);
        procQueue.Post(args);
    }

    // Wait until all requests completed
    procQueue.WaitForCompletion();
    std::cout << ">>> " << __func__ << ": End Of TestProcessQueue part 1" << std::endl;

    // Post more requests to process queue
    for(int i = 0; i < 10; i++)
    {
        Args args(i);
        procQueue.Post(args);
    }

    // Wait until all requests completed
    procQueue.WaitForCompletion();

    // We are done with Process Queue test
    std::cout << ">>> " << __func__ << ": End Of TestProcessQueue part 2" << std::endl;
}

int main()
{
    TestProcessPool();

    std::cout << ">>> " << __func__ << ": Sleep for a few seconds before the next test..." << std::endl;
    sleep(5);

    TestProcessQueue();
    return 0;
}

