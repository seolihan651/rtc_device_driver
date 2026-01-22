// ds1302_min.c - DS1302 minimal read-only driver (STM32-style bitbang)

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mutex.h>

#define DRIVER_NAME "ds1302_min"
#define CLASS_NAME  "ds1302_min_class"

/* ==== GPIO 핀 (BCM 번호) ==== */
#define DS1302_CE   17    /* RST/CE */
#define DS1302_IO   27    /* DAT (bidirectional) */
#define DS1302_SCLK 22   /* CLK */

/* DS1302 register write addresses (LSB=0). Read = addr|1 */
#define ADDR_SECONDS     0x80
#define ADDR_MINUTES     0x82
#define ADDR_HOURS       0x84
#define ADDR_DATE        0x86
#define ADDR_MONTH       0x88
#define ADDR_DAYOFWEEK   0x8A
#define ADDR_YEAR        0x8C

static dev_t devno;
static struct cdev cdev_ds;
static struct class *cls;
static DEFINE_MUTEX(ds_lock);

static inline void ce_high(void)  { gpio_set_value(DS1302_CE, 1); }
static inline void ce_low(void)   { gpio_set_value(DS1302_CE, 0); }
static inline void clk_high(void) { gpio_set_value(DS1302_SCLK, 1); }
static inline void clk_low(void)  { gpio_set_value(DS1302_SCLK, 0); }

static inline void io_as_output(int val)
{
    gpio_direction_output(DS1302_IO, val);
}

static inline void io_as_input(void)
{
    gpio_direction_input(DS1302_IO);
}

static inline void io_write(int val)
{
    gpio_set_value(DS1302_IO, val);
}

static inline int io_read(void)
{
    return gpio_get_value(DS1302_IO);
}

/* 짧은 딜레이: STM32에서의 HAL_Delay_us 느낌 */
static inline void ds_delay(void) { udelay(2); }

/* STM32 스타일: High->Low pulse */
static inline void ds_clock_pulse(void)
{
    clk_high();
    ds_delay();
    clk_low();
    ds_delay();
}

/* LSB-first 전송 (STM32 ds1302_tx) */
static void ds1302_tx(u8 tx)
{
    int i;

    io_as_output(0);

    for (i = 0; i < 8; i++) {
        io_write((tx & (1 << i)) ? 1 : 0);
        ds_delay();
        ds_clock_pulse();
    }
}

/* LSB-first 수신 (STM32 ds1302_rx: 마지막 비트 뒤에는 추가 pulse 안침) */
static void ds1302_rx(u8 *out)
{
    int i;
    u8 temp = 0;

    io_as_input();
    ds_delay();

    for (i = 0; i < 8; i++) {
        if (io_read())
            temp |= (1 << i);

        if (i != 7)
            ds_clock_pulse();
    }

    *out = temp;
}

static inline u8 bcd2bin(u8 bcd)
{
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

/* 단일 레지스터 read: addr는 write 주소(LSB=0) 넣으면 됨 */
static u8 ds1302_read_reg_raw(u8 addr_write)
{
    u8 v = 0;

    ce_high();
    ds_delay();

    ds1302_tx(addr_write | 1);  /* READ 주소 */
    ds1302_rx(&v);

    ce_low();
    ds_delay();

    return v;
}

/* 시간 읽기: 레지스터별로 읽어서 BCD->BIN */
static int ds1302_read_time(int *yy, int *mo, int *dd, int *hh, int *mm, int *ss, int *wd)
{
    u8 rsec, rmin, rhour, rdate, rmon, rwday, ryear;

    rsec  = ds1302_read_reg_raw(ADDR_SECONDS);
    rmin  = ds1302_read_reg_raw(ADDR_MINUTES);
    rhour = ds1302_read_reg_raw(ADDR_HOURS);
    rdate = ds1302_read_reg_raw(ADDR_DATE);
    rmon  = ds1302_read_reg_raw(ADDR_MONTH);
    rwday = ds1302_read_reg_raw(ADDR_DAYOFWEEK);
    ryear = ds1302_read_reg_raw(ADDR_YEAR);

    /* CH bit(sec bit7) 제거 */
    rsec &= 0x7F;
    rmin &= 0x7F;
    rhour &= 0x3F; /* 24h 가정 */
    rdate &= 0x3F;
    rmon &= 0x1F;
    rwday &= 0x07;

    *ss = bcd2bin(rsec);
    *mm = bcd2bin(rmin);
    *hh = bcd2bin(rhour);
    *dd = bcd2bin(rdate);
    *mo = bcd2bin(rmon);
    *wd = bcd2bin(rwday);
    *yy = 2000 + bcd2bin(ryear);

    return 0;
}

/* ===== char device: read() only ===== */

static ssize_t ds_read(struct file *f, char __user *ubuf, size_t cnt, loff_t *ppos)
{
    char kbuf[64];
    int len;
    int yy, mo, dd, hh, mm, ss, wd;

    /* 한 번 읽고 EOF */
    if (*ppos > 0)
        return 0;

    mutex_lock(&ds_lock);
    ds1302_read_time(&yy, &mo, &dd, &hh, &mm, &ss, &wd);
    mutex_unlock(&ds_lock);

    len = snprintf(kbuf, sizeof(kbuf),
                   "%04d-%02d-%02d %02d:%02d:%02d (wday=%d)\n",
                   yy, mo, dd, hh, mm, ss, wd);

    if (len > cnt)
        len = cnt;

    if (copy_to_user(ubuf, kbuf, len))
        return -EFAULT;

    *ppos += len;
    return len;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .read  = ds_read,
};

static int __init ds_init(void)
{
    int ret;

    pr_info("ds1302_min: init\n");

    ret = alloc_chrdev_region(&devno, 0, 1, DRIVER_NAME);
    if (ret < 0) return ret;

    cdev_init(&cdev_ds, &fops);
    ret = cdev_add(&cdev_ds, devno, 1);
    if (ret < 0) goto err_cdev;

    cls = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(cls)) { ret = PTR_ERR(cls); goto err_class; }

    if (IS_ERR(device_create(cls, NULL, devno, NULL, DRIVER_NAME))) {
        ret = -ENOMEM; goto err_dev;
    }

    /* GPIO request */
    ret = gpio_request(DS1302_CE, "ds1302_ce");
    if (ret) goto err_gpio;
    ret = gpio_request(DS1302_SCLK, "ds1302_sclk");
    if (ret) goto err_gpio2;
    ret = gpio_request(DS1302_IO, "ds1302_io");
    if (ret) goto err_gpio3;

    /* 기본 idle 상태: CE=0, CLK=0, IO는 output low로 시작(STM32 초기화 느낌) */
    gpio_direction_output(DS1302_CE, 0);
    gpio_direction_output(DS1302_SCLK, 0);
    io_as_output(0);

    pr_info("ds1302_min: /dev/%s created (major=%d)\n", DRIVER_NAME, MAJOR(devno));
    return 0;

err_gpio3:
    gpio_free(DS1302_SCLK);
err_gpio2:
    gpio_free(DS1302_CE);
err_gpio:
    device_destroy(cls, devno);
err_dev:
    class_destroy(cls);
err_class:
    cdev_del(&cdev_ds);
err_cdev:
    unregister_chrdev_region(devno, 1);
    return ret;
}

static void __exit ds_exit(void)
{
    pr_info("ds1302_min: exit\n");

    gpio_free(DS1302_IO);
    gpio_free(DS1302_SCLK);
    gpio_free(DS1302_CE);

    device_destroy(cls, devno);
    class_destroy(cls);
    cdev_del(&cdev_ds);
    unregister_chrdev_region(devno, 1);
}

module_init(ds_init);
module_exit(ds_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kkk");
MODULE_DESCRIPTION("DS1302 minimal read-only driver (STM32-style bitbang)");

