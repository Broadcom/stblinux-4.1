/*
 * Copyright (C) 2010 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/usb/hcd.h>
#include <linux/phy/phy.h>

/***********************************************************************
 * Library functions
 ***********************************************************************/

int brcm_usb_probe(struct platform_device *pdev,
		const struct hc_driver *hc_driver,
		struct usb_hcd **hcdptr,
		struct clk **hcd_clk_ptr,
		struct phy **hcd_phy_ptr)
{
	struct resource *res_mem;
	int irq;
	struct usb_hcd *hcd;
	struct device_node *dn = pdev->dev.of_node;
	struct clk *usb_clk;
	struct phy *phy;
	int err;

	if (usb_disabled())
		return -ENODEV;

	/* Get the phy */
	phy = devm_phy_get(&pdev->dev, "usbphy");
	if (IS_ERR(phy)) {
		if (PTR_ERR(phy) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		dev_err(&pdev->dev, "USB Phy not found.\n");
		return PTR_ERR(phy);
	}
	phy_init(phy);
	*hcd_phy_ptr = phy;

	res_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res_mem) {
		dev_err(&pdev->dev, "platform_get_resource error.\n");
		return -ENODEV;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "platform_get_irq error.\n");
		return -ENODEV;
	}

	usb_clk = of_clk_get_by_name(dn, "sw_usb");
	if (IS_ERR(usb_clk)) {
		if (PTR_ERR(usb_clk) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		dev_err(&pdev->dev, "Clock not found in Device Tree\n");
		usb_clk = NULL;
	}
	err = clk_prepare_enable(usb_clk);
	if (err)
		return err;
	*hcd_clk_ptr = usb_clk;

	/* initialize hcd */
	hcd = usb_create_hcd(hc_driver, &pdev->dev, dev_name(&pdev->dev));
	if (!hcd) {
		dev_err(&pdev->dev, "Failed to create hcd\n");
		clk_disable(usb_clk);
		return -ENOMEM;
	}
	*hcdptr = hcd;
	hcd->rsrc_start = res_mem->start;
	hcd->rsrc_len = resource_size(res_mem);

	hcd->regs = devm_ioremap_resource(&pdev->dev, res_mem);
	if (IS_ERR(hcd->regs)) {
		err = PTR_ERR(hcd->regs);
		goto err_put_hcd;
	}
	err = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (err)
		goto err_put_hcd;

	device_wakeup_enable(hcd->self.controller);
	platform_set_drvdata(pdev, hcd);
	return err;

err_put_hcd:
	clk_disable(usb_clk);
	usb_put_hcd(hcd);

	return err;
}
EXPORT_SYMBOL(brcm_usb_probe);

int brcm_usb_remove(struct platform_device *pdev, struct clk *hcd_clk)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);
	usb_remove_hcd(hcd);
	usb_put_hcd(hcd);
	clk_disable(hcd_clk);

	return 0;
}
EXPORT_SYMBOL(brcm_usb_remove);

MODULE_DESCRIPTION("BRCM USB Glue");
MODULE_AUTHOR("Al Cooper");
MODULE_LICENSE("GPL");
