#include <linux/init.h>
#include <linux/module.h>

static void hello_exit(void)
{
    printk(KERN_ALERT "Goodbye, cruel world\n");
}

module_exit(hello_exit);