#include "board_dac_hal.h"

#include "debug_logging.h"

/*
 * ============================================================================
 * DAC1 / PA4 ref-ulostulon HAL-kerros
 * ============================================================================
 *
 * Tämä tiedosto toteuttaa projektin tämänhetkisen analogisen referenssi-
 * ulostulon matalan tason hallinnan.
 *
 * Fyysinen pinout:
 *
 *   PA4 -> DAC1_OUT1 -> REF_1V_DAC1_OUT1 / CC_REF
 *
 * Tavoite tässä vaiheessa:
 * - saada DAC1 käyttöön vakaasti ja selkeästi
 * - pystyä asettamaan esimerkiksi looginen 1.000 V referenssi
 * - pitää toteutus erillään measurement_core- ja topology-kerroksista
 *
 * Miksi oma tiedosto:
 * - analoginen referenssi on oma lohkonsa, eri vastuulla kuin ADC
 * - myöhempi low-power / sleep / wake -hallinta on helpompi tehdä
 * - DAC voidaan testata erikseen ennen kuin sitä sidotaan mittauslogiikkaan
 *
 * Huom kalibroinnista:
 * - Käytännön mittauksessa havaittiin, että pyydetty 1.000 V tuottaa noin
 *   0.993 V fyysisen ulostulon.
 * - Tätä kompensoidaan tässä tiedostossa yksinkertaisella gain-korjauksella,
 *   jotta ulkoinen käyttö voi edelleen pyytää loogisesti "1.000 V".
 * - Korjauskerroin on erotettu omaksi define-makrokseen eikä jätetty
 *   hajalleen taikalukuna koodiin.
 */

#define BOARD_DAC_LOG_TAG                             "DAC"

/*
 * ============================================================================
 * Projektikohtaiset DAC-asetukset
 * ============================================================================
 *
 * DAC-kanava:
 * - käytössä DAC1 channel 1 -> PA4
 *
 * 12-bit skaala:
 * - DAC-raaka-arvon alue on 0 ... 4095
 *
 * Startup-oletus:
 * - initin jälkeen ulostulo asetetaan 0.0 V:iin
 *
 * Pyöristys:
 * - raakaarvon muodostuksessa käytetään +0.5f pyöristystä lähimpään
 *   kokonaislukuun
 */

#define BOARD_DAC_CHANNEL                             DAC_CHANNEL_1
#define BOARD_DAC_MAX_RAW_12BIT                       4095U
#define BOARD_DAC_STARTUP_VOLTAGE_V                   0.0f
#define BOARD_DAC_RAW_ROUNDING_OFFSET                 0.5f

/*
 * ============================================================================
 * Sisäinen tila
 * ============================================================================
 *
 * Viimeksi asetetut arvot pidetään muistissa debugia ja tilaseurantaa varten.
 *
 * Huom:
 * - raw-arvo tallennetaan sellaisena kuin se oikeasti kirjoitettiin DAC:lle
 * - jännite tallennetaan "loogisena pyydettynä jännitteenä", ei sisäisesti
 *   korjattuna arvona
 */

DAC_HandleTypeDef hdac1;

static uint16_t g_board_dac_last_raw_12bit = 0U;
static float g_board_dac_last_voltage_v = 0.0f;

/*
 * ============================================================================
 * Sisäiset helperit
 * ============================================================================
 */

/*
 * Alustaa DAC1:n tarvitseman GPIO-pinnin.
 *
 * PA4 asetetaan analog-tilaan ilman vetoja.
 *
 * Huom:
 * DAC-ulkolähtöä varten pinniä ei konfiguroida alternate function -tilaan,
 * vaan analog-tilaan DAC-yksikön käyttöä varten.
 */
static void board_dac_hal_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;

    HAL_GPIO_Init(GPIOA, &gpio);
}

/*
 * Rajaa jännitearvon välille [0, reference_voltage_v].
 *
 * Jos referenssijännite on epävalidi (<= 0), palautetaan 0.0 V.
 */
static float board_dac_hal_clamp_voltage(
    float voltage_v,
    float reference_voltage_v)
{
    if (reference_voltage_v <= 0.0f)
    {
        return 0.0f;
    }

    if (voltage_v < 0.0f)
    {
        return 0.0f;
    }

    if (voltage_v > reference_voltage_v)
    {
        return reference_voltage_v;
    }

    return voltage_v;
}

/*
 * Soveltaa projektikohtaista gain-korjausta pyydettyyn jännitteeseen.
 *
 * Esimerkki:
 * - käyttäjä pyytää 1.000 V
 * - sisäinen korjattu pyyntö on noin 1.007 V
 *
 * Korjattu arvo rajataan myöhemmin vielä turvallisesti referenssijännitteen
 * alueelle clamp-helperillä.
 */
static float board_dac_hal_apply_gain_correction(float requested_voltage_v)
{
    return requested_voltage_v * BOARD_DAC_OUTPUT_GAIN_CORRECTION;
}

/*
 * Muuntaa jännitteen 12-bittiseksi DAC-raakaarvoksi.
 *
 * Käytetään pyöristystä lähimpään kokonaislukuun.
 */
static uint16_t board_dac_hal_voltage_to_raw_12bit(
    float voltage_v,
    float reference_voltage_v)
{
    float clamped_voltage;
    float scaled_value;
    uint32_t raw_value;

    if (reference_voltage_v <= 0.0f)
    {
        return 0U;
    }

    clamped_voltage = board_dac_hal_clamp_voltage(
        voltage_v,
        reference_voltage_v);

    scaled_value =
        (clamped_voltage * (float)BOARD_DAC_MAX_RAW_12BIT) /
        reference_voltage_v;

    /*
     * Pyöristys lähimpään kokonaislukuun.
     */
    raw_value = (uint32_t)(scaled_value + BOARD_DAC_RAW_ROUNDING_OFFSET);

    if (raw_value > (uint32_t)BOARD_DAC_MAX_RAW_12BIT)
    {
        raw_value = (uint32_t)BOARD_DAC_MAX_RAW_12BIT;
    }

    return (uint16_t)raw_value;
}

/*
 * ============================================================================
 * Julkinen API
 * ============================================================================
 */

HAL_StatusTypeDef board_dac_hal_initialize(void)
{
    HAL_StatusTypeDef status;
    DAC_ChannelConfTypeDef dac_config = {0};

    /*
     * Kellot käyttöön:
     * - GPIOA PA4:lle
     * - DAC1:lle
     */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_DAC1_CLK_ENABLE();

    board_dac_hal_gpio_init();

    hdac1.Instance = DAC1;

    status = HAL_DAC_Init(&hdac1);
    if (status != HAL_OK)
    {
        LOG_ERROR(BOARD_DAC_LOG_TAG, "HAL_DAC_Init failed");
        return status;
    }

    /*
     * DAC-kanava 1 / PA4
     *
     * Tässä vaiheessa käytetään ohjelmallisesti ohjattua jatkuvaa ulostuloa:
     * - ei triggeriä
     * - output buffer päällä
     * - sample & hold pois
     */
    dac_config.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
    dac_config.DAC_Trigger = DAC_TRIGGER_NONE;
    dac_config.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
    dac_config.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_DISABLE;
    dac_config.DAC_UserTrimming = DAC_TRIMMING_FACTORY;

    status = HAL_DAC_ConfigChannel(&hdac1, &dac_config, BOARD_DAC_CHANNEL);
    if (status != HAL_OK)
    {
        LOG_ERROR(BOARD_DAC_LOG_TAG, "HAL_DAC_ConfigChannel failed");
        return status;
    }

    /*
     * Käynnistetään DAC-kanava.
     */
    status = HAL_DAC_Start(&hdac1, BOARD_DAC_CHANNEL);
    if (status != HAL_OK)
    {
        LOG_ERROR(BOARD_DAC_LOG_TAG, "HAL_DAC_Start failed");
        return status;
    }

    /*
     * Turvallinen oletus: 0 V ulos heti initin jälkeen.
     */
    status = board_dac_hal_set_voltage(
        BOARD_DAC_STARTUP_VOLTAGE_V,
        BOARD_DAC_DEFAULT_REFERENCE_VOLTAGE_V);

    if (status != HAL_OK)
    {
        LOG_ERROR(BOARD_DAC_LOG_TAG, "Startup voltage set failed");
        return status;
    }

    LOG_INFO(
        BOARD_DAC_LOG_TAG,
        "DAC initialized on PA4 / DAC1_OUT1 (startup=%.3f V, raw=%u)",
        (double)g_board_dac_last_voltage_v,
        (unsigned int)g_board_dac_last_raw_12bit);

    return HAL_OK;
}

HAL_StatusTypeDef board_dac_hal_deinitialize(void)
{
    HAL_StatusTypeDef status;

    status = HAL_DAC_Stop(&hdac1, BOARD_DAC_CHANNEL);
    if ((status != HAL_OK) && (status != HAL_ERROR))
    {
        LOG_ERROR(BOARD_DAC_LOG_TAG, "HAL_DAC_Stop failed");
        return status;
    }

    status = HAL_DAC_DeInit(&hdac1);
    if (status != HAL_OK)
    {
        LOG_ERROR(BOARD_DAC_LOG_TAG, "HAL_DAC_DeInit failed");
        return status;
    }

    __HAL_RCC_DAC1_CLK_DISABLE();

    /*
     * Vapautetaan PA4.
     */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_4);

    g_board_dac_last_raw_12bit = 0U;
    g_board_dac_last_voltage_v = 0.0f;

    LOG_INFO(BOARD_DAC_LOG_TAG, "DAC deinitialized");

    return HAL_OK;
}

HAL_StatusTypeDef board_dac_hal_set_raw_12bit(uint16_t raw_value)
{
    HAL_StatusTypeDef status;
    uint32_t limited_raw;

    limited_raw = (uint32_t)raw_value;
    if (limited_raw > (uint32_t)BOARD_DAC_MAX_RAW_12BIT)
    {
        limited_raw = (uint32_t)BOARD_DAC_MAX_RAW_12BIT;
    }

    status = HAL_DAC_SetValue(
        &hdac1,
        BOARD_DAC_CHANNEL,
        DAC_ALIGN_12B_R,
        limited_raw);

    if (status != HAL_OK)
    {
        LOG_ERROR(BOARD_DAC_LOG_TAG, "HAL_DAC_SetValue failed");
        return status;
    }

    g_board_dac_last_raw_12bit = (uint16_t)limited_raw;

    LOG_DEBUG(
        BOARD_DAC_LOG_TAG,
        "Set raw=%u",
        (unsigned int)g_board_dac_last_raw_12bit);

    return HAL_OK;
}

HAL_StatusTypeDef board_dac_hal_set_voltage(
    float voltage_v,
    float reference_voltage_v)
{
    HAL_StatusTypeDef status;
    float effective_reference_voltage_v;
    float corrected_voltage_v;
    float clamped_corrected_voltage_v;
    uint16_t raw_value;

    /*
     * Jos kutsuja antaa epävalidin referenssin, käytetään projektin oletusta.
     *
     * Tämä tekee rajapinnasta hieman robustimman ja poistaa turhan tarpeen
     * ripotella 3.3f-tasoa kaikkialle kutsukoodiin.
     */
    effective_reference_voltage_v =
        (reference_voltage_v > 0.0f) ?
        reference_voltage_v :
        BOARD_DAC_DEFAULT_REFERENCE_VOLTAGE_V;

    /*
     * Sovelletaan yksipistekalibroinnin gain-korjausta.
     */
    corrected_voltage_v = board_dac_hal_apply_gain_correction(voltage_v);

    /*
     * Rajataan korjattu jännite turvallisesti DAC:n käyttöalueelle.
     */
    clamped_corrected_voltage_v = board_dac_hal_clamp_voltage(
        corrected_voltage_v,
        effective_reference_voltage_v);

    raw_value = board_dac_hal_voltage_to_raw_12bit(
        clamped_corrected_voltage_v,
        effective_reference_voltage_v);

    status = board_dac_hal_set_raw_12bit(raw_value);
    if (status != HAL_OK)
    {
        return status;
    }

    /*
     * Talletetaan "looginen" pyydetty jännite getteriä varten.
     *
     * Näin ulkoinen koodi näkee edelleen esimerkiksi 1.000 V pyynnön,
     * vaikka sisäinen DAC-kirjoitus on hieman korjattu arvo.
     */
    g_board_dac_last_voltage_v = board_dac_hal_clamp_voltage(
        voltage_v,
        effective_reference_voltage_v);

    LOG_INFO(
        BOARD_DAC_LOG_TAG,
        "Set voltage request=%.3f V corrected=%.3f V ref=%.3f V raw=%u",
        (double)g_board_dac_last_voltage_v,
        (double)clamped_corrected_voltage_v,
        (double)effective_reference_voltage_v,
        (unsigned int)g_board_dac_last_raw_12bit);

    return HAL_OK;
}

HAL_StatusTypeDef board_dac_hal_set_kelvin_reference_default(void)
{
    return board_dac_hal_set_voltage(
        BOARD_DAC_KELVIN_REFERENCE_TARGET_VOLTAGE_V,
        BOARD_DAC_DEFAULT_REFERENCE_VOLTAGE_V);
}

uint16_t board_dac_hal_get_last_raw_12bit(void)
{
    return g_board_dac_last_raw_12bit;
}

float board_dac_hal_get_last_voltage(void)
{
    return g_board_dac_last_voltage_v;
}