#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/usb_c/tcpci_priv.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/usb_c/tcpci.h>
#include <zephyr/usb_c/usbc.h>

bool policy_cb_check(const struct device *dev,
		     const enum usbc_policy_check_t policy_check)
{
	switch (policy_check) {
	case CHECK_POWER_ROLE_SWAP:
		/* Reject power role swaps */
		return false;
	case CHECK_DATA_ROLE_SWAP_TO_DFP:
		/* Reject data role swap to DFP */
		return false;
	case CHECK_DATA_ROLE_SWAP_TO_UFP:
		/* Accept data role swap to UFP */
		return true;
	case CHECK_SNK_AT_DEFAULT_LEVEL:
		/* This device is always at the default power level */
		return true;
	default:
		/* Reject all other policy checks */
		return false;
	}
}

uint32_t policy_cb_get_rdo(const struct device *dev)
{
	union pd_rdo rdo;

	/* Maximum operating current 100mA (GIVEBACK = 0) */
	rdo.fixed.min_or_max_operating_current =
		PD_CONVERT_MA_TO_FIXED_PDO_CURRENT(3000);
	/* Operating current 100mA */
	rdo.fixed.operating_current = PD_CONVERT_MA_TO_FIXED_PDO_CURRENT(3000);
	/* Unchunked Extended Messages Not Supported */
	rdo.fixed.unchunked_ext_msg_supported = 0;
	/* No USB Suspend */
	rdo.fixed.no_usb_suspend = 1;
	/* Not USB Communications Capable */
	rdo.fixed.usb_comm_capable = 1;
	/* No capability mismatch */
	rdo.fixed.cap_mismatch = 0;
	/* Don't giveback */
	rdo.fixed.giveback = 0;
	/* Object position 1 (5V PDO -> 9V) */
	rdo.fixed.object_pos = 2;

	return rdo.raw_value;
}

#define USBC_START(node)                                                  \
	{                                                                 \
		const struct device *usbc_port;                           \
		usbc_port = DEVICE_DT_GET(node);                          \
		if (!device_is_ready(usbc_port)) {                        \
			printk("Device %s not ready\n", usbc_port->name); \
			ret |= -EIO;                                      \
		}                                                         \
		usbc_set_policy_cb_check(usbc_port, policy_cb_check);     \
		usbc_set_policy_cb_get_rdo(usbc_port, policy_cb_get_rdo); \
		ret |= usbc_start(usbc_port);                             \
	}

static int cmd_startpd(const struct shell *sh, size_t argc, char **argv)
{
	int ret = 0;

	DT_N_S_usbc_FOREACH_CHILD(USBC_START);

	return ret;
}
SHELL_CMD_REGISTER(startpd, NULL, "Start PD communication", cmd_startpd);
