// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 */

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/sched/mm.h>
#include <linux/vfio_pci_core.h>
#include <linux/anon_inodes.h>

#include "cmd.h"

/* Arbitrary to prevent userspace from consuming endless memory */
#define MAX_MIGRATION_SIZE (512*1024*1024)

struct mlx5vf_pci_core_device {
	struct vfio_pci_core_device core_device;
	u16 vhca_id;
	u8 migrate_cap:1;
	u8 deferred_reset:1;
	/* protect migration state */
	struct mutex state_mutex;
	enum vfio_device_mig_state mig_state;
	/* protect the reset_done flow */
	spinlock_t reset_lock;
	struct mlx5_vf_migration_file *resuming_migf;
	struct mlx5_vf_migration_file *saving_migf;
};

static struct page *
mlx5vf_get_migration_page(struct mlx5_vf_migration_file *migf,
			  unsigned long offset)
{
	unsigned long cur_offset = 0;
	struct scatterlist *sg;
	unsigned int i;

	/* All accesses are sequential */
	if (offset < migf->last_offset || !migf->last_offset_sg) {
		migf->last_offset = 0;
		migf->last_offset_sg = migf->table.sgt.sgl;
		migf->sg_last_entry = 0;
	}

	cur_offset = migf->last_offset;

	for_each_sg(migf->last_offset_sg, sg,
			migf->table.sgt.orig_nents - migf->sg_last_entry, i) {
		if (offset < sg->length + cur_offset) {
			migf->last_offset_sg = sg;
			migf->sg_last_entry += i;
			migf->last_offset = cur_offset;
			return nth_page(sg_page(sg),
					(offset - cur_offset) / PAGE_SIZE);
		}
		cur_offset += sg->length;
	}
	return NULL;
}

static int mlx5vf_add_migration_pages(struct mlx5_vf_migration_file *migf,
				      unsigned int npages)
{
	unsigned int to_alloc = npages;
	struct page **page_list;
	unsigned long filled;
	unsigned int to_fill;
	int ret;

	to_fill = min_t(unsigned int, npages, PAGE_SIZE / sizeof(*page_list));
	page_list = kvzalloc(to_fill * sizeof(*page_list), GFP_KERNEL);
	if (!page_list)
		return -ENOMEM;

	do {
		filled = alloc_pages_bulk_array(GFP_KERNEL, to_fill, page_list);
		if (!filled) {
			ret = -ENOMEM;
			goto err;
		}
		to_alloc -= filled;
		ret = sg_alloc_append_table_from_pages(
			&migf->table, page_list, filled, 0,
			filled << PAGE_SHIFT, UINT_MAX, SG_MAX_SINGLE_ALLOC,
			GFP_KERNEL);

		if (ret)
			goto err;
		migf->allocated_length += filled * PAGE_SIZE;
		/* clean input for another bulk allocation */
		memset(page_list, 0, filled * sizeof(*page_list));
		to_fill = min_t(unsigned int, to_alloc,
				PAGE_SIZE / sizeof(*page_list));
	} while (to_alloc > 0);

	kvfree(page_list);
	return 0;

err:
	kvfree(page_list);
	return ret;
}

static void mlx5vf_disable_fd(struct mlx5_vf_migration_file *migf)
{
	struct sg_page_iter sg_iter;

	mutex_lock(&migf->lock);
	/* Undo alloc_pages_bulk_array() */
	for_each_sgtable_page(&migf->table.sgt, &sg_iter, 0)
		__free_page(sg_page_iter_page(&sg_iter));
	sg_free_append_table(&migf->table);
	migf->disabled = true;
	migf->total_length = 0;
	migf->allocated_length = 0;
	migf->filp->f_pos = 0;
	mutex_unlock(&migf->lock);
}

static int mlx5vf_release_file(struct inode *inode, struct file *filp)
{
	struct mlx5_vf_migration_file *migf = filp->private_data;

	mlx5vf_disable_fd(migf);
	mutex_destroy(&migf->lock);
	kfree(migf);
	return 0;
}

static ssize_t mlx5vf_save_read(struct file *filp, char __user *buf, size_t len,
			       loff_t *pos)
{
	struct mlx5_vf_migration_file *migf = filp->private_data;
	ssize_t done = 0;

	if (pos)
		return -ESPIPE;
	pos = &filp->f_pos;

	mutex_lock(&migf->lock);
	if (*pos > migf->total_length) {
		done = -EINVAL;
		goto out_unlock;
	}
	if (migf->disabled) {
		done = -ENODEV;
		goto out_unlock;
	}

	len = min_t(size_t, migf->total_length - *pos, len);
	while (len) {
		size_t page_offset;
		struct page *page;
		size_t page_len;
		u8 *from_buff;
		int ret;

		page_offset = (*pos) % PAGE_SIZE;
		page = mlx5vf_get_migration_page(migf, *pos - page_offset);
		if (!page) {
			if (done == 0)
				done = -EINVAL;
			goto out_unlock;
		}

		page_len = min_t(size_t, len, PAGE_SIZE - page_offset);
		from_buff = kmap_local_page(page);
		ret = copy_to_user(buf, from_buff + page_offset, page_len);
		kunmap_local(from_buff);
		if (ret) {
			done = -EFAULT;
			goto out_unlock;
		}
		*pos += page_len;
		len -= page_len;
		done += page_len;
		buf += page_len;
	}

out_unlock:
	mutex_unlock(&migf->lock);
	return done;
}

static const struct file_operations mlx5vf_save_fops = {
	.owner = THIS_MODULE,
	.read = mlx5vf_save_read,
	.release = mlx5vf_release_file,
	.llseek = no_llseek,
};

static struct mlx5_vf_migration_file *
mlx5vf_pci_save_device_data(struct mlx5vf_pci_core_device *mvdev)
{
	struct mlx5_vf_migration_file *migf;
	int ret;

	migf = kzalloc(sizeof(*migf), GFP_KERNEL);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	migf->filp = anon_inode_getfile("mlx5vf_mig", &mlx5vf_save_fops, migf,
					O_RDONLY);
	if (IS_ERR(migf->filp)) {
		int err = PTR_ERR(migf->filp);

		kfree(migf);
		return ERR_PTR(err);
	}

	stream_open(migf->filp->f_inode, migf->filp);
	mutex_init(&migf->lock);

	ret = mlx5vf_cmd_query_vhca_migration_state(
		mvdev->core_device.pdev, mvdev->vhca_id, &migf->total_length);
	if (ret)
		goto out_free;

	ret = mlx5vf_add_migration_pages(
		migf, DIV_ROUND_UP_ULL(migf->total_length, PAGE_SIZE));
	if (ret)
		goto out_free;

	ret = mlx5vf_cmd_save_vhca_state(mvdev->core_device.pdev,
					 mvdev->vhca_id, migf);
	if (ret)
		goto out_free;
	return migf;
out_free:
	fput(migf->filp);
	return ERR_PTR(ret);
}

static ssize_t mlx5vf_resume_write(struct file *filp, const char __user *buf,
				   size_t len, loff_t *pos)
{
	struct mlx5_vf_migration_file *migf = filp->private_data;
	loff_t requested_length;
	ssize_t done = 0;

	if (pos)
		return -ESPIPE;
	pos = &filp->f_pos;

	if (*pos < 0 ||
	    check_add_overflow((loff_t)len, *pos, &requested_length))
		return -EINVAL;

	if (requested_length > MAX_MIGRATION_SIZE)
		return -ENOMEM;

	mutex_lock(&migf->lock);
	if (migf->disabled) {
		done = -ENODEV;
		goto out_unlock;
	}

	if (migf->allocated_length < requested_length) {
		done = mlx5vf_add_migration_pages(
			migf,
			DIV_ROUND_UP(requested_length - migf->allocated_length,
				     PAGE_SIZE));
		if (done)
			goto out_unlock;
	}

	while (len) {
		size_t page_offset;
		struct page *page;
		size_t page_len;
		u8 *to_buff;
		int ret;

		page_offset = (*pos) % PAGE_SIZE;
		page = mlx5vf_get_migration_page(migf, *pos - page_offset);
		if (!page) {
			if (done == 0)
				done = -EINVAL;
			goto out_unlock;
		}

		page_len = min_t(size_t, len, PAGE_SIZE - page_offset);
		to_buff = kmap_local_page(page);
		ret = copy_from_user(to_buff + page_offset, buf, page_len);
		kunmap_local(to_buff);
		if (ret) {
			done = -EFAULT;
			goto out_unlock;
		}
		*pos += page_len;
		len -= page_len;
		done += page_len;
		buf += page_len;
		migf->total_length += page_len;
	}
out_unlock:
	mutex_unlock(&migf->lock);
	return done;
}

static const struct file_operations mlx5vf_resume_fops = {
	.owner = THIS_MODULE,
	.write = mlx5vf_resume_write,
	.release = mlx5vf_release_file,
	.llseek = no_llseek,
};

static struct mlx5_vf_migration_file *
mlx5vf_pci_resume_device_data(struct mlx5vf_pci_core_device *mvdev)
{
	struct mlx5_vf_migration_file *migf;

	migf = kzalloc(sizeof(*migf), GFP_KERNEL);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	migf->filp = anon_inode_getfile("mlx5vf_mig", &mlx5vf_resume_fops, migf,
					O_WRONLY);
	if (IS_ERR(migf->filp)) {
		int err = PTR_ERR(migf->filp);

		kfree(migf);
		return ERR_PTR(err);
	}
	stream_open(migf->filp->f_inode, migf->filp);
	mutex_init(&migf->lock);
	return migf;
}

static void mlx5vf_disable_fds(struct mlx5vf_pci_core_device *mvdev)
{
	if (mvdev->resuming_migf) {
		mlx5vf_disable_fd(mvdev->resuming_migf);
		fput(mvdev->resuming_migf->filp);
		mvdev->resuming_migf = NULL;
	}
	if (mvdev->saving_migf) {
		mlx5vf_disable_fd(mvdev->saving_migf);
		fput(mvdev->saving_migf->filp);
		mvdev->saving_migf = NULL;
	}
}

static struct file *
mlx5vf_pci_step_device_state_locked(struct mlx5vf_pci_core_device *mvdev,
				    u32 new)
{
	u32 cur = mvdev->mig_state;
	int ret;

	if (cur == VFIO_DEVICE_STATE_RUNNING_P2P && new == VFIO_DEVICE_STATE_STOP) {
		ret = mlx5vf_cmd_suspend_vhca(
			mvdev->core_device.pdev, mvdev->vhca_id,
			MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_RESPONDER);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RUNNING_P2P) {
		ret = mlx5vf_cmd_resume_vhca(
			mvdev->core_device.pdev, mvdev->vhca_id,
			MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_RESPONDER);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_RUNNING_P2P) {
		ret = mlx5vf_cmd_suspend_vhca(
			mvdev->core_device.pdev, mvdev->vhca_id,
			MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_INITIATOR);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_RUNNING_P2P && new == VFIO_DEVICE_STATE_RUNNING) {
		ret = mlx5vf_cmd_resume_vhca(
			mvdev->core_device.pdev, mvdev->vhca_id,
			MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_INITIATOR);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_STOP_COPY) {
		struct mlx5_vf_migration_file *migf;

		migf = mlx5vf_pci_save_device_data(mvdev);
		if (IS_ERR(migf))
			return ERR_CAST(migf);
		get_file(migf->filp);
		mvdev->saving_migf = migf;
		return migf->filp;
	}

	if ((cur == VFIO_DEVICE_STATE_STOP_COPY && new == VFIO_DEVICE_STATE_STOP)) {
		mlx5vf_disable_fds(mvdev);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP && new == VFIO_DEVICE_STATE_RESUMING) {
		struct mlx5_vf_migration_file *migf;

		migf = mlx5vf_pci_resume_device_data(mvdev);
		if (IS_ERR(migf))
			return ERR_CAST(migf);
		get_file(migf->filp);
		mvdev->resuming_migf = migf;
		return migf->filp;
	}

	if (cur == VFIO_DEVICE_STATE_RESUMING && new == VFIO_DEVICE_STATE_STOP) {
		ret = mlx5vf_cmd_load_vhca_state(mvdev->core_device.pdev,
						 mvdev->vhca_id,
						 mvdev->resuming_migf);
		if (ret)
			return ERR_PTR(ret);
		mlx5vf_disable_fds(mvdev);
		return NULL;
	}

	/*
	 * vfio_mig_get_next_state() does not use arcs other than the above
	 */
	WARN_ON(true);
	return ERR_PTR(-EINVAL);
}

/*
 * This function is called in all state_mutex unlock cases to
 * handle a 'deferred_reset' if exists.
 */
static void mlx5vf_state_mutex_unlock(struct mlx5vf_pci_core_device *mvdev)
{
again:
	spin_lock(&mvdev->reset_lock);
	if (mvdev->deferred_reset) {
		mvdev->deferred_reset = false;
		spin_unlock(&mvdev->reset_lock);
		mvdev->mig_state = VFIO_DEVICE_STATE_RUNNING;
		mlx5vf_disable_fds(mvdev);
		goto again;
	}
	mutex_unlock(&mvdev->state_mutex);
	spin_unlock(&mvdev->reset_lock);
}

static struct file *
mlx5vf_pci_set_device_state(struct vfio_device *vdev,
			    enum vfio_device_mig_state new_state)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		vdev, struct mlx5vf_pci_core_device, core_device.vdev);
	enum vfio_device_mig_state next_state;
	struct file *res = NULL;
	int ret;

	mutex_lock(&mvdev->state_mutex);
	while (new_state != mvdev->mig_state) {
		ret = vfio_mig_get_next_state(vdev, mvdev->mig_state,
					      new_state, &next_state);
		if (ret) {
			res = ERR_PTR(ret);
			break;
		}
		res = mlx5vf_pci_step_device_state_locked(mvdev, next_state);
		if (IS_ERR(res))
			break;
		mvdev->mig_state = next_state;
		if (WARN_ON(res && new_state != mvdev->mig_state)) {
			fput(res);
			res = ERR_PTR(-EINVAL);
			break;
		}
	}
	mlx5vf_state_mutex_unlock(mvdev);
	return res;
}

static int mlx5vf_pci_get_device_state(struct vfio_device *vdev,
				       enum vfio_device_mig_state *curr_state)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		vdev, struct mlx5vf_pci_core_device, core_device.vdev);

	mutex_lock(&mvdev->state_mutex);
	*curr_state = mvdev->mig_state;
	mlx5vf_state_mutex_unlock(mvdev);
	return 0;
}

static void mlx5vf_pci_aer_reset_done(struct pci_dev *pdev)
{
	struct mlx5vf_pci_core_device *mvdev = dev_get_drvdata(&pdev->dev);

	if (!mvdev->migrate_cap)
		return;

	/*
	 * As the higher VFIO layers are holding locks across reset and using
	 * those same locks with the mm_lock we need to prevent ABBA deadlock
	 * with the state_mutex and mm_lock.
	 * In case the state_mutex was taken already we defer the cleanup work
	 * to the unlock flow of the other running context.
	 */
	spin_lock(&mvdev->reset_lock);
	mvdev->deferred_reset = true;
	if (!mutex_trylock(&mvdev->state_mutex)) {
		spin_unlock(&mvdev->reset_lock);
		return;
	}
	spin_unlock(&mvdev->reset_lock);
	mlx5vf_state_mutex_unlock(mvdev);
}

static int mlx5vf_pci_open_device(struct vfio_device *core_vdev)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		core_vdev, struct mlx5vf_pci_core_device, core_device.vdev);
	struct vfio_pci_core_device *vdev = &mvdev->core_device;
	int vf_id;
	int ret;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	if (!mvdev->migrate_cap) {
		vfio_pci_core_finish_enable(vdev);
		return 0;
	}

	vf_id = pci_iov_vf_id(vdev->pdev);
	if (vf_id < 0) {
		ret = vf_id;
		goto out_disable;
	}

	ret = mlx5vf_cmd_get_vhca_id(vdev->pdev, vf_id + 1, &mvdev->vhca_id);
	if (ret)
		goto out_disable;

	mvdev->mig_state = VFIO_DEVICE_STATE_RUNNING;
	vfio_pci_core_finish_enable(vdev);
	return 0;
out_disable:
	vfio_pci_core_disable(vdev);
	return ret;
}

static void mlx5vf_pci_close_device(struct vfio_device *core_vdev)
{
	struct mlx5vf_pci_core_device *mvdev = container_of(
		core_vdev, struct mlx5vf_pci_core_device, core_device.vdev);

	mlx5vf_disable_fds(mvdev);
	vfio_pci_core_close_device(core_vdev);
}

static const struct vfio_device_ops mlx5vf_pci_ops = {
	.name = "mlx5-vfio-pci",
	.open_device = mlx5vf_pci_open_device,
	.close_device = mlx5vf_pci_close_device,
	.ioctl = vfio_pci_core_ioctl,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read = vfio_pci_core_read,
	.write = vfio_pci_core_write,
	.mmap = vfio_pci_core_mmap,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
	.migration_set_state = mlx5vf_pci_set_device_state,
	.migration_get_state = mlx5vf_pci_get_device_state,
};

static int mlx5vf_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct mlx5vf_pci_core_device *mvdev;
	int ret;

	mvdev = kzalloc(sizeof(*mvdev), GFP_KERNEL);
	if (!mvdev)
		return -ENOMEM;
	vfio_pci_core_init_device(&mvdev->core_device, pdev, &mlx5vf_pci_ops);

	if (pdev->is_virtfn) {
		struct mlx5_core_dev *mdev =
			mlx5_vf_get_core_dev(pdev);

		if (mdev) {
			if (MLX5_CAP_GEN(mdev, migration)) {
				mvdev->migrate_cap = 1;
				mvdev->core_device.vdev.migration_flags =
					VFIO_MIGRATION_STOP_COPY |
					VFIO_MIGRATION_P2P;
				mutex_init(&mvdev->state_mutex);
				spin_lock_init(&mvdev->reset_lock);
			}
			mlx5_vf_put_core_dev(mdev);
		}
	}

	ret = vfio_pci_core_register_device(&mvdev->core_device);
	if (ret)
		goto out_free;

	dev_set_drvdata(&pdev->dev, mvdev);
	return 0;

out_free:
	vfio_pci_core_uninit_device(&mvdev->core_device);
	kfree(mvdev);
	return ret;
}

static void mlx5vf_pci_remove(struct pci_dev *pdev)
{
	struct mlx5vf_pci_core_device *mvdev = dev_get_drvdata(&pdev->dev);

	vfio_pci_core_unregister_device(&mvdev->core_device);
	vfio_pci_core_uninit_device(&mvdev->core_device);
	kfree(mvdev);
}

static const struct pci_device_id mlx5vf_pci_table[] = {
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_VENDOR_ID_MELLANOX, 0x101e) }, /* ConnectX Family mlx5Gen Virtual Function */
	{}
};

MODULE_DEVICE_TABLE(pci, mlx5vf_pci_table);

static const struct pci_error_handlers mlx5vf_err_handlers = {
	.reset_done = mlx5vf_pci_aer_reset_done,
	.error_detected = vfio_pci_core_aer_err_detected,
};

static struct pci_driver mlx5vf_pci_driver = {
	.name = KBUILD_MODNAME,
	.id_table = mlx5vf_pci_table,
	.probe = mlx5vf_pci_probe,
	.remove = mlx5vf_pci_remove,
	.err_handler = &mlx5vf_err_handlers,
};

static void __exit mlx5vf_pci_cleanup(void)
{
	pci_unregister_driver(&mlx5vf_pci_driver);
}

static int __init mlx5vf_pci_init(void)
{
	return pci_register_driver(&mlx5vf_pci_driver);
}

module_init(mlx5vf_pci_init);
module_exit(mlx5vf_pci_cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Gurtovoy <mgurtovoy@nvidia.com>");
MODULE_AUTHOR("Yishai Hadas <yishaih@nvidia.com>");
MODULE_DESCRIPTION(
	"MLX5 VFIO PCI - User Level meta-driver for MLX5 device family");
