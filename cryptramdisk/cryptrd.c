/*
 * Based on sbull from ch 16 LDD 3rd ed. Will need a lot of updating.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/timer.h>
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>

MODULE_LICENSE("Dual BSD/GPL");

static int cryptrd_major; /* dynamic major */
module_param(cryptrd_major, int, S_IRUGO);
MODULE_PARM_DESC(cryptrd_major, "Major number for the device");

static int hardsect_size = 512;
module_param(hardsect_size, int, S_IRUGO);
MODULE_PARM_DESC(hardsect_size, "Sector size");

static int nsectors = 1024;
module_param(nsectors, int, S_IRUGO);
MODULE_PARM_DESC(nsectors, "Number of sectors");

/*
 * The different "request modes" we can use.
 */
enum {
	RM_SIMPLE  = 0,	/* The extra-simple request function */
	RM_FULL    = 1,	/* The full-blown version */
	RM_NOQUEUE = 2,	/* Use make_request */
};

static int request_mode = RM_SIMPLE;
module_param(request_mode, int, 0);
MODULE_PARM_DESC(request_mode, "Which request mode to use");

/*
 * Minor number and partition management.
 */
#define CRYPTRD_MINORS	16
#define MINOR_SHIFT	4
#define DEVNUM(kdevnum)	(MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT)

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE	512

/*
 * After this much idle time, the driver will simulate a media change.
 */
#define INVALIDATE_DELAY	(30*HZ)

/*
 * The internal representation of our device.
 */
struct cryptrd_dev {
	int size;                       /* Device size in sectors */
	u8 *data;                       /* The data array */
	short users;                    /* How many users */
	short media_change;             /* Flag a media change? */
	spinlock_t lock;                /* For mutual exclusion */
	struct request_queue *queue;    /* The device request queue */
	struct gendisk *gd;             /* The gendisk structure */
	struct timer_list timer;        /* For simulated media changes */
};

static struct cryptrd_dev *devices;

/*
 * Handle an I/O request.
 */
static void cryptrd_transfer(struct cryptrd_dev *dev, unsigned long sector,
		unsigned long nsect, char *buffer, int write)
{
	unsigned long offset = sector*KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect*KERNEL_SECTOR_SIZE;

	if ((offset + nbytes) > dev->size) {
		pr_notice("Beyond-end write (%ld %ld)\n", offset, nbytes);
		return;
	}
	if (write)
		memcpy(dev->data + offset, buffer, nbytes);
	else
		memcpy(buffer, dev->data + offset, nbytes);
}

/*
 * The simple form of the request function.
 */
static void cryptrd_request(struct request_queue *q)
{
	struct request *req;

	while ((req = blk_fetch_request(q)) != NULL) {
		struct cryptrd_dev *dev = req->rq_disk->private_data;

		if (req->cmd_type != REQ_TYPE_FS) {
			pr_notice("Skip non-fs request\n");
			__blk_end_request_cur(req, -EIO);
			continue;
		}

		cryptrd_transfer(dev, blk_rq_pos(req), blk_rq_cur_sectors(req),
				req->buffer, rq_data_dir(req));
		__blk_end_request_cur(req, 1);
	}
}


/*
 * Transfer a single BIO.
 */
static int cryptrd_xfer_bio(struct cryptrd_dev *dev, struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	sector_t sector = bio->bi_sector;

	/* Do each segment independently. */
	bio_for_each_segment(bvec, bio, i) {
		char *buffer = __bio_kmap_atomic(bio, i);

		cryptrd_transfer(dev, sector, bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE,
				buffer, bio_data_dir(bio) == WRITE);
		sector += bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE;
		__bio_kunmap_atomic(bio);
	}
	return 0; /* Always "succeed" */
}

/*
 * Transfer a full request.
 */
static int cryptrd_xfer_request(struct cryptrd_dev *dev, struct request *req)
{
	struct bio *bio;
	int nsect = 0;

	__rq_for_each_bio(bio, req) {
		cryptrd_xfer_bio(dev, bio);
		nsect += bio->bi_size/KERNEL_SECTOR_SIZE;
	}
	return nsect;
}



/*
 * Smarter request function that "handles clustering".
 */
static void cryptrd_full_request(struct request_queue *q)
{
	struct request *req;
	int sectors_xferred;
	struct cryptrd_dev *dev = q->queuedata;

	while ((req = blk_fetch_request(q)) != NULL) {
		if (req->cmd_type != REQ_TYPE_FS) {
			pr_notice("Skip non-fs request\n");
			__blk_end_request(req, -EIO, blk_rq_cur_bytes(req));
			continue;
		}
		sectors_xferred = cryptrd_xfer_request(dev, req);
		__blk_end_request(req, 0, sectors_xferred);
	}
}



/*
 * The direct make request version.
 */
static void cryptrd_make_request(struct request_queue *q, struct bio *bio)
{
	struct cryptrd_dev *dev = q->queuedata;
	int status;

	status = cryptrd_xfer_bio(dev, bio);
	bio_endio(bio, status);
}


/*
 * Open and close.
 */

static int cryptrd_open(struct block_device *bdev, fmode_t mode)
{
	struct cryptrd_dev *dev = bdev->bd_disk->private_data;

	del_timer_sync(&dev->timer);
	spin_lock(&dev->lock);
	if (!dev->users)
		check_disk_change(bdev);
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

static void cryptrd_release(struct gendisk *disk, fmode_t mode)
{
	struct cryptrd_dev *dev = disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;

	if (!dev->users) {
		dev->timer.expires = jiffies + INVALIDATE_DELAY;
		add_timer(&dev->timer);
	}
	spin_unlock(&dev->lock);
}

/*
 * Look for a (simulated) media change.
 */
int cryptrd_media_changed(struct gendisk *gd)
{
	struct cryptrd_dev *dev = gd->private_data;

	return dev->media_change;
}

/*
 * Revalidate.  WE DO NOT TAKE THE LOCK HERE, for fear of deadlocking
 * with open.  That needs to be reevaluated.
 */
int cryptrd_revalidate(struct gendisk *gd)
{
	struct cryptrd_dev *dev = gd->private_data;

	if (dev->media_change) {
		dev->media_change = 0;
		memset(dev->data, 0, dev->size);
	}
	return 0;
}

/*
 * The "invalidate" function runs out of the device timer; it sets
 * a flag to simulate the removal of the media.
 */
void cryptrd_invalidate(unsigned long ldev)
{
	struct cryptrd_dev *dev = (struct cryptrd_dev *) ldev;

	spin_lock(&dev->lock);
	if (dev->users || !dev->data)
		pr_warn("cryptrd: timer sanity check failed\n");
	else
		dev->media_change = 1;
	spin_unlock(&dev->lock);
}

/*
 * The ioctl() implementation
 */

int cryptrd_ioctl(struct block_device *bdev, fmode_t fmode,
		   unsigned int cmd, unsigned long arg)
{
	long size;
	struct hd_geometry geo;
	struct cryptrd_dev *dev = bdev->bd_disk->private_data;

	switch (cmd) {
	case HDIO_GETGEO:
		/*
		 * Get geometry: since we are a virtual device, we have to make
		 * up something plausible.  So we claim 16 sectors, four heads,
		 * and calculate the corresponding number of cylinders.  We set the
		 * start of data at sector four.
		 */
		size = dev->size*(hardsect_size/KERNEL_SECTOR_SIZE);
		geo.cylinders = (size & ~0x3f) >> 6;
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = 4;
		if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
			return -EFAULT;
		return 0;
	}

	return -ENOTTY; /* unknown command */
}



/*
 * The device operations structure.
 */
static const struct block_device_operations cryptrd_ops = {
	.owner		= THIS_MODULE,
	.open		= cryptrd_open,
	.release	= cryptrd_release,
	.media_changed	= cryptrd_media_changed,
	.revalidate_disk = cryptrd_revalidate,
	.ioctl		= cryptrd_ioctl
};


/*
 * Set up our internal device.
 */
static void setup_device(struct cryptrd_dev *dev)
{

	dev->size = nsectors*hardsect_size;
	dev->data = vmalloc(dev->size);
	if (dev->data == NULL) {
		pr_notice("vmalloc failure.\n");
		return;
	}
	spin_lock_init(&dev->lock);

	/*
	 * The timer which "invalidates" the device.
	 */
	init_timer(&dev->timer);
	dev->timer.data = (unsigned long) dev;
	dev->timer.function = cryptrd_invalidate;

	/*
	 * The I/O queue, depending on whether we are using our own
	 * make_request function or not.
	 */
	switch (request_mode) {
	case RM_NOQUEUE:
		dev->queue = blk_alloc_queue(GFP_KERNEL);
		if (dev->queue == NULL)
			goto out_vfree;
		blk_queue_make_request(dev->queue, cryptrd_make_request);
		break;

	case RM_FULL:
		dev->queue = blk_init_queue(cryptrd_full_request, &dev->lock);
		if (dev->queue == NULL)
			goto out_vfree;
		break;

	default:
		pr_notice("Bad request mode %d, using simple\n", request_mode);
		/* fall into.. */

	case RM_SIMPLE:
		dev->queue = blk_init_queue(cryptrd_request, &dev->lock);
		if (dev->queue == NULL)
			goto out_vfree;
		break;
	}
	blk_queue_logical_block_size(dev->queue, hardsect_size);
	dev->queue->queuedata = dev;
	/*
	 * And the gendisk structure.
	 */
	dev->gd = alloc_disk(CRYPTRD_MINORS);
	if (!dev->gd) {
		pr_notice("alloc_disk failure\n");
		goto out_vfree;
	}
	dev->gd->major = cryptrd_major;
	dev->gd->first_minor = 0;
	dev->gd->fops = &cryptrd_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf(dev->gd->disk_name, 32, "cryptrd");
	set_capacity(dev->gd, nsectors*(hardsect_size/KERNEL_SECTOR_SIZE));
	add_disk(dev->gd);
	return;

out_vfree:
	if (dev->data)
		vfree(dev->data);
}



static int __init cryptrd_init(void)
{
	cryptrd_major = register_blkdev(cryptrd_major, "cryptrd");
	if (cryptrd_major <= 0) {
		pr_warn("cryptrd: unable to get major number\n");
		return -EIO;
	}
	/*
	 * Allocate the device array, and initialize each one.
	 */
	devices = kzalloc(sizeof(struct cryptrd_dev), GFP_KERNEL);
	if (devices == NULL)
		goto out_unregister;
	setup_device(devices);

	return 0;

out_unregister:
	unregister_blkdev(cryptrd_major, "cryptrd");
	return -ENOMEM;
}

static void cryptrd_exit(void)
{
	struct cryptrd_dev *dev = devices;

	del_timer_sync(&dev->timer);
	if (dev->gd) {
		del_gendisk(dev->gd);
		put_disk(dev->gd);
	}
	if (dev->queue) {
		if (request_mode == RM_NOQUEUE)
			blk_put_queue(dev->queue);
		else
			blk_cleanup_queue(dev->queue);
	}
	if (dev->data)
		vfree(dev->data);

	unregister_blkdev(cryptrd_major, "cryptrd");
	kfree(devices);
}

module_init(cryptrd_init);
module_exit(cryptrd_exit);
