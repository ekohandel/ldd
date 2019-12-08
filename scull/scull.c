
#include <linux/init.h>
#include <linux/module.h>

#include <linux/cdev.h>
#include <linux/fs.h>       /* used for chrdev operations */
#include <linux/slab.h>     /* kmalloc                    */
#include <linux/errno.h>    /* error codes                */
#include <linux/uaccess.h>  /* copy to user               */


MODULE_LICENSE("Dual BSD/GPL");

static int scull_major   = 0;
static int scull_minor   = 0;
static int scull_nr_devs = 4;
static int scull_quantum = 4000;
static int scull_qset    = 1000;

module_param(scull_major,   int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);
module_param(scull_qset,    int, S_IRUGO);

struct scull_qset {
    void              **data;
    struct scull_qset  *next;
};

struct scull_dev {
    struct scull_qset   *data;        /* pointer to first quantum set   */
    int                  quantum;     /* size of each quantum           */
    int                  qset;        /* number of quantums in set      */
    unsigned long        size;
    struct cdev          cdev;        /* Char device structure          */
};

static struct scull_dev *scull_devices;

static struct class *scull_class;

/*
 * Empty out the scull device; must be called with the device
 * semaphore held.
 */
static int scull_trim(struct scull_dev *dev)
{
    struct scull_qset *next;
    struct scull_qset *dptr;
    int                qset = dev->qset;
    int                i;

    for (dptr = dev->data; dptr; dptr = next) { /* all the list items */
        if (dptr->data) {
            for (i = 0; i < qset; i++)
                kfree(dptr->data[i]);
            kfree(dptr->data);
            dptr->data = NULL;
        }
        next = dptr->next;
        kfree(dptr);
    }
    dev->size    = 0;
    dev->quantum = scull_quantum;
    dev->qset    = scull_qset;
    dev->data    = NULL;

    return 0;
}

static int scull_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev; /* device information */

    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev; /* for other methods */

    /* if open is write-only, then trim the device */
    if (O_WRONLY == (filp->f_flags & O_ACCMODE))
        scull_trim(dev);

    return 0;
}

static int scull_release(struct inode *inode, struct file *filp)
{
    filp->private_data = NULL;

    return 0;
}

struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
    struct scull_qset *qs = dev->data;

    /* Allocate first qset explicitly if need be */
    if (!qs) {
        qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
        if (qs == NULL)
            return NULL;  /* Never mind */
        memset(qs, 0, sizeof(struct scull_qset));
    }

    /* Then follow the list */
    while (n--) {
        if (!qs->next) {
            qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
            if (qs->next == NULL)
                return NULL;  /* Never mind */
            memset(qs->next, 0, sizeof(struct scull_qset));
        }
        qs = qs->next;
        continue;
    }
    return qs;
}

static ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_qset *dptr;
    int                item;
    int                s_pos;
    int                q_pos;
    int                rest;
    struct scull_dev  *dev      = filp->private_data;
    int                quantum  = dev->quantum;
    int                qset     = dev->qset;
    int                itemsize = quantum * qset; /* how many bytes in the listitem */
    ssize_t            retval   = 0;

    if (*f_pos >= dev->size)
        goto out;

    if (*f_pos + count > dev->size)
        count = dev->size - *f_pos;

    /* find listitem, qset index, and offset in the quantum */
    item  = (long)*f_pos / itemsize;
    rest  = (long)*f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    /* follow the list up to the right position (defined elsewhere) */
    dptr = scull_follow(dev, item);

    if (!dptr || !dptr->data || !dptr->data[s_pos])
        goto out; /* don't fill holes */

    /* read only up to the end of this quantum */
    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

out:
    return retval;
}

static ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_qset *dptr;
    int                item;
    int                s_pos;
    int                q_pos;
    int                rest;
    struct scull_dev  *dev      = filp->private_data;
    int                quantum  = dev->quantum;
    int                qset     = dev->qset;
    int                itemsize = quantum * qset;
    ssize_t            retval   = -ENOMEM; /* value used in "goto out" statements */

    /* find listitem, qset index and offset in the quantum */
    item  = (long)*f_pos / itemsize;
    rest  = (long)*f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    /* follow the list up to the right position */
    dptr = scull_follow(dev, item);
    if (!dptr)
        goto out;

    if (!dptr->data) {
        dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
        if (!dptr->data)
            goto out;
        memset(dptr->data, 0, qset * sizeof(char *));
    }

    if (!dptr->data[s_pos]) {
        dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
        if (!dptr->data[s_pos])
            goto out;
    }

    /* write only up to the end of this quantum */
    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_from_user(dptr->data[s_pos]+q_pos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

    /* update the size */
    if (dev->size < *f_pos)
        dev->size = *f_pos;

out:
    return retval;
}

struct file_operations scull_fops = {
    .owner   = THIS_MODULE,
    .open    = scull_open,
    .release = scull_release,
    .read    = scull_read,
    .write   = scull_write,
};

static void scull_cleanup_module(void)
{
    int i;
    dev_t devno = MKDEV(scull_major, scull_minor);

    if (scull_devices) {
        for (i = 0; i < scull_nr_devs; i++) {
            cdev_del(&scull_devices[i].cdev);
            device_destroy(scull_class, MKDEV(scull_major, scull_minor + i));
        }
        kfree(scull_devices);
    }

    class_destroy(scull_class);

    unregister_chrdev_region(devno, scull_nr_devs);
}

static void scull_setup_cdev(struct scull_dev *dev, int index)
{
    int err;
    int devno = MKDEV(scull_major, scull_minor + index);

    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops   = &scull_fops;

    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
        printk(KERN_NOTICE "scull: error %d adding scull%d\n", err, index);

    device_create(scull_class, NULL, devno, NULL, "scull%d", index);
}

static int __init scull_init_module(void)
{
    dev_t devno;
    int result = 0;
    int i;

    /*
     * Use the major number if provided, otherwise allocate one dynamically
     */
    if (scull_major) {
        devno = MKDEV(scull_major, scull_minor);
        result = register_chrdev_region(devno, scull_nr_devs, "scull");
    } else {
        result = alloc_chrdev_region(&devno, scull_minor, scull_nr_devs, "scull");
        scull_major = MAJOR(devno);
    }

    if (result < 0) {
        printk(KERN_DEBUG "scull: can't get major %d\n", scull_major);
        return result;
    }

    /*
     * Allocate memory for devices
     */
    scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
    if (!scull_devices) {
        result = -ENOMEM;
        goto fail;
    }
    memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));

    scull_class = class_create(THIS_MODULE, "scull");
    if (IS_ERR(scull_class)) {
        printk(KERN_ERR "scull: error creating class\n");
        result = PTR_ERR(scull_class);
        goto free_devices;
    }

    for (i = 0; i < scull_nr_devs; i++)
        scull_setup_cdev(&scull_devices[i], i);

    return 0;

 free_devices:
    kfree(scull_devices);
 fail:
    scull_cleanup_module();

    return result;
}

static void __exit scull_exit(void)
{
    scull_cleanup_module();
}

module_init(scull_init_module);
module_exit(scull_exit);