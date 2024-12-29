//
// main.cpp
//
#include <iostream>
#include <string.h>
#include <unistd.h>
#include "processPool.hpp"
#include "processQueue.hpp"

void TestProcessPool()
{
    std::cout << ">>> " << __func__ << ": Beginning of ProcessPool test" << std::endl;

    // Note: Create() is blocked for a parent process, it doesn't 
    // return until all child processes stopped.
    ProcessPool procPool;
    if(!procPool.Create(4)) // 4 processes
    {
        std::cout << ">>> " << __func__ << ": ProcessPool::Create() failed" << std::endl;
        return;
    }

    // Are we a child process?
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
    std::cout << ">>> " << __func__ << ": End of ProcessPool test" << std::endl;
}

void TestProcessQueue()
{
    std::cout << ">>> " << __func__ << ": Beginning of ProcessQueue test" << std::endl;

    // Arguments for the routine that will be executed by child processes. 
    // Note: Arguments will be copied to a shared memory in order to be accessable
    // in a child process. Don't include complex types that internally allocates
    // memory since that allocation(s) might not be accessible in child process.
    struct Args
    {
        Args() = default;
        Args(int countIn, const char* nameIn)
            : count(countIn) { strncpy(name, nameIn, sizeof(name)-1); }
        int count{0};
        char name[32]{};
    };

    // This it routine that will be executed by child processes
    auto fptr = [](const Args& args)
    {
        // Do something here...
        usleep((random() % 5) * 1000); // Add a random 0-4 ms delay
        std::cout << "[pid=" << getpid() << "] Got request: " << args.count << " '" << args.name << "'" << std::endl;
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
        Args args(i, "hello1");
        procQueue.Post(args);
    }

    // Wait until all requests completed
    procQueue.WaitForCompletion();
    std::cout << ">>> " << __func__ << ": End of ProcessQueue test part 1" << std::endl;

    // Post more requests to process queue
    for(int i = 0; i < 10; i++)
    {
        Args args(i, "hello2");
        procQueue.Post(args);
    }

    // Wait until all requests completed
    procQueue.WaitForCompletion();

    // We are done with Process Queue test
    std::cout << ">>> " << __func__ << ": End of ProcessQueue test part 2" << std::endl;
}

int main()
{
    TestProcessPool();
    TestProcessQueue();
    return 0;
}

