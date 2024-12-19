//
// ProcessPool.hpp
//
#ifndef _PROCESS_POOL_HPP_
#define _PROCESS_POOL_HPP_

#include <vector>
#include <string>
#include <signal.h>     // sighandler_t

//
// Utility class to fork children processes and wait for them to exit
//
class ProcessPool
{
public:
    ProcessPool() = default;
    virtual ~ProcessPool();

    // Omit implementation of the copy constructor and assignment operator
    ProcessPool(const ProcessPool&) = delete;
    ProcessPool& operator=(const ProcessPool&) = delete;

    // Fork procCount number of processes and wait for them to complete.
    // If procCount bigger then maxConcurrentProcs, then first
    // wait for any child process to exit before forking new one.
    // If maxConcurrentProcs is 0 then procCount will be used.
    bool Create(int procCount, int maxConcurrentProcs=0)
    {
        return Fork(procCount, (maxConcurrentProcs > 0 ? maxConcurrentProcs : procCount));
    }

    // Exit/Idle completed child:
    // If keepIdle is true then idle process instead of exiting.
    // Parent will terminate process later.
    void Exit(bool status, bool keepIdle = false);

    // Are we a parent of a child?
    bool IsParent() const { return (mChildIndex < 0); }
    bool IsChild() const { return !IsParent(); }
    pid_t GetParent() { return mParentPID; }
    int GetChildIndex() { return mChildIndex; }

    // Notification sent to derived class to collect statistics, etc
    enum class NOTIFY_TYPE : char
    {
        PRE_FORK=1,         // Send right before forking children
        CHILD_FORK,         // Send right after forking a child
        POST_FORK,          // Send right after forking all children
        CHILDREN_DONE       // Send right after all children done (but might be alive and idle)
    };

    virtual void OnNotify(NOTIFY_TYPE /*notifyType*/) {}

protected:
    // Wait for children processes to complete
    bool WaitForAll();

    bool IsProcessAlive(pid_t pid);

    // Logging
    virtual void OnInfo(const std::string& msg) const { /*std::cout << msg << std::endl;*/ }
    virtual void OnError(const std::string& msg) const { std::cout << msg << std::endl; }

    // Child process status enumerator
    enum class CHILD_STATUS : char
    {
        NOT_RUNNING=1, // The child is not running (before fork or after exit)
        RUNNING,       // The child is running
        DONE           // The child completed its run, but not yet terminated
    };

    // Child process running info
    struct ChildPID
    {
        pid_t pid = 0;
        CHILD_STATUS status = CHILD_STATUS::NOT_RUNNING;
    };

private:
    // Fork totalChildren number of children and wait for them to complete
    bool Fork(int totalChildren, int maxConcurrentChildren);

    bool PreFork(int totalChildren);
    void PostFork();

    // Wait for child to complete its task using high-speed loop
    // Returns:
    //   <process id> and "crashed status" of the completed child
    //    0  if all children completed
    //
    pid_t WaitForOne(bool* isCrashed);

    // Kill all running children processes
    void KillAll();

    // Create/Delete children completion status array in shared memory
    bool CreateCompletionStatusArray(int totalChildren);
    bool DeleteCompletionStatusArray();

    bool SetSigAction(int signum, sighandler_t handler, sighandler_t* oldHandler = nullptr);

    // Zero-based index of the child process in the order of forking; -1 for the parent
    int mChildIndex = -1;

    // Parent process id
    pid_t mParentPID = 0;

    // Old (previous) SIGCHLD signal handler
    sighandler_t mOld_SIGCHLD_handler = nullptr;

protected:
    // Shared memory array that holds children completion status.
    unsigned char* mIsChildDone = nullptr;
    size_t mIsChildDoneSize = 0;            // Size of allocated shared memory

    // All forked children processes PIDs and running status
    std::vector<ChildPID> mChildrenPIDs;

    // Block parent until all children complete
    bool mWaitForAll = true;
};

//
// Helper macros to log info/error messages
//
#include <sstream>  // std:::stringstream

#ifndef PROCESS_POOL_INFO
#define PROCESS_POOL_INFO(msg) \
    do { \
         std::stringstream buf; \
         buf << "[INFO][" << __FILE__ << ":" << __LINE__ << "] " << __func__ << ": " << msg; \
         OnInfo(buf.str()); \
    } while(0);
#endif // PROCESS_POOL_INFO

#ifndef PROCESS_POOL_ERROR
#define PROCESS_POOL_ERROR(msg) \
    do { \
         std::stringstream buf; \
         buf << "[ERROR][" << __FILE__ << ":" << __LINE__ << "] " << __func__ << ": " << msg; \
         OnError(buf.str()); \
    } while(0);
#endif // PROCESS_POOL_ERROR

//
// ProcessPool class implementation
//
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>       // stat
#include <assert.h>         // assert
#include <sys/mman.h>       // mmap
#include <iostream>         // std::cout

inline ProcessPool::~ProcessPool()
{
    // If we are parent then delete children completion status array
    // in shared memory (if we have any)
    if(IsParent())
        DeleteCompletionStatusArray();
}

inline bool ProcessPool::PreFork(int totalChildren)
{
    mChildrenPIDs.clear();
    mChildIndex = -1;
    mParentPID = getpid();
    mOld_SIGCHLD_handler = nullptr;

    // Delete children completion status array since number of children might changes
    DeleteCompletionStatusArray();

    bool result = false; // Initially
    while(true)
    {
        // Ignore the SIGCHLD to prevent children from transforming into
        // zombies so we don't need to wait and reap them.
        if(!SetSigAction(SIGCHLD, SIG_IGN, &mOld_SIGCHLD_handler))
        {
            std::string errmsg = strerror(errno);
            PROCESS_POOL_ERROR("sigaction(SIGCHLD) failed because " << errmsg);
            break;
        }

        // Create children completion status array in shared memory
        if(!CreateCompletionStatusArray(totalChildren))
        {
            PROCESS_POOL_ERROR("Couldn't create children completion status array in shared memory");
            break;
        }

        result = true;
        break;
    }

    // If we failed then clean up
    if(!result)
        PostFork();

    return result;
}

inline void ProcessPool::PostFork()
{
    // Restore the original SIGCHLD handler
    if(mOld_SIGCHLD_handler && !SetSigAction(SIGCHLD, mOld_SIGCHLD_handler))
    {
        std::string errmsg = strerror(errno);
        PROCESS_POOL_ERROR("sigaction(SIGCHLD old) failed because " << errmsg);
    }

    // Delete children completion status array in shared memory (if we have any)
    DeleteCompletionStatusArray();
}

// Fork totalChildren number of children and wait for them to complete.
// Note: We ignore SIGCHLD signal to prevent children from transforming into zombies
inline bool ProcessPool::Fork(int totalChildren, int maxConcurrentChildren)
{
    assert(IsParent());

    // Initial setup, create original signal handlers
    if(!PreFork(totalChildren))
        return false;

    // Vector of running children ids (initialized with default constructor)
    assert(mChildrenPIDs.empty());
    mChildrenPIDs.resize(totalChildren, ChildPID());

    // Fork child processes...
    int maxChildCount = std::min(totalChildren, maxConcurrentChildren);
    int childCount = 0;  // Number or currently running children

    PROCESS_POOL_INFO("Forking " << totalChildren << " children processes using "
                      << maxChildCount << " processors in parallel");

    // Pre-fork notification - for profiling, etc.
    OnNotify(NOTIFY_TYPE::PRE_FORK);

    bool result = true;
    for(int i = 0; i < totalChildren; i++)
    {
        if(childCount == maxChildCount)
        {
            // We are running maximum number of children.
            // We have to wait for some child to complete before continue.
            PROCESS_POOL_INFO("childCount=" << childCount << ", maxChildCount=" << maxChildCount
                              << ": waiting for any child to complete before forking another one");

            bool isCrashed = false;
            pid_t completedChildPID = WaitForOne(&isCrashed);
            if(isCrashed)
            {
                result = false;
                break;
            }
            else if(completedChildPID == 0)
                childCount = 0; // All children are done
            else
                childCount--;   // One child is done

            // We can fork another child now
            PROCESS_POOL_INFO("childCount=" << childCount << ", maxChildCount=" << maxChildCount
                              << ": we can now fork another child");
        }

        // Flush all parent's open output streams
        fflush(nullptr);

        // Fork a child
        pid_t childPID = fork();

        if(childPID < 0)
        {
            std::string errmsg = strerror(errno);
            PROCESS_POOL_ERROR("Parent " << mParentPID << " couldn't fork child " << i << " because " << errmsg);
            result = false;
            break;
        }
        else if(childPID == 0)
        {
            // Running as a child.
            mChildIndex = i;
            PROCESS_POOL_INFO("Child " << mChildIndex << " (" << getpid() << ") is running");
            return true;
        }

        // Running as a parent...
        PROCESS_POOL_INFO("Parent " << mParentPID << " forked child " << i << " (" << childPID << ")");

        // Child forking notification - for profiling, etc.
        OnNotify(NOTIFY_TYPE::CHILD_FORK);

        mChildrenPIDs[i].pid = childPID;
        mChildrenPIDs[i].status = CHILD_STATUS::RUNNING;

        childCount++;
    }

    // We must be parent if we are here
    assert(IsParent());

    if(!result)
    {
        // Something went wrong
        KillAll();      // Terminate children we've started
        PostFork();     // Restore the original signal handlers
    }
    else
    {
        // Post-fork notification - for profiling, etc.
        OnNotify(NOTIFY_TYPE::POST_FORK);

        // Wait for all children to complete if we have to
        if(mWaitForAll)
        {
            result = WaitForAll();
            KillAll();  // Terminate idle children processes
            PostFork(); // Restore the original signal handlers
        }
    }

    return result;
}

// Wait for children processes to complete
inline bool ProcessPool::WaitForAll()
{
    // Running as the parent. Wait for children to complete run.
    PROCESS_POOL_INFO("Waiting for children processes to complete...");

    // Wait for all children to complete or any child crashed
    bool isCrashed = false;

    while(true)
    {
        if(WaitForOne(&isCrashed) == 0)
            break;  // All children are done
        else if(isCrashed)
            break;  // Some child has crashed
    }

    if(!isCrashed)
    {
        // At this point all children are either exited or alive but idle.
        PROCESS_POOL_INFO("All children completed");

        // Send "All children done" notification - for profiling, clean up, etc.
        OnNotify(NOTIFY_TYPE::CHILDREN_DONE);
    }

    return !isCrashed;
}

inline void ProcessPool::Exit(bool status, bool keepIdle /*= false*/)
{
    if(IsParent())
    {
        PROCESS_POOL_ERROR("This method is not allowed in the parent process");
        return;
    }

    // Flush all open streams to make sure we don't miss any output
    fflush(nullptr);

    // If child is succeeded (status == true), then update child's
    // done flag and enter idle mode if required.
    // If child is failed (status != true), then just exit here.
    // Once parent sees that child is no longer running, it will
    // treat it as crashed and hence terminate remaining children.
    if(status)
    {
        assert(mIsChildDone);
        mIsChildDone[mChildIndex] = 1; // TODO: Do we need to do this under lock?

        // Do we need to exit child now OR keep it it looping (idle)
        // until terminated (by parent) or if parent is no longer alive?
        if(keepIdle)
        {
            // If child uses shared memory or other shared resourced then we need
            // to keep the child alive in order for parent (or siblings) to have
            // access to it.Once the child exits, the system might release
            // that shared memory so it will not be available for others.
            while(IsProcessAlive(mParentPID))
                usleep(500000); // 500 ms (half second)

            // If we are here then parent is no longer alive (crashed or terminated).
            PROCESS_POOL_INFO("Child " << mChildIndex << " (" << getpid() << ")"
                              << " exiting because parent " << mParentPID << " is no longer alive.");
        }
    }
    else
    {
        PROCESS_POOL_ERROR("Child " << mChildIndex << " (" << getpid() << ") has failed");
    }

    // Exit child with _exit() to tell the OS to ignore the its completion
    _exit(status ? 0 : 1);
}

// Kill all running children and wait for them to exit
inline void ProcessPool::KillAll()
{
    if(mChildrenPIDs.empty())
        return; // No running children processes to terminate

    // Kill all the running children...if we have any
    bool haveRunningChildren = false;
    int childIndex = 0;

    // Kill all running or idle children
    for(ChildPID& child : mChildrenPIDs)
    {
        if(child.status != CHILD_STATUS::NOT_RUNNING)
        {
            if(IsProcessAlive(child.pid))
            {
                haveRunningChildren = true;
                PROCESS_POOL_INFO("Parent " << getpid() << " terminates child " << childIndex << " (" << child.pid << ")");
                kill(child.pid, SIGKILL /*9*/);
            }
            else
            {
                child.status = CHILD_STATUS::NOT_RUNNING;
            }
        }
        childIndex++;
    }

    if(!haveRunningChildren)
        return;

    // Wait until all children are gone
    const int SLEEP_MICROSEC = 10000; // 10 ms

    while(true)
    {
        haveRunningChildren = false;

        for(ChildPID& child : mChildrenPIDs)
        {
            if(child.status != CHILD_STATUS::NOT_RUNNING)
            {
                if(IsProcessAlive(child.pid))
                    haveRunningChildren = true;
                else
                    child.status = CHILD_STATUS::NOT_RUNNING;
            }
        }

        // Are there any running children?
        if(!haveRunningChildren)
            break;

        // Some children are still running
        usleep(SLEEP_MICROSEC); // sleep for SLEEP_MICROSEC and check again
    }
}

// Wait for child to complete its task using high-speed loop
// Returns:
//   <process id> and "crashed status" of the completed child
//   0  if all children completed
inline pid_t ProcessPool::WaitForOne(bool* isCrashed)
{
    // We are going to use a high speed poll loop to watch completion status.
    // This will assure microseconds class restarts rather than using
    // exit()/wait3() approach, which requires both full shutdown of the child
    // and parent rescheduling by the operating system. Since children don't
    // need to be reaped, we can either kill() them later or have them exit
    // with _exit() to tell the OS to ignore theirs completion.
    assert(IsParent());
    assert(mIsChildDone);

    // Pool loop frequency
    const int SLEEP_MICROSEC = 10000;   // 10 ms

    // How often to check for crashed children
    const int CRASH_TEST_INTERVAL  = 10;  // 100 ms (10 * SLEEP_MICROSEC)

    size_t childrenCount = mChildrenPIDs.size();
    pid_t childPID = 0;

    // If none of the children completed within FIRST_CRASH_TEST_INTERVAL seconds,
    // then check if any of them crashes.
    // Note: We will do ongoing test in NEXT_CRASH_TEST_INTERVAL seconds
    int crashTestTimer = CRASH_TEST_INTERVAL;

    bool haveRunningChildren = false;

    // Do we have any completed children?
    while(true)
    {
        // Check completion status of the every child
        haveRunningChildren = false;

        for(size_t childIndex = 0; childIndex < childrenCount; childIndex++)
        {
            if(mChildrenPIDs[childIndex].status != CHILD_STATUS::RUNNING)
                continue; // Skip the child that is not running or done

            childPID = mChildrenPIDs[childIndex].pid;

            if(mIsChildDone[childIndex])
            {
                // The child is done with its task.
                mChildrenPIDs[childIndex].status = CHILD_STATUS::DONE;

                // Note: The child might exited or still be idle. But in
                // either case, it has completed with its task.
                PROCESS_POOL_INFO("Child " << childIndex << " (" << childPID << ") complete");
                *isCrashed = false;
                return childPID;
            }

            // If no children completed within crashTestTimer seconds, then
            // check if any of them have crashed
            if(crashTestTimer == 0 && !IsProcessAlive(childPID))
            {
                // The child has crashed or failed (exited with an error)
                // TODO: Should we have a different CHILD_STATUS for a crashed child?
                mChildrenPIDs[childIndex].status = CHILD_STATUS::DONE;

                PROCESS_POOL_ERROR("Child " << childIndex << " (" << childPID << ") is no longer running (crashed or failed)");
                *isCrashed = true;
                return childPID;
            }

            // The child is still running
            haveRunningChildren = true;
        }

        // Do we have any running children?
        if(!haveRunningChildren)
            break; // All children are done

        // Reset the crash timer if expired
        if(crashTestTimer == 0)
            crashTestTimer = CRASH_TEST_INTERVAL;

        // Some children are still running
        usleep(SLEEP_MICROSEC); // sleep for SLEEP_MICROSEC and check again
        crashTestTimer--;
    }

    // All children are done
    *isCrashed = false;
    return 0;
}

inline bool ProcessPool::CreateCompletionStatusArray(int totalChildren)
{
    // Clean up first
    DeleteCompletionStatusArray();

    assert(mIsChildDone == nullptr);
    assert(mIsChildDoneSize == 0);

    // Get a shared memory
    size_t len = sizeof(unsigned char) * totalChildren;
    void* addr = ::mmap(nullptr, len, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    if(addr == MAP_FAILED)
    {
        std::string errmsg = strerror(errno);
        PROCESS_POOL_ERROR("mmap for " << len << " bytes failed with error \"" << errmsg << "\"");
        return false;
    }

    // Create children completion status array in a shared memory
    mIsChildDone = new (addr) unsigned char[totalChildren]{};
    mIsChildDoneSize = len;
    return true;
}

inline bool ProcessPool::DeleteCompletionStatusArray()
{
    if(mIsChildDone == nullptr)
    {
        mIsChildDoneSize = 0;
        return true; // Nothing to delete
    }

    bool result = true;
    if(::munmap(mIsChildDone, mIsChildDoneSize) < 0)
    {
        std::string errmsg = strerror(errno);
        PROCESS_POOL_ERROR("munmap failed with error \"" << errmsg << "\"");
        result = false;
    }

    mIsChildDone = nullptr;
    mIsChildDoneSize = 0;
    return result;
}

inline bool ProcessPool::SetSigAction(int signum, sighandler_t handler, sighandler_t* oldHandler /*= nullptr*/)
{
    struct sigaction sa, old_sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // To restart interrupted system calls

    int ret = sigaction(signum, &sa, &old_sa);
    if(ret == 0 && oldHandler)
        *oldHandler = old_sa.sa_handler;
    return (ret == 0);
}

inline bool ProcessPool::IsProcessAlive(pid_t pid)
{
//    // Assume we have proc file system
//    char pidpath[32]{};
//    sprintf(pidpath, "/proc/%d", pid);
//    struct stat pidstat;
//    int rc = stat(pidpath, &pidstat);
//    if(rc == -1 && errno != 0)
//        return false;
//    return true;

    return (kill(pid, 0) == 0);
}




#endif // _PROCESS_POOL_HPP_
