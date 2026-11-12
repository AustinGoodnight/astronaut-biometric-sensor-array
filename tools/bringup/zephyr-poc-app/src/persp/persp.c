#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "persp.h"

LOG_MODULE_REGISTER(perspiration, LOG_LEVEL_INF);

#define ADC_NODE DT_PATH(zephyr_user)
#define LED_NODE DT_ALIAS(led0)

#define ADC_SAMPLE_COUNT 5
#define ADC_SAMPLE_INTERVAL_US 2000
#define ADC_SAMPLE_BUFFER_SIZE (ADC_SAMPLE_COUNT * sizeof(int16_t))

#define SUPPLY_VOLTAGE_MV 3300.0f
#define FIXED_RESISTOR_OHMS 10000.0f
#define ALARM_THRESHOLD_KOHMS 1.0f
#define OPEN_CIRCUIT_THRESHOLD 0.99f

#define FILTER_ALPHA 0.25f
#define MAX_RESISTANCE_KOHMS 2046
#define OPEN_CIRCUIT_KOHMS 2047

static const struct adc_dt_spec adc_channel =
	ADC_DT_SPEC_GET_BY_IDX(ADC_NODE, 0);
static const struct gpio_dt_spec alarm_led =
	GPIO_DT_SPEC_GET(LED_NODE, gpios);

static float filtered_resistance_kohms;
static bool filter_initialized;

static int read_adc_average_mv(float *voltage_mv)
{
	int16_t samples[ADC_SAMPLE_COUNT] = {0};
	struct adc_sequence_options options = {
		.interval_us = ADC_SAMPLE_INTERVAL_US,
		.extra_samplings = ADC_SAMPLE_COUNT - 1,
	};
	struct adc_sequence sequence = {
		.buffer = samples,
		.buffer_size = ADC_SAMPLE_BUFFER_SIZE,
		.options = &options,
	};
	int32_t sample_sum = 0;
	int32_t average;
	int ret;

	ret = adc_sequence_init_dt(&adc_channel, &sequence);
	if (ret < 0) {
		return ret;
	}

	ret = adc_read_dt(&adc_channel, &sequence);
	if (ret < 0) {
		return ret;
	}

	for (size_t i = 0; i < ADC_SAMPLE_COUNT; ++i) {
		if (samples[i] > 0) {
			sample_sum += samples[i];
		}
	}

	average = sample_sum / ADC_SAMPLE_COUNT;
	ret = adc_raw_to_millivolts_dt(&adc_channel, &average);
	if (ret < 0) {
		return ret;
	}

	*voltage_mv = (float)average;
	return 0;
}

static int set_alarm(bool active)
{
	return gpio_pin_set_dt(&alarm_led, active ? 1 : 0);
}

static void reset_filter(void)
{
	filtered_resistance_kohms = 0.0f;
	filter_initialized = false;
}

int persp_init(void)
{
	int ret;

	if (!adc_is_ready_dt(&adc_channel)) {
		return -ENODEV;
	}

	ret = adc_channel_setup_dt(&adc_channel);
	if (ret < 0) {
		return ret;
	}

	if (!gpio_is_ready_dt(&alarm_led)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&alarm_led, GPIO_OUTPUT_INACTIVE);
}

int persp_read_resistance_kohms(uint16_t *resistance_kohms)
{
	float voltage_mv;
	float resistance;
	int ret;

	if (resistance_kohms == NULL) {
		return -EINVAL;
	}

	ret = read_adc_average_mv(&voltage_mv);
	if (ret < 0) {
		int led_ret = set_alarm(false);

		reset_filter();
		if (led_ret < 0) {
			LOG_ERR("Failed to turn off skin-resistance alarm LED: %d", led_ret);
		}
		return ret;
	}

	if (voltage_mv >= SUPPLY_VOLTAGE_MV * OPEN_CIRCUIT_THRESHOLD) {
		ret = set_alarm(false);
		reset_filter();
		if (ret < 0) {
			return ret;
		}

		*resistance_kohms = OPEN_CIRCUIT_KOHMS;
		LOG_WRN("Skin resistance is open circuit");
		return 0;
	}

	resistance = (FIXED_RESISTOR_OHMS * voltage_mv) /
		     (SUPPLY_VOLTAGE_MV - voltage_mv) / 1000.0f;

	if (!filter_initialized) {
		filtered_resistance_kohms = resistance;
		filter_initialized = true;
	} else {
		filtered_resistance_kohms =
			FILTER_ALPHA * resistance +
			(1.0f - FILTER_ALPHA) * filtered_resistance_kohms;
	}

	ret = set_alarm(resistance < ALARM_THRESHOLD_KOHMS);
	if (ret < 0) {
		return ret;
	}

	if (filtered_resistance_kohms >= MAX_RESISTANCE_KOHMS) {
		*resistance_kohms = MAX_RESISTANCE_KOHMS;
	} else {
		*resistance_kohms =
			(uint16_t)(filtered_resistance_kohms + 0.5f);
	}

	return 0;
}
