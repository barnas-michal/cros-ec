/* Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <zephyr/ztest_assert.h>
#include <zephyr/ztest_test_new.h>

#include "usb_pd_flags.h"

ZTEST_SUITE(usb_pd_flags, NULL, NULL, NULL, NULL, NULL);

ZTEST_USER(usb_pd_flags, test_usb_pd_charger_otg)
{
	set_usb_pd_charger_otg(USB_PD_CHARGER_OTG_ENABLED);
	zassert_equal(get_usb_pd_charger_otg(), USB_PD_CHARGER_OTG_ENABLED);

	set_usb_pd_charger_otg(USB_PD_CHARGER_OTG_DISABLED);
	zassert_equal(get_usb_pd_charger_otg(), USB_PD_CHARGER_OTG_DISABLED);
}
