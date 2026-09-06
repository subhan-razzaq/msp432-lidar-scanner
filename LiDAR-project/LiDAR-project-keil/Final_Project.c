// Final Project - 2DX3 Spatial Mapping
// Subhan Razzaq, 400557958, razzas2
//
// How it works:
//   PJ1 triggers a single 360 degree scan, motor unwinds after each one
//   keep pressing PJ1 for as many scans as you want (no limit)
//   PJ0 stops scanning and sends all buffered data to PC over UART
//   MATLAB receives the data and builds a 3D plot
//
// Student number params (400557958):
//   Bus speed = 34 MHz (J=8)
//   Measurement LED = PF4 (H=5)
//   UART Tx LED = PN1 (H=5)
//   Additional LED = PN0 (H=5)

#include <stdint.h>
#include <stdio.h>
#include "PLL.h"
#include "SysTick.h"
#include "uart.h"
#include "onboardLEDs.h"
#include "tm4c1294ncpdt.h"
#include "VL53L1X_api.h"

// I2C register bit definitions (from TM4C datasheet)
#define I2C_MCS_ACK             0x00000008
#define I2C_MCS_DATACK          0x00000008
#define I2C_MCS_ADRACK          0x00000004
#define I2C_MCS_STOP            0x00000004
#define I2C_MCS_START           0x00000002
#define I2C_MCS_ERROR           0x00000002
#define I2C_MCS_RUN             0x00000001
#define I2C_MCS_BUSY            0x00000001
#define I2C_MCR_MFE             0x00000010

// scan config
#define MAX_SCANS               20       // max scans we can buffer (20 * 32 * 2 bytes = 1280 bytes, fits in RAM fine)
#define STEPS_FULL_REV          512      // 512 full steps = one complete 360 degree revolution
#define STEP_INTERVAL           16       // take a measurement every 16 steps (11.25 degrees)
#define MEASUREMENTS_PER_REV    32       // 512 / 16 = 32 data points per scan
#define X_SPACING               100      // assume 10 cm (100 mm) between each scan position
#define TOF_TIMEOUT             50       // max number of 5ms polling cycles before giving up on a reading
#define MAX_DIST                4000     // anything beyond 4 meters is unreliable, cap to this value

// button return codes
#define BTN_NONE                0
#define BTN_SCAN                1        // PJ1
#define BTN_STOP                2        // PJ0

// ToF sensor I2C slave address
uint16_t dev = 0x29;
int status = 0;

// motor position tracking
volatile int motorPos = 0;       // tracks how many steps we've moved from home
volatile int homePos = 0;        // where we started (always 0)

// data buffer - stores distance readings for all scans
// each scan has 32 measurements, we support up to MAX_SCANS before needing to transmit
uint16_t scanData[MAX_SCANS][MEASUREMENTS_PER_REV];
int numScansDone = 0;


// ---- Port Initialization ----

// Port H [3:0] - stepper motor coil outputs
void PortH_Init(void)
{
	SYSCTL_RCGCGPIO_R |= SYSCTL_RCGCGPIO_R7;                 // enable clock for Port H
	while((SYSCTL_PRGPIO_R & SYSCTL_PRGPIO_R7) == 0)
	{
	}
	GPIO_PORTH_DIR_R |= 0x0F;                                  // PH0-PH3 as outputs
	GPIO_PORTH_DEN_R |= 0x0F;                                  // enable digital
}

// Port N [1:0] - onboard LEDs (PN1 = D1 = UART LED, PN0 = D2 = unwind LED)
void PortN_Init(void)
{
	SYSCTL_RCGCGPIO_R |= SYSCTL_RCGCGPIO_R12;
	while((SYSCTL_PRGPIO_R & SYSCTL_PRGPIO_R12) == 0)
	{
	}
	GPIO_PORTN_DIR_R |= 0x03;                                 // PN0, PN1 outputs
	GPIO_PORTN_AFSEL_R &= ~0x03;                              // no alt function
	GPIO_PORTN_DEN_R |= 0x03;                                 // digital enable
	GPIO_PORTN_AMSEL_R &= ~0x03;                              // no analog
}

// Port F - PF4 (measurement LED), PF0 (spare), PF2 (clock output for AD3)
void PortF_Init(void)
{
	SYSCTL_RCGCGPIO_R |= SYSCTL_RCGCGPIO_R5;
	while((SYSCTL_PRGPIO_R & SYSCTL_PRGPIO_R5) == 0)
	{
	}
	GPIO_PORTF_DIR_R |= 0x15;                                 // PF0, PF2, PF4 as outputs
	GPIO_PORTF_AFSEL_R &= ~0x15;
	GPIO_PORTF_DEN_R |= 0x15;
	GPIO_PORTF_AMSEL_R &= ~0x15;
}

// Port J [1:0] - onboard pushbuttons (active low, internal pull-ups)
// PJ0 = stop/transmit button, PJ1 = scan trigger button
void PortJ_Init(void)
{
	SYSCTL_RCGCGPIO_R |= SYSCTL_RCGCGPIO_R8;
	while((SYSCTL_PRGPIO_R & SYSCTL_PRGPIO_R8) == 0)
	{
	}
	GPIO_PORTJ_DIR_R &= ~0x03;                                // PJ0, PJ1 as inputs
	GPIO_PORTJ_DEN_R |= 0x03;                                 // digital enable
	GPIO_PORTJ_PUR_R |= 0x03;                                 // internal pull-ups (buttons read 1 when not pressed)
	GPIO_PORTJ_AMSEL_R &= ~0x03;
	GPIO_PORTJ_PCTL_R &= ~0xFF;                               // make sure its GPIO not alt function
}

// Port G [0] - XSHUT pin for ToF sensor hardware reset
void PortG_Init(void)
{
	SYSCTL_RCGCGPIO_R |= SYSCTL_RCGCGPIO_R6;
	while((SYSCTL_PRGPIO_R & SYSCTL_PRGPIO_R6) == 0)
	{
	}
	GPIO_PORTG_DIR_R &= ~0x01;                                // start as input (high-Z lets sensor run normally)
	GPIO_PORTG_AFSEL_R &= ~0x01;
	GPIO_PORTG_DEN_R |= 0x01;
	GPIO_PORTG_AMSEL_R &= ~0x01;
}


// ---- I2C Setup ----

// configures I2C0 on PB2 (SCL) and PB3 (SDA) for talking to the ToF sensor
void I2C_Init(void)
{
	SYSCTL_RCGCI2C_R |= SYSCTL_RCGCI2C_R0;                   // enable I2C module 0
	SYSCTL_RCGCGPIO_R |= SYSCTL_RCGCGPIO_R1;                  // enable Port B clock
	while((SYSCTL_PRGPIO_R & 0x02) == 0)
	{
	}

	GPIO_PORTB_AFSEL_R |= 0x0C;                               // PB2, PB3 use alternate function (I2C)
	GPIO_PORTB_ODR_R |= 0x08;                                 // open drain on PB3 (SDA needs this for I2C)
	GPIO_PORTB_DEN_R |= 0x0C;                                 // digital enable
	GPIO_PORTB_PCTL_R = (GPIO_PORTB_PCTL_R & 0xFFFF00FF) | 0x00002200;  // set alt function to I2C

	I2C0_MCR_R = I2C_MCR_MFE;                                 // enable master mode
	I2C0_MTPR_R = 0x3B;                                       // set clock to 100kbps
}

// these are needed by the VL53L1X driver library
void EnableInt(void)
{
	__asm("cpsie i\n");
}

void DisableInt(void)
{
	__asm("cpsid i\n");
}

void WaitForInt(void)
{
	__asm("wfi\n");
}


// ---- ToF Sensor Reset ----

// pulls XSHUT low to reset the sensor, then releases it to let it boot
void VL53L1X_XSHUT(void)
{
	GPIO_PORTG_DIR_R |= 0x01;                                 // make PG0 output
	GPIO_PORTG_DATA_R &= ~0x01;                               // drive low = sensor reset
	FlashAllLEDs();                                            // visual indicator that reset is happening
	SysTick_Wait10ms(10);                                      // hold for 100ms
	GPIO_PORTG_DIR_R &= ~0x01;                                // back to input (high-Z), sensor starts booting
}


// ---- LED Wrappers ----
// these map to my student-specific LED assignments (H=5)
// using the onboardLEDs library functions

// PF4 = D3 = flash when a distance measurement is taken
void BlinkMeasurementLED(void)
{
	FlashLED3(1);
}

// PN1 = D1 = flash when UART data is being sent
void BlinkUartLED(void)
{
	FlashLED1(1);
}

// PN0 = D2 = stays on while motor is unwinding (not a blink, a hold)
void SetUnwindLED(int on)
{
	if(on)
	{
		GPIO_PORTN_DATA_R |= 0x01;
	}
	else
	{
		GPIO_PORTN_DATA_R &= ~0x01;
	}
}


// ---- Stepper Motor Control ----

// one full step clockwise (4 phase sequence)
void StepCW(void)
{
	GPIO_PORTH_DATA_R = 0b00000011;
	SysTick_Wait10ms(1);
	GPIO_PORTH_DATA_R = 0b00000110;
	SysTick_Wait10ms(1);
	GPIO_PORTH_DATA_R = 0b00001100;
	SysTick_Wait10ms(1);
	GPIO_PORTH_DATA_R = 0b00001001;
	SysTick_Wait10ms(1);
	motorPos++;
}

// one full step counter-clockwise (reverse phase sequence)
void StepCCW(void)
{
	GPIO_PORTH_DATA_R = 0b00001001;
	SysTick_Wait10ms(1);
	GPIO_PORTH_DATA_R = 0b00001100;
	SysTick_Wait10ms(1);
	GPIO_PORTH_DATA_R = 0b00000110;
	SysTick_Wait10ms(1);
	GPIO_PORTH_DATA_R = 0b00000011;
	SysTick_Wait10ms(1);
	motorPos--;
}

// drives motor back to home position then cuts power to the coils
void ReturnHome(void)
{
	while(motorPos > homePos)
	{
		StepCCW();
	}
	while(motorPos < homePos)
	{
		StepCW();
	}
	GPIO_PORTH_DATA_R = 0x00;                                  // de-energize so motor doesnt overheat
}


// ---- Button Helper ----

// reads both buttons with debounce, returns BTN_SCAN, BTN_STOP, or BTN_NONE
// this is a single non-blocking check, not a blocking wait
int GetButtonPress(void)
{
	uint8_t raw = GPIO_PORTJ_DATA_R & 0x03;                   // read both buttons at once

	// both high = nothing pressed
	if(raw == 0x03)
	{
		return BTN_NONE;
	}

	// something is pressed, debounce
	SysTick_Wait10ms(5);                                       // 50ms debounce
	uint8_t confirmed = GPIO_PORTJ_DATA_R & 0x03;

	// check if same button is still held after debounce
	if(confirmed != raw)
	{
		return BTN_NONE;                                         // was noise
	}

	// PJ1 pressed (bit 1 = 0) - prioritize scan button
	if((confirmed & 0x02) == 0)
	{
		while((GPIO_PORTJ_DATA_R & 0x02) == 0)                  // wait for release
		{
		}
		SysTick_Wait10ms(5);                                     // post-release debounce
		return BTN_SCAN;
	}

	// PJ0 pressed (bit 0 = 0)
	if((confirmed & 0x01) == 0)
	{
		while((GPIO_PORTJ_DATA_R & 0x01) == 0)                  // wait for release
		{
		}
		SysTick_Wait10ms(5);                                     // post-release debounce
		return BTN_STOP;
	}

	return BTN_NONE;
}

// blocks until either button is pressed, returns BTN_SCAN or BTN_STOP
int WaitForButton(void)
{
	int btn = BTN_NONE;

	// make sure both buttons are released before we start polling
	while((GPIO_PORTJ_DATA_R & 0x03) != 0x03)
	{
	}
	SysTick_Wait10ms(10);                                      // 100ms settle time

	// poll until we get a valid press
	while(btn == BTN_NONE)
	{
		btn = GetButtonPress();
	}

	return btn;
}


// ---- ToF Distance Reading ----

// takes a single distance measurement from the sensor
// returns distance in mm, capped at MAX_DIST
// returns 0 if range status is bad or sensor timed out (caller substitutes last valid)
uint16_t TakeMeasurement(void)
{
	uint8_t ready = 0;
	int waitCount = 0;
	uint16_t dist = 0;
	uint8_t rngStatus = 0;

	status = VL53L1X_ClearInterrupt(dev);                      // clear old interrupt so we get a fresh one

	// keep polling until sensor says data is ready
	while(ready == 0 && waitCount < TOF_TIMEOUT)
	{
		status = VL53L1X_CheckForDataReady(dev, &ready);
		if(!ready)
		{
			VL53L1_WaitMs(dev, 5);
		}
		waitCount++;
	}

	if(!ready)
	{
		return 0;                                                // timeout, caller will substitute
	}

	// grab the distance and the range status
	status = VL53L1X_GetDistance(dev, &dist);
	status = VL53L1X_GetRangeStatus(dev, &rngStatus);
	status = VL53L1X_ClearInterrupt(dev);

	// cap anything beyond 4m to exactly 4000 instead of discarding
	if(dist > MAX_DIST)
	{
		dist = MAX_DIST;
	}

	// if range status is bad, return 0 so caller substitutes last valid reading
	if(rngStatus != 0)
	{
		return 0;
	}

	return dist;
}


// ---- Single Scan (One Full Revolution) ----

// rotates the motor 360 degrees CW, takes 32 measurements, then unwinds back home
// invalid readings are replaced with the last valid measurement
void DoOneScan(int scanNum)
{
	int idx = 0;
	uint16_t lastValid = 0;                                    // tracks last good reading for substitution

	sprintf(printf_buffer, "--- Scan %d ---\r\n", scanNum + 1);
	UART_printf(printf_buffer);
	BlinkUartLED();

	// rotate through 512 steps, measuring every 16th step
	for(int step = 0; step < STEPS_FULL_REV; step++)
	{
		StepCW();

		if((step + 1) % STEP_INTERVAL == 0)
		{
			uint16_t reading = TakeMeasurement();

			// if invalid, substitute with last valid reading
			if(reading == 0)
			{
				reading = lastValid;
			}
			else
			{
				lastValid = reading;
			}

			scanData[scanNum][idx] = reading;                    // store in buffer

			BlinkMeasurementLED();                               // flash PF4 for each measurement

			// print to terminal for debugging
			float angle = (float)(idx + 1) * 11.25f;
			if(reading == 0)
			{
				sprintf(printf_buffer, "  %.2f deg -> NO DATA\r\n", angle);
			}
			else
			{
				sprintf(printf_buffer, "  %.2f deg -> %u mm\r\n", angle, reading);
			}
			UART_printf(printf_buffer);
			BlinkUartLED();

			idx++;
		}
	}

	// unwind back to home, PN0 stays lit during this
	SetUnwindLED(1);
	ReturnHome();
	SetUnwindLED(0);

	sprintf(printf_buffer, "Scan %d done, motor home.\r\n", scanNum + 1);
	UART_printf(printf_buffer);
	BlinkUartLED();
}


// ---- Transmit All Buffered Data to PC ----

// sends everything we've collected in a format MATLAB can easily parse
// format: BEGIN_DATA, then SCAN headers with displacement, then 32 distances, then END_DATA
void SendAllData(void)
{
	UART_printf("BEGIN_DATA\r\n");
	BlinkUartLED();

	for(int s = 0; s < numScansDone; s++)
	{
		// SCAN <index> <x_displacement_mm>
		sprintf(printf_buffer, "SCAN %d %d\r\n", s, s * X_SPACING);
		UART_printf(printf_buffer);
		BlinkUartLED();

		// 32 distance values for this scan
		for(int p = 0; p < MEASUREMENTS_PER_REV; p++)
		{
			sprintf(printf_buffer, "%u\r\n", scanData[s][p]);
			UART_printf(printf_buffer);
			BlinkUartLED();
		}
	}

	UART_printf("END_DATA\r\n");
	BlinkUartLED();

	sprintf(printf_buffer, "Sent %d scans to PC.\r\n", numScansDone);
	UART_printf(printf_buffer);
	BlinkUartLED();
}


// ---- MAIN ----

int main(void)
{
	uint8_t sensorState = 0;
	uint16_t sensorID = 0;

	// initialize all peripherals
	PLL_Init();                                                // sets bus clock to 34 MHz (configured in PLL.c)
	SysTick_Init();
	onboardLEDs_Init();
	I2C_Init();
	UART_Init();
	PortH_Init();
	PortN_Init();
	PortF_Init();
	PortG_Init();
	PortJ_Init();

	UART_printf("2DX3 Spatial Mapper - Subhan Razzaq 400557958\r\n");
	BlinkUartLED();

	// reset the ToF sensor through XSHUT
	VL53L1X_XSHUT();
	SysTick_Wait10ms(10);

	// verify sensor communication by reading its ID
	status = VL53L1X_GetSensorId(dev, &sensorID);
	sprintf(printf_buffer, "Sensor ID: 0x%X\r\n", sensorID);
	UART_printf(printf_buffer);
	BlinkUartLED();

	// wait for sensor to finish booting internally
	while(sensorState == 0)
	{
		status = VL53L1X_BootState(dev, &sensorState);
		SysTick_Wait10ms(10);
	}
	UART_printf("Sensor booted.\r\n");
	BlinkUartLED();

	// configure the ToF sensor
	status = VL53L1X_ClearInterrupt(dev);
	status = VL53L1X_SensorInit(dev);
	Status_Check("SensorInit", status);

	status = VL53L1X_SetDistanceMode(dev, 2);                  // mode 2 = long range (up to 4m)
	Status_Check("SetDistanceMode", status);

	status = VL53L1X_SetTimingBudgetInMs(dev, 50);             // 50ms timing budget
	Status_Check("SetTimingBudget", status);

	status = VL53L1X_SetInterMeasurementInMs(dev, 55);         // inter-measurement must be >= timing budget
	Status_Check("SetInterMeasurement", status);

	status = VL53L1X_StartRanging(dev);
	Status_Check("StartRanging", status);

	UART_printf("Ready. PJ1=scan, PJ0=stop and send data\r\n");
	BlinkUartLED();

	// ---- main scan loop ----
	// no fixed number of scans - keep going until user presses PJ0 or we hit the buffer limit
	numScansDone = 0;

	while(numScansDone < MAX_SCANS)
	{
		sprintf(printf_buffer, "Waiting... (%d scans done)\r\n", numScansDone);
		UART_printf(printf_buffer);
		BlinkUartLED();

		// wait for user to press PJ1 (scan) or PJ0 (stop)
		int btn = WaitForButton();

		// debug: print which button was detected
		if(btn == BTN_SCAN)
		{
			UART_printf("Button: PJ1 (scan)\r\n");
			BlinkUartLED();
		}
		else if(btn == BTN_STOP)
		{
			UART_printf("Button: PJ0 (stop)\r\n");
			BlinkUartLED();
		}

		// if user pressed PJ0, break out and send data
		if(btn == BTN_STOP)
		{
			break;
		}

		SysTick_Wait10ms(5);                                     // brief pause before motor starts

		// restart the ranging cleanly between scans
		if(numScansDone > 0)
		{
			status = VL53L1X_StopRanging(dev);
			SysTick_Wait10ms(20);                                    // give sensor time to fully stop
			status = VL53L1X_ClearInterrupt(dev);
			status = VL53L1X_SensorInit(dev);                        // full re-init, not just start
			status = VL53L1X_SetDistanceMode(dev, 2);
			status = VL53L1X_SetTimingBudgetInMs(dev, 50);
			status = VL53L1X_SetInterMeasurementInMs(dev, 55);
			status = VL53L1X_StartRanging(dev);
			SysTick_Wait10ms(20);                                    // let first measurement cycle begin
		}

		// do one full revolution scan
		DoOneScan(numScansDone);
		numScansDone++;
	}

	// if we maxed out the buffer, let the user know
	if(numScansDone >= MAX_SCANS)
	{
		UART_printf("Buffer full, sending data automatically.\r\n");
		BlinkUartLED();
	}

	// ---- transmit phase ----
	// send all collected data to MATLAB
	if(numScansDone > 0)
	{
		UART_printf("Sending data to MATLAB...\r\n");
		BlinkUartLED();
		SendAllData();
	}
	else
	{
		UART_printf("No scans taken.\r\n");
		BlinkUartLED();
	}

	// done - stop sensor and kill motor
	status = VL53L1X_StopRanging(dev);
	GPIO_PORTH_DATA_R = 0x00;

	UART_printf("Done.\r\n");
	BlinkUartLED();

	// idle loop - toggle PF2 so we can measure bus speed on the AD3
	while(1)
	{
		GPIO_PORTF_DATA_R ^= 0x04;
		SysTick_Wait(340000);
		GPIO_PORTF_DATA_R ^= 0x04;
	}
}
