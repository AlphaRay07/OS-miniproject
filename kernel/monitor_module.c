#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include "monitor_shared.h"
#include "../shared/monitor_shared.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Manav");
MODULE_DESCRIPTION("OS Project Container Monitor");

#define DEVICE_NAME "container_monitor"
static int major_number;
static struct timer_list monitor_timer;

struct monitored_process {
    int pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
    struct list_head list;
};

static LIST_HEAD(process_list);
static DEFINE_SPINLOCK(list_lock);

// The "Police" function that runs every 1 second
void check_callback(struct timer_list *t) {
    struct monitored_process *entry, *tmp;
    struct task_struct *task;
    unsigned long rss_kb;

    spin_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &process_list, list) {
        task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);

        if (task && task->mm) {
            // Calculate RSS in KB
            rss_kb = get_mm_rss(task->mm) * (PAGE_SIZE / 1024);

            if (rss_kb > entry->hard_limit) {
                pr_info("Monitor: [HARD LIMIT] Killing PID %d (%lu KB)\n", entry->pid, rss_kb);
                send_sig(SIGKILL, task, 1);
            } else if (rss_kb > entry->soft_limit) {
                pr_warn("Monitor: [SOFT LIMIT] Warning PID %d (%lu KB)\n", entry->pid, rss_kb);
            }
        } else {
            // Process is gone, remove from list (Stale entry cleanup)
            list_del(&entry->list);
            kfree(entry);
        }
    }
    spin_unlock(&list_lock);

    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct container_config config;
    struct monitored_process *new_proc;

    if (cmd == REGISTER_CONTAINER) {
        if (copy_from_user(&config, (struct container_config __user *)arg, sizeof(config)))
            return -EFAULT;

        new_proc = kmalloc(sizeof(*new_proc), GFP_KERNEL);
        if (!new_proc) return -ENOMEM;

        new_proc->pid = config.pid;
        new_proc->soft_limit = config.soft_limit_kb;
        new_proc->hard_limit = config.hard_limit_kb;

        spin_lock(&list_lock);
        list_add(&new_proc->list, &process_list);
        spin_unlock(&list_lock);

        printk(KERN_INFO "Monitor: Registered PID %d\n", new_proc->pid);
        return 0;
    }
    return -EINVAL;
}

static struct file_operations fops = {
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void) {
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) return major_number;

    timer_setup(&monitor_timer, check_callback, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));

    printk(KERN_INFO "Monitor: Loaded. Major number: %d\n", major_number);
    return 0;
}

static void __exit monitor_exit(void) {
    struct monitored_process *entry, *tmp;
    del_timer_sync(&monitor_timer);

    spin_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &process_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    spin_unlock(&list_lock);

    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_INFO "Monitor: Unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
