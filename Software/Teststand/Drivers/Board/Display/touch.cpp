#include "touch.hpp"
#include "FreeRTOS.h"
#include "semphr.h"

#define CS_LOW()			(TOUCH_CS_GPIO_Port->BSRR = TOUCH_CS_Pin<<16u)
#define CS_HIGH()			(TOUCH_CS_GPIO_Port->BSRR = TOUCH_CS_Pin)
#define PENIRQ()			(!(TOUCH_IRQ_GPIO_Port->IDR & TOUCH_IRQ_Pin))

/* A2-A0 bits in control word */
#define CHANNEL_X			(0x10)
#define CHANNEL_Y			(0x50)
#define CHANNEL_3			(0x20)
#define CHANNEL_4			(0x60)

/* SER/DFR bit */
#define SINGLE_ENDED		(0x04)
#define DIFFERENTIAL		(0x00)

/* Resolution */
#define BITS8				(0x80)
#define BITS12				(0x00)

/* Power down mode */
#define PD_PENIRQ			(0x00)
#define PD_NOIRQ			(0x01)
#define PD_ALWAYS_ON		(0x03)


extern SPI_HandleTypeDef hspi1;
extern SemaphoreHandle_t xMutexSPI1;

void Touch::Init(void) {
	CS_HIGH();
}

static uint16_t ADS7843_Read(uint8_t control) {
	/* SPI3 is also used for the SD card. Reduce speed now to properly work with
	 * ADS7843 */
	uint16_t cr1 = hspi1.Instance->CR1;
	/* Minimum CLK frequency is 2.5MHz, this could be achieved with a prescaler of 16
	 * (2.25MHz). For reliability, a prescaler of 32 is used */
	hspi1.Instance->CR1 = (hspi1.Instance->CR1 & ~SPI_BAUDRATEPRESCALER_256 )
			| SPI_BAUDRATEPRESCALER_32;
	CS_LOW();
	/* highest bit in control must always be one */
	control |= 0x80;
	uint8_t read[2];
	/* transmit control word */
	HAL_SPI_Transmit(&hspi1, &control, 1, 10);
	/* read ADC result */
	uint16_t dummy = 0;
	HAL_SPI_TransmitReceive(&hspi1, (uint8_t*) &dummy, (uint8_t*) read, 2, 10);
	/* shift and mask 12-bit result */
	uint16_t res = ((uint16_t) read[0] << 8) + read[1];
	res >>= 3;
	res &= 0x0FFF;
	CS_HIGH();
	/* Reset SPI speed to previous value */
	hspi1.Instance->CR1 = cr1;
	return 4095 - res;
}

static void touch_SampleADC(int16_t *rawX, int16_t *rawY, uint16_t samples) {
	uint16_t i;
	int32_t X = 0;
	int32_t Y = 0;
	for (i = 0; i < samples; i++) {
		X += ADS7843_Read(
		CHANNEL_X | DIFFERENTIAL | BITS12 | PD_PENIRQ);
	}
	for (i = 0; i < samples; i++) {

		Y += ADS7843_Read(
		CHANNEL_Y | DIFFERENTIAL | BITS12 | PD_PENIRQ);
	}
	*rawX = X / samples;
	*rawY = Y / samples;
}

bool Touch::GetCoordinates(coords_t &c) {
	if (PENIRQ()) {
		uint16_t rawY, rawX;
		/* screen is being touched */
		/* Acquire SPI resource */
		if (xSemaphoreTake(xMutexSPI1, 10)) {
			touch_SampleADC(&c.x, &c.y, 20);
			/* Release SPI resource */
			xSemaphoreGive(xMutexSPI1);
		} else {
			/* SPI is busy */
			return true;
		}
		if (!PENIRQ()) {
			/* touch has been released during measurement */
			return false;
		}
//		/* convert to screen resolution */
//		c.x = rawX * scaleX + offsetX;
//		c.y = rawY * scaleY + offsetY;
//		if(c.x < 0)
//			c.x = 0;
//		else if(c.x >= TOUCH_RESOLUTION_X)
//			c.x = TOUCH_RESOLUTION_X - 1;
//		if(c.y < 0)
//			c.y = 0;
//		else if(c.y >= TOUCH_RESOLUTION_Y)
//			c.y = TOUCH_RESOLUTION_Y - 1;
		return true;
	} else {
		return false;
	}
}

//static uint8_t touch_SaveCalibration(void) {
//	if (file_open("TOUCH.CAL", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
//		return 0;
//	}
//	file_WriteParameters(touchCal, 4);
//	file_close();
//	return 1;
//}
//
//uint8_t touch_LoadCalibration(void) {
//	if (file_open("TOUCH.CAL", FA_OPEN_EXISTING | FA_READ) != FR_OK) {
//		return 0;
//	}
//	if (file_ReadParameters(touchCal, 4) == FILE_OK) {
//		file_close();
//		return 1;
//	} else {
//		file_close();
//		return 0;
//	}
//}
//
//void touch_Calibrate(void) {
//	if(!xSemaphoreTake(xMutexSPI1, 500)) {
//		/* SPI is busy */
//		return;
//	}
//	calibrating = 1;
//	uint8_t done = 0;
//	/* display first calibration cross */
//	display_SetBackground(COLOR_WHITE);
//	display_SetForeground(COLOR_BLACK);
//	display_Clear();
//	display_Line(0, 0, 40, 40);
//	display_Line(40, 0, 0, 40);
//	coords_t p1;
//	do {
//		/* wait for touch to be pressed */
//		while (!PENIRQ())
//			;
//		/* get raw data */
//		touch_SampleADC((uint16_t*) &p1.x, (uint16_t*) &p1.y, 1000);
//		if (p1.x <= 1000 && p1.y <= 1000)
//			done = 1;
//	} while (!done);
//	while (PENIRQ())
//		;
//	/* display second calibration cross */
//	display_Clear();
//	display_Line(DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1, DISPLAY_WIDTH - 41,
//	DISPLAY_HEIGHT - 41);
//	display_Line(DISPLAY_WIDTH - 41, DISPLAY_HEIGHT - 1, DISPLAY_WIDTH - 1,
//	DISPLAY_HEIGHT - 41);
//	coords_t p2;
//	done = 0;
//	do {
//		/* wait for touch to be pressed */
//		while (!PENIRQ())
//			;
//		/* get raw data */
//		touch_SampleADC((uint16_t*) &p2.x, (uint16_t*) &p2.y, 1000);
//		if (p2.x >= 3000 && p2.y >= 3000)
//			done = 1;
//	} while (!done);
//
//	/* calculate new calibration values */
//	/* calculate scale */
//	scaleX = (float) (DISPLAY_WIDTH - 40) / (p2.x - p1.x);
//	scaleY = (float) (DISPLAY_HEIGHT - 40) / (p2.y - p1.y);
//	/* calculate offset */
//	offsetX = 20 - p1.x * scaleX;
//	offsetY = 20 - p1.y * scaleY;
//
//	while(PENIRQ());
//
//	/* Release SPI resource */
//	xSemaphoreGive(xMutexSPI1);
//
//	/* Try to write calibration data to file */
//	if(touch_SaveCalibration()) {
//		printf("Wrote touch calibration file\n");
//	} else {
//		printf("Failed to create touch calibration file\n");
//	}
//
//	calibrating = 0;
//}

bool Touch::SetPENCallback(exti_callback_t cb, void* ptr) {
	return exti_set_callback(TOUCH_IRQ_GPIO_Port, TOUCH_IRQ_Pin,
			EXTI_TYPE_FALLING, EXTI_PULL_UP, cb, ptr) == EXTI_RES_OK;
}

bool Touch::ClearPENCallback(void) {
	return exti_clear_callback(TOUCH_IRQ_GPIO_Port, TOUCH_IRQ_Pin)
			== EXTI_RES_OK;
}

