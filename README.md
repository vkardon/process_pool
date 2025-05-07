# process_pool

C++ ProcessPool class to parallelize tasks by forking multiple processes and waiting for them to finish.

Also includes an experimental ProcessQueue implementation: forks multiple persistent processes; the parent queues tasks, and available children pick them up for natural load balancing.
