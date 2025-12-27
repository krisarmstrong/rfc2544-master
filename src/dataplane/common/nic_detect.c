/*
 * nic_detect.c - NIC Auto-Detection and Capability Discovery
 *
 * Detects network interface capabilities including:
 * - Link speed
 * - Hardware timestamping support
 * - XDP/AF_XDP support
 * - MTU and MAC address
 */

#include "rfc2544.h"
#include "rfc2544_internal.h"
#include "platform_config.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>

#ifdef __linux__
#include <linux/ethtool.h>
#include <linux/if.h>
#include <linux/sockios.h>
#endif

#ifdef __linux__
/**
 * Read sysfs value as string
 */
static int read_sysfs(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -errno;

	if (!fgets(buf, len, f)) {
		fclose(f);
		return -EIO;
	}

	fclose(f);

	/* Remove trailing newline */
	size_t slen = strlen(buf);
	if (slen > 0 && buf[slen - 1] == '\n')
		buf[slen - 1] = '\0';

	return 0;
}

/**
 * Read sysfs value as uint64
 */
static int read_sysfs_u64(const char *path, uint64_t *value)
{
	char buf[64];
	int ret = read_sysfs(path, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	*value = strtoull(buf, NULL, 10);
	return 0;
}

/**
 * Check if interface supports XDP
 */
static bool check_xdp_support(const char *interface)
{
	/* Check for XDP support by looking for driver XDP capability */
	char path[256];
	snprintf(path, sizeof(path), "/sys/class/net/%s/device/driver", interface);

	if (access(path, F_OK) != 0)
		return false;

	/* Known XDP-capable drivers */
	static const char *xdp_drivers[] = {
		"i40e", "ixgbe", "mlx4_en", "mlx5_core", "nfp", "virtio_net",
		"veth", "tun", "bnxt_en", "qede", "igb", "e1000e", NULL
	};

	char driver_link[512];
	ssize_t len = readlink(path, driver_link, sizeof(driver_link) - 1);
	if (len < 0)
		return false;
	driver_link[len] = '\0';

	/* Extract driver name from path */
	char *driver_name = strrchr(driver_link, '/');
	if (driver_name)
		driver_name++;
	else
		driver_name = driver_link;

	for (int i = 0; xdp_drivers[i]; i++) {
		if (strstr(driver_name, xdp_drivers[i]))
			return true;
	}

	return false;
}

/**
 * Check if interface supports hardware timestamping
 */
static bool check_hw_timestamp_support(const char *interface)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return false;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);

	struct ethtool_ts_info ts_info;
	memset(&ts_info, 0, sizeof(ts_info));
	ts_info.cmd = ETHTOOL_GET_TS_INFO;
	ifr.ifr_data = (char *)&ts_info;

	bool supported = false;
	if (ioctl(fd, SIOCETHTOOL, &ifr) >= 0) {
		/* Check for hardware TX/RX timestamps */
		supported = (ts_info.so_timestamping &
		             (SOF_TIMESTAMPING_TX_HARDWARE |
		              SOF_TIMESTAMPING_RX_HARDWARE)) != 0;
	}

	close(fd);
	return supported;
}
#endif /* __linux__ */

/**
 * Detect NIC capabilities
 */
int rfc2544_detect_nic(const char *interface, nic_info_t *info)
{
	if (!interface || !info)
		return -EINVAL;

	memset(info, 0, sizeof(*info));
	strncpy(info->name, interface, sizeof(info->name) - 1);

#ifdef __linux__
	char path[256];

	/* Check if interface exists */
	snprintf(path, sizeof(path), "/sys/class/net/%s", interface);
	if (access(path, F_OK) != 0)
		return -ENOENT;

	/* Get link speed */
	snprintf(path, sizeof(path), "/sys/class/net/%s/speed", interface);
	uint64_t speed_mbps = 0;
	if (read_sysfs_u64(path, &speed_mbps) == 0) {
		info->link_speed = speed_mbps * 1000000ULL; /* Convert to bps */
	}

	/* Get operstate (up/down) */
	snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", interface);
	char operstate[32];
	if (read_sysfs(path, operstate, sizeof(operstate)) == 0) {
		info->is_up = (strcmp(operstate, "up") == 0);
	}

	/* Get MTU */
	snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", interface);
	uint64_t mtu;
	if (read_sysfs_u64(path, &mtu) == 0) {
		info->mtu = (uint32_t)mtu;
	}

	/* Get MAC address */
	snprintf(path, sizeof(path), "/sys/class/net/%s/address", interface);
	char mac_str[32];
	if (read_sysfs(path, mac_str, sizeof(mac_str)) == 0) {
		unsigned int mac[6];
		if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
		           &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
			for (int i = 0; i < 6; i++)
				info->mac[i] = (uint8_t)mac[i];
		}
	}

	/* Check XDP support */
	info->supports_xdp = check_xdp_support(interface);

	/* Check hardware timestamping */
	info->supports_hw_ts = check_hw_timestamp_support(interface);

#else
	/* macOS: Use ioctl for basic info */
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -errno;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, interface, sizeof(ifr.ifr_name) - 1);

	/* Get flags (up/down) */
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) >= 0) {
		info->is_up = (ifr.ifr_flags & IFF_UP) != 0;
	}

	/* Get MTU */
	if (ioctl(fd, SIOCGIFMTU, &ifr) >= 0) {
		info->mtu = ifr.ifr_mtu;
	}

	close(fd);

	/* Estimate link speed based on interface name */
	if (strstr(interface, "en") == interface) {
		info->link_speed = 1000000000ULL; /* Assume 1G for Ethernet */
	}
#endif

	rfc2544_log(LOG_INFO, "NIC %s: %s, speed=%lu Mbps, MTU=%u, XDP=%s, HW-TS=%s",
	            info->name, info->is_up ? "UP" : "DOWN",
	            info->link_speed / 1000000,
	            info->mtu,
	            info->supports_xdp ? "yes" : "no",
	            info->supports_hw_ts ? "yes" : "no");

	return 0;
}

/**
 * List available network interfaces suitable for testing
 */
int rfc2544_list_interfaces(nic_info_t *interfaces, uint32_t max_count)
{
	if (!interfaces || max_count == 0)
		return -EINVAL;

	uint32_t count = 0;

#ifdef __linux__
	DIR *dir = opendir("/sys/class/net");
	if (!dir)
		return -errno;

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && count < max_count) {
		/* Skip . and .. */
		if (entry->d_name[0] == '.')
			continue;

		/* Skip loopback */
		if (strcmp(entry->d_name, "lo") == 0)
			continue;

		/* Get interface info */
		if (rfc2544_detect_nic(entry->d_name, &interfaces[count]) == 0) {
			count++;
		}
	}

	closedir(dir);
#else
	/* macOS: Use ifconfig or similar */
	/* For simplicity, check common interface names */
	const char *common_interfaces[] = {"en0", "en1", "en2", "en3", NULL};

	for (int i = 0; common_interfaces[i] && count < max_count; i++) {
		if (rfc2544_detect_nic(common_interfaces[i], &interfaces[count]) == 0) {
			count++;
		}
	}
#endif

	return (int)count;
}

/**
 * Recommend best interface for testing
 */
int rfc2544_recommend_interface(nic_info_t *info)
{
	if (!info)
		return -EINVAL;

	nic_info_t interfaces[16];
	int count = rfc2544_list_interfaces(interfaces, 16);
	if (count <= 0)
		return -ENOENT;

	/* Score each interface */
	int best_idx = -1;
	int best_score = -1;

	for (int i = 0; i < count; i++) {
		int score = 0;

		/* Must be up */
		if (!interfaces[i].is_up)
			continue;

		/* Higher speed is better */
		score += interfaces[i].link_speed / 1000000000ULL; /* Points per Gbps */

		/* XDP support is valuable */
		if (interfaces[i].supports_xdp)
			score += 10;

		/* Hardware timestamping is valuable */
		if (interfaces[i].supports_hw_ts)
			score += 5;

		/* Jumbo MTU is good */
		if (interfaces[i].mtu >= 9000)
			score += 3;

		if (score > best_score) {
			best_score = score;
			best_idx = i;
		}
	}

	if (best_idx < 0)
		return -ENOENT;

	memcpy(info, &interfaces[best_idx], sizeof(nic_info_t));

	rfc2544_log(LOG_INFO, "Recommended interface: %s (score=%d)", info->name, best_score);

	return 0;
}
