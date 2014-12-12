/*
 * LEGO MINDSTORMS NXT I2C sensor device driver
 *
 * Copyright (C) 2013-2014 David Lechner <david@lechnology.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Note: The comment block below is used to generate docs on the ev3dev website.
 * Use kramdown (markdown) format. Use a '.' as a placeholder when blank lines
 * or leading whitespace is important for the markdown syntax.
 */

/**
 * DOC: website
 *
 * NXT I2C Sensor Driver
 *
 * The `nxt-i2c-sensor` module provides all of the drivers for I2C/NXT
 * sensors. You can find the complete list [here][supported sensors].
 * .
 * ### sysfs Attributes
 * .
 * These drivers provide a [lego-sensor device], which is where all the really
 * useful attributes are.
 * .
 * You can find this device at `/sys/bus/legoev3/devices/in<N>:<device-name>`
 * where `<N>` is the number of an input port (1 to 4) and `<device-name>` is
 * the name of one of the drivers in the `nxt-analog-sensor` module (e.g.
 * `lego-nxt-sound`).
 * .
 * `device_type` (read-only)
 * : Returns `nxt-i2c-sensor`
 * .
 * `port_name` (read-only)
 * : Returns the name of the port this host is connected to (e.g. `in1`).
 * .
 * ### Module Parameters
 * .
 * Note: These parameters can be changed at runtime at
 * `/sys/module/nxt_i2c_sensor/parameters/<parameter>`.
 * .
 * `allow_autodetect`
 * : Setting to `N` disables probing of sensors. Default is `Y`.
 * .
 * `default_poll_ms`
 * : This provides the default value for the poll_ms attribute. A value of `0`
 * .    will disable polling by default. Values of less that the minimum 50
 * .    msec will be rounded up to 50 msec. Changes only affect sensors plugged
 * .    in after the change was made. Default is 100 msec.
 * .
 * [supported sensors]: ../#supported-sensors
 * [lego-sensor device]: ../lego-sensor-class
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/bug.h>
#include <linux/i2c.h>
#include <linux/i2c-legoev3.h>
#include <linux/workqueue.h>
#include <linux/legoev3/legoev3_ports.h>
#include <linux/legoev3/ev3_input_port.h>

#include <lego_sensor_class.h>

#include "nxt_i2c_sensor.h"

#define NXT_I2C_MIN_POLL_MS 50

static u16 default_poll_ms = 100;
module_param(default_poll_ms, ushort, 0644);
MODULE_PARM_DESC(default_poll_ms, "Polling period in milliseconds. Minimum "
	"value is "__stringify(NXT_I2C_MIN_POLL_MS)" or 0 to disable polling.");
static bool allow_autodetect = 1;
module_param(allow_autodetect, bool, 0644);
MODULE_PARM_DESC(allow_autodetect, "Allow NXT I2C sensors to be automatically detected.");

void nxt_i2c_sensor_poll_work(struct work_struct *work);

static int nxt_i2c_sensor_set_mode(void *context, u8 mode)
{
	struct nxt_i2c_sensor_data *sensor = context;
	int err;

	if (sensor->info.ops.set_mode_pre_cb) {
		err = sensor->info.ops.set_mode_pre_cb(sensor, mode);
		if (err < 0)
			return err;
	}

	if (sensor->info.i2c_mode_info[mode].set_mode_reg) {
		err = i2c_smbus_write_byte_data(sensor->client,
			sensor->info.i2c_mode_info[mode].set_mode_reg,
			sensor->info.i2c_mode_info[mode].set_mode_data);
		if (err < 0)
			return err;
	}

	sensor->in_port->in_ops.set_pin1_gpio(sensor->in_port,
				sensor->info.i2c_mode_info[mode].pin1_state);
	nxt_i2c_sensor_poll_work(&sensor->poll_work.work);

	if (sensor->info.ops.set_mode_post_cb)
		sensor->info.ops.set_mode_post_cb(sensor, mode);

	return 0;
}

static int nxt_i2c_sensor_send_command(void *context, u8 command)
{
	struct nxt_i2c_sensor_data *sensor = context;
	int err;

	if (sensor->info.ops.send_cmd_pre_cb) {
		err = sensor->info.ops.send_cmd_pre_cb(sensor, command);
		if (err)
			return err;
	}

	err = i2c_smbus_write_byte_data(sensor->client,
		sensor->info.i2c_cmd_info[command].cmd_reg,
		sensor->info.i2c_cmd_info[command].cmd_data);
	if (err)
		return err;

	if (sensor->info.ops.send_cmd_post_cb)
		sensor->info.ops.send_cmd_post_cb(sensor, command);

	return 0;
}

static ssize_t nxt_i2c_sensor_write_data(void *context, char *data, loff_t off,
					 size_t count)
{
	struct nxt_i2c_sensor_data *sensor = context;
	int err;

	if (off)
		return -EINVAL;

	if (count == 0)
		err = 0;
	else if (count == 1)
		err = i2c_smbus_write_byte(sensor->client, data[0]);
	else
		err = i2c_smbus_write_i2c_block_data(sensor->client, data[0],
						     count - 1, &data[1]);
	if (err < 0)
		return err;

	return count;
}

static int nxt_i2c_sensor_get_poll_ms(void *context)
{
	struct nxt_i2c_sensor_data *sensor = context;

	return sensor->poll_ms;
}

static int nxt_i2c_sensor_set_poll_ms(void *context, unsigned value)
{
	struct nxt_i2c_sensor_data *sensor = context;

	if (value > 0 && value < NXT_I2C_MIN_POLL_MS)
		return -EINVAL;
	if (sensor->poll_ms == value)
		return 0;
	if (!value)
		cancel_delayed_work(&sensor->poll_work);
	else if (!sensor->poll_ms)
		schedule_delayed_work(&sensor->poll_work,
				      msecs_to_jiffies(value));

	sensor->poll_ms = value;
	return 0;
}

void nxt_i2c_sensor_poll_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct nxt_i2c_sensor_data *sensor =
		container_of(dwork, struct nxt_i2c_sensor_data, poll_work);
	struct nxt_i2c_sensor_mode_info *i2c_mode_info =
		&sensor->info.i2c_mode_info[sensor->sensor.mode];
	struct lego_sensor_mode_info *mode_info =
			&sensor->info.mode_info[sensor->sensor.mode];

	if (sensor->info.ops.poll_cb)
		sensor->info.ops.poll_cb(sensor);
	else
		i2c_smbus_read_i2c_block_data(sensor->client,
			i2c_mode_info->read_data_reg,
			lego_sensor_get_raw_data_size(mode_info),
			mode_info->raw_data);

	if (sensor->poll_ms && !delayed_work_pending(&sensor->poll_work))
		schedule_delayed_work(&sensor->poll_work,
				      msecs_to_jiffies(sensor->poll_ms));
}

static int nxt_i2c_sensor_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct nxt_i2c_sensor_data *data;
	struct i2c_legoev3_platform_data *apdata =
					client->adapter->dev.platform_data;
	const struct nxt_i2c_sensor_info *sensor_info;
	const struct i2c_device_id *i2c_dev_id = id;
	char version[NXT_I2C_ID_STR_LEN + 1] = { 0 };
	int err, i;

	if (WARN(!apdata, "Adapter platform data is required."))
		return -EINVAL;
	if (WARN(!apdata->in_port, "Adapter platform data missing input port."))
		return -EINVAL;

	sensor_info = &nxt_i2c_sensor_defs[i2c_dev_id->driver_data];

	data = kzalloc(sizeof(struct nxt_i2c_sensor_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->in_port = apdata->in_port;
	data->type = i2c_dev_id->driver_data;
	memcpy(&data->info, sensor_info, sizeof(struct nxt_i2c_sensor_info));

	strncpy(data->sensor.name, i2c_dev_id->name, LEGO_SENSOR_NAME_SIZE);
	strncpy(data->sensor.port_name, dev_name(&data->in_port->dev),
		LEGO_SENSOR_NAME_SIZE);
	data->sensor.num_modes = data->info.num_modes;
	data->sensor.num_view_modes = 1;
	data->sensor.mode_info = data->info.mode_info;
	data->sensor.num_commands = data->info.num_commands;
	data->sensor.cmd_info = data->info.cmd_info;
	data->sensor.set_mode = nxt_i2c_sensor_set_mode;
	data->sensor.send_command = nxt_i2c_sensor_send_command;
	data->sensor.write_data = nxt_i2c_sensor_write_data;
	data->sensor.get_poll_ms = nxt_i2c_sensor_get_poll_ms;
	data->sensor.set_poll_ms = nxt_i2c_sensor_set_poll_ms;
	data->sensor.context = data;
	i2c_smbus_read_i2c_block_data(client, NXT_I2C_FW_VER_REG,
				      NXT_I2C_ID_STR_LEN, version);
	/*
	 * HiTechnic sensors have 0xfd before the version because of an early
	 * bug in the NXT I2C code where register 0x00 could not be read
	 * reliably. So, we just ignore it along with the whitespace.
	 */
	strncpy(data->sensor.fw_version, strim(version[0] == 0xfd ?
		(version + 1) : version), NXT_I2C_ID_STR_LEN + 1);
	data->sensor.address = client->addr;

	for (i = 0; i < data->sensor.num_modes; i++) {
		struct lego_sensor_mode_info *minfo = &data->info.mode_info[i];

		if (!minfo->raw_min && !minfo->raw_max)
			minfo->raw_max = 255;
		if (!minfo->pct_min && !minfo->pct_max)
			minfo->pct_max = 100;
		if (!minfo->si_min && !minfo->si_max)
			minfo->si_max = 255;
		if (!minfo->data_sets)
			minfo->data_sets = 1;
		if (!minfo->figures)
			minfo->figures = 5;
	}

	INIT_DELAYED_WORK(&data->poll_work, nxt_i2c_sensor_poll_work);
	if (default_poll_ms && default_poll_ms < NXT_I2C_MIN_POLL_MS)
		default_poll_ms = NXT_I2C_MIN_POLL_MS;
	data->poll_ms = default_poll_ms;
	i2c_set_clientdata(client, data);

	if (data->info.ops.probe_cb) {
		err = data->info.ops.probe_cb(data);
		if (err < 0)
			goto err_probe_cb;
	}

	err = register_lego_sensor(&data->sensor, &client->dev);
	if (err) {
		dev_err(&client->dev, "could not register sensor!\n");
		goto err_register_lego_sensor;
	}

	if (data->type == LEGO_NXT_ULTRASONIC_SENSOR)
		msleep (1);
	nxt_i2c_sensor_set_mode(data, data->sensor.mode);

	return 0;

err_probe_cb:
err_register_lego_sensor:
	i2c_set_clientdata(client, NULL);
	kfree(data);

	return err;
}

static int nxt_i2c_sensor_remove(struct i2c_client *client)
{
	struct nxt_i2c_sensor_data *data = i2c_get_clientdata(client);

	if (data->info.ops.remove_cb)
		data->info.ops.remove_cb(data);
	data->poll_ms = 0;
	if (delayed_work_pending(&data->poll_work))
		cancel_delayed_work_sync(&data->poll_work);
	data->in_port->in_ops.set_pin1_gpio(data->in_port, 0);
	unregister_lego_sensor(&data->sensor);
	kfree(data);

	return 0;
}

static struct i2c_device_id nxt_i2c_sensor_id_table[];

static int nxt_i2c_sensor_detect(struct i2c_client *client,
				 struct i2c_board_info *info)
{
	char vendor_id[NXT_I2C_ID_STR_LEN + 1] = { 0 };
	char product_id[NXT_I2C_ID_STR_LEN + 1] = { 0 };
	int ret, i;
	int tries = 2;

	if (!allow_autodetect)
		return -ENODEV;

	/*
	 * Some sensors can fall asleep during boot, so we try reading twice
	 * to make sure we wake them up.
	 */
	while (tries--) {
		ret = i2c_smbus_read_i2c_block_data(client, NXT_I2C_VEND_ID_REG,
						    NXT_I2C_ID_STR_LEN, vendor_id);
		if (ret > 0 || !tries)
			break;
		msleep(1);
	}
	if (ret < 0)
		return -ENODEV;

	/*
	 * NXT Ultrasonic sensor requires a long delay between reads or else
	 * we will get NAKed. msleep(1) tends to vary between 10 and 20msec.
	 */
	msleep(1);
	ret = i2c_smbus_read_i2c_block_data(client, NXT_I2C_PROD_ID_REG,
					    NXT_I2C_ID_STR_LEN, product_id);
	if (ret < 0)
		return -ENODEV;

	for (i = 0; i < NUM_NXT_I2C_SENSORS; i++)
	{
		if (!strcmp(nxt_i2c_sensor_defs[i].vendor_id, strim(vendor_id))
			&& !strcmp(nxt_i2c_sensor_defs[i].product_id, strim(product_id)))
		{
			snprintf(info->type, I2C_NAME_SIZE, nxt_i2c_sensor_defs[i].name);
			return 0;
		}
	}
	return -ENODEV;
}

static struct i2c_device_id nxt_i2c_sensor_id_table[] = {
	NXT_I2C_SENSOR_ID_TABLE_DATA
};
MODULE_DEVICE_TABLE(i2c, nxt_i2c_sensor_id_table);

static struct i2c_driver nxt_i2c_sensor_driver = {
	.driver = {
		.name	= "nxt-i2c-sensor",
	},
	.id_table	= nxt_i2c_sensor_id_table,
	.probe		= nxt_i2c_sensor_probe,
	.remove		= nxt_i2c_sensor_remove,
	.class		= I2C_CLASS_LEGOEV3,
	.detect		= nxt_i2c_sensor_detect,
	.address_list	= I2C_ADDRS(0x01, 0x02, 0x08, 0x0A, 0x11, 0x18, 0x50,
				    0x51, 0x52, 0x58),
};
module_i2c_driver(nxt_i2c_sensor_driver);

MODULE_DESCRIPTION("LEGO MINDSTORMS NXT I2C sensor device driver");
MODULE_AUTHOR("David Lechner <david@lechnology.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("legoev3:nxt-i2c-host");