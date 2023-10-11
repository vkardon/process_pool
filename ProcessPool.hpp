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

    // Shared memory array that holds children completion status.
    unsigned char* mIsChildDone = nullptr;
    size_t mIsChildDoneSize = 0;            // Size of allocated shared memory

    // Old (previous) SIGCHLD signal handler
    sighandler_t mOld_SIGCHLD_handler = nullptr;

protected:
    // All forked children processes PIDs and running status
    std::vector<ChildPID> mChildrenPIDs;

    // Block parent until all children complete
    bool mWaitForAll = true;
};

//
// Helper macros to log info/error messages
//
#include <sstream>  // std:::stringstream

#ifndef INFOMSG
#define INFOMSG(msg) \
    do { \
         std::stringstream buf; \
         buf << "[INFO][" << __FILE__ << ":" << __LINE__ << "] " << __func__ << ": " << msg; \
         OnInfo(buf.str()); \
    } while(0);
#endif // INFOMSG

#ifndef ERRORMSG
#define ERRORMSG(msg) \
    do { \
         std::stringstream buf; \
         buf << "[ERROR][" << __FILE__ << ":" << __LINE__ << "] " << __func__ << ": " << msg; \
         OnError(buf.str()); \
    } while(0);
#endif // ERRORMSG

#endif // _PROCESS_POOL_HPP_
