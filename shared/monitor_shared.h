#ifndef MONITOR_SHARED_H
#define MONITOR_SHARED_H

#include <linux/ioctl.h>

// Structure passed from Supervisor to Kernel
struct container_config {
    int pid;
    unsigned long soft_limit_kb;
    unsigned long hard_limit_kb;
};

// IOCTL command definitions
#define MONITOR_IOC_MAGIC 'k'
#define REGISTER_CONTAINER _IOW(MONITOR_IOC_MAGIC, 1, struct container_config)

#endif
