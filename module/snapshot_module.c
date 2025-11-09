// snapshot_module.c
// Lightweight snapshot registry: validate PID, hold task ref, then release on restore/rebind.
// NOT a full memory/register checkpoint-restore. Provides safe ioctl-based plumbing.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/uidgid.h>
#include <linux/ioctl.h>

#define DEVICE_NAME "snapshotctl"

/* IOCTLs:
 * - IOCTL_SNAPSHOT: arg is pid (passed as integer value)
 * - IOCTL_RESTORE: arg is pointer to struct snap_ioc (oldpid,newpid)
 *   if newpid == 0 => release and remove snapshot entry
 *   if newpid != 0 => attempt to rebind snapshot entry to newpid (transfer ref)
 */
struct snap_ioc {
    pid_t oldpid;
    pid_t newpid;
};

#define IOCTL_SNAPSHOT _IOW('s', 1, int)
#define IOCTL_RESTORE  _IOW('s', 2, struct snap_ioc)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("snapshotter");
MODULE_DESCRIPTION("Lightweight snapshot registry kernel module (validation & refs)");

struct snap_entry {
    pid_t pid;
    struct task_struct *task; /* held reference */
    kuid_t uid;
    char comm[TASK_COMM_LEN];
};

#define MAX_SNAPS 64
static struct snap_entry snaps[MAX_SNAPS];
static int snaps_count = 0;

static int major = 0;

/* find snapshot index for pid */
static int find_snap(pid_t pid)
{
    int i;
    for (i = 0; i < snaps_count; i++) {
        if (snaps[i].pid == pid)
            return i;
    }
    return -1;
}

/* take snapshot: validate task exists and is user process; keep task ref */
static long do_snapshot(pid_t pid)
{
    struct pid *pid_struct;
    struct task_struct *task;

    if (snaps_count >= MAX_SNAPS) {
        pr_err("snapshot_module: snapshot table full\n");
        return -ENOMEM;
    }

    pid_struct = find_get_pid(pid);
    if (!pid_struct) {
        pr_err("snapshot_module: find_get_pid failed for %d\n", pid);
        return -EINVAL;
    }

    task = pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);

    if (!task) {
        pr_err("snapshot_module: pid_task returned NULL for %d\n", pid);
        return -EINVAL;
    }

    /* reject kernel threads */
    if (task->flags & PF_KTHREAD) {
        pr_err("snapshot_module: pid %d is kernel thread, cannot snapshot\n", pid);
        return -EINVAL;
    }

    /* require a user mm (user-space process) */
    if (!task->mm) {
        pr_err("snapshot_module: pid %d has no mm_struct (likely short-lived or kernel thread)\n", pid);
        return -EINVAL;
    }

    /* increase task refcount and store */
    get_task_struct(task);

    snaps[snaps_count].pid = pid;
    snaps[snaps_count].task = task;
    snaps[snaps_count].uid = task->cred->uid;
    get_task_comm(snaps[snaps_count].comm, task);
    snaps_count++;

    pr_info("snapshot_module: recorded snapshot for pid=%d comm=%s uid=%u\n",
            pid, snaps[snaps_count-1].comm, from_kuid(&init_user_ns, snaps[snaps_count-1].uid));

    return 0;
}

/* helper to validate a candidate task for rebind */
static int validate_user_task(struct task_struct *task)
{
    if (!task) return -EINVAL;
    if (task->flags & PF_KTHREAD) {
        pr_err("snapshot_module: validate: kernel thread\n");
        return -EINVAL;
    }
    if (!task->mm) {
        pr_err("snapshot_module: validate: candidate has no mm\n");
        return -EINVAL;
    }
    return 0;
}

/* restore/rebind:
 * - if newpid == 0: release stored ref and remove entry
 * - if newpid != 0: find newpid's task_struct, validate, then replace stored ref with new task
 */
static long do_restore_rebind(pid_t oldpid, pid_t newpid)
{
    int idx = find_snap(oldpid);
    if (idx < 0) {
        pr_err("snapshot_module: restore: no snapshot found for old pid %d\n", oldpid);
        return -EINVAL;
    }

    if (newpid == 0) {
        /* simple release/remove */
        if (snaps[idx].task) {
            put_task_struct(snaps[idx].task);
            snaps[idx].task = NULL;
        }
        snaps_count--;
        if (idx < snaps_count)
            snaps[idx] = snaps[snaps_count];
        pr_info("snapshot_module: removed snapshot entry for pid=%d (restored)\n", oldpid);
        return 0;
    } else {
        struct pid *pid_struct;
        struct task_struct *new_task;

        pid_struct = find_get_pid(newpid);
        if (!pid_struct) {
            pr_err("snapshot_module: rebind: find_get_pid failed for new pid %d\n", newpid);
            return -EINVAL;
        }
        new_task = pid_task(pid_struct, PIDTYPE_PID);
        put_pid(pid_struct);

        if (!new_task) {
            pr_err("snapshot_module: rebind: pid_task NULL for new pid %d\n", newpid);
            return -EINVAL;
        }

        /* validate candidate */
        if (validate_user_task(new_task) < 0)
            return -EINVAL;

        /* attempt to transfer the reference: get new, put old */
        get_task_struct(new_task); /* take ref on new */
        if (snaps[idx].task)
            put_task_struct(snaps[idx].task); /* drop old */

        snaps[idx].task = new_task;
        snaps[idx].pid = newpid;
        snaps[idx].uid = new_task->cred->uid;
        get_task_comm(snaps[idx].comm, new_task);

        pr_info("snapshot_module: rebound snapshot oldpid=%d -> newpid=%d comm=%s uid=%u\n",
                oldpid, newpid, snaps[idx].comm, from_kuid(&init_user_ns, snaps[idx].uid));

        return 0;
    }
}

/* ioctl: snapshot uses pid passed directly in arg (integer)
 * restore expects pointer to struct snap_ioc passed from userland
 */
static long snapshot_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    long ret = -EINVAL;

    switch (cmd) {
    case IOCTL_SNAPSHOT: {
        pid_t pid = (pid_t)arg;
        pr_debug("snapshot_module: ioctl SNAPSHOT pid=%d\n", pid);
        ret = do_snapshot(pid);
        break;
    }
    case IOCTL_RESTORE: {
        struct snap_ioc ioc;
        if (copy_from_user(&ioc, (void __user *)arg, sizeof(ioc))) {
            pr_err("snapshot_module: restore: copy_from_user failed\n");
            ret = -EFAULT;
            break;
        }
        pr_debug("snapshot_module: ioctl RESTORE oldpid=%d newpid=%d\n", ioc.oldpid, ioc.newpid);
        ret = do_restore_rebind(ioc.oldpid, ioc.newpid);
        break;
    }
    default:
        pr_err("snapshot_module: unknown ioctl cmd=%u\n", cmd);
        ret = -EINVAL;
        break;
    }

    return ret;
}

static int snapshot_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int snapshot_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations snapshot_fops = {
    .owner = THIS_MODULE,
    .open = snapshot_open,
    .release = snapshot_release,
    .unlocked_ioctl = snapshot_ioctl,
};

static int __init snapshot_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &snapshot_fops);
    if (major < 0) {
        pr_err("snapshot_module: register_chrdev failed: %d\n", major);
        return major;
    }
    /* zero table */
    memset(snaps, 0, sizeof(snaps));
    snaps_count = 0;

    pr_info("snapshot_module: registered device /dev/%s with major %d\n", DEVICE_NAME, major);
    return 0;
}

static void __exit snapshot_exit(void)
{
    int i;
    /* release any held task refs */
    for (i = 0; i < snaps_count; i++) {
        if (snaps[i].task)
            put_task_struct(snaps[i].task);
    }
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("snapshot_module: unloaded\n");
}

module_init(snapshot_init);
module_exit(snapshot_exit);
