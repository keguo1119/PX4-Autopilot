/****************************************************************************
 *
 *   Copyright (c) 2013, 2014 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file meas_airspeed.cpp
 * @author Lorenz Meier
 * @author Sarthak Kaingade
 * @author Simon Wilks
 * @author Thomas Gubler
 *
 * Driver for the MEAS Spec series connected via I2C.
 *
 * Supported sensors:
 *
 *    - MS4525DO (http://www.meas-spec.com/downloads/MS4525DO.pdf)
 *
 * Interface application notes:
 *
 *    - Interfacing to MEAS Digital Pressure Modules (http://www.meas-spec.com/downloads/Interfacing_to_MEAS_Digital_Pressure_Modules.pdf)
 */

#include <px4_platform_common/getopt.h>
#include <px4_platform_common/module.h>
#include <px4_platform_common/i2c_spi_buses.h>
#include <drivers/device/i2c.h>
#include <uORB/topics/sensor_differential_pressure.h>
#include <uORB/PublicationMulti.hpp>

enum MS_DEVICE_TYPE {
	DEVICE_TYPE_MS4515	= 4515,
	DEVICE_TYPE_MS4525	= 4525
};

/* I2C bus address is 1010001x */
#define I2C_ADDRESS_MS4515DO	0x46
#define I2C_ADDRESS_MS4525DO	0x28	/**< 7-bit address. Depends on the order code (this is for code "I") */

/* Register address */
#define ADDR_READ_MR			0x00	/* write to this address to start conversion */

/* Measurement rate is 100Hz */
#define MEAS_RATE 100
#define CONVERSION_INTERVAL	(1000000 / MEAS_RATE)	/* microseconds */

class MEASAirspeed : public device::I2C, public I2CSPIDriver<MEASAirspeed>
{
public:
	MEASAirspeed(I2CSPIBusOption bus_option, const int bus, int bus_frequency, int address = I2C_ADDRESS_MS4525DO);

	virtual ~MEASAirspeed() = default;

	static I2CSPIDriverBase *instantiate(const BusCLIArguments &cli, const BusInstanceIterator &iterator,
					     int runtime_instance);
	static void print_usage();

	void	RunImpl();

private:
	int	measure();
	int	collect();

	bool _sensor_ok{false};
	int _measure_interval{0};
	bool _collect_phase{false};
	unsigned _conversion_interval{0};

	int16_t _dp_raw_prev{0};
	int16_t _dT_raw_prev{0};

	uORB::PublicationMulti<sensor_differential_pressure_s>	_differential_pressure_pub{ORB_ID(sensor_differential_pressure)};

	perf_counter_t _sample_perf{perf_alloc(PC_ELAPSED, MODULE_NAME": read")};
	perf_counter_t _comms_errors{perf_alloc(PC_COUNT, MODULE_NAME": com err")};
};

MEASAirspeed::MEASAirspeed(I2CSPIBusOption bus_option, const int bus, int bus_frequency, int address) :
	I2C(DRV_DIFF_PRESS_DEVTYPE_MS4525, MODULE_NAME, bus, address, bus_frequency),
	I2CSPIDriver(MODULE_NAME, px4::device_bus_to_wq(get_device_id()), bus_option, bus, address)
{
}

int MEASAirspeed::measure()
{
	// Send the command to begin a measurement.
	uint8_t cmd = 0;
	int ret = transfer(&cmd, 1, nullptr, 0);

	if (OK != ret) {
		perf_count(_comms_errors);
	}

	return ret;
}

int MEASAirspeed::collect()
{
	/* read from the sensor */
	uint8_t val[4] {};

	perf_begin(_sample_perf);

	const hrt_abstime timestamp_sample = hrt_absolute_time();

	int ret = transfer(nullptr, 0, &val[0], 4);

	if (ret < 0) {
		perf_count(_comms_errors);
		perf_end(_sample_perf);
		return ret;
	}

	uint8_t status = (val[0] & 0xC0) >> 6;

	switch (status) {
	case 0:
		// Normal Operation. Good Data Packet
		break;

	case 1:
		// Reserved
		return -EAGAIN;

	case 2:
		// Stale Data. Data has been fetched since last measurement cycle.
		return -EAGAIN;

	case 3:
		// Fault Detected
		perf_count(_comms_errors);
		perf_end(_sample_perf);
		return -EAGAIN;
	}

	/* mask the used bits */
	int16_t dp_raw = (0x3FFF & ((val[0] << 8) + val[1]));
	int16_t dT_raw = (0xFFE0 & ((val[2] << 8) + val[3])) >> 5;

	// dT max is almost certainly an invalid reading
	if (dT_raw == 2047) {
		perf_count(_comms_errors);
		return -EAGAIN;
	}

	// only publish changes
	if ((dp_raw != _dp_raw_prev) && (dT_raw != _dT_raw_prev)) {

		_dp_raw_prev = dp_raw;
		_dT_raw_prev = dT_raw;

		float temperature = ((200.0f * dT_raw) / 2047) - 50;

		// Calculate differential pressure. As its centered around 8000
		// and can go positive or negative
		const float P_min = -1.0f;
		const float P_max = 1.0f;
		const float PSI_to_Pa = 6894.757f;
		/*
		this equation is an inversion of the equation in the
		pressure transfer function figure on page 4 of the datasheet

		We negate the result so that positive differential pressures
		are generated when the bottom port is used as the static
		port on the pitot and top port is used as the dynamic port
		*/
		float diff_press_PSI = -((dp_raw - 0.1f * 16383) * (P_max - P_min) / (0.8f * 16383) + P_min);
		float diff_press_pa_raw = diff_press_PSI * PSI_to_Pa;

		/*
		With the above calculation the MS4525 sensor will produce a
		positive number when the top port is used as a dynamic port
		and bottom port is used as the static port
		*/
		sensor_differential_pressure_s report{};
		report.timestamp_sample = timestamp_sample;
		report.device_id = get_device_id();
		report.differential_pressure_pa = diff_press_pa_raw;
		report.temperature = temperature;
		report.error_count = perf_event_count(_comms_errors);
		report.timestamp = hrt_absolute_time();
		_differential_pressure_pub.publish(report);
	}

	perf_end(_sample_perf);

	return PX4_OK;
}

void MEASAirspeed::RunImpl()
{
	int ret;

	/* collection phase? */
	if (_collect_phase) {

		/* perform collection */
		ret = collect();

		if (OK != ret) {
			/* restart the measurement state machine */
			_collect_phase = false;
			_sensor_ok = false;
			ScheduleNow();
			return;
		}

		/* next phase is measurement */
		_collect_phase = false;

		/*
		 * Is there a collect->measure gap?
		 */
		if (_measure_interval > CONVERSION_INTERVAL) {

			/* schedule a fresh cycle call when we are ready to measure again */
			ScheduleDelayed(_measure_interval - CONVERSION_INTERVAL);

			return;
		}
	}

	/* measurement phase */
	ret = measure();

	if (PX4_OK != ret) {
		DEVICE_DEBUG("measure error");
	}

	_sensor_ok = (ret == OK);

	/* next phase is collection */
	_collect_phase = true;

	/* schedule a fresh cycle call when the measurement is done */
	ScheduleDelayed(CONVERSION_INTERVAL);
}

I2CSPIDriverBase *MEASAirspeed::instantiate(const BusCLIArguments &cli, const BusInstanceIterator &iterator,
		int runtime_instance)
{
	MEASAirspeed *instance = new MEASAirspeed(iterator.configuredBusOption(), iterator.bus(), cli.bus_frequency,
			cli.i2c_address);

	if (instance == nullptr) {
		PX4_ERR("alloc failed");
		return nullptr;
	}

	if (instance->init() != PX4_OK) {
		delete instance;
		return nullptr;
	}

	instance->ScheduleNow();
	return instance;
}

void MEASAirspeed::print_usage()
{
	PRINT_MODULE_USAGE_NAME("ms4525_airspeed", "driver");
	PRINT_MODULE_USAGE_SUBCATEGORY("airspeed_sensor");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_PARAMS_I2C_SPI_DRIVER(true, false);
	PRINT_MODULE_USAGE_PARAM_STRING('T', "4525", "4525|4515", "Device type", true);
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();
}

extern "C" __EXPORT int ms4525_airspeed_main(int argc, char *argv[])
{
	int ch;
	using ThisDriver = MEASAirspeed;
	BusCLIArguments cli{true, false};
	cli.default_i2c_frequency = 100000;
	int device_type = DEVICE_TYPE_MS4525;

	while ((ch = cli.getopt(argc, argv, "T:")) != EOF) {
		switch (ch) {
		case 'T':
			device_type = atoi(cli.optarg());
			break;
		}
	}

	const char *verb = cli.optarg();

	if (!verb) {
		ThisDriver::print_usage();
		return -1;
	}

	if (device_type == DEVICE_TYPE_MS4525) {
		cli.i2c_address = I2C_ADDRESS_MS4525DO;

	} else {
		cli.i2c_address = I2C_ADDRESS_MS4515DO;
	}

	BusInstanceIterator iterator(MODULE_NAME, cli,
				     DRV_DIFF_PRESS_DEVTYPE_MS4525);

	if (!strcmp(verb, "start")) {
		return ThisDriver::module_start(cli, iterator);
	}

	if (!strcmp(verb, "stop")) {
		return ThisDriver::module_stop(iterator);
	}

	if (!strcmp(verb, "status")) {
		return ThisDriver::module_status(iterator);
	}

	ThisDriver::print_usage();
	return -1;
}
