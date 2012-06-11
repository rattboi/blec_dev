#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/timer.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <asm/uaccess.h>

#define DRIVER_AUTHOR "Bradon Kanyid & Kevin Riedl"
#define DRIVER_DESC "BLECMU USB Driver"
#define DRIVER_NAME "blec_usb"

#define MAX_DEV 15

#define LABJACK_VENDOR_ID 0x0cd5
#define LABJACK_HV_PRODUCT_ID 0x0003

#define WQ_NAME_A "blec_wq_port_a"
#define WQ_NAME_B "blec_wq_port_b"

static int blec_probe(struct usb_interface *interface, const struct usb_device_id *id);
static void blec_disconnect(struct usb_interface *interface);

static   int  access_count;
module_param(access_count, int, S_IRUGO);

static struct blec_mod {
  dev_t                 t_node;
  struct class          *dev_class;
  struct usb_interface  *intf_pool[MAX_DEV * 3];
  struct mutex          *usb_rw_mutex;
} blec_mod_g;

struct blec_dev {
  struct usb_device        *udev;
  struct usb_interface     *interface;
  struct cdev              cdev_a;
  struct cdev              cdev_b;
  struct cdev              cdev_c;
  dev_t                    cdev_a_t;
  dev_t                    cdev_b_t;
  dev_t                    cdev_c_t;

  __u8                     bulk_in_endpointAddr;
  __u8                     bulk_out_endpointAddr;

  struct workqueue_struct  *port_a_tmr_wq;
  struct delayed_work      port_a_tmr_w;
  struct mutex             *port_a_mutex;
  int                      port_a_voltage;
  int                      port_a_file_count;

  struct workqueue_struct  *port_b_tmr_wq;
  struct delayed_work      port_b_tmr_w;
  int                      port_b_delay;
  int                      port_b_mode;
  unsigned long int        port_b_last_jiffies;
  int                      port_b_file_count;

  int                      port_c_file_count;
};

static struct usb_device_id lj_table[] = {
  { USB_DEVICE(LABJACK_VENDOR_ID, LABJACK_HV_PRODUCT_ID) },
  { }
};

MODULE_DEVICE_TABLE(usb, lj_table);
static struct usb_driver blec_driver = {
  .name       = "blec_usb",
  .id_table   = lj_table,
  .probe      = blec_probe,
  .disconnect = blec_disconnect,
};

u16 extendedChecksum16(u8 *b, int n)
{
    int i, a = 0;

    //Sums bytes 6 to n-1 to a unsigned 2 byte value
    for( i = 6; i < n; i++ )
        a += (u16)b[i];

    return a;
}

u8 extendedChecksum8(u8 *b)
{
    int i, a, bb;

    //Sums bytes 1 to 5. Sums quotient and remainder of 256 division. Again,
    //sums quotient and remainder of 256 division.
    for( i = 1, a = 0; i < 6; i++ )
        a += (u16)b[i];

    bb=a / 256;
    a=(a - 256*bb) + bb;
    bb=a / 256;

    return (u8)((a - 256*bb) + bb);
}

void extendedChecksum(u8 *b, int n)
{
    u16 a;

    a = extendedChecksum16(b, n);
    b[4] = (u8)(a & 0xFF);
    b[5] = (u8)((a/256) & 0xFF);
    b[0] = extendedChecksum8(b);
}

static struct blec_dev* get_blec_dev(struct inode *inode)
{
  struct blec_dev *my_dev;
  struct usb_interface *intf;
  int subminor;
  int retval;

  subminor = iminor(inode);
  intf = blec_mod_g.intf_pool[subminor];

  if (!intf)
  {
    printk(KERN_INFO "BLEC_USB: Can't find device with minor number: %d\n",subminor);
    retval = -ENODEV;
    goto exit;
  }

  my_dev = usb_get_intfdata(intf);
  if (!my_dev)
  {
    printk(KERN_INFO "BLEC_USB: Can't find device, again...\n");
    retval = -ENODEV;
    goto exit;
  }

  return my_dev;

exit:
  return NULL;
}

static int labjack_access(struct blec_dev *my_dev, u8 *cmd_to_write, int write_length, u8 *response, int read_length)
{
  int write_amount, read_amount, retval;

  mutex_lock(blec_mod_g.usb_rw_mutex);

  extendedChecksum(cmd_to_write, write_length);

  retval = usb_bulk_msg(my_dev->udev,
                        usb_sndbulkpipe(my_dev->udev, my_dev->bulk_out_endpointAddr), 
                        cmd_to_write, write_length, &write_amount, (HZ*1)/10);
  if(response) 
    retval = usb_bulk_msg(my_dev->udev, 
                          usb_rcvbulkpipe(my_dev->udev, my_dev->bulk_in_endpointAddr), 
                          response, read_length, &read_amount, (HZ*1)/10);

  mutex_unlock(blec_mod_g.usb_rw_mutex);

  return retval;

}

static void port_a_work_callback(struct work_struct *taskp)
{
  u8 eio2_read_cmd[10] = {0x00, 0xF8, 0x02, 0x00, 0x00, 0x00, 0xAA, 1, 10, 31}; // AIN, AIN10 = EIO2, Single-ended
  u8 eio2_read_resp[12];
  int bits;
  int volts;
  int retval;

  struct blec_dev *my_dev = (struct blec_dev *)container_of(taskp, struct blec_dev, port_a_tmr_w.work);
  queue_delayed_work(my_dev->port_a_tmr_wq, &(my_dev->port_a_tmr_w), 1*HZ);

  retval = labjack_access(my_dev, eio2_read_cmd, 10, eio2_read_resp, 12);

  bits = (eio2_read_resp[10] << 8) + (eio2_read_resp[9]);
  volts = (bits*244)>>16;
  my_dev->port_a_voltage = volts;
}

static int port_a_open(struct inode *inode, struct file *file)
{
  struct blec_dev *my_dev;

  u8 eio2_config_io_cmd[12] = {0x00, 0xF8, 0x03, 0x0B, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x04 }; // ConfigIO
  u8 eio2_config_io_resp[12];
  int retval = 0;

  printk(KERN_INFO "PORTA: open called\n");

  my_dev = get_blec_dev(inode);

  if (!my_dev)
  {
    retval = -ENODEV;
    goto exit;
  }

  file->private_data = my_dev;

  printk(KERN_INFO "PORTA: filecount = %d\n", my_dev->port_a_file_count);
  if (my_dev->port_a_file_count++ == 0)
  {
    printk(KERN_INFO "PORTA: initting: filecount = %d\n", my_dev->port_a_file_count);

    retval = labjack_access(my_dev, eio2_config_io_cmd, 12, eio2_config_io_resp, 12);

    printk(KERN_INFO "PORTA: OPEN: error_code = %d", eio2_config_io_resp[6]);

    if (!my_dev->port_a_tmr_wq)
    {
      my_dev->port_a_tmr_wq = create_workqueue(WQ_NAME_A);
      INIT_DELAYED_WORK(&(my_dev->port_a_tmr_w), port_a_work_callback);
      
      queue_delayed_work(my_dev->port_a_tmr_wq, &(my_dev->port_a_tmr_w), 1 * HZ);
    }
  }

  printk(KERN_INFO "PORTA: open over\n");

exit:
  return retval;
}

static void port_a_destroy(struct blec_dev *my_dev)
{
    cancel_delayed_work(&(my_dev->port_a_tmr_w));

    if (my_dev->port_a_tmr_wq)
    {
      flush_workqueue(my_dev->port_a_tmr_wq);
      destroy_workqueue(my_dev->port_a_tmr_wq);
      my_dev->port_a_tmr_wq = NULL;
    }
}

static int port_a_release(struct inode *inode, struct file *file)
{
  struct blec_dev *my_dev;
  int retval = 0;

  printk(KERN_INFO "PORTA: release called\n");

  my_dev = get_blec_dev(inode);

  if (!my_dev)
  {
    printk(KERN_INFO "PORTA: release, no interface\n");
    retval = -ENODEV;
    goto exit;
  }

  if (--(my_dev->port_a_file_count) == 0)
  {
    printk(KERN_INFO "PORTA: Release: deleting: filecount = %d\n", my_dev->port_a_file_count);
    port_a_destroy(my_dev);
  }

exit:
  return 0;
}


static ssize_t port_a_read(struct file *file, char *buf, size_t count, loff_t *offset)
{
  struct blec_dev *my_dev;
  char airlock_open_string[] = "Airlock Open!";
  ssize_t len;
  my_dev = file->private_data;

  access_count++;

  mutex_lock(my_dev->port_a_mutex);
  while (my_dev->port_a_voltage < 100)
    msleep(100);
  mutex_unlock(my_dev->port_a_mutex);

  printk(KERN_INFO "Airlock Open!\n");

  len = min_t(ssize_t, count, sizeof(airlock_open_string));
  copy_to_user(buf, airlock_open_string, len);

  return len;
}

static struct file_operations port_a_fops = {
  .owner    = THIS_MODULE,
  .open     = port_a_open,
  .release  = port_a_release,
  .read     = port_a_read,
};

static void port_b_work_callback(struct work_struct *taskp)
{
  int retval;
  u8 fio4_buffer1[10] = {0x00, 0xF8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x84, 0x00};
  u8 fio4_buffer0[10] = {0x00, 0xF8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x04, 0x00};

  struct blec_dev *my_dev = (struct blec_dev *)container_of(taskp, struct blec_dev, port_b_tmr_w.work);

  my_dev->port_b_last_jiffies = jiffies;
  queue_delayed_work(my_dev->port_b_tmr_wq, &(my_dev->port_b_tmr_w), my_dev->port_b_delay*HZ);

  if (!(my_dev->port_b_mode))
    retval = labjack_access(my_dev, fio4_buffer1, 10, NULL, 0);
  else
    retval = labjack_access(my_dev, fio4_buffer0, 10, NULL, 0);

  my_dev->port_b_mode = !(my_dev->port_b_mode);
}

static int port_b_open(struct inode *inode, struct file *file)
{
  struct blec_dev *my_dev;
  int retval = 0;
  u8 fio4_buffer1[10] = {0x00, 0xF8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x84, 0x00};

  printk(KERN_INFO "PORTB: open called\n");

  my_dev = get_blec_dev(inode);

  if (!my_dev)
  {
    retval = -ENODEV;
    goto exit;
  }

  my_dev->port_b_delay = 60;
  my_dev->port_b_mode = 1;

  file->private_data = my_dev;

  printk(KERN_INFO "PORTB: open called\n");

  printk(KERN_INFO "PORTB: filecount = %d\n", my_dev->port_b_file_count);
  if (my_dev->port_b_file_count++ == 0)
  {
    printk(KERN_INFO "PORTB: initting: filecount = %d\n", my_dev->port_b_file_count);

    if (!my_dev->port_b_tmr_wq)
    {
      my_dev->port_b_tmr_wq = create_workqueue(WQ_NAME_B);
      INIT_DELAYED_WORK(&(my_dev->port_b_tmr_w), port_b_work_callback);
      
      queue_delayed_work(my_dev->port_b_tmr_wq, &(my_dev->port_b_tmr_w), my_dev->port_b_delay * HZ);
      my_dev->port_b_last_jiffies = jiffies;
    }
  }

  retval = labjack_access(my_dev, fio4_buffer1, 10, NULL, 0);

  printk(KERN_INFO "PORTB: open over\n");

exit:
  return retval;
}

static ssize_t port_b_read(struct file *file, char *buf, size_t count, loff_t *offset)
{
  struct blec_dev *my_dev;
  my_dev = file->private_data;

  access_count++;

  printk(KERN_INFO "jiffies since last: %lu\n", jiffies - my_dev->port_b_last_jiffies);
  printk(KERN_INFO "time since last: %lu\n", (jiffies - my_dev->port_b_last_jiffies)/HZ);

  return 0;
}

static ssize_t port_b_write(struct file *file, const char *buf, size_t count, loff_t *offset)
{
  struct blec_dev *my_dev;
  my_dev = file->private_data;

  access_count++;
  
  my_dev->port_b_delay = 10;

  return 0;
}

static void port_b_destroy(struct blec_dev *my_dev)
{
  cancel_delayed_work(&(my_dev->port_b_tmr_w));

  if (my_dev->port_b_tmr_wq)
  {
    flush_workqueue(my_dev->port_b_tmr_wq);
    destroy_workqueue(my_dev->port_b_tmr_wq);
    my_dev->port_b_tmr_wq = NULL;
  }
}

static int port_b_release(struct inode *inode, struct file *file)
{
  struct blec_dev *my_dev;
  int retval;

  u8 fio4_buffer0[10] = {0x00, 0xF8, 0x02, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x04, 0x00};

  printk(KERN_INFO "PORTB: release called\n");

  my_dev = get_blec_dev(inode);

  if (!my_dev)
  {
    retval = -ENODEV;
    goto exit;
  }

  if (--(my_dev->port_b_file_count) == 0)
  {
    printk(KERN_INFO "PORTB: Release: deleting: filecount = %d\n", my_dev->port_b_file_count);
    port_b_destroy(my_dev);
  }

  retval = labjack_access(my_dev, fio4_buffer0, 10, NULL, 0);

exit:
  return retval;
}

static struct file_operations port_b_fops = {
  .owner    = THIS_MODULE,
  .open     = port_b_open,
  .read     = port_b_read,
  .write    = port_b_write,
  .release  = port_b_release,
};

static int port_c_open(struct inode *inode, struct file *file)
{
  struct blec_dev *my_dev;
  int retval = 0;

  printk(KERN_INFO "PORTC: open called\n");

  my_dev = get_blec_dev(inode);

  if (!my_dev)
  {
    retval = -ENODEV;
    goto exit;
  }

  file->private_data = my_dev;
  printk(KERN_INFO "PORTC: open over\n");
exit:
  return retval;
}

static ssize_t port_c_read(struct file *file, char *buf, size_t count, loff_t *offset)
{
  u8 fb_cmd_buf[10] = {0x00, 0xF8, 0x02, 0x00, 0x00, 0x00, 0x88, 1, 30, 31 };
  u8 fb_cmd_resp[12];

  u8 read_mem_cmd_buf[8] = {0x00, 0xF8, 0x01, 0x2D,0x00,0x00,0x00,0x02};
  u8 read_mem_cmd_resp[40];

  struct blec_dev *my_dev;
  int retval;
  int i;
  int bits;
  unsigned long slope;
  unsigned long long temp_in_k;
  int temp_in_c;

  access_count++;

  my_dev = file->private_data;

  retval = labjack_access(my_dev, fb_cmd_buf, 10, fb_cmd_resp, 12);
  retval = labjack_access(my_dev, read_mem_cmd_buf, 8, read_mem_cmd_resp, 40);
  
  bits = (fb_cmd_resp[10] << 8) + (fb_cmd_resp[9]);

  slope = 0;
  for (i = 0; i < 8; i++)
    slope = (slope << 8) | read_mem_cmd_resp[15-i];

  temp_in_k = bits * slope;
  temp_in_k >>= 32;

  printk(KERN_INFO "PORTC: KELVIN: %lld\n", temp_in_k);
  temp_in_c = temp_in_k - 273;
  printk(KERN_INFO "PORTC: CELSIUS: %d\n", temp_in_c);

  return 0;
}

static struct file_operations port_c_fops = {
  .owner = THIS_MODULE,
  .open  = port_c_open,
  .read  = port_c_read,
};

int find_next_minor_block(struct usb_interface *interface)
{
  int i;

  for (i = 0; i < MAX_DEV * 3; i++)
    if (!blec_mod_g.intf_pool[i])
    {
      blec_mod_g.intf_pool[i+0] = interface;
      blec_mod_g.intf_pool[i+1] = interface;
      blec_mod_g.intf_pool[i+2] = interface;
      return i;
    }

  return -1;
}

int remove_from_intf_pool(struct usb_interface *interface)
{
  int i;

  for (i = 0; i < MAX_DEV * 3; i++)
    if (blec_mod_g.intf_pool[i] == interface)
    {
      blec_mod_g.intf_pool[i+0] = NULL;
      blec_mod_g.intf_pool[i+1] = NULL;
      blec_mod_g.intf_pool[i+2] = NULL;
      return i;
    }

  return -1;
}

static int blec_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
  struct blec_dev *probed_dev;
  struct usb_host_interface *iface_desc;
  struct usb_endpoint_descriptor *endpoint;

  int cdev_block_index;
  int cdev_maj_min;
  int i;

  printk(KERN_INFO "BLEC_USB: blec_probe() called...\n");

  probed_dev = kzalloc(sizeof(struct blec_dev),GFP_KERNEL);

  cdev_init(&(probed_dev->cdev_a),&port_a_fops);
  cdev_init(&(probed_dev->cdev_b),&port_b_fops);
  cdev_init(&(probed_dev->cdev_c),&port_c_fops);

  cdev_block_index = find_next_minor_block(interface);
  cdev_maj_min = cdev_block_index + blec_mod_g.t_node;

  if (cdev_block_index != -1)
  {
    cdev_add(&(probed_dev->cdev_a),cdev_maj_min,1);
    cdev_add(&(probed_dev->cdev_b),cdev_maj_min+1,1);
    cdev_add(&(probed_dev->cdev_c),cdev_maj_min+2,1);
  }

  probed_dev->cdev_a_t = cdev_maj_min;
  probed_dev->cdev_b_t = cdev_maj_min+1;
  probed_dev->cdev_c_t = cdev_maj_min+2;

  device_create(blec_mod_g.dev_class, NULL, cdev_maj_min,   NULL, "lab%dportA", cdev_block_index/3);
  device_create(blec_mod_g.dev_class, NULL, cdev_maj_min+1, NULL, "lab%dportB", cdev_block_index/3);
  device_create(blec_mod_g.dev_class, NULL, cdev_maj_min+2, NULL, "lab%dportC", cdev_block_index/3);

  iface_desc = interface->cur_altsetting;
  for (i = 0; i< iface_desc->desc.bNumEndpoints; i++)
  {
    endpoint = &iface_desc->endpoint[i].desc;

    if (!probed_dev->bulk_in_endpointAddr && usb_endpoint_is_bulk_in(endpoint))
      probed_dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;

    if (!probed_dev->bulk_out_endpointAddr && usb_endpoint_is_bulk_out(endpoint))
      probed_dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
  }

  probed_dev->udev = usb_get_dev(interface_to_usbdev(interface));
  
  probed_dev->port_a_mutex = kzalloc(sizeof(struct mutex),GFP_KERNEL);
  mutex_init(probed_dev->port_a_mutex);

  usb_set_intfdata(interface, probed_dev);

  return 0;
}

static void blec_disconnect(struct usb_interface *interface)
{
  struct blec_dev *probed_dev;
  int err;

  printk(KERN_INFO "BLEC_USB: blec_disconnect() called...\n");

  probed_dev = usb_get_intfdata(interface);

  if (probed_dev->port_a_file_count != 0)
  {
    printk(KERN_INFO "BLEC_USB: blec_disconnect: port a files open, destroying...\n");
    port_a_destroy(probed_dev);
  }

  if (probed_dev->port_b_file_count != 0)
  {
    printk(KERN_INFO "BLEC_USB: blec_disconnect: port b files open, destroying...\n");
    port_b_destroy(probed_dev);
  }

  printk(KERN_INFO "BLEC_USB: blec_disconnect(): device_destroy\n");

  device_destroy(blec_mod_g.dev_class, probed_dev->cdev_a_t);
  device_destroy(blec_mod_g.dev_class, probed_dev->cdev_b_t);
  device_destroy(blec_mod_g.dev_class, probed_dev->cdev_c_t);
  
  printk(KERN_INFO "BLEC_USB: blec_disconnect(): device_destroy complete\n");
  printk(KERN_INFO "BLEC_USB: blec_disconnect(): cdev_del \n");

  cdev_del(&(probed_dev->cdev_a));
  cdev_del(&(probed_dev->cdev_b));
  cdev_del(&(probed_dev->cdev_c));

  printk(KERN_INFO "BLEC_USB: blec_disconnect(): cdev_del complete \n");

  err = remove_from_intf_pool(interface);
  if (err == -1)
    printk(KERN_INFO "BLEC_USB: blec_disconnect(): didn't find interface in pool (this bad) \n");

  if (probed_dev->port_a_mutex)
    kfree(probed_dev->port_a_mutex);

  kfree(probed_dev);
}

static int __init blec_usb_init(void)
{
  printk(KERN_INFO "BLEC_USB: module loading...\n");


  memset(&blec_mod_g, 0, sizeof(struct blec_mod));

  if (alloc_chrdev_region(&blec_mod_g.t_node, 0, MAX_DEV*3, DRIVER_NAME ))
  {
    printk(KERN_ERR "BLEC_USB: alloc_chrdev_region() failed!\n");
    goto chrdev_err;
  }

  if (IS_ERR(blec_mod_g.dev_class = class_create(THIS_MODULE, DRIVER_NAME)))
  {
    printk(KERN_ERR "BLEC_USB: class_create() failed!\n");
    goto cls_crt_err;
  }
  
  if (usb_register(&blec_driver))
  {
    printk(KERN_ERR "BLEC_USB: usb_register() failed!\n");
    goto usb_reg_err;
  }

  blec_mod_g.usb_rw_mutex = kzalloc(sizeof(struct mutex),GFP_KERNEL);
  mutex_init(blec_mod_g.usb_rw_mutex);

  return 0;

usb_reg_err:
  class_destroy(blec_mod_g.dev_class);
cls_crt_err:
  unregister_chrdev_region(blec_mod_g.t_node, MAX_DEV*3);
chrdev_err:
  return -1;
}

static void __exit blec_usb_exit(void)
{
  printk(KERN_INFO "BLEC_USB: module unloading...\n");

  if (blec_mod_g.usb_rw_mutex)
    kfree(blec_mod_g.usb_rw_mutex);

  usb_deregister(&blec_driver);
  class_destroy(blec_mod_g.dev_class);
  unregister_chrdev_region(blec_mod_g.t_node, MAX_DEV*3);
}

module_init(blec_usb_init);
module_exit(blec_usb_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION("0.2");
