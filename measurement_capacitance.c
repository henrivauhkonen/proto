#include "measurement_capacitance.h"

#include "debug_logging.h"

/*
 * ============================================================================
 * Kapasitanssimittaus TIM2 input capture -tekniikalla
 * ============================================================================
 *
 * Tämä moduuli toteuttaa saman perusidean kuin referenssikoodi:
 *
 * - käytetään TIM2:ta free-running-ajastimena
 * - yksi input capture -kanava mittaa reunojen aikaleimat
 * - ohjelma vaihtaa capture-polariteettia sen mukaan, mitataanko
 *   HIGH-pulssi vai LOW-pulssi
 *
 * Tässä versiossa valitaan oletuksena:
 *   TIM2_CH1 -> PA0
 *
 * Perustelut:
 * - TIM2 on käyttäjän toivoma timeri
 * - PA0 on STM32L432KC:llä TIM2_CH1-pinni
 * - PA0 on projektin kokonaisuudessa helpompi valinta kuin PA5/PA6
 *   (PA5/PA6 board-rajoitteet) tai PA2 (serial/debug-rooli)
 */

#define MEASUREMENT_CAP_LOG_TAG                  "CAP"

/*
 * Kapasitanssikaavan vakioita.
 *
 * Sama muoto kuin käyttäjän referenssissä:
 *
 *   C = 1.44 / ((R1 + 2*R2) * f)
 *
 * Tulosta skaalataan pikofaradeiksi.
 *
 * Nämä kannattaa myöhemmin säätää todellisten mitattujen vastusten mukaan.
 */
#define MEASUREMENT_CAP_R1_OHMS                  9973.0f
#define MEASUREMENT_CAP_R2_OHMS                 99092.5f
#define MEASUREMENT_CAP_CAL_FACTOR                  1.0f

/*
 * Aikakatkaisu yhdelle pulse capture -operaatiolle.
 */
#define MEASUREMENT_CAP_TIMEOUT_MS              1000U

/*
 * Vähimmäispituus hyväksyttävälle pulssille.
 *
 * Tämä vastaa refun suojausta, jotta aivan järjettömät mittaukset
 * karsiutuvat pois.
 */
#define MEASUREMENT_CAP_MIN_PULSE_NS             100U

/*
 * Montako näytettä otetaan normaalisti.
 *
 * Sama ajatus kuin referenssissä: otetaan useita periodimittauksia
 * ja tehdään niistä trimmattu keskiarvo.
 */
#define MEASUREMENT_CAP_MAX_SAMPLES               12

/*
 * TIM2 / pinni-valinta tälle toteutukselle.
 *
 * Oletus:
 *   PA0 -> TIM2_CH1
 *
 * Jos halutaan myöhemmin käyttää täsmälleen referenssin CH3-tyyliä,
 * nämä makrot voidaan vaihtaa:
 *
 *   GPIOA / PIN_2 / TIM_CHANNEL_3 / CCR3 / CC3IF / CC3P
 */
#define MEASUREMENT_CAP_TIM_INSTANCE             TIM2
#define MEASUREMENT_CAP_TIM_CHANNEL              TIM_CHANNEL_1
#define MEASUREMENT_CAP_TIM_GPIO_PORT            GPIOA
#define MEASUREMENT_CAP_TIM_GPIO_PIN             GPIO_PIN_0
#define MEASUREMENT_CAP_TIM_GPIO_AF              GPIO_AF1_TIM2

#define MEASUREMENT_CAP_TIM_CCIF_FLAG            TIM_SR_CC1IF
#define MEASUREMENT_CAP_TIM_CCR_REG(timer)       ((timer)->CCR1)
#define MEASUREMENT_CAP_TIM_CCER_POL_BIT         TIM_CCER_CC1P

/*
 * Sisäinen TIM-handle.
 */
static TIM_HandleTypeDef g_measurement_cap_htim2;

/*
 * Yhden kerran tehtävä init-lippu.
 */
static uint8_t g_measurement_cap_initialized = 0U;

/*
 * ============================================================================
 * Sisäiset helperit
 * ============================================================================
 */

/*
 * Palauttaa TIM2:n laskentataajuuden hertseinä.
 *
 * STM32L4-timereillä APB-prescaler vaikuttaa timerikelloon niin,
 * että jos APB-jakaja on >1, timerikello voi olla 2x PCLK.
 *
 * Tämä helperi laskee tick-taajuuden turvallisemmin kuin pelkkä
 * SystemCoreClock-oletus.
 */
static uint32_t measurement_capacitance_get_tim_clock_hz(void)
{
    RCC_ClkInitTypeDef clock_config = {0};
    uint32_t flash_latency = 0U;
    uint32_t pclk1_hz;
    uint32_t tim_clk_hz;

    HAL_RCC_GetClockConfig(&clock_config, &flash_latency);
    pclk1_hz = HAL_RCC_GetPCLK1Freq();

    if (clock_config.APB1CLKDivider == RCC_HCLK_DIV1)
    {
        tim_clk_hz = pclk1_hz;
    }
    else
    {
        tim_clk_hz = pclk1_hz * 2U;
    }

    return tim_clk_hz;
}

/*
 * Järjestää taulukon pienimmästä suurimpaan.
 *
 * Kapasitanssimittauksessa käytetään useita näytteitä ja trimmattua
 * keskiarvoa, jotta yksittäiset häiriöpiikit eivät hallitse tulosta.
 */
static void measurement_capacitance_sort_float_array(float *values, int count)
{
    int i;
    int j;

    for (i = 1; i < count; ++i)
    {
        float key = values[i];
        j = i - 1;

        while ((j >= 0) && (values[j] > key))
        {
            values[j + 1] = values[j];
            --j;
        }

        values[j + 1] = key;
    }
}

/*
 * Muuntaa periodin (ns) kapasitanssiksi (pF) refun kaavalla.
 */
static float measurement_capacitance_period_ns_to_pf(uint32_t period_ns)
{
    float frequency_hz;

    if (period_ns == 0U)
    {
        return -1.0f;
    }

    frequency_hz = 1.0e9f / (float)period_ns;

    if (frequency_hz <= 0.0f)
    {
        return -1.0f;
    }

    return
        (1.44e12f / ((MEASUREMENT_CAP_R1_OHMS + (2.0f * MEASUREMENT_CAP_R2_OHMS)) * frequency_hz))
        * MEASUREMENT_CAP_CAL_FACTOR;
}

/*
 * ============================================================================
 * Julkinen API
 * ============================================================================
 */

HAL_StatusTypeDef measurement_capacitance_initialize(void)
{
    TIM_IC_InitTypeDef input_capture_config = {0};
    GPIO_InitTypeDef gpio_config = {0};

    if (g_measurement_cap_initialized != 0U)
    {
        return HAL_OK;
    }

    /*
     * Kellot päälle:
     * - GPIOA PA0:lle
     * - TIM2:lle
     */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();

    /*
     * PA0 -> TIM2_CH1 alternate function.
     */
    gpio_config.Pin = MEASUREMENT_CAP_TIM_GPIO_PIN;
    gpio_config.Mode = GPIO_MODE_AF_PP;
    gpio_config.Pull = GPIO_NOPULL;
    gpio_config.Speed = GPIO_SPEED_FREQ_LOW;
    gpio_config.Alternate = MEASUREMENT_CAP_TIM_GPIO_AF;

    HAL_GPIO_Init(MEASUREMENT_CAP_TIM_GPIO_PORT, &gpio_config);

    /*
     * TIM2 free-running, maksimi periodi, prescaler=0.
     *
     * Tämä vastaa referenssin ideaa:
     * halutaan suora, nopeasti tikittävä 32-bit laskuri.
     */
    g_measurement_cap_htim2.Instance = MEASUREMENT_CAP_TIM_INSTANCE;
    g_measurement_cap_htim2.Init.Prescaler = 0U;
    g_measurement_cap_htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    g_measurement_cap_htim2.Init.Period = 0xFFFFFFFFU;
    g_measurement_cap_htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    g_measurement_cap_htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_IC_Init(&g_measurement_cap_htim2) != HAL_OK)
    {
        LOG_ERROR(MEASUREMENT_CAP_LOG_TAG, "HAL_TIM_IC_Init failed");
        return HAL_ERROR;
    }

    /*
     * Alustetaan input capture rising-edge / direct TI -tyyliin.
     * Polariteettia vaihdetaan mittauksen aikana rekisteritasolla.
     */
    input_capture_config.ICPolarity = TIM_INPUTCHANNELPOLARITY_RISING;
    input_capture_config.ICSelection = TIM_ICSELECTION_DIRECTTI;
    input_capture_config.ICPrescaler = TIM_ICPSC_DIV1;
    input_capture_config.ICFilter = 0U;

    if (HAL_TIM_IC_ConfigChannel(
            &g_measurement_cap_htim2,
            &input_capture_config,
            MEASUREMENT_CAP_TIM_CHANNEL) != HAL_OK)
    {
        LOG_ERROR(MEASUREMENT_CAP_LOG_TAG, "HAL_TIM_IC_ConfigChannel failed");
        return HAL_ERROR;
    }

    if (HAL_TIM_IC_Start(&g_measurement_cap_htim2, MEASUREMENT_CAP_TIM_CHANNEL) != HAL_OK)
    {
        LOG_ERROR(MEASUREMENT_CAP_LOG_TAG, "HAL_TIM_IC_Start failed");
        return HAL_ERROR;
    }

    g_measurement_cap_initialized = 1U;

    LOG_INFO(
        MEASUREMENT_CAP_LOG_TAG,
        "TIM2 input capture ready on PA0 / TIM2_CH1");

    return HAL_OK;
}

uint32_t measurement_capacitance_pulse_in_ns(GPIO_PinState state)
{
    uint32_t timeout_deadline_ms;
    uint32_t tim_clock_mhz;
    uint32_t t_start;
    uint32_t t_end;

    if (measurement_capacitance_initialize() != HAL_OK)
    {
        return 0U;
    }

    timeout_deadline_ms = HAL_GetTick() + MEASUREMENT_CAP_TIMEOUT_MS;
    tim_clock_mhz = measurement_capacitance_get_tim_clock_hz() / 1000000U;

    if (tim_clock_mhz == 0U)
    {
        return 0U;
    }

    /*
     * 1) Synkronointi vastakkaiseen reunaan
     *
     * Jos halutaan mitata HIGH-pulssi:
     * - synkronoidaan ensin fallingiin
     * - jotta ei aloiteta kesken jo käynnissä olevaa HIGH-jaksoa
     *
     * Jos halutaan mitata LOW-pulssi:
     * - synkronoidaan ensin risingiin
     */
    if (state == GPIO_PIN_SET)
    {
        MEASUREMENT_CAP_TIM_INSTANCE->CCER |= MEASUREMENT_CAP_TIM_CCER_POL_BIT;
    }
    else
    {
        MEASUREMENT_CAP_TIM_INSTANCE->CCER &= ~MEASUREMENT_CAP_TIM_CCER_POL_BIT;
    }

    MEASUREMENT_CAP_TIM_INSTANCE->SR &= ~MEASUREMENT_CAP_TIM_CCIF_FLAG;

    while ((MEASUREMENT_CAP_TIM_INSTANCE->SR & MEASUREMENT_CAP_TIM_CCIF_FLAG) == 0U)
    {
        if (HAL_GetTick() > timeout_deadline_ms)
        {
            return 0U;
        }
    }

    /*
     * 2) Aloitusreuna
     *
     * HIGH-pulssille:
     * - aloitus on rising
     *
     * LOW-pulssille:
     * - aloitus on falling
     */
    if (state == GPIO_PIN_SET)
    {
        MEASUREMENT_CAP_TIM_INSTANCE->CCER &= ~MEASUREMENT_CAP_TIM_CCER_POL_BIT;
    }
    else
    {
        MEASUREMENT_CAP_TIM_INSTANCE->CCER |= MEASUREMENT_CAP_TIM_CCER_POL_BIT;
    }

    MEASUREMENT_CAP_TIM_INSTANCE->SR &= ~MEASUREMENT_CAP_TIM_CCIF_FLAG;

    while ((MEASUREMENT_CAP_TIM_INSTANCE->SR & MEASUREMENT_CAP_TIM_CCIF_FLAG) == 0U)
    {
        if (HAL_GetTick() > timeout_deadline_ms)
        {
            return 0U;
        }
    }

    t_start = MEASUREMENT_CAP_TIM_CCR_REG(MEASUREMENT_CAP_TIM_INSTANCE);

    /*
     * 3) Lopetusreuna
     *
     * HIGH-pulssille:
     * - lopetus on falling
     *
     * LOW-pulssille:
     * - lopetus on rising
     */
    if (state == GPIO_PIN_SET)
    {
        MEASUREMENT_CAP_TIM_INSTANCE->CCER |= MEASUREMENT_CAP_TIM_CCER_POL_BIT;
    }
    else
    {
        MEASUREMENT_CAP_TIM_INSTANCE->CCER &= ~MEASUREMENT_CAP_TIM_CCER_POL_BIT;
    }

    MEASUREMENT_CAP_TIM_INSTANCE->SR &= ~MEASUREMENT_CAP_TIM_CCIF_FLAG;

    while ((MEASUREMENT_CAP_TIM_INSTANCE->SR & MEASUREMENT_CAP_TIM_CCIF_FLAG) == 0U)
    {
        if (HAL_GetTick() > timeout_deadline_ms)
        {
            return 0U;
        }
    }

    t_end = MEASUREMENT_CAP_TIM_CCR_REG(MEASUREMENT_CAP_TIM_INSTANCE);

    /*
     * Tickit -> nanosekunnit
     */
    return (uint32_t)(((uint64_t)(t_end - t_start) * 1000ULL) / tim_clock_mhz);
}

HAL_StatusTypeDef measurement_capacitance_measure(
    measurement_capacitance_result_t *result)
{
    float values_pf[MEASUREMENT_CAP_MAX_SAMPLES];
    uint32_t t_high_0;
    uint32_t t_low_0;
    uint32_t period_ns;
    int requested_samples;
    int accepted_samples;
    int i;
    int trim;
    float sum_pf;
    float frequency_hz;

    if (result == NULL)
    {
        return HAL_ERROR;
    }

    /*
     * Nollataan tulosrakenne siististi.
     */
    result->pulse_high_ns = 0U;
    result->pulse_low_ns = 0U;
    result->frequency_hz = -1.0f;
    result->capacitance_pf = -1.0f;

    /*
     * Ensimmäinen karkea mittaus:
     * tällä päätetään myös montako lisänäytettä tarvitaan.
     */
    t_high_0 = measurement_capacitance_pulse_in_ns(GPIO_PIN_SET);
    t_low_0 = measurement_capacitance_pulse_in_ns(GPIO_PIN_RESET);

    if ((t_high_0 < MEASUREMENT_CAP_MIN_PULSE_NS) ||
        (t_low_0 < MEASUREMENT_CAP_MIN_PULSE_NS))
    {
        LOG_WARN(MEASUREMENT_CAP_LOG_TAG, "Initial pulse too short / invalid");
        return HAL_ERROR;
    }

    result->pulse_high_ns = t_high_0;
    result->pulse_low_ns = t_low_0;

    period_ns = t_high_0 + t_low_0;
    frequency_hz = 1.0e9f / (float)period_ns;

    result->frequency_hz = frequency_hz;

    /*
     * Sama adaptiivinen idea kuin referenssissä:
     * hitaalle signaalille vähemmän näytteitä, nopealle enemmän.
     */
    if ((period_ns / 1000000UL) > 200UL)
    {
        requested_samples = 3;
    }
    else if ((period_ns / 1000000UL) > 50UL)
    {
        requested_samples = 6;
    }
    else
    {
        requested_samples = MEASUREMENT_CAP_MAX_SAMPLES;
    }

    accepted_samples = 0;
    values_pf[accepted_samples++] = measurement_capacitance_period_ns_to_pf(period_ns);

    /*
     * Lisänäytteet.
     */
    for (i = 1; i < requested_samples; ++i)
    {
        uint32_t t_high = measurement_capacitance_pulse_in_ns(GPIO_PIN_SET);
        uint32_t t_low = measurement_capacitance_pulse_in_ns(GPIO_PIN_RESET);
        uint32_t p_ns = t_high + t_low;
        float c_pf;

        if ((t_high < MEASUREMENT_CAP_MIN_PULSE_NS) ||
            (t_low < MEASUREMENT_CAP_MIN_PULSE_NS))
        {
            continue;
        }

        c_pf = measurement_capacitance_period_ns_to_pf(p_ns);

        if (c_pf <= 0.0f)
        {
            continue;
        }

        values_pf[accepted_samples++] = c_pf;
    }

    if (accepted_samples < 2)
    {
        LOG_WARN(MEASUREMENT_CAP_LOG_TAG, "Too few valid cap samples");
        return HAL_ERROR;
    }

    /*
     * Lajitellaan ja trimmataan ääripäitä pois.
     */
    measurement_capacitance_sort_float_array(values_pf, accepted_samples);

    trim = (accepted_samples >= 4) ? (accepted_samples / 4) : 0;
    sum_pf = 0.0f;

    for (i = trim; i < (accepted_samples - trim); ++i)
    {
        sum_pf += values_pf[i];
    }

    result->capacitance_pf =
        sum_pf / (float)(accepted_samples - (2 * trim));

    LOG_INFO(
        MEASUREMENT_CAP_LOG_TAG,
        "CAP: tH=%lu ns tL=%lu ns f=%.2f Hz C=%.2f pF",
        (unsigned long)result->pulse_high_ns,
        (unsigned long)result->pulse_low_ns,
        (double)result->frequency_hz,
        (double)result->capacitance_pf);

    return HAL_OK;
}