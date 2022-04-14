#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>

#include <linux/timer.h>
#include <linux/jiffies.h>

#define SERVO_DT_COMPATIBLE "servo"
#define SERVO_CLASS_NAME "servo-class"

struct gpio_descs *leds;
struct gpio_desc *detector;

static ssize_t servo_write_handler(struct file *filp, const char *data, size_t data_len, loff_t *offset);
static ssize_t servo_read_handler(struct file *filp, char *buffer, size_t length, loff_t *offset);
static int servo_open_handler(struct inode *inode, struct file *file);
static void led_on(struct gpio_descs *leds, const int led_idx);

struct servo_state {
	unsigned curr_pos;
	unsigned dest_pos;
	enum state {IDLE, CALLIB, IN_PROGRESS} state;
};

struct servo {
	/* char driver */
	dev_t  devt;
	struct cdev cdev;
	struct device *device;
	struct class *class;

	/* gpio */
	struct gpio_descs *leds;
	struct gpio_desc *detector;
	int led_active_idx;

	struct timer_list timer;
	
	struct servo_state servo_state;
};

struct file_operations fops = {
	.open = servo_open_handler,
	.write = servo_write_handler,
	.read = servo_read_handler
};

static ssize_t servo_callib_store (struct device *dev, struct device_attribute *attr,
						 const char *buf, size_t count)
{
	struct servo *servo = (struct servo*)dev_get_drvdata(dev);

	if (servo->servo_state.state == IDLE) {
		servo->servo_state.state = CALLIB;
	}

	return count;
}

static DEVICE_ATTR(callib, S_IWUSR, NULL, servo_callib_store);

static ssize_t servo_write_handler(struct file *filp, const char *data, size_t data_len, loff_t *offset)
{
        char private_buffer[10];
        long received_value;
		struct servo *servo;

        if (data_len > 10) {
                return -EMSGSIZE;
        }

        copy_from_user(private_buffer, data, data_len);
        private_buffer[data_len] = 0;

        printk("Got message %s of size %ld\n", private_buffer, data_len);

        if (kstrtol(private_buffer, 10, &received_value) != 0) {
                return -EINVAL;
        }

		servo = filp->private_data;
		if (servo->servo_state.state != CALLIB && servo->servo_state.state != IN_PROGRESS) {
			servo->servo_state.dest_pos = received_value;
			servo->servo_state.state = IN_PROGRESS;
		}

        return data_len;
}

static ssize_t servo_read_handler(struct file *filp, char *buffer, size_t length, loff_t *offset)
{
        char private_buffer[10];
        int data_len;
		struct servo *servo;

		servo = filp->private_data;
        data_len = snprintf(private_buffer, 10, "%d\n", servo->servo_state.curr_pos);
        copy_to_user(buffer, private_buffer, data_len);
        
        return data_len;
}

static int servo_open_handler(struct inode *inode, struct file *file)
{
	struct servo *servo;

	servo = container_of(inode->i_cdev, struct servo, cdev);
	file->private_data = servo;

	return 0;
}

static void led_on(struct gpio_descs *leds, const int led_idx)
{
	int leds_ctr;

	if ((led_idx < 0) || (led_idx >= leds->ndescs)) {
		return;
	}

	for (leds_ctr = 0; leds_ctr<leds->ndescs; leds_ctr++) {
		if (leds_ctr == led_idx) {
			gpiod_set_value(leds->desc[leds_ctr], 1);
		} else {
			gpiod_set_value(leds->desc[leds_ctr], 0);
		}
	}
}

static void servo_update(struct timer_list *timer)
{
	struct servo *servo = container_of(timer, struct servo, timer);

	switch(servo->servo_state.state) {
		case CALLIB:
			if (gpiod_get_value(servo->detector)) {
				servo->led_active_idx = (servo->led_active_idx + 1) % servo->leds->ndescs;
				led_on(servo->leds, servo->led_active_idx);
			} else {
				servo->servo_state.state = IDLE;
				servo->servo_state.curr_pos = 0;
				servo->servo_state.dest_pos = 0;
			}
			break;
		case IN_PROGRESS:
			if (servo->servo_state.curr_pos > servo->servo_state.dest_pos) {
				servo->led_active_idx = (servo->led_active_idx - 1) % servo->leds->ndescs;
				led_on(servo->leds, servo->led_active_idx);
				servo->servo_state.curr_pos--;
			} else if (servo->servo_state.curr_pos < servo->servo_state.dest_pos) {
				servo->led_active_idx = (servo->led_active_idx + 1) % servo->leds->ndescs;
				led_on(servo->leds, servo->led_active_idx);
				servo->servo_state.curr_pos++;
			} else {
				servo->servo_state.state = IDLE;
			}
		case IDLE:
		default:
			break;
	}

	mod_timer(timer, jiffies + msecs_to_jiffies(100));
}

static int servo_probe(struct platform_device *device)
{
	struct servo *servo = devm_kzalloc(&device->dev, sizeof(struct servo), GFP_KERNEL);
	dev_set_drvdata(&device->dev, servo);

	servo->leds = devm_gpiod_get_array(&device->dev, "coil", GPIOD_OUT_LOW);
	servo->detector = devm_gpiod_get(&device->dev, "detector", GPIOD_IN);

	alloc_chrdev_region(&servo->devt, 0, 1, THIS_MODULE->name);
	cdev_init(&servo->cdev, &fops);
	servo->cdev.owner = THIS_MODULE;

	cdev_add(&servo->cdev, servo->devt, 1);

	servo->class = class_create(THIS_MODULE, SERVO_CLASS_NAME);
	servo->device = device_create(servo->class, NULL, servo->devt, NULL, THIS_MODULE->name);
	
	device_create_file(&device->dev, &dev_attr_callib);

	servo->timer.function = servo_update;
	servo->led_active_idx = 0;
	led_on(servo->leds, 0);

	servo->servo_state.state = CALLIB;

	servo->timer.expires = jiffies + msecs_to_jiffies(200);
	add_timer(&servo->timer);

	printk("Servo probed!! %d\n", gpiod_get_value(servo->detector));
	return 0;
}

static int servo_remove(struct platform_device *device)
{
	struct servo *servo = (struct servo*)dev_get_drvdata(&device->dev);

	device_remove_file(&device->dev, &dev_attr_callib);
	del_timer(&servo->timer);
	device_destroy(servo->class, servo->devt);
	class_destroy(servo->class);
	cdev_del(&servo->cdev);
	unregister_chrdev_region(servo->devt, 1);

	printk("Servo removed!!\n");
	return 0;
}

static const struct of_device_id gpio_pins_match[] = {
	{ .compatible = "servo" },
	{ /* Intentionally left blank */ },
};

static struct platform_driver servo_driver = {
	.probe = servo_probe,
	.remove = servo_remove,
	.driver = {
		.name = THIS_MODULE->name,
		.owner = THIS_MODULE,
		.of_match_table = gpio_pins_match,
	},
};

module_platform_driver(servo_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Zajdel <zajdel.filip97@gmail.com>");
