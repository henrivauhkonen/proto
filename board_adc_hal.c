#include "board_adc_hal.h"

ADC_HandleTypeDef hadc1;

/*
 * ============================================================================
 * Sisäinen MSP-/rauta-alustus ADC1:lle
 * ============================================================================
 *
 * Tämä helperi hoitaa:
 * - ADC-kellon valinnan
 * - ADC:n tarvitsemien GPIO-porttien kellot
 * - ADC-tulopinnejä analog-tilaan
 *
 * Projektin tämänhetkiset ADC-tulot:
 *
 *   PA7 -> ADC1_IN12 -> VDIVS ADC
 *   PB0 -> ADC1_IN15 -> KELVIN ADC
 *
 * Huom:
 * VDIVS ADC lukee aina samaa VDIVS-solmua. Se, mikä resistiivinen haara
 * (LOW / MID / HIGH) kyseistä solmua kulloinkin syöttää, päätetään
 * topologiakerroksessa eikä tässä tiedostossa.
 */
static HAL_StatusTypeDef board_adc_hal_msp_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_PeriphCLKInitTypeDef periph_clk = {0};

    /*
     * Sama ADC-kelloratkaisun idea kuin nykyisessä CubeMX adc.c:ssä.
     *
     * ADC kellotetaan PLLSAI1-lähteestä.
     * Tämä on tässä vaiheessa hyvä ja toimiva perusratkaisu bring-upiin.
     */
    periph_clk.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    periph_clk.AdcClockSelection = RCC_ADCCLKSOURCE_PLLSAI1;
    periph_clk.PLLSAI1.PLLSAI1Source = RCC_PLLSOURCE_MSI;
    periph_clk.PLLSAI1.PLLSAI1M = 1;
    periph_clk.PLLSAI1.PLLSAI1N = 16;
    periph_clk.PLLSAI1.PLLSAI1P = RCC_PLLP_DIV7;
    periph_clk.PLLSAI1.PLLSAI1Q = RCC_PLLQ_DIV2;
    periph_clk.PLLSAI1.PLLSAI1R = RCC_PLLR_DIV2;
    periph_clk.PLLSAI1.PLLSAI1ClockOut = RCC_PLLSAI1_ADC1CLK;

    if (HAL_RCCEx_PeriphCLKConfig(&periph_clk) != HAL_OK)
    {
        return HAL_ERROR;
    }

    __HAL_RCC_ADC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /*
     * PA7 -> ADC1_IN12 -> VDIVS
     *
     * Tämä on yhteinen VDIVS-lukupiste kaikille VDIVS-haaroille:
     * - LOW  = 820 Ω
     * - MID  = 56 kΩ
     * - HIGH = 470 kΩ
     *
     * Topologiakerros päättää mikä haara on aktiivinen.
     * ADC HAL vain konfiguroi lukupisteen.
     */
    gpio.Pin = GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_ANALOG_ADC_CONTROL;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    /*
     * PB0 -> ADC1_IN15 -> KELVIN
     *
     * Tämä tulokanava on varattu Kelvin-haaran lukemiseen.
     */
    gpio.Pin = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_ANALOG_ADC_CONTROL;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &gpio);

    return HAL_OK;
}

HAL_StatusTypeDef board_adc_hal_initialize(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    HAL_StatusTypeDef status;

    status = board_adc_hal_msp_init();
    if (status != HAL_OK)
    {
        return status;
    }

    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait = DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.NbrOfConversion = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.DMAContinuousRequests = DISABLE;
    hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
    hadc1.Init.OversamplingMode = DISABLE;

    status = HAL_ADC_Init(&hadc1);
    if (status != HAL_OK)
    {
        return status;
    }

    /*
     * Oletuskanava vain validia init-polkuja varten.
     *
     * Varsinainen measurement_adc.c vaihtaa kanavaa myöhemmin itse.
     * Oletukseksi valitaan tässä KELVIN-kanava (ADC1_IN15 / PB0),
     * kuten aiemmassakin rungossa.
     */
    sConfig.Channel = ADC_CHANNEL_15;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_47CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0U;

    status = HAL_ADC_ConfigChannel(&hadc1, &sConfig);
    if (status != HAL_OK)
    {
        return status;
    }

    return HAL_OK;
}

HAL_StatusTypeDef board_adc_hal_deinitialize(void)
{
    HAL_StatusTypeDef status;

    status = HAL_ADC_DeInit(&hadc1);
    if (status != HAL_OK)
    {
        return status;
    }

    __HAL_RCC_ADC_CLK_DISABLE();

    /*
     * Vapautetaan tähän kerrokseen kuuluvat ADC-pinnit.
     */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_7); /* VDIVS ADC */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_0); /* KELVIN ADC */

    return HAL_OK;
}
