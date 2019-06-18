#include "DriverControl.hpp"
#include "driver.hpp"
#include "PPMDriver.hpp"
#include "BLCTRLDriver.hpp"
#include "BLDriver.hpp"
#include "App.hpp"
#include "log.h"
#include "gui.hpp"
#include "file.hpp"
#include "Config.hpp"
#include "progress.hpp"

const char *drivers[] = {
		"None",
		"PPM",
		"BLDriver",
		"BLCtrl1.2",
		nullptr,
};

enum class Notification : uint32_t {
	DriverChange,
	ControlModeChange,
	MotorOnOff,
	SetPointChange,
};

#define DRIVER_CHANGE		 	0x01
#define CONTROL_MODE_CHANGE		0x02
#define MOTOR_ON_OFF			0x04
#define SET_POINT_CHANGE		0x08
#define CONFIGURE_RAMP			0x10
#define RUN_RAMP				0x20

using DriverSettings = struct {
	uint8_t driver;
	bool motorOn;
	Driver::ControlMode control;
	int32_t setpoint;
};

using RampSettings = struct {
	int32_t start, stop;
	int32_t steps;
	int32_t length;
	char filename[15];
};

static DriverSettings *settings;
static RampSettings *ramp;

static bool WriteConfig(void *ptr) {
	if (!settings) {
		return false;
	}
	File::WriteLine("# Driver configuration\n");
	const File::Entry entries[] = {
		{ "Driver::Driver", (void*) drivers[settings->driver], File::PointerType::STRING},
		{ "Driver::ControlMode", &settings->control, File::PointerType::INT8},
		{ "Driver::Setpoint", &settings->setpoint, File::PointerType::INT32},
	};
	File::WriteParameters(entries, 3);
	return true;
}

static bool ReadConfig(void *ptr) {
	if (!settings) {
		return false;
	}
	char driverName[15];
	const File::Entry entries[] = {
		{ "Driver::Driver", driverName, File::PointerType::STRING},
		{ "Driver::ControlMode", &settings->control, File::PointerType::INT8},
		{ "Driver::Setpoint", &settings->setpoint, File::PointerType::INT32},
	};
	File::ReadParameters(entries, 3);
	uint8_t driverNum = 0;
	while (drivers[driverNum]) {
		if (!strcmp(driverName, drivers[driverNum])) {
			settings->driver = driverNum;
			break;
		}
		driverNum++;
	}
	if (!drivers[driverNum]) {
		LOG(Log_Config, LevelWarn, "No such driver found: %s", driverName);
		driverNum = 0;
	}
	settings->motorOn = false;
	TaskHandle_t h = (TaskHandle_t) ptr;
	xTaskNotify(h,
			DRIVER_CHANGE | CONTROL_MODE_CHANGE | MOTOR_ON_OFF | SET_POINT_CHANGE,
			eSetBits);

	// send 2 empty notifications, guaranteeing that the previous notification has been
	// handled (necessary because some motor driver might add their own config parse functions)
	while (xTaskNotify(h, 0, eSetValueWithoutOverwrite) == pdFAIL) {
		vTaskDelay(10);
	}
	while (xTaskNotify(h, 0, eSetValueWithoutOverwrite) == pdFAIL) {
		vTaskDelay(10);
	}
	return true;
}

static void ConfigureRamp(void) {
	Window *w = new Window("Configure ramp", Font_Big, COORDS(320, 240));
	Container *c = new Container(w->getAvailableArea());
	c->attach(new Label("Start:", Font_Big), COORDS(0, 5));
	c->attach(new Label("Stop:", Font_Big), COORDS(0, 30));
	c->attach(new Label("Steps:", Font_Big), COORDS(0, 55));
	c->attach(new Label("Length:", Font_Big), COORDS(0, 80));

	const Unit::unit **u = Unit::None;
	switch (settings->control) {
	case Driver::ControlMode::Percentage:
		u = Unit::Percent;
		break;
	case Driver::ControlMode::RPM:
		u = Unit::None;
		break;
	case Driver::ControlMode::Thrust:
		u = Unit::Force;
		break;
	}

	c->attach(new Entry(&ramp->start, nullptr, nullptr, Font_Big, 8, u),
			COORDS(85, 3));
	c->attach(new Entry(&ramp->stop, nullptr, nullptr, Font_Big, 8, u),
			COORDS(85, 28));
	c->attach(
			new Entry(&ramp->steps, nullptr, nullptr, Font_Big, 8, Unit::None),
			COORDS(85, 53));
	c->attach(new Entry(&ramp->length, 0, 1800000000, Font_Big, 8, Unit::Time),
			COORDS(85, 78));

	auto start = new Button("Start", Font_Big, [](void *ptr, Widget *w) {
		TaskHandle_t h = (TaskHandle_t) ptr;
		xTaskNotify(h, RUN_RAMP,
				eSetBits);
			}, xTaskGetCurrentTaskHandle(),
			COORDS(c->getSize().y / 2 - 10, c->getSize().y / 2 - 10));
	auto abort = new Button("Abort", Font_Big, [](void *ptr, Widget *w) {
		Window *window = (Window*) ptr;
		delete window;
	}, w, COORDS(c->getSize().y / 2 - 10, c->getSize().y / 2 - 10));

	c->attach(start, COORDS(c->getSize().x - start->getSize().x - 5, 5));
	c->attach(abort,
			COORDS(c->getSize().x - start->getSize().x - 5,
					10 + start->getSize().y));

	w->setMainWidget(c);
}

static void RunRamp(void) {
	constexpr uint16_t samples = 100;
	constexpr uint32_t deltaT = pdMS_TO_TICKS(20);
	auto p = new ProgressDialog("Running ramp...", 0);
	for(uint16_t i=0;i<samples;i++) {
		p->SetPercentage(i * 100 / samples);
		vTaskDelay(deltaT);
	}
	delete p;
}

void DriverControl::Task(void* a) {
	App *app = (App*) a;
	LOG(Log_App, LevelInfo, "Driver task");
	Driver *pDriver = nullptr;

	settings = new DriverSettings;
	ramp = new RampSettings;

	auto c = new Container(COORDS(280, 240));

	c->attach(new Label("Driver:", Font_Big), COORDS(2, 2));
	settings->driver = 0;
	auto *iDriver = new ItemChooser(drivers, &settings->driver, Font_Big, 3,
			130);
	iDriver->setCallback([](void *ptr, Widget*) {
		TaskHandle_t h = (TaskHandle_t) ptr;
		xTaskNotify(h, DRIVER_CHANGE,
				eSetBits);
	}, xTaskGetCurrentTaskHandle());
	c->attach(iDriver, COORDS(2, 18));

	c->attach(new Label("Motor on:", Font_Big), COORDS(1, 70));
	settings->motorOn = false;
	auto *cOn = new Checkbox(&settings->motorOn, [](void *ptr, Widget*) {
		TaskHandle_t h = (TaskHandle_t) ptr;
		xTaskNotify(h, MOTOR_ON_OFF,
				eSetBits);
	}, xTaskGetCurrentTaskHandle(), COORDS(25, 25));
	c->attach(cOn, COORDS(107, 70));

	c->attach(new Label("Mode:", Font_Big), COORDS(1, 87));
	settings->control = Driver::ControlMode::Percentage;
	auto *rPercent = new Radiobutton((uint8_t*) &settings->control, 20,
			(int) Driver::ControlMode::Percentage);
	c->attach(rPercent, COORDS(2, 103));
	c->attach(new Label("Percent", Font_Big), COORDS(25, 105));
	auto *rRPM = new Radiobutton((uint8_t*) &settings->control, 20,
			(int) Driver::ControlMode::RPM);
	c->attach(rRPM, COORDS(2, 128));
	c->attach(new Label("RPM", Font_Big), COORDS(25, 130));
	auto *rThrust = new Radiobutton((uint8_t*) &settings->control, 20,
			(int) Driver::ControlMode::Thrust);
	c->attach(rThrust, COORDS(2, 153));
	c->attach(new Label("Thrust", Font_Big), COORDS(25, 155));
	Radiobutton::Set set;
	set.cb = [](void *ptr, Widget*,uint8_t) {
		TaskHandle_t h = (TaskHandle_t) ptr;
		xTaskNotify(h, CONTROL_MODE_CHANGE,
				eSetBits);
	};
	set.ptr = xTaskGetCurrentTaskHandle();
	set.first = nullptr;
	rPercent->AddToSet(set);
	rRPM->AddToSet(set);
	rThrust->AddToSet(set);
	rPercent->setSelectable(false);
	rRPM->setSelectable(false);
	rThrust->setSelectable(false);
	cOn->setSelectable(false);

	c->attach(new Label("Setpoint:", Font_Big), COORDS(2, 176));
	settings->setpoint = 0;
	auto *eSet = new Entry(&settings->setpoint, &Unit::maxPercent, &Unit::null,
			Font_Big, 8, Unit::Percent, COLOR_BLACK);
	eSet->setCallback([](void *ptr, Widget*) {
		TaskHandle_t h = (TaskHandle_t) ptr;
		xTaskNotify(h, SET_POINT_CHANGE,
				eSetBits);
	}, xTaskGetCurrentTaskHandle());
	c->attach(eSet, COORDS(2, 196));
	eSet->setSelectable(false);
	auto *sSet = new Slider(&settings->setpoint, Unit::maxPercent, Unit::null,
			COORDS(21, 220));
	sSet->setCallback([](void *ptr, Widget*) {
		TaskHandle_t h = (TaskHandle_t) ptr;
		xTaskNotify(h, SET_POINT_CHANGE,
				eSetBits);
	}, xTaskGetCurrentTaskHandle());
	c->attach(sSet, COORDS(135, 10));
	sSet->setSelectable(false);

	Button *ramp = new Button("RUN RAMP", Font_Big, [](void *ptr, Widget*) {
		TaskHandle_t h = (TaskHandle_t) ptr;
		xTaskNotify(h, CONFIGURE_RAMP,
				eSetBits);
	}, xTaskGetCurrentTaskHandle());
	ramp->setSelectable(false);
	c->attach(ramp, COORDS(2, 217));

	constexpr coords_t driverSize = COORDS(120, 170);
	// Readback widgets
	Driver::Readback readback;
	memset(&readback, 0, sizeof(readback));
	c->attach(new Label("Readback:", Font_Big), COORDS(168, 170));
	auto *eReadCurrent = new Entry(&readback.current, nullptr, nullptr,
			Font_Medium, 8, Unit::Current);
	auto *eReadVoltage = new Entry(&readback.voltage, nullptr, nullptr,
			Font_Medium, 8, Unit::Voltage);
	auto *eReadRPM = new Entry(&readback.RPM, nullptr, nullptr,
			Font_Medium, 8, Unit::None);
	auto *eReadThrust = new Entry(&readback.thrust, nullptr, nullptr,
			Font_Medium, 8, Unit::Force);
	eReadCurrent->setSelectable(false);
	eReadCurrent->setVisible(false);
	eReadVoltage->setSelectable(false);
	eReadVoltage->setVisible(false);
	eReadRPM->setSelectable(false);
	eReadRPM->setVisible(false);
	eReadThrust->setSelectable(false);
	eReadThrust->setVisible(false);

	c->attach(eReadCurrent, COORDS(166, 190));
	c->attach(eReadVoltage, COORDS(223, 190));
	c->attach(eReadRPM, COORDS(166, 210));
	c->attach(eReadThrust, COORDS(223, 210));

	uint32_t configIndex = Config::AddParseFunctions(WriteConfig, ReadConfig,
			xTaskGetCurrentTaskHandle());

	app->StartComplete(c);

	while (1) {
		uint32_t n;
		if (xTaskNotifyWait(0, 0xFFFFFFFF, &n, 100)) {
			if (n & DRIVER_CHANGE) {
				settings->motorOn = false;
				memset(&readback, 0, sizeof(readback));
				cOn->requestRedrawFull();
				if (pDriver) {
					delete pDriver;
					pDriver = nullptr;
				}
				switch (settings->driver) {
				case 1:
					pDriver = new PPMDriver(driverSize);
					break;
				case 2:
					pDriver = new BLDriver(driverSize);
					break;
				case 3:
					pDriver = new BLCTRLDriver(driverSize);
					break;
				}
				if (!pDriver) {
					rPercent->setSelectable(false);
					rRPM->setSelectable(false);
					rThrust->setSelectable(false);
					cOn->setSelectable(false);
					eSet->setSelectable(false);
					sSet->setSelectable(false);
					ramp->setSelectable(false);
					eReadCurrent->setVisible(false);
					eReadVoltage->setVisible(false);
					eReadRPM->setVisible(false);
					eReadThrust->setVisible(false);
				} else {
					auto f = pDriver->GetFeatures();
					cOn->setSelectable(f.OnOff);
					rPercent->setSelectable(f.Control.Percentage);
					rRPM->setSelectable(f.Control.RPM);
					rThrust->setSelectable(f.Control.Thrust);
					eReadCurrent->setVisible(f.Readback.Current);
					eReadVoltage->setVisible(f.Readback.Voltage);
					eReadRPM->setVisible(f.Readback.RPM);
					eReadThrust->setVisible(f.Readback.Thrust);
					eSet->setSelectable(true);
					sSet->setSelectable(true);
					ramp->setSelectable(true);
					c->attach(pDriver->GetTopWidget(),
							COORDS(c->getSize().x - driverSize.x, 0));
				}
				c->requestRedrawFull();
			}
			if (n & CONTROL_MODE_CHANGE) {
			}
			if (n & MOTOR_ON_OFF) {
				if (pDriver) {
					pDriver->SetRunning(settings->motorOn);
					pDriver->SetControl(settings->control, settings->setpoint);
				}
			}
			if (n & SET_POINT_CHANGE) {
				if (pDriver) {
					pDriver->SetControl(settings->control, settings->setpoint);
				}
				eSet->requestRedraw();
				sSet->requestRedrawFull();
			}
			if( n & CONFIGURE_RAMP) {
				ConfigureRamp();
			}
			if( n & RUN_RAMP) {
				RunRamp();
			}
		}
		if (pDriver) {
			readback = pDriver->GetData();
			eReadCurrent->requestRedraw();
			eReadVoltage->requestRedraw();
			eReadRPM->requestRedraw();
			eReadThrust->requestRedraw();
		}
		if (app->Closed()) {
			if (pDriver) {
				delete pDriver;
				pDriver = nullptr;
			}
			Config::RemoveParseFunctions(configIndex);
			delete settings;
			delete ramp;
			settings = nullptr;
			app->Exit();
			vTaskDelete(nullptr);
		}
	}
}
