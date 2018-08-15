#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct sharp_panel {
    struct drm_panel base;
    struct mipi_dsi_device *dsi;

    struct regulator *supply;
    struct gpio_desc *reset_gpio;
    struct gpio_desc *enable_gpio;

    bool prepared;
    bool enabled;

    const struct drm_display_mode *mode;
};

static inline struct sharp_panel *to_sharp_panel(struct drm_panel *panel)
{
	return container_of(panel, struct sharp_panel, base);
}


static int sharp_panel_off(struct sharp_panel *sharp)
{
    struct mipi_dsi_device *dsi = sharp->dsi;
    int ret;

    printk("sharp_panel_off()\n");

    dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

    ret = mipi_dsi_dcs_set_display_off(dsi);
    if (ret < 0)
        return ret;

    ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
    if (ret < 0)
        return ret;

    return 0;
}

static int sharp_panel_on(struct sharp_panel *sharp)
{
    struct mipi_dsi_device *dsi = sharp->dsi;
    int ret;

    printk("sharp_panel_on()\n");
    dsi->mode_flags |= MIPI_DSI_MODE_LPM;

    ret = mipi_dsi_dcs_set_display_on(dsi);
    if (ret < 0)
        return ret;

    return 0;
}

static int sharp_panel_init(struct sharp_panel *sharp)
{
    struct mipi_dsi_device *dsi = sharp->dsi;
    int ret;

    printk("sharp_panel_init()\n");

    dsi->mode_flags |= MIPI_DSI_MODE_LPM;

    ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
    if (ret < 0)
        return ret;

    msleep(120);

    return 0;
}

static int sharp_panel_disable(struct drm_panel *panel)
{
    struct sharp_panel *sharp = to_sharp_panel(panel);

    printk("sharp_panel_disable()\n");
    if (!sharp->enabled)
        return 0;

    sharp->enabled = false;

    return 0;
}

static int sharp_panel_unprepare(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	int ret;

	printk("sharp_panel_unprepare()\n");

	if (!sharp->prepared)
		return 0;

	ret = sharp_panel_off(sharp);
	if (ret < 0) {
		dev_err(panel->dev, "failed to set panel off: %d\n", ret);
		return ret;
	}

	msleep(120);

	if (sharp->supply) {
		regulator_disable(sharp->supply);
	}
	if (sharp->reset_gpio)
        	gpiod_set_value(sharp->reset_gpio, 0);

	sharp->prepared = false;

	return 0;
}

static int sharp_panel_prepare(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);
	int ret;

	printk("sharp_panel_prepare()\n");

	if (sharp->prepared)
		return 0;

	if (sharp->supply) {
		ret = regulator_enable(sharp->supply);
		if (ret < 0)
			return ret;
		msleep(20);
	}

	if (sharp->enable_gpio) {
		gpiod_set_value(sharp->enable_gpio, 1);
		msleep(100);
	}

	printk("sharp->reset_gpio\n");
	if (sharp->reset_gpio) {
		gpiod_set_value(sharp->reset_gpio, 0);
		msleep(20);
		gpiod_set_value(sharp->reset_gpio, 1);
		msleep(50);
		gpiod_set_value(sharp->reset_gpio, 0);
		msleep(10);
	}

	ret = sharp_panel_init(sharp);
	if (ret < 0) {
		dev_err(panel->dev, "failed to init panel: %d\n", ret);
		goto poweroff;
	}

    ret = sharp_panel_on(sharp);
    if (ret < 0) {
        dev_err(panel->dev, "failed to set panel on: %d\n", ret);
        goto poweroff;
    }

	sharp->prepared = true;

	/* wait for 6 frames before continuing */
	msleep(120);
	return 0;

poweroff:
	if (sharp->enable_gpio)
		gpiod_set_value(sharp->enable_gpio, 0);
	if (sharp->supply)
		regulator_disable(sharp->supply);
	if (sharp->reset_gpio)
		gpiod_set_value(sharp->reset_gpio, 0);
	return ret;
}



static int sharp_panel_enable(struct drm_panel *panel)
{
	struct sharp_panel *sharp = to_sharp_panel(panel);

	printk("sharp_panel_enable()\n");

	if (sharp->enabled)
		return 0;

	sharp->enabled = true;

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = 144000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 50,
	.hsync_end = 1080 + 50 + 8,
	.htotal = 1080 + 50 + 8 + 62,
	.vdisplay = 1920,
	.vsync_start = 1920 + 73,
	.vsync_end = 1920 + 73 + 2,
	.vtotal = 1920 + 73 + 2 + 5,
	.vrefresh = 60,
};

static int sharp_panel_get_modes(struct drm_panel *panel)
{
    struct drm_display_mode *mode;
    struct drm_connector *connector = panel->connector;
    struct sharp_panel *sharp = to_sharp_panel(panel);

    printk("sharp_panel_get_modes()\n");
    printk("drm_mode_duplcate()\n");
    mode = drm_mode_duplicate(panel->drm, &default_mode);
    if (!mode) {
        dev_err(&sharp->dsi->dev, "failed to add mode %ux%ux@%u\n",
                default_mode.hdisplay, default_mode.vdisplay,
                default_mode.vrefresh);
        return -ENOMEM;
    }

    printk("drm_mode_set_name()\n");
    drm_mode_set_name(mode);

    printk("mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED\n");
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    drm_mode_probed_add(connector, mode);

    panel->connector->display_info.width_mm = 54;
    panel->connector->display_info.height_mm = 95;

    printk("return 1\n");
    return 1;
}

static const struct of_device_id sharp_of_match[] = {
	{ .compatible = "sharp,ls050t1sx01", },
	{ }
};
MODULE_DEVICE_TABLE(of, sharp_of_match);

static const struct drm_panel_funcs sharp_panel_funcs = {
	.disable = sharp_panel_disable,
	.unprepare = sharp_panel_unprepare,
	.prepare = sharp_panel_prepare,
	.enable = sharp_panel_enable,
	.get_modes = sharp_panel_get_modes,
};

static int sharp_panel_add(struct sharp_panel *sharp)
{
    struct device *dev = &sharp->dsi->dev;

    printk("sharp_panel_add()\n");
    sharp->mode = &default_mode;

    sharp->supply = devm_regulator_get(dev, "avdd");
    if (IS_ERR(sharp->supply)) {
        dev_err(dev, "cannot get avdd supply %ld\n", PTR_ERR(sharp->supply));
        sharp->supply = NULL;
    }

    sharp->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(sharp->reset_gpio)) {
        dev_err(dev, "cannot get reset-gpios %ld\n",
                PTR_ERR(sharp->reset_gpio));
        sharp->reset_gpio = NULL;
    } else {
        gpiod_set_value(sharp->reset_gpio, 0);
    }

    sharp->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
    if (IS_ERR(sharp->enable_gpio)) {
        dev_err(dev, "cannot get enable-gpios %ld\n",
                PTR_ERR(sharp->enable_gpio));
        sharp->enable_gpio = NULL;
    } else {
        gpiod_set_value(sharp->enable_gpio, 0);
    }

    drm_panel_init(&sharp->base);
    sharp->base.funcs = &sharp_panel_funcs;
    sharp->base.dev = &sharp->dsi->dev;

    return drm_panel_add(&sharp->base);
}

static void sharp_panel_del(struct sharp_panel *sharp)
{
    printk("sharp_panel_del()\n");
    if (sharp->base.dev)
        drm_panel_remove(&sharp->base);
}

static int sharp_panel_probe(struct mipi_dsi_device *dsi)
{
    struct sharp_panel *sharp;
    int ret;
    printk("sharp_panel_probe()\n");

    dsi->lanes = 4;
    dsi->format = MIPI_DSI_FMT_RGB888;
    dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
                      MIPI_DSI_MODE_VIDEO_HSE |
                      MIPI_DSI_CLOCK_NON_CONTINUOUS |
                      MIPI_DSI_MODE_EOT_PACKET;

    sharp = devm_kzalloc(&dsi->dev, sizeof(*sharp), GFP_KERNEL);
    if (!sharp)
        return -ENOMEM;

    mipi_dsi_set_drvdata(dsi, sharp);

    sharp->dsi = dsi;

    ret = sharp_panel_add(sharp);
    if (ret < 0)
        return ret;

    return mipi_dsi_attach(dsi);
}

static int sharp_panel_remove(struct mipi_dsi_device *dsi)
{
    struct sharp_panel *sharp = mipi_dsi_get_drvdata(dsi);
    int ret;

    printk("sharp_panel_remove()\n");
    ret = sharp_panel_disable(&sharp->base);
    if (ret < 0)
        dev_err(&dsi->dev, "failed to disable panel: %d\n", ret);

    ret = mipi_dsi_detach(dsi);
    if (ret < 0)
        dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

    drm_panel_detach(&sharp->base);
    sharp_panel_del(sharp);

    return 0;
}

static void sharp_panel_shutdown(struct mipi_dsi_device *dsi)
{
    struct sharp_panel *sharp = mipi_dsi_get_drvdata(dsi);
    printk("sharp_panel_shutdown()\n");

    sharp_panel_disable(&sharp->base);
}

static struct mipi_dsi_driver sharp_panel_driver = {
	.driver = {
		.name = "panel-sharp-ls050t1sx01",
		.of_match_table = sharp_of_match,
	},
	.probe = sharp_panel_probe,
	.remove = sharp_panel_remove,
	.shutdown = sharp_panel_shutdown,
};
module_mipi_dsi_driver(sharp_panel_driver);

MODULE_AUTHOR("Roman Beranek <roman.beranek@prusa3d.com>");
MODULE_DESCRIPTION("Sharp LS050T1SX01 R63311-based FullHD video mode panel driver");
MODULE_LICENSE("GPL v2");
