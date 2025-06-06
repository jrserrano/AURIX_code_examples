/**********************************************************************************************************************
 * \file Slk_DAct_Fuc3.c
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
#include "Digital_Acquisition_Actuation/Digital_Actuation/Slk_DAct_Fuc3.h"
#include "Digital_Acquisition_Actuation/Slk_DA_Global.h"
#include "SafetyLiteKit/Slk_Cfg.h"
#include "SafetyLiteKit/Smu/SMU.h"
#include "IfxCcu6_TPwm.h"
#include "IfxCcu6_PwmHl.h"
#include "Ifx_Types.h"
#include "IfxGpt12.h"
#include "IfxCpu_Irq.h"
#include "IfxCcu6_TimerWithTrigger.h"
#include "Bsp.h"

/*********************************************************************************************************************/
/*------------------------------------------------------Macros-------------------------------------------------------*/
/*********************************************************************************************************************/
#define CCU6_BASE_FREQUENCY     781250                                       /* CCU6 base frequency, in Hertz        */
#define PWM_FREQUENCY           50                                           /* PWM signal frequency, in Hertz       */
#define PWM_PERIOD              (CCU6_BASE_FREQUENCY / PWM_FREQUENCY)        /* PWM signal period, in ticks          */
#define CHANNEL1_DUTY_CYCLE     25                                           /* PWM Signal 1 Duty cycle, in percent  */
#define CHANNEL1_COMPARE_VALUE  ((PWM_PERIOD / 100) * (100 - CHANNEL1_DUTY_CYCLE))

/*  GTP12 Macros */
#define MAX_VAL_16_BIT              0xFFFF          /* Used for calculation of timer value with overflows            */

/*********************************************************************************************************************/
/*-------------------------------------------------Global variables--------------------------------------------------*/
/*********************************************************************************************************************/
static IfxCcu6_TPwm tPwm;

uint32  g_cntOverflow           = 0;    /* Global counter variable for the timer overflow between two edges          */
uint32  g_previousCntVal        = 0;    /* Global variable which stores the timer value of the previous interrupt    */
IFX_EXTERN DigitalAcqActStatusDatatype      g_digitalAcqActStatus;

/*********************************************************************************************************************/
/*--------------------------------------------Private Variables/Constants--------------------------------------------*/
/*********************************************************************************************************************/

/*********************************************************************************************************************/
/*------------------------------------------------Function Prototypes------------------------------------------------*/
/*********************************************************************************************************************/
void checkPlausibilityDActFuc3(void);
void initCCU6Mission(void);
void initGPT12Monitor(void);

/* Macro to define the Interrupt Service Routine */
IFX_INTERRUPT(isrHandlerGpt12T2, CPU1_RUNNING_TASK, ISR_PRIORITY_GPT12_T2_INT_DACT_FUC3);
IFX_INTERRUPT(isrHandlerGpt12T3, CPU1_RUNNING_TASK, ISR_PRIORITY_GPT12_T3_INT_DACT_FUC3);
IFX_INTERRUPT(isrCcu6TPwm, CPU1_RUNNING_TASK, ISR_PRIORITY_CCU6_MISSION_DACT_FUC3);

/*********************************************************************************************************************/
/*---------------------------------------------Function Implementations----------------------------------------------*/
/*********************************************************************************************************************/
/* Interrupt Service Routine of CCU6 timer */
void isrCcu6TPwm(void)
{
    checkPlausibilityDActFuc3();
}

/* Interrupt Service Routine of timer T4, gets triggered by rising edge on input pin of timer T4 */
void isrHandlerGpt12T2(void)
{
    uint32 currentCntVal = IfxGpt12_T2_getTimerValue(&MODULE_GPT120); /* Get timer value of timer T4 */
    uint32 finalCntVal = 0; /* Variable to calculate final timer counter value */

    if(g_cntOverflow == 0)
    {
        /* If no overflow detected */
        finalCntVal = currentCntVal - g_previousCntVal; /* Total counter value calculation */
    }
    else
    {
        /* One or more overflows detected */
        /* Add to the current counter value, the amount of counter ticks which passed before the first overflow,
         * plus 65525 (0xFFFF) for each additional overflow since the previous rising edge.
         */
        finalCntVal = (uint32) (currentCntVal + (MAX_VAL_16_BIT - g_previousCntVal)
                + ((g_cntOverflow - 1) * MAX_VAL_16_BIT));
    }
    /* Calculation of the PWM frequency by dividing the frequency of timer T3 through the final total counter value */
    g_digitalAcqActStatus.pwmFrequency[1] = IfxGpt12_T3_getFrequency(&MODULE_GPT120) / finalCntVal;

    g_previousCntVal = currentCntVal;    /* Set g_previousCntVal to currentCntVal for the next calculation */
    g_cntOverflow = 0;                   /* Reset overflow flag */
}

/* Interrupt Service Routine of timer T3, gets triggered after T3 timer overflow */
void isrHandlerGpt12T3(void)
{
    g_cntOverflow++; /* Increase overflow counter */
}

/*
 * this function perform plausibility check
 * */
void checkPlausibilityDActFuc3(void)
{
    static uint8 count = 0;
    /* Ignore first three measurements as measurement might return 0 at the beginning (self-calibration) */
    if (count < 3)
    {
        count++;
    }
    else
    {
        g_digitalAcqActStatus.pwmFrequency[0] = PWM_FREQUENCY;

        /* Pwm frequency comparison */
        comparePwmFrequency(g_digitalAcqActStatus.pwmFrequency[0], g_digitalAcqActStatus.pwmFrequency[1]);

        /* Reset measurement mission frequency */
        g_digitalAcqActStatus.pwmFrequency[1] = 0;
    }
}

/*
 * This function initializes timer T2 and T3 of the block GPT1 of module GPT12 to capture PWM signals.
 * */
void initGPT12Monitor(void)
{
    /* Enable GPT12 module */
    IfxGpt12_enableModule(&MODULE_GPT120);

    /* Select 4 as prescaler for the GPT1 module -> fastest clock frequency for best accuracy */
    IfxGpt12_setGpt1BlockPrescaler(&MODULE_GPT120, IfxGpt12_Gpt1BlockPrescaler_4);

    /* Set core timer T3 in timer mode */
    IfxGpt12_T3_setMode(&MODULE_GPT120, IfxGpt12_Mode_timer);

    /* Set auxiliary timer T2 in capture mode */
    IfxGpt12_T2_setMode(&MODULE_GPT120, IfxGpt12_Mode_capture);


    /* Select input pin B of timer T2 which is P33.7  */
    IfxGpt12_initTxInPin(&GPT12_INPUT_MONITOR_PIN_FUC3, IfxPort_InputMode_pullUp);

    /* Select rising edge as capture event */
    IfxGpt12_T2_setCaptureInputMode(&MODULE_GPT120, IfxGpt12_CaptureInputMode_risingEdgeTxIN);

    /* Service request configuration */
    /* Get source pointer of timer T2, initialize and enable */
    volatile Ifx_SRC_SRCR *src = IfxGpt12_T2_getSrc(&MODULE_GPT120);
    IfxSrc_init(src, IfxSrc_Tos_cpu1, ISR_PRIORITY_GPT12_T2_INT_DACT_FUC3);
    IfxSrc_enable(src);

    /* Get source pointer of timer T3, initialize and enable */
    src = IfxGpt12_T3_getSrc(&MODULE_GPT120);
    IfxSrc_init(src, IfxSrc_Tos_cpu1, ISR_PRIORITY_GPT12_T3_INT_DACT_FUC3);
    IfxSrc_enable(src);

    /* Start timer T3*/
    IfxGpt12_T3_run(&MODULE_GPT120, IfxGpt12_TimerRun_start);
}

/*
 * Function to initialize the CCU6 module to generate PWM signals
 * */
void initCCU6Mission(void)
{
    boolean interruptState = IfxCpu_disableInterrupts();      /* Disable global interrupts */
    /* create configuration */
    IfxCcu6_TPwm_Config tPwmConfig;
    IfxCcu6_TPwm_initModuleConfig(&tPwmConfig, CCU6_OUTPUT_MISSION_PIN.module);

    /* configure the frequency of the timer in case of internal start
    this frequency will be set for the timer block selected later */
    tPwmConfig.base.frequency = CCU6_BASE_FREQUENCY;

    /* configure the period of the timer */
    tPwmConfig.base.period = PWM_PERIOD;

    /* configure the waiting time in case of delayed T13 start in sync with T12 */
    tPwmConfig.base.waitingTime = 20;

    /* select the active state of the output */
    tPwmConfig.base.activeState = Ifx_ActiveState_high;

    /* select the timer through which PWM is to be generated */
    tPwmConfig.timer = IfxCcu6_TimerId_t13;

    /* configure the clock for internal mode */
    tPwmConfig.clock.t13ExtClockEnabled   = FALSE;
    tPwmConfig.clock.t13ExtClockInput     = NULL_PTR;
    tPwmConfig.clock.t13countingInputMode = IfxCcu6_CountingInputMode_internal;

    /* configure the selected timer block */
    tPwmConfig.timer13.counterValue     = 0;
    tPwmConfig.timer13.compareValue     = CHANNEL1_COMPARE_VALUE;
    /* configure the synchronization, in case of sync start with T12 */
    tPwmConfig.timer13.t12SyncEvent     = IfxCcu6_T13TriggerEvent_onCC60RCompare;
    tPwmConfig.timer13.t12SyncDirection = IfxCcu6_T13TriggerDirection_onT12CountingUp;

    /*select the channel out for modulation */
    tPwmConfig.channelOut = IfxCcu6_ChannelOut_cout3;

    /* pin configuration */
    IfxCcu6_TPwm_Pins pins = {
      NULL,    /* CC60Out pin not used */
      NULL,    /* CC61Out pin not used */
      NULL,    /* CC62Out pin not used */
      NULL,    /* COUT60 pin not used */
      NULL,    /* COUT61 pin not used */
      NULL,    /* COUT62 pin not used */
      &CCU6_OUTPUT_MISSION_PIN,   /* COUT63 pin */
      IfxPort_OutputMode_pushPull,
      IfxPort_PadDriver_cmosAutomotiveSpeed1
    };
    tPwmConfig.pins = &pins;

    /* configure the interrupts */
    tPwmConfig.interrupt1.source = IfxCcu6_InterruptSource_t13CompareMatch;
    tPwmConfig.interrupt1.serviceRequest  = IfxCcu6_ServiceRequest_2;
    tPwmConfig.interrupt1.priority        = ISR_PRIORITY_CCU6_MISSION_DACT_FUC3;
    tPwmConfig.interrupt1.typeOfService   = IfxCpu_Irq_getTos(CPU1_RUNNING_TASK);

    /* initialize the module */
    IfxCcu6_TPwm_initModule(&tPwm, &tPwmConfig);

    /* Restore interrupts to their initial state */
    IfxCpu_restoreInterrupts(interruptState);

    /* start PWM  */
    IfxCcu6_TPwm_start(&tPwm);
}

/*
 * SM:CCU6_GPT12_MONITORING
 * */
void initDActFuc3(void)
{
    resetDAconfiguration();
    /* Initialize the GPT module */
    initGPT12Monitor();
    /* Initialize the CCU6 module and start the PWM generation */
    initCCU6Mission();
}
