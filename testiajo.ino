#include "debug_logging.h"

extern "C" {
#include "board_adc_hal.h"
#include "measurement_adc.h"
#include "measurement_capacitance.h"
#include "measurement_topology.h"
#include "measurement_core.h"
}

/*
 * ============================================================================
 * Testiajo:
 *   1) resistiivinen baseline
 *   2) Kelvin probe
 *   3) capacitance probe
 *   4) diode probe
 * ============================================================================
 *
 * Kytkinmuistilistat:
 *
 * Resistive baseline:
 *   K1 = oikea
 *   K2 = vasen
 *
 * Kelvin:
 *   K1 = oikea
 *   K2 = oikea
 *
 * Capacitance:
 *   K1 = vasen
 *   K2 = ei merkitystä
 *
 * Diode:
 *   K1 = oikea
 *   K2 = vasen
 *
 * Tärkeä DUT1-yhteenveto:
 *
 *   SAFE                  -> DUT1 Hi-Z
 *   RESISTANCE_HIGH_RANGE -> DUT1 LOW
 *   RESISTANCE_LOW_RANGE  -> DUT1 LOW
 *   KELVIN                -> DUT1 HIGH
 *   CAPACITANCE           -> DUT1 LOW
 *   DIODE_*               -> DUT1 LOW   (tässä ensimmäisessä toteutuksessa)
 *
 * Tässä vaiheessa:
 * - ei tehdä vielä lopullista koko järjestelmän komponenttitunnistusta
 * - kelvin / cap / diode ovat erillisiä probe-vaiheita
 */

#define TEST_LOG_TAG               "TEST"
#define TEST_LOOP_DELAY_MS       1500U

/*
 * Yksi globaali tulosrakenne baseline-ajolle, Kelvinille, capille ja diodille.
 */
static measurement_data_t g_measurement_data;

/*
 * ============================================================================
 * Helperit baseline-tuloksen lokitukseen
 * ============================================================================
 */

static void test_log_baseline_result(const measurement_data_t *data)
{
    component_type_t detected_type;

    if (data == nullptr)
    {
        LOG_ERROR(TEST_LOG_TAG, "Result pointer is NULL");
        return;
    }

    detected_type = measurement_core_detect_component(data);

    LOG_INFO(
        TEST_LOG_TAG,
        "Detected component type -> %s",
        measurement_core_get_component_type_name(detected_type));

    LOG_DEBUG(
        TEST_LOG_TAG,
        "Flags: high_valid=%u low_valid=%u",
        data->res_high_resistance_valid ? 1U : 0U,
        data->res_low_resistance_valid ? 1U : 0U);

    if (data->res_high_resistance_valid)
    {
        LOG_DEBUG(
            TEST_LOG_TAG,
            "Estimated RES_HIGH resistance -> %.1f ohm",
            (double)data->res_high_estimated_resistance_ohms);
    }

    if (data->res_low_resistance_valid)
    {
        LOG_DEBUG(
            TEST_LOG_TAG,
            "Estimated RES_LOW resistance -> %.1f ohm",
            (double)data->res_low_estimated_resistance_ohms);
    }
}

/*
 * ============================================================================
 * Helperi Kelvin-tuloksen lokitukseen
 * ============================================================================
 */

static void test_log_kelvin_result(const measurement_data_t *data)
{
    if (data == nullptr)
    {
        LOG_ERROR(TEST_LOG_TAG, "Kelvin result pointer is NULL");
        return;
    }

    if (!data->kelvin_valid)
    {
        LOG_WARN(TEST_LOG_TAG, "Kelvin result is not valid");
        return;
    }

    if (data->kelvin_resistance_valid)
    {
        LOG_INFO(
            TEST_LOG_TAG,
            "Kelvin probe -> raw=%u voltage=%.5f V estimated=%.3f ohm",
            data->kelvin_adc_raw,
            (double)data->kelvin_voltage_v,
            (double)data->kelvin_estimated_resistance_ohms);
    }
    else
    {
        LOG_INFO(
            TEST_LOG_TAG,
            "Kelvin probe -> raw=%u voltage=%.5f V estimated=N/A",
            data->kelvin_adc_raw,
            (double)data->kelvin_voltage_v);
    }
}

/*
 * ============================================================================
 * Helperi cap-tuloksen lokitukseen
 * ============================================================================
 */

static void test_log_capacitance_result(const measurement_data_t *data)
{
    if (data == nullptr)
    {
        LOG_ERROR(TEST_LOG_TAG, "Cap result pointer is NULL");
        return;
    }

    if (!data->cap_valid)
    {
        LOG_WARN(TEST_LOG_TAG, "Capacitance result is not valid");
        return;
    }

    LOG_INFO(
        TEST_LOG_TAG,
        "Cap probe -> tH=%lu ns tL=%lu ns f=%.2f Hz raw=%.2f pF corrected=%.2f pF",
        (unsigned long)data->cap_pulse_high_ns,
        (unsigned long)data->cap_pulse_low_ns,
        (double)data->cap_frequency_hz,
        (double)data->cap_raw_pf,
        (double)data->cap_corrected_pf);
}

/*
 * ============================================================================
 * Helperi diode-tuloksen lokitukseen
 * ============================================================================
 */

static void test_log_diode_result(const measurement_data_t *data)
{
    if (data == nullptr)
    {
        LOG_ERROR(TEST_LOG_TAG, "Diode result pointer is NULL");
        return;
    }

    if (!data->diode_valid)
    {
        LOG_WARN(TEST_LOG_TAG, "Diode result is not valid");
        return;
    }

    LOG_INFO(
        TEST_LOG_TAG,
        "Diode probe -> type=%s",
        measurement_core_get_component_type_name(data->diode_probe_type));

    LOG_INFO(
        TEST_LOG_TAG,
        "Diode voltages -> FWD_LOW=%.4f FWD_HIGH=%.4f REV_LOW=%.4f REV_HIGH=%.4f",
        (double)data->diode_forward_low_voltage_v,
        (double)data->diode_forward_high_voltage_v,
        (double)data->diode_reverse_low_voltage_v,
        (double)data->diode_reverse_high_voltage_v);

    LOG_DEBUG(
        TEST_LOG_TAG,
        "Diode nonlinearities -> dF=%.4f dR=%.4f ratio=%.3f",
        (double)data->diode_nonlinearity_forward_v,
        (double)data->diode_nonlinearity_reverse_v,
        (double)data->diode_asymmetry_ratio);
}

/*
 * ============================================================================
 * setup()
 * ============================================================================
 */

void setup()
{
    Serial.begin(115200);
    delay(1000);

    LOG_INFO(TEST_LOG_TAG, "BOOT -> baseline + kelvin + capacitance + diode test");

    /*
     * ADC HAL.
     */
    if (board_adc_hal_initialize() != HAL_OK)
    {
        LOG_ERROR(TEST_LOG_TAG, "board_adc_hal_initialize failed");
        while (1) { }
    }

    /*
     * ADC measurement layer.
     */
    if (measurement_adc_initialize() != HAL_OK)
    {
        LOG_ERROR(TEST_LOG_TAG, "measurement_adc_initialize failed");
        while (1) { }
    }

    /*
     * TIM2-based capacitance measurement init.
     *
     * Tässä toteutuksessa:
     *   TIM2_CH1 / PA0
     */
    if (measurement_capacitance_initialize() != HAL_OK)
    {
        LOG_ERROR(TEST_LOG_TAG, "measurement_capacitance_initialize failed");
        while (1) { }
    }

    /*
     * Topologia turvalliseen alkutilaan.
     */
    measurement_topology_initialize_safe_state();

    LOG_INFO(TEST_LOG_TAG, "INIT -> ready");
    LOG_INFO(TEST_LOG_TAG, "Switch reminder (baseline): K1 = right, K2 = left");
    LOG_INFO(TEST_LOG_TAG, "Switch reminder (kelvin):   K1 = right, K2 = right");
    LOG_INFO(TEST_LOG_TAG, "Switch reminder (cap):      K1 = left,  K2 = don't care");
    LOG_INFO(TEST_LOG_TAG, "Switch reminder (diode):    K1 = right, K2 = left");
}

/*
 * ============================================================================
 * loop()
 * ============================================================================
 */

void loop()
{
    HAL_StatusTypeDef status;

    /*
     * =========================
     * 1) Resistiivinen baseline
     * =========================
     *
     * Kytkimet:
     * - K1 = oikea
     * - K2 = vasen
     */
    LOG_INFO(TEST_LOG_TAG, "Running resistor baseline");

    status = measurement_core_run_resistor_baseline(&g_measurement_data);
    if (status != HAL_OK)
    {
        LOG_ERROR(TEST_LOG_TAG, "measurement_core_run_resistor_baseline failed");
        (void)measurement_topology_apply(MEASUREMENT_TOPOLOGY_SAFE);

        delay(TEST_LOOP_DELAY_MS);
        return;
    }

    test_log_baseline_result(&g_measurement_data);

    /*
     * =========================
     * 2) Kelvin probe
     * =========================
     *
     * Kytkimet:
     * - K1 = oikea
     * - K2 = oikea
     */
    LOG_INFO(TEST_LOG_TAG, "Running Kelvin probe");

    status = measurement_core_run_kelvin_probe(&g_measurement_data);
    if (status != HAL_OK)
    {
        LOG_ERROR(TEST_LOG_TAG, "measurement_core_run_kelvin_probe failed");
        (void)measurement_topology_apply(MEASUREMENT_TOPOLOGY_SAFE);

        delay(TEST_LOOP_DELAY_MS);
        return;
    }

    test_log_kelvin_result(&g_measurement_data);

    /*
     * =========================
     * 3) Capacitance probe
     * =========================
     *
     * Kytkimet:
     * - K1 = vasen
     * - K2 = ei merkitystä
     */
    LOG_INFO(TEST_LOG_TAG, "Running capacitance probe");

    status = measurement_core_run_capacitance_probe(&g_measurement_data);
    if (status != HAL_OK)
    {
        LOG_ERROR(TEST_LOG_TAG, "measurement_core_run_capacitance_probe failed");
        (void)measurement_topology_apply(MEASUREMENT_TOPOLOGY_SAFE);

        delay(TEST_LOOP_DELAY_MS);
        return;
    }

    test_log_capacitance_result(&g_measurement_data);

    /*
     * =========================
     * 4) Diode probe
     * =========================
     *
     * Kytkimet:
     * - K1 = oikea
     * - K2 = vasen
     *
     * Tämä käyttää VDIVS-polun ADC-lukua sekä diode direction -pinniä.
     */
    LOG_INFO(TEST_LOG_TAG, "Running diode probe");

    status = measurement_core_run_diode_probe(&g_measurement_data);
    if (status != HAL_OK)
    {
        LOG_ERROR(TEST_LOG_TAG, "measurement_core_run_diode_probe failed");
        (void)measurement_topology_apply(MEASUREMENT_TOPOLOGY_SAFE);

        delay(TEST_LOOP_DELAY_MS);
        return;
    }

    test_log_diode_result(&g_measurement_data);

    /*
     * =========================
     * 5) Kierroksen lopetus
     * =========================
     */
    LOG_INFO(TEST_LOG_TAG, "----------------------------------------");
    delay(TEST_LOOP_DELAY_MS);
}
