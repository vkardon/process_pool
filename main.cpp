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
    ProcessPool procPool;
    if(!procPool.Create(8, 4)) // process#, concurrent process#
    {
        std::cout << ">>> " << __func__ << ": ProcessPool::Create() failed" << std::endl;
        return;
    }

    // Are we a child?
    if(procPool.IsChild())
    {
        // Randomize starting point of rand() based on child process id
        srand(getpid());

        for(int i=0; i<30; i++)
        {
            usleep((random() % 5) * 1000); // Add a random 0-4 ms delay
            std::cout << "[pid=" << getpid() << "] Do something... " << i << std::endl;
        }

        procPool.Exit(true);
    }

    // OK, we are parent. All children processes complete.
    std::cout << ">>> " << __func__ << ": End Of TestProcessPool" << std::endl;
}

void TestProcessQueue()
{
    struct Request
    {
        Request() = default;
        Request(int numberIn) : number(numberIn) {}
        int number{0};
    };

    // Request processing routine
    void (*fptr)(const Request&) = [](const Request& req)
    {
        usleep((random() % 5) * 1000); // Add a random 0-4 ms delay
        std::cout << "[pid=" << getpid() << "] Got request: " << req.number << std::endl;
    };

    // Create process queue
    ProcessQueue<Request> procQueue;
    if(!procQueue.Create(8, fptr))  // process#
    {
        std::cout << ">>> " << __func__ << ": ProcessQueue::Create() failed" << std::endl;
        return;
    }

    // Add requests to process queue
    for(int i = 0; i < 1000; i++)
    {
        Request req(i);
        procQueue.AddRequest(req);
    }

    // Wait for process queue to complete all requests
    procQueue.WaitForCompletion();
    procQueue.Destroy();

    // We are done with Process Queue test
    std::cout << ">>> " << __func__ << ": End Of TestProcessQueue" << std::endl;
}

int main()
{
    TestProcessPool();

    std::cout << ">>> " << __func__ << ": Sleep for a few seconds before the next test..." << std::endl;
    sleep(5);

    TestProcessQueue();
    return 0;
}

