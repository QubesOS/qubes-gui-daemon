/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <xen/page.h>
#include <linux/highmem.h>
/// User virtual address to mfn translator
/**
    \param cmd ignored
    \param data the user-specified address
    \return mfn corresponding to "data" argument, or -1 on error
*/   
static int u2mfn_ioctl(struct inode *i, struct file *f, unsigned int cmd,
		       unsigned long data)
{
	struct page *user_page;
	void *kaddr;
	int ret;
	down_read(&current->mm->mmap_sem);
	ret=get_user_pages
	    (current, current->mm, data, 1, 1, 0, &user_page, 0);
        up_read(&current->mm->mmap_sem);
        if (ret != 1)
		return -1;
	kaddr = kmap(user_page);
	ret = virt_to_mfn(kaddr);
	kunmap(user_page);
	put_page(user_page);
	return ret;
}

static struct file_operations u2mfn_fops = {
	.ioctl = u2mfn_ioctl
};
/// u2mfn module registration
/**
    tries to register "/proc/u2mfn" pseudofile
*/
static int u2mfn_init(void)
{
	struct proc_dir_entry *u2mfn_node =
	    proc_create_data("u2mfn", 0600, NULL,
			     &u2mfn_fops, 0);
	if (!u2mfn_node)
		return -1;
	return 0;
}

static void u2mfn_exit(void)
{
	remove_proc_entry("u2mfn", 0);
}

module_init(u2mfn_init);
module_exit(u2mfn_exit);
MODULE_LICENSE("GPL");
