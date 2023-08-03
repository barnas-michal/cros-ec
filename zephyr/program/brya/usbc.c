#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
// #include <zephyr/drivers/usb_c/tcpci_priv.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
// #include <zephyr/usb_c/tcpci.h>
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

int port1_policy_cb_src_en(const struct device *dev, bool en)
{
	// source_ctrl_set(en ? SOURCE_5V : SOURCE_0V);

	return 0;
}

#define SOURCE_PDO(node_id, prop, idx)	(DT_PROP_BY_IDX(node_id, prop, idx)),
uint32_t src_caps[DT_PROP_LEN(DT_NODELABEL(usbc_port1), source_pdos)] = {DT_FOREACH_PROP_ELEM(DT_NODELABEL(usbc_port1), source_pdos, SOURCE_PDO)};
uint32_t src_cap_cnt = DT_PROP_LEN(DT_NODELABEL(usbc_port1), source_pdos);

int port1_policy_cb_get_src_caps(const struct device *dev,
			const uint32_t **pdos, uint32_t *num_pdos)
{
	struct port1_data_t *dpm_data = usbc_get_dpm_data(dev);

	*pdos = src_caps;
	*num_pdos = src_cap_cnt;

	return 0;
}

static enum usbc_snk_req_reply_t port1_policy_cb_check_sink_request(const struct device *dev,
					const uint32_t request_msg)
{
	return SNK_REQUEST_VALID;
}

static bool port1_policy_cb_is_ps_ready(const struct device *dev)
{
	return true;
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
		/*usbc_set_policy_cb_get_rdo(usbc_port, policy_cb_get_rdo);*/ \
		usbc_set_policy_cb_src_en(usbc_port, port1_policy_cb_src_en); \
		usbc_set_policy_cb_get_src_caps(usbc_port, port1_policy_cb_get_src_caps);\
		usbc_set_policy_cb_check_sink_request(usbc_port, port1_policy_cb_check_sink_request); \
		usbc_set_policy_cb_is_ps_ready(usbc_port, port1_policy_cb_is_ps_ready); \
		ret |= usbc_start(usbc_port);                             \
		usbc_request(usbc_port, REQUEST_NOP); \
		usbc_request(usbc_port, REQUEST_NOP); \
		usbc_request(usbc_port, REQUEST_NOP); \
	}

static int cmd_startpd(const struct shell *sh, size_t argc, char **argv)
{
	int ret = 0;

	DT_N_S_usbc_FOREACH_CHILD(USBC_START);

	return ret;
}
SHELL_CMD_REGISTER(startpd, NULL, "Start PD communication", cmd_startpd);

int startpd_fn()
{
	return cmd_startpd(NULL, 0, NULL);
}

#define MY_INIT_PRIO 99
SYS_INIT(startpd_fn, APPLICATION, MY_INIT_PRIO);
