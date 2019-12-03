#include <linux/init.h>
#include <linux/module.h>

#include <linux/sched.h> /* Used for current */
MODULE_LICENSE("Dual BSD/GPL");

static int hello_init(void)
{
    printk(KERN_ALERT "Hello, world\n");
    printk(KERN_INFO "The process is \"%s\" (pid %i)\n", current->comm, current->pid);

    return 0;
}

module_init(hello_init);