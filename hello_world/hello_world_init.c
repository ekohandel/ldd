#include <linux/init.h>
#include <linux/module.h>

#include <linux/sched.h> /* Used for current */
MODULE_LICENSE("Dual BSD/GPL");

static char *whom = "world";
static int howmany = 1;

module_param(howmany, int, S_IRUGO);
module_param(whom, charp, S_IRUGO);

static int __init hello_init(void)
{
    int i;

    printk(KERN_INFO "The process is \"%s\" (pid %i)\n", current->comm, current->pid);

    for (i = 0; i < howmany; i++)
        printk(KERN_ALERT "Hello, %s\n", whom);

    return 0;
}

module_init(hello_init);