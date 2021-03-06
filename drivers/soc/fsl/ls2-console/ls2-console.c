/* Copyright 2015 Freescale Semiconductor Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * * Neither the name of the above-listed copyright holders nor the
 * names of any contributors may be used to endorse or promote products
 * derived from this software without specific prior written permission.
 *
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") as published by the Free Software
 * Foundation, either version 2 of that License or (at your option) any
 * later version.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/compat.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>

/* MC and IOP character device to read from RAM */

#define MC_BASE_ADDR 0x83c0000000

#define MC_BUFFER_OFFSET 0x01000000
#define MC_BUFFER_SIZE (1024*1024*16)
#define MC_OFFSET_DELTA (MC_BUFFER_OFFSET)

#define AIOP_BUFFER_OFFSET 0x06000000
#define AIOP_BUFFER_SIZE (1024*1024*16)
#define AIOP_OFFSET_DELTA (0)

struct log_header {
	char magic_word[8]; /* magic word */
	uint32_t buf_start; /* holds the 32-bit little-endian
		offset of the start of the buffer */
	uint32_t buf_length; /* holds the 32-bit little-endian
		length of the buffer */
	uint32_t last_byte; /* holds the 32-bit little-endian offset
		of the byte after the last byte that was written */
	char reserved[44];
};

#define LOG_HEADER_FLAG_BUFFER_WRAPAROUND 0x80000000
#define LOG_VERSION_MAJOR 1
#define LOG_VERSION_MINOR 0


#define invalidate(p) { asm volatile("dc ivac, %0" : : "r" (p) : "memory"); }

struct console_data {
	char *map_addr;
	struct log_header *hdr;
	char *start_addr; /* Start of buffer */
	char *end_addr; /* End of buffer */
	char *end_of_data; /* Current end of data */
	char *cur_ptr; /* Last data sent to console */
};

#define LAST_BYTE(a) ((a) & ~(LOG_HEADER_FLAG_BUFFER_WRAPAROUND))

static inline void __adjust_end(struct console_data *cd)
{
	cd->end_of_data = cd->start_addr
				+ LAST_BYTE(le32_to_cpu(cd->hdr->last_byte));
}

static inline void adjust_end(struct console_data *cd)
{
	invalidate(cd->hdr);
	__adjust_end(cd);
}


static int fsl_ls2_generic_console_open(struct inode *node, struct file *fp,
				u64 offset, u64 size,
				uint8_t *emagic, uint8_t magic_len,
				u32 offset_delta)
{
	struct console_data *cd;
	uint8_t *magic;
	uint32_t wrapped;

	cd = kmalloc(sizeof(*cd), GFP_KERNEL);
	if (cd == NULL)
		return -ENOMEM;
	fp->private_data = cd;
	cd->map_addr = ioremap(MC_BASE_ADDR + offset, size);

	cd->hdr = (struct log_header *) cd->map_addr;
	invalidate(cd->hdr);

	magic = cd->hdr->magic_word;
	if (memcmp(magic, emagic, magic_len)) {
		pr_info("magic didn't match!\n");
		pr_info("expected: %02x %02x %02x %02x %02x %02x %02x %02x\n",
				emagic[0], emagic[1], emagic[2], emagic[3],
				emagic[4], emagic[5], emagic[6], emagic[7]);
		pr_info("    seen: %02x %02x %02x %02x %02x %02x %02x %02x\n",
				magic[0], magic[1], magic[2], magic[3],
				magic[4], magic[5], magic[6], magic[7]);
		kfree(cd);
		iounmap(cd->map_addr);
		return -EIO;
	}

	cd->start_addr = cd->map_addr
			 + le32_to_cpu(cd->hdr->buf_start) - offset_delta;
	cd->end_addr = cd->start_addr + le32_to_cpu(cd->hdr->buf_length);

	wrapped = le32_to_cpu(cd->hdr->last_byte)
			 & LOG_HEADER_FLAG_BUFFER_WRAPAROUND;

	__adjust_end(cd);
	if (wrapped && (cd->end_of_data != cd->end_addr))
		cd->cur_ptr = cd->end_of_data+1;
	else
		cd->cur_ptr = cd->start_addr;

	return 0;
}

static int fsl_ls2_mc_console_open(struct inode *node, struct file *fp)
{
	uint8_t magic_word[] = { 0, 1, 'C', 'M' };

	return fsl_ls2_generic_console_open(node, fp,
			MC_BUFFER_OFFSET, MC_BUFFER_SIZE,
			magic_word, sizeof(magic_word),
			MC_OFFSET_DELTA);
}

static int fsl_ls2_aiop_console_open(struct inode *node, struct file *fp)
{
	uint8_t magic_word[] = { 'P', 'O', 'I', 'A' };

	return fsl_ls2_generic_console_open(node, fp,
			AIOP_BUFFER_OFFSET, AIOP_BUFFER_SIZE,
			magic_word, sizeof(magic_word),
			AIOP_OFFSET_DELTA);
}

static int fsl_ls2_console_close(struct inode *node, struct file *fp)
{
	struct console_data *cd = fp->private_data;

	iounmap(cd->map_addr);
	kfree(cd);
	return 0;
}

ssize_t fsl_ls2_console_read(struct file *fp, char __user *buf, size_t count,
			     loff_t *f_pos)
{
	struct console_data *cd = fp->private_data;
	size_t bytes = 0;
	char data;

	/* Check if we need to adjust the end of data addr */
	adjust_end(cd);

	while ((count != bytes) && (cd->end_of_data != cd->cur_ptr)) {
		if (((u64)cd->cur_ptr) % 64 == 0)
			invalidate(cd->cur_ptr);

		data = *(cd->cur_ptr);
		if (copy_to_user(&buf[bytes], &data, 1))
			return -EFAULT;
		cd->cur_ptr++;
		if (cd->cur_ptr >= cd->end_addr)
			cd->cur_ptr = cd->start_addr;
		++bytes;
	}
	return bytes;
}

static const struct file_operations fsl_ls2_mc_console_fops = {
	.owner          = THIS_MODULE,
	.open           = fsl_ls2_mc_console_open,
	.release        = fsl_ls2_console_close,
	.read           = fsl_ls2_console_read,
};

static struct miscdevice fsl_ls2_mc_console_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "fsl_mc_console",
	.fops = &fsl_ls2_mc_console_fops
};

static const struct file_operations fsl_ls2_aiop_console_fops = {
	.owner          = THIS_MODULE,
	.open           = fsl_ls2_aiop_console_open,
	.release        = fsl_ls2_console_close,
	.read           = fsl_ls2_console_read,
};

static struct miscdevice fsl_ls2_aiop_console_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "fsl_aiop_console",
	.fops = &fsl_ls2_aiop_console_fops
};

static int __init fsl_ls2_console_init(void)
{
	int err = 0;

	pr_info("Freescale LS2 console driver\n");
	err = misc_register(&fsl_ls2_mc_console_dev);
	if (err) {
		pr_err("fsl_mc_console: cannot register device\n");
		return err;
	}
	pr_info("fsl-ls2-console: device %s registered\n",
		fsl_ls2_mc_console_dev.name);

	err = misc_register(&fsl_ls2_aiop_console_dev);
	if (err) {
		pr_err("fsl_aiop_console: cannot register device\n");
		return err;
	}
	pr_info("fsl-ls2-console: device %s registered\n",
		fsl_ls2_aiop_console_dev.name);

	return 0;
}

static void __exit fsl_ls2_console_exit(void)
{
	misc_deregister(&fsl_ls2_mc_console_dev);
	pr_info("device %s deregistered\n",
		fsl_ls2_mc_console_dev.name);

	misc_deregister(&fsl_ls2_aiop_console_dev);
	pr_info("device %s deregistered\n",
		fsl_ls2_aiop_console_dev.name);
}

module_init(fsl_ls2_console_init);
module_exit(fsl_ls2_console_exit);

MODULE_AUTHOR("Roy Pledge <roy.pledge@freescale.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Freescale LS2 console driver");
