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

static int cryptrd_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	struct cryptrd_dev *dev = bdev->bd_disk->private_data;

	geo->cylinders = (dev->size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 4;
	return 0;
}

/*
 * The device operations structure.
 */
static const struct block_device_operations cryptrd_ops = {
	.owner		= THIS_MODULE,
	.getgeo		= cryptrd_getgeo,
};


/*
 * Set up our internal device.
 */
static void setup_device(struct cryptrd_dev *dev)
{

	dev->size = nsectors * KERNEL_SECTOR_SIZE;
	dev->data = vmalloc(dev->size);
	if (dev->data == NULL) {
		pr_notice("vmalloc failure.\n");
		return;
	}
	spin_lock_init(&dev->lock);

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
	blk_queue_logical_block_size(dev->queue, KERNEL_SECTOR_SIZE);
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
	set_capacity(dev->gd, nsectors);
	add_disk(dev->gd);
	return;

out_vfree:
	if (dev->data)
		vfree(dev->data);
}

static int __init cryptrd_init(void)
{
	cryptrd_major = register_blkdev(0, "cryptrd");
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
