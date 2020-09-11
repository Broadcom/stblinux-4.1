/*
 * sdhci-brcmstb.c Support for SDHCI on Broadcom BRCMSTB SoC's
 *
 * Copyright (C) 2015 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/pinctrl-state.h>

#include "sdhci-pltfm.h"

struct sdhci_brcmstb_priv {
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
};

static int sdhci_brcmstb_enable_dma(struct sdhci_host *host)
{
	if (host->flags & SDHCI_USE_64_BIT_DMA)
		if (host->quirks2 & SDHCI_QUIRK2_BROKEN_64_BIT_DMA)
			host->flags &= ~SDHCI_USE_64_BIT_DMA;
	return 0;
}

#ifdef CONFIG_PM_SLEEP

static int sdhci_brcmstb_suspend(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	int res;

	res = sdhci_suspend_host(host);
	if (res)
		return res;
	clk_disable(pltfm_host->clk);
	return res;
}

static int sdhci_brcmstb_resume(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_brcmstb_priv *priv = pltfm_host->priv;
	int err;

	if (!IS_ERR(priv->pins_default))
		pinctrl_select_state(priv->pinctrl, priv->pins_default);

	err = clk_enable(pltfm_host->clk);
	if (err)
		return err;
	return sdhci_resume_host(host);
}

#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(sdhci_brcmstb_pmops, sdhci_brcmstb_suspend,
			sdhci_brcmstb_resume);

static const struct sdhci_ops sdhci_brcmstb_ops = {
	.set_clock = sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset = sdhci_reset,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
	.enable_dma = sdhci_brcmstb_enable_dma,
};

static struct sdhci_pltfm_data sdhci_brcmstb_pdata = {
	.ops = &sdhci_brcmstb_ops,
};

static int sdhci_brcmstb_probe(struct platform_device *pdev)
{
	struct device_node *dn = pdev->dev.of_node;
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_brcmstb_priv *priv;
	struct clk *clk;
	int res;

	clk = of_clk_get_by_name(dn, "sw_sdio");
	if (IS_ERR(clk)) {
		if (PTR_ERR(clk) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_err(&pdev->dev, "Clock not found in Device Tree\n");
		clk = NULL;
	}
	res = clk_prepare_enable(clk);
	if (res)
		goto undo_clk_get;

	host = sdhci_pltfm_init(pdev, &sdhci_brcmstb_pdata, 0);
	if (IS_ERR(host)) {
		res = PTR_ERR(host);
		goto undo_clk_prep;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		res = -ENOMEM;
		goto undo_clk_prep;
	}

	/* Enable MMC_CAP2_HC_ERASE_SZ for better max discard calculations */
	host->mmc->caps2 |= MMC_CAP2_HC_ERASE_SZ;

	sdhci_get_of_property(pdev);
	mmc_of_parse(host->mmc);

	priv->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(priv->pinctrl)) {
		res = PTR_ERR(priv->pinctrl);
		goto undo_pltfm_init;
	}

	priv->pins_default = pinctrl_lookup_state(priv->pinctrl,
						  PINCTRL_STATE_DEFAULT);

	res = sdhci_add_host(host);
	if (res)
		goto undo_pltfm_init;

	pltfm_host = sdhci_priv(host);
	pltfm_host->priv = priv;
	pltfm_host->clk = clk;
	return res;

undo_pltfm_init:
	sdhci_pltfm_free(pdev);
undo_clk_prep:
	clk_disable_unprepare(clk);
undo_clk_get:
	clk_put(clk);
	return res;
}

static const struct of_device_id sdhci_brcm_of_match[] = {
	{ .compatible = "brcm,sdhci-brcmstb" },
	{},
};
MODULE_DEVICE_TABLE(of, sdhci_brcm_of_match);

static struct platform_driver sdhci_brcmstb_driver = {
	.driver		= {
		.name	= "sdhci-brcmstb",
		.owner	= THIS_MODULE,
		.pm	= &sdhci_brcmstb_pmops,
		.of_match_table = of_match_ptr(sdhci_brcm_of_match),
	},
	.probe		= sdhci_brcmstb_probe,
	.remove		= sdhci_pltfm_unregister,
};

module_platform_driver(sdhci_brcmstb_driver);

MODULE_DESCRIPTION("SDHCI driver for Broadcom BRCMSTB SoCs");
MODULE_AUTHOR("Broadcom");
MODULE_LICENSE("GPL v2");
