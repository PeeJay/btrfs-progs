/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sysmacros.h>

#include <btrfsutil.h>

#include "kernel-shared/ctree.h"
#include "cmds/commands.h"
#include "common/utils.h"
#include "props.h"

#define XATTR_BTRFS_PREFIX     "btrfs."
#define XATTR_BTRFS_PREFIX_LEN (sizeof(XATTR_BTRFS_PREFIX) - 1)

/*
 * Defined as synonyms in attr/xattr.h
 */
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

static int prop_read_only(enum prop_object_type type,
			  const char *object,
			  const char *name,
			  const char *value)
{
	enum btrfs_util_error err;
	bool read_only;

	if (value) {
		if (!strcmp(value, "true")) {
			read_only = true;
		} else if (!strcmp(value, "false")) {
			read_only = false;
		} else {
			error("invalid value for property: %s", value);
			return -EINVAL;
		}

		err = btrfs_util_set_subvolume_read_only(object, read_only);
		if (err) {
			error_btrfs_util(err);
			return -errno;
		}
	} else {
		err = btrfs_util_get_subvolume_read_only(object, &read_only);
		if (err) {
			error_btrfs_util(err);
			return -errno;
		}

		printf("ro=%s\n", read_only ? "true" : "false");
	}

	return 0;
}

static int prop_label(enum prop_object_type type,
		      const char *object,
		      const char *name,
		      const char *value)
{
	int ret;

	if (value) {
		ret = set_label((char *) object, (char *) value);
	} else {
		char label[BTRFS_LABEL_SIZE];

		ret = get_label((char *) object, label);
		if (!ret)
			fprintf(stdout, "label=%s\n", label);
	}

	return ret;
}

static int prop_compression(enum prop_object_type type,
			    const char *object,
			    const char *name,
			    const char *value)
{
	int ret;
	ssize_t sret;
	int fd = -1;
	DIR *dirstream = NULL;
	char *buf = NULL;
	char *xattr_name = NULL;
	int open_flags = value ? O_RDWR : O_RDONLY;

	fd = open_file_or_dir3(object, &dirstream, open_flags);
	if (fd == -1) {
		ret = -errno;
		error("failed to open %s: %m", object);
		goto out;
	}

	xattr_name = malloc(XATTR_BTRFS_PREFIX_LEN + strlen(name) + 1);
	if (!xattr_name) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy(xattr_name, XATTR_BTRFS_PREFIX, XATTR_BTRFS_PREFIX_LEN);
	memcpy(xattr_name + XATTR_BTRFS_PREFIX_LEN, name, strlen(name));
	xattr_name[XATTR_BTRFS_PREFIX_LEN + strlen(name)] = '\0';

	if (value) {
		if (strcmp(value, "no") == 0 || strcmp(value, "none") == 0)
			value = "";
		sret = fsetxattr(fd, xattr_name, value, strlen(value), 0);
	} else {
		sret = fgetxattr(fd, xattr_name, NULL, 0);
	}
	if (sret < 0) {
		ret = -errno;
		if (ret != -ENOATTR)
			error("failed to %s compression for %s: %m",
			      value ? "set" : "get", object);
		else
			ret = 0;
		goto out;
	}
	if (!value) {
		size_t len = sret;

		buf = malloc(len);
		if (!buf) {
			ret = -ENOMEM;
			goto out;
		}
		sret = fgetxattr(fd, xattr_name, buf, len);
		if (sret < 0) {
			ret = -errno;
			error("failed to get compression for %s: %m", object);
			goto out;
		}
		fprintf(stdout, "compression=%.*s\n", (int)len, buf);
	}

	ret = 0;
out:
	free(xattr_name);
	free(buf);
	if (fd >= 0)
		close_file_or_dir(fd, dirstream);

	return ret;
}

static int btrfs_find_devid_and_mnt(const char *devpath, int *devid,
				    char *path, int maxpath)
{
	int ret, i, fd;
	DIR *dir;
	struct stat stdevpath;
	struct btrfs_ioctl_fs_info_args fi_args;
	struct btrfs_ioctl_dev_info_args dev_info;

	ret = get_btrfs_mount(devpath, path, maxpath);
	if (ret)
		return ret;

	fd = btrfs_open_dir(path, &dir, 1);
	if (fd < 0)
		return fd;

	ret = stat(devpath, &stdevpath);
	if (ret) {
		error("cannot stat '%s'", devpath);
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_FS_INFO, &fi_args);
	if (ret < 0) {
		if (errno == EPERM)
			return -errno;
		error("cannot get filesystem info: %m");
		ret = -10;
		goto out;
	}

	for (i = 0 ; i <= fi_args.max_id ; i++) {
		struct stat st;

		memset(&dev_info, 0, sizeof(dev_info));
		ret = get_device_info(fd, i, &dev_info);
		if (ret == -ENODEV)
			continue;
		if (ret) {
			error("cannot get info about device devid=%d", i);
			goto out;
		}

		if (!dev_info.path)
			/* missing devices */
			continue;

		ret = stat((char *)dev_info.path, &st);
		if (ret) {
			error("cannot stat '%s'", devpath);
			goto out;
		}

		if (major(st.st_rdev) == major(stdevpath.st_rdev) &&
		    minor(st.st_rdev) == minor(stdevpath.st_rdev)) {
			*devid = dev_info.devid;
			ret = 0;
			goto out;
		}
	}

	ret = -12;

out:
	close_file_or_dir(fd, dir);
	return ret;
}

static struct ull_charp_pair_t {
	u64		value;
	const char	*descr;
} allocation_hint_description[] = {
	{BTRFS_DEV_ALLOCATION_PREFERRED_METADATA, "PREFERRED_METADATA"},
	{BTRFS_DEV_ALLOCATION_METADATA_ONLY, "METADATA_ONLY"},
	{BTRFS_DEV_ALLOCATION_PREFERRED_DATA, "PREFERRED_DATA"},
	{BTRFS_DEV_ALLOCATION_DATA_ONLY, "DATA_ONLY"},
	{0, NULL}
};


static int prop_allocation_hint(enum prop_object_type type,
				const char *object,
				const char *name,
				const char *value)
{
	int ret, devid, fd;
	char path[PATH_MAX];
	DIR *dir;
	struct btrfs_ioctl_dev_properties props;
	int i;
	u64 v;

	ret = btrfs_find_devid_and_mnt(object, &devid, path, sizeof(path));
	if (ret)
		return -5;

	fd = btrfs_open_dir(path, &dir, 1);
	if (fd < 0)
		return fd;

	memset(&props, 0, sizeof(props));
	props.devid = devid;
	props.properties = BTRFS_DEV_PROPERTY_TYPE|BTRFS_DEV_PROPERTY_READ;
	ret = ioctl(fd, BTRFS_IOC_DEV_PROPERTIES, &props);
	if (ret < 0) {
		error("Cannot perform BTRFS_IOC_DEV_PROPERTIES ioctl on '%s'",
				path);
		ret = -1;
		goto out;
	}

	if (!value) {
		v = props.type & BTRFS_DEV_ALLOCATION_MASK;
		for (i = 0 ; allocation_hint_description[i].descr ; i++)
			if (v == allocation_hint_description[i].value)
				break;
		if (allocation_hint_description[i].descr)
			printf("devid=%d, path=%s: allocation_hint=%s\n",
				devid, object,
				allocation_hint_description[i].descr);
		else
			printf("devid=%d, path=%s: allocation_hint=unknown:%llu\n",
				devid, object,
				v);
		ret = 0;
		goto out;
	}

	for (i = 0 ; allocation_hint_description[i].descr ; i++)
		if (!strcmp(value, allocation_hint_description[i].descr))
			break;

	if (allocation_hint_description[i].descr) {
		v = allocation_hint_description[i].value;
	} else if (sscanf(value, "%llu", &v) != 1) {
		error("Invalid value '%s'\n", value);
		ret = -3;
		goto out;
	} else if (v & ~BTRFS_DEV_ALLOCATION_MASK) {
		error("Invalid value '%s'\n", value);
		ret = -3;
		goto out;
	}

	props.type &= ~BTRFS_DEV_ALLOCATION_MASK;
	props.type |= (v & BTRFS_DEV_ALLOCATION_MASK);

	props.properties = BTRFS_DEV_PROPERTY_TYPE;
	props.devid = devid;
	ret = ioctl(fd, BTRFS_IOC_DEV_PROPERTIES, &props);
	if (ret < 0) {
		error("Cannot perform BTRFS_IOC_DEV_PROPERTIES ioctl on '%s'",
			path);
		ret = -4;
		goto out;
	}

	ret = 0;
out:
	close_file_or_dir(fd, dir);
	return ret;
}

const struct prop_handler prop_handlers[] = {
	{
		.name ="ro",
		.desc = "read-only status of a subvolume",
		.read_only = 0,
		.types = prop_object_subvol,
	 	.handler = prop_read_only
	},
	{
		.name = "label",
		.desc = "label of the filesystem",
		.read_only = 0,
		.types = prop_object_dev | prop_object_root,
		.handler = prop_label
	},
	{
		.name = "compression",
		.desc = "compression algorithm for the file or directory",
		.read_only = 0,
	 	.types = prop_object_inode, prop_compression
	},
	{
		.name = "allocation_hint",
		.desc = "hint to store the data/metadata chunks",
		.types = prop_object_dev,
		.handler = prop_allocation_hint
	},
	{NULL, NULL, 0, 0, NULL}
};
