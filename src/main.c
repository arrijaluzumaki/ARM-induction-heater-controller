#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "stm32f30x_new.h"
#include "macros.h"
#include "hw.h"
#include "gpio.h"
#include "clocks.h"
#include "dpll.h"
#include "pwm.h"
#include "interrupts.h"
#include "exti.h"

/*--------------------------------------------------------------------*/
// Private function prototypes 
/*--------------------------------------------------------------------*/

void systemInit();
static void setSysClock();
void init();
void initPwmTimer();
void initIcTimer();
void initComp();

/*--------------------------------------------------------------------*/
// Main
//
int main(void) 
{
    systemInit();
    init();

	for(;;) 
    {
        // Run DPLL algorithm if we have a new count, adjusts PWM-frequency
        computeDpll();
    }

	return 0;
}
/*--------------------------------------------------------------------*/
// Configures clock and hardware for operation 
//
void systemInit()
{
    //#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    // FPU settings, set CP10 and CP11 full access
    SCB->CPACR |= ((3UL << 10*2)|(3UL << 11*2));
    //#endif

    // Configure the clocks
    setSysClock();

    #ifdef VECT_TAB_SRAM
    // Vector Table Relocation in Internal SRAM
    SCB->VTOR = SRAM_BASE | VECT_TAB_OFFSET; 
    #else
    // Vector Table Relocation in Internal FLASH
    SCB->VTOR = FLASH_BASE | VECT_TAB_OFFSET; 
    #endif  
}
/*--------------------------------------------------------------------*/
// Configures system clocks 
//
static void setSysClock()
{
    uint32_t startUpCounter = 0, hsiStatus = 0;

    // Enable HSI
    RCC->CR |= RCC_CR_HSION_ON_gc;

    // Wait till HSI is ready and if timeout is reached, exit
    do
    {
        hsiStatus = (RCC->CR & RCC_CR_HSIRDY_bm);
        startUpCounter++;  
    } while((hsiStatus == 0) && (startUpCounter != HSI_STARTUP_TIMEOUT));

    if ((RCC->CR & RCC_CR_HSIRDY_bm) != 0)
    {
        hsiStatus = 1;
    }
    else
    {
        hsiStatus = 0;
    }  

    if (hsiStatus == 1)
    {
        // Enable Prefetch Buffer and set flash Latency
        FLASH->ACR = (FLASH_ACR_PRFTBE_EN_gc | FLASH_ACR_LATENCY_1_gc);

        // HCLK = SYSCLK, PCLK1, PCLK2 = HCLK
        RCC->CFGR |= (  RCC_CFGR_HPRE_NODIV_gc | RCC_CFGR_PPRE2_NODIV_gc |
                        RCC_CFGR_PPRE1_NODIV_gc);

        // PLL configuration: PLLCLK = HSI/2 * 17 = 68 MHz
        RCC->CFGR |= (RCC_CFGR_PLLSRC_HSIDIVBY2_gc | RCC_CFGR_PLLMUL_17_gc);

        // Enable PLL
        RCC->CR |= RCC_CR_PLLON_ON_gc;

        // Wait till PLL is ready
        while((RCC->CR & RCC_CR_PLLRDY_bm) == 0) {}

        // Select PLL as system clock source
        RCC->CFGR &= (~(RCC_CFGR_SW_bm));
        RCC->CFGR |= RCC_CFGR_SW_PLL_gc;    

        // Wait till PLL is used as system clock source
        while ((RCC->CFGR & RCC_CFGR_SWS_bm) != RCC_CFGR_SWS_PLL_gc) {}
    }
    else
    { 
        /* If HSI fails to start-up, the application will have wrong clock 
         configuration. User can add here some code to deal with this error */
    }
}
/*--------------------------------------------------------------------*/
// Initializes hardware and system
//
void init() {
    // GPIO clocks
    enableAhbPeriphClk(RCC_AHBENR_IOPAEN_gc, ENABLE);
    enableAhbPeriphClk(RCC_AHBENR_IOPBEN_gc, ENABLE);

    // SysCfg clock, needed for EXTI
    enableApb2PeriphClk(RCC_APB2ENR_SYSCFGEN_gc, ENABLE);

    // LEDs
    setPinMode(&LED1_PORT, LED1, GPIO_MODER_OUT_gc);
    setPinMode(&LED2_PORT, LED2, GPIO_MODER_OUT_gc);
    setPinMode(&LED3_PORT, LED3, GPIO_MODER_OUT_gc);

    // PWM
    setPinMode(&DRVR_OE_PORT, DRVR_OE, GPIO_MODER_OUT_gc);
    setPin(&DRVR_OE_PORT, DRVR_OE);

    setPinMode(&GD_OE_PORT, GD_OE, GPIO_MODER_OUT_gc);
    setPin(&GD_OE_PORT, GD_OE);

    setAltFunct(&PWM_CH1_PORT, PWM_CH1, GPIO_AFR_AF6_gc);
    setAltFunct(&PWM_CH1N_PORT, PWM_CH1N, GPIO_AFR_AF6_gc);

    setPinMode(&PWM_CH1_PORT, PWM_CH1, GPIO_MODER_AF_gc);
    setPinMode(&PWM_CH1N_PORT, PWM_CH1N, GPIO_MODER_AF_gc);
    
    // Inputs
    // Comparator
    setPinMode(&FB_PIN_PORT, FB_PIN, GPIO_MODER_ANA_gc);

    // Enable interrupts
    initInterrupt(PWM_TIM_IRQN, 1, 2, ENABLE);
    initInterrupt(PWM_TIM_UP_IRQN, 1, 3, ENABLE);

    // Enable line driver and gate drivers
    resetPin(&DRVR_OE_PORT, DRVR_OE);
    resetPin(&GD_OE_PORT, GD_OE);

    // Initialize timers
    initPwmTimer();
    initComp();
    initIcTimer();
    setPin(&LED1_PORT, LED1);

    // Software modules
    initDpll();

    enableInterrupts();
}
/*--------------------------------------------------------------------*/
// Initialize PWM timer
//
void initPwmTimer()
{
    // TODO: timer outputs not resetting consistently, fix with pulldowns!
    // Before enabling TIM1 clock, set it to use doubled PLL as the clock
    SET_MASK(RCC->CFGR3, RCC_CFGR3_TIM1SW_bm, RCC_CFGR3_TIM1SW_PLL_gc);
    enableApb2PeriphClk(RCC_APB2ENR_TIM1EN_gc, ENABLE);

    uint32_t tempCcmr1 = PWM_TIM.CCMR1;
    // Set OC1 as output compare
    SET_MASK(tempCcmr1, AC_TIM_CCMR1_CC1S_bm, AC_TIM_CCMR1_CC1S_OUT_gc);
    // Set PWM mode to PWM mode 1
    SET_MASK(tempCcmr1, AC_TIM_CCMR1_OC1M_bm, AC_TIM_CCMR1_OC1M_PWMM1_gc);
    SET_MASK(tempCcmr1, AC_TIM_CCMR1_OC1PE_bm, AC_TIM_CCMR1_OC1PE_EN_gc);
    PWM_TIM.CCMR1 = tempCcmr1;

    // Put initial frequency and prescaler value to the auto-reload register
    PWM_TIM.ARR = INIT_PWM_FREQ;
    PWM_TIM.PSC = 0;
    // Put initial pulse-width values to the capture/compare registers
    PWM_TIM.CCR1 = INIT_PWM_FREQ / 2;

    // Enable OC1 pre-load
    // TODO: changed location: SET_MASK(PWM_TIM.CCMR1, AC_TIM_CCMR1_OC1PE_bm, AC_TIM_CCMR1_OC1PE_EN_gc);
    uint32_t tempCr1 = PWM_TIM.CR1;   
    // Enable pre-load
    SET_MASK(tempCr1, AC_TIM_CR1_ARPE_bm, AC_TIM_CR1_ARPE_BUF_gc);
    // Set counter mode to center-aligned (interrupt when counting up)
    SET_MASK(tempCr1, AC_TIM_CR1_CMS_bm, AC_TIM_CR1_CMS_CENTER1_gc);
    PWM_TIM.CR1 = tempCr1;

    // Configure interrupts
    uint32_t tempDier = PWM_TIM.DIER;
    SET_MASK(tempDier, AC_TIM_DIER_CC1IE_bm, AC_TIM_DIER_CC1IE_EN_gc);
    SET_MASK(tempDier, AC_TIM_DIER_UIE_bm, AC_TIM_DIER_UIE_EN_gc);
    PWM_TIM.DIER = tempDier;

    // Enable capture/compares and their complementary channels
    uint32_t tempCcer = PWM_TIM.CCER;
    SET_MASK(tempCcer, AC_TIM_CCER_CC1E_bm, AC_TIM_CCER_CC1E_EN_gc);
    SET_MASK(tempCcer, AC_TIM_CCER_CC1NE_bm, AC_TIM_CCER_CC1NE_EN_gc);
    PWM_TIM.CCER = tempCcer;

    // Set UG bit to cause update event to load register values from pre-load
    SET_MASK(PWM_TIM.EGR, AC_TIM_EGR_UG_bm, AC_TIM_EGR_UG_REINIT_gc);
    // Enable counter
    SET_MASK(PWM_TIM.CR1, AC_TIM_CR1_CEN_bm, AC_TIM_CR1_CEN_EN_gc);

    // Set automatic output enable
    uint32_t tempBdtr = PWM_TIM.BDTR; 
    SET_MASK(tempBdtr, AC_TIM_BDTR_AOE_bm, AC_TIM_BDTR_AOE_EN_gc);
    SET_MASK(tempBdtr, AC_TIM_BDTR_DTG_bm, DEADTIME);
    PWM_TIM.BDTR = tempBdtr;
}
/*--------------------------------------------------------------------*/
// Initialize timer for input capture
//
void initIcTimer()
{
    // Enable clock, enable 2x PLL clock
    enableApb2PeriphClk(RCC_APB2ENR_TIM8EN_gc, ENABLE);
    SET_MASK(RCC->CFGR3, RCC_CFGR3_TIM8SW_bm, RCC_CFGR3_TIM8SW_PLL_gc);
    // Set maximum period
    IC_TIM.ARR = MAX_PERIOD;
}
/*--------------------------------------------------------------------*/
// Initialize comparator
//
void initComp()
{
    //SET_MASK(FB_COMP.CSR, COMP1_CSR_POL_bm, COMP1_CSR_POL_INV_gc);
    // Set negative input to be vcc/2
    uint32_t tempCsr = FB_COMP.CSR;
    SET_MASK(tempCsr, COMP1_CSR_INMSEL_bm, COMP1_CSR_INMSEL_VCCDIV2_gc);
    // Enable comparator
    SET_MASK(tempCsr, COMP1_CSR_EN_bm, COMP1_CSR_EN_EN_gc);
    FB_COMP.CSR = tempCsr;

    initExtiLine(FB_COMP_EXTIN, RISING_EDGE);
    initInterrupt(FB_COMP_IRQN, 0, 1, ENABLE);
}

// Dummy function to avoid compiler error
void _init() {}
