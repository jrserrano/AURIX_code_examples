/**********************************************************************************************************************
 * \file Slk_DieTemp.c
 * \copyright Copyright (C) Infineon Technologies AG 2019
 * 
 * Use of this file is subject to the terms of use agreed between (i) you or the company in which ordinary course of 
 * business you are acting and (ii) Infineon Technologies AG or its licensees. If and as long as no such terms of use
 * are agreed, use of this file is subject to following:
 * 
 * Boost Software License - Version 1.0 - August 17th, 2003
 * 
 * Permission is hereby granted, free of charge, to any person or organization obtaining a copy of the software and 
 * accompanying documentation covered by this license (the "Software") to use, reproduce, display, distribute, execute,
 * and transmit the Software, and to prepare derivative works of the Software, and to permit third-parties to whom the
 * Software is furnished to do so, all subject to the following:
 * 
 * The copyright notices in the Software and this entire statement, including the above license grant, this restriction
 * and the following disclaimer, must be included in all copies of the Software, in whole or in part, and all 
 * derivative works of the Software, unless such copies or derivative works are solely in the form of 
 * machine-executable object code generated by a source language processor.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE 
 * COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN 
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
 * IN THE SOFTWARE.
 *********************************************************************************************************************/


/*********************************************************************************************************************/
/*-----------------------------------------------------Includes------------------------------------------------------*/
/*********************************************************************************************************************/
#include "SafetyLiteKit/Avoid_Detect_CCF/Slk_DieTemp.h"
#include "SafetyLiteKit/Slk_Cfg.h"
#include "SafetyLiteKit/Slk_Main.h"
#include "SafetyLiteKit/Smu/SMU.h"
#include "Ifx_Types.h"
#include "IfxDts_Dts.h"
#include "IfxPms_reg.h"
#include "IfxCpu_Irq.h"

/*********************************************************************************************************************/
/*------------------------------------------------------Macros-------------------------------------------------------*/
/*********************************************************************************************************************/
#define MIN_TEMP_LIMIT          -35     /* Lower temperature limit */
#define MAX_TEMP_LIMIT          150     /* Upper temperature limit */

/*********************************************************************************************************************/
/*-------------------------------------------------Global variables--------------------------------------------------*/
/*********************************************************************************************************************/

/*********************************************************************************************************************/
/*--------------------------------------------Private Variables/Constants--------------------------------------------*/
/*********************************************************************************************************************/

/*********************************************************************************************************************/
/*------------------------------------------------Function Prototypes------------------------------------------------*/
/*********************************************************************************************************************/

/*********************************************************************************************************************/
/*---------------------------------------------Function Implementations----------------------------------------------*/
/*********************************************************************************************************************/
/* Macro to define Interrupt Service Routine.
 * This macro makes following definitions:
 * 1) Define linker section as .intvec_tc<vector number>_<interrupt priority>.
 * 2) define compiler specific attribute for the interrupt functions.
 * 3) define the Interrupt Service Routine as ISR function.
 *
 * IFX_INTERRUPT(isr, vectabNum, priority)
 *  - isr: Name of the ISR function.
 *  - vectabNum: Vector table number.
 *  - priority: Interrupt priority. Refer Usage of Interrupt Macro for more details.
 */
IFX_INTERRUPT(dtsMeasurementISR, CPU2_RUNNING_TASK, ISR_PRIORITY_DTS);

void dtsMeasurementISR(void)
{
    /* Ignore first three measurements as core temperature sensor value might return 0 at the beginning (self-calibration) */
    static uint8 init_cnt;

    init_cnt++;
    if (init_cnt < 3)
    {
        return;
    }

    float32 dieTemperaturePms;
    float32 dieTemperatureCore;

    /* Get the new PMS die temperature measurement */
    dieTemperaturePms = IfxDts_Dts_getTemperatureCelsius();

    /* Also get the CORE die temperature from DTSCSTAT register of SCU module */
    dieTemperatureCore = IfxDts_Dts_convertToCelsius((uint16) MODULE_SCU.DTSCSTAT.B.RESULT);

    /* SM:DTS:DTS_RESULT */
    /* Calculate the absolute value of the temperature difference, trigger SMU software alarm if value is above limit. */
    float32 dieTempDifference;
    dieTempDifference =
            (dieTemperatureCore > dieTemperaturePms) ?
                    dieTemperatureCore - dieTemperaturePms : dieTemperaturePms - dieTemperatureCore;

    g_SlkStatus.dieTempStatus.dieTemperaturePms = dieTemperaturePms;
    g_SlkStatus.dieTempStatus.dieTemperatureCore = dieTemperatureCore;
    g_SlkStatus.dieTempStatus.dieTempDifference = dieTempDifference;

    float32 higherTemp =
            g_SlkStatus.dieTempStatus.dieTemperatureCore > g_SlkStatus.dieTempStatus.dieTemperaturePms ?
                    g_SlkStatus.dieTempStatus.dieTemperatureCore : g_SlkStatus.dieTempStatus.dieTemperaturePms;
    float32 lowerTemp =
            g_SlkStatus.dieTempStatus.dieTemperatureCore < g_SlkStatus.dieTempStatus.dieTemperaturePms ?
                    g_SlkStatus.dieTempStatus.dieTemperatureCore : g_SlkStatus.dieTempStatus.dieTemperaturePms;

    /* limit checking for highest */
    if (higherTemp > g_SlkStatus.dieTempStatus.dieTempHighest)
    {
        g_SlkStatus.dieTempStatus.dieTempHighest = higherTemp; /* Overwrite if new value is higher as old maximum */
    }

    /* limit checking for lowest value */
    if (lowerTemp < g_SlkStatus.dieTempStatus.dieTempLowest)
    {
        g_SlkStatus.dieTempStatus.dieTempLowest = lowerTemp; /* Overwrite if new value is lower as old minimum */
    }

    if (dieTempDifference > MAX_DIE_TEMP_DIFF)
    {
        softwareCoreAlarmTriggerSMU(SOFT_SMU_ALM_DTS);
    }
}

/* Function to initialize the Die Temperature Sensors
 * SM:DTS:DTS_CFG
 * */
void initDieTemperatureSensors(void)
{
    /* First initialize the PMS Die temperature sensor */
    /* Get the default configuration */
    IfxDts_Dts_Config dtsConf;
    IfxDts_Dts_initModuleConfig(&dtsConf);                                  /* Initialize the structure with default values              */

    dtsConf.lowerTemperatureLimit = MIN_TEMP_LIMIT;                         /* Set the lower temperature limit                           */
    dtsConf.upperTemperatureLimit = MAX_TEMP_LIMIT;                         /* Set the upper temperature limit                           */
    dtsConf.isrPriority = ISR_PRIORITY_DTS;                                 /* Set the interrupt priority for new measurement events     */
    dtsConf.isrTypeOfService = IfxCpu_Irq_getTos(CPU2_RUNNING_TASK);    /* Set the service provider responsible for handling
                                                                             * the interrupts                                            */
    /* Clear potential lower limit or upper limit overflow bits, might have happened during startup self tests */
    MODULE_PMS.DTSLIM.B.LLU = 0;
    MODULE_PMS.DTSLIM.B.UOF = 0;

    IfxDts_Dts_initModule(&dtsConf);                                        /* Initialize the DTS with the given configuration           */

    /* Also initialize the Die temperature sensor of the DTS core */
    /* Clear potential lower limit or upper limit overflow bits, might have happened during startup self tests */
    MODULE_SCU.DTSCLIM.B.LLU = 0;
    MODULE_SCU.DTSCLIM.B.UOF = 0;

    /* Wait for a few measurements before enabling the requested limits */
    MODULE_SCU.DTSCLIM.B.LOWER = 0;
    MODULE_SCU.DTSCLIM.B.UPPER = 4095;

    /* Change to the requested limits */
    MODULE_SCU.DTSCLIM.B.LOWER = IfxDts_Dts_convertFromCelsius(MIN_TEMP_LIMIT);
    MODULE_SCU.DTSCLIM.B.UPPER = IfxDts_Dts_convertFromCelsius(MAX_TEMP_LIMIT);

    /* Finally enable the Die temperature sensor of the DTS core */
    MODULE_SCU.DTSCLIM.B.EN = TRUE;

    /* Set default values for temperature tracking since PORST */
    g_SlkStatus.dieTempStatus.dieTempLowest   = MAX_TEMP_LIMIT;
    g_SlkStatus.dieTempStatus.dieTempHighest  = MIN_TEMP_LIMIT;
}
