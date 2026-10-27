cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(wrist_app LANGUAGES C)

target_include_directories(app PRIVATE include)

target_sources(app PRIVATE
	src/main.c

	src/core/acq.c
	src/core/proc.c
	src/core/packager.c
	src/core/health.c
	src/core/mode.c
	src/core/power.c
	src/core/supervisor.c
	src/core/channels.c
	src/core/record.c
	src/core/counters.c
	src/core/persist.c
	src/core/watchdog.c
	src/core/leds.c

	src/sensors/board.c
	src/sensors/ppg.c
	src/sensors/accel.c
	src/sensors/skin_temp.c
	src/sensors/gsr.c
	src/sensors/flex.c
	src/sensors/fuel_gauge.c

	src/processing/validate.c
	src/processing/hr.c
	src/processing/motion.c
	src/processing/resp.c
	src/processing/core_temp.c
	src/processing/skin_temp_proc.c
	src/processing/gsr_proc.c

	src/comms/ble.c
	src/comms/ble_svc.c

	src/storage/storage.c
)

target_sources_ifdef(CONFIG_APP_SHELL app PRIVATE src/core/shell.c)

# The driver layer: real register-level drivers, or the fake generators that
# implement the same include/app/drv/*.h interfaces (see ARCHITECTURE.md §5, §11).
if(CONFIG_APP_FAKE_SENSORS)
	target_sources(app PRIVATE src/sim/fake_sensors.c)
else()
	target_sources(app PRIVATE
		src/drivers/max30101.c
		src/drivers/max3020x.c
		src/drivers/max17048.c
		src/drivers/analog_saadc.c
	)
	target_sources_ifdef(CONFIG_APP_ACCEL_PART_NONE app PRIVATE src/drivers/accel_none.c)
endif()

target_sources_ifdef(CONFIG_APP_STORAGE_BACKEND_NOR app PRIVATE src/storage/storage_nor.c)
target_sources_ifdef(CONFIG_APP_STORAGE_BACKEND_SD app PRIVATE src/storage/storage_sd.c)
