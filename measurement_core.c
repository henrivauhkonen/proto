#include "measurement_core.h"

#include <math.h>
#include <string.h>

#include "debug_logging.h"
#include "measurement_adc.h"
#include "measurement_capacitance.h"
#include "measurement_topology.h"

#define MEASUREMENT_CORE_LOG_TAG                         "CORE"

#define MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS       20U
#define MEASUREMENT_CORE_MANUAL_SWITCH_DELAY_MS       3000U

/*
 * ADC:n jänniteskaalan referenssi.
 *
 * Huom:
 * Tämä EI ole Kelvin-haaran CC_REF, vaan ADC:n digitaalisen muunnoksen
 * skaalausreferenssi.
 */
#define MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V         3.3f

/*
 * ============================================================================
 * VDIVS-haarojen vastusarvot nykyisen suunnitelman mukaisesti
 * ============================================================================
 *
 * LOW:
 *   820 Ω
 *
 * MID:
 *   56 kΩ
 *
 * HIGH:
 *   470 kΩ
 *
 * Huom:
 * LOW-haara toimii myös diodimittauksessa etuvastuksena.
 */
#define MEASUREMENT_CORE_LOW_RANGE_TOP_RESISTOR_OHMS       820.0f
#define MEASUREMENT_CORE_MID_RANGE_TOP_RESISTOR_OHMS     56000.0f
#define MEASUREMENT_CORE_HIGH_RANGE_TOP_RESISTOR_OHMS   470000.0f

/*
 * Yleiset jakajamallin validiteettirajat VDIVS-baselinelle.
 */
#define MEASUREMENT_CORE_MIN_VALID_DIVIDER_VOLTAGE_V        0.001f
#define MEASUREMENT_CORE_MAX_VALID_MARGIN_V                 0.001f

/*
 * ADC-keskiarvoistus:
 * - hylätään muutama alkuarvo
 * - pidetään useampi näyte
 */
#define MEASUREMENT_CORE_DISCARD_SAMPLES                    2U
#define MEASUREMENT_CORE_KEEP_SAMPLES                       8U

/*
 * Open-päätöksen alustava marginaali.
 *
 * Huom:
 * Tätä heuristiikkaa hiotaan myöhemmin, mutta debug-runkona tämä on edelleen
 * hyödyllinen.
 */
#define MEASUREMENT_CORE_OPEN_MARGIN_V                     0.050f

/*
 * ============================================================================
 * Kelvin-mittauksen oikea mittausmalli
 * ============================================================================
 *
 * Tässä protoversiossa Kelvin-haara EI ole tavallinen jännitejakaja.
 *
 * Sen sijaan vasemman puolen U1 + M1 + Rsense muodostavat transistoriohjatun
 * virtalähdehaaran:
 *
 * - CC_REF = 1.0 V toimii ohjausreferenssinä
 * - RREF1 = 47 Ω toimii virtareferenssivastuksena
 * - DUT:n läpi kulkee noin vakio testivirta
 *
 * Nimellinen testivirta:
 *
 *   I_TEST_NOM ≈ CC_REF / RREF1
 *              ≈ 1.0 V / 47 Ω
 *              ≈ 21.28 mA
 *
 * Oikealla puolella oleva measurement amplifier mittaa sense+- ja sense--solmujen
 * erotusjännitettä ja vahvistaa sen ennen ADC:tä.
 *
 * Kaavion ideaalinen vastusverkko:
 *   R1 = 1 kΩ
 *   R2 = 1 kΩ
 *   R3 = 4.7 kΩ
 *   R4 = 4.7 kΩ
 *
 * antaa ideaalisen ensimmäisen kertaluvun differentiaalivahvistuksen:
 *
 *   G_MEAS ≈ 4.7k / 1k ≈ 4.7
 *
 * Tällöin:
 *
 *   V_ADC ≈ G_MEAS * V_DUT
 *   V_DUT = I_TEST_NOM * R_DUT
 *
 * jolloin:
 *
 *   R_DUT ≈ V_ADC / (G_MEAS * I_TEST_NOM)
 *
 * Huom:
 * Tämä on edelleen prototason ensimmäisen kertaluvun malli. Lopulliseen
 * tarkkuuteen vaikuttavat mm.
 * - virtalähdehaaran todellinen virta vs. nimellisvirta
 * - measurement amplifierin todellinen gain / offset
 * - common-mode-alue ja headroom
 * - johtimien / kytkimien resistanssit
 * - komponenttitoleranssit
 *
 * Mutta tämä malli on selvästi oikeampi kuin aiempi versio, jossa
 * measurement amplifierin gain jäi laskennasta pois ja Kelvin-arviot
 * menivät liian suuriksi.
 */
#define MEASUREMENT_CORE_KELVIN_CC_REF_VOLTAGE_V           1.0f
#define MEASUREMENT_CORE_KELVIN_RREF1_OHMS                47.0f
#define MEASUREMENT_CORE_KELVIN_MEAS_GAIN_V_PER_V          4.7f

/*
 * Nimellinen Kelvin-testivirta.
 */
#define MEASUREMENT_CORE_KELVIN_NOMINAL_TEST_CURRENT_A \
    (MEASUREMENT_CORE_KELVIN_CC_REF_VOLTAGE_V / MEASUREMENT_CORE_KELVIN_RREF1_OHMS)

/*
 * Kelvin-polun ideaalinen siirtokerroin:
 *
 *   V_ADC ≈ (G_MEAS * I_TEST_NOM) * R_DUT
 *
 * Tämä erotetaan omaksi makrokseen, jotta laskenta pysyy selkeänä
 * eikä koodiin jää "taikanumeroita".
 */
#define MEASUREMENT_CORE_KELVIN_TRANSFER_V_PER_OHM \
    (MEASUREMENT_CORE_KELVIN_MEAS_GAIN_V_PER_V * MEASUREMENT_CORE_KELVIN_NOMINAL_TEST_CURRENT_A)

/*
 * Mahdollinen Kelvin-offset ADC-ulostulossa.
 *
 * Tämä jätetään nyt nollaan, mutta on hyödyllinen myöhempää bench-kalibrointia
 * varten, jos measurement amplifierissa tai koko signaaliketjussa havaitaan
 * systemaattinen nollatasosiirtymä.
 */
#define MEASUREMENT_CORE_KELVIN_ADC_OFFSET_V               0.0f

/*
 * Kelvin-lukeman validiteettirajat.
 *
 * Tässä mallissa ADC mittaa measurement amplifierin ulostuloa.
 *
 * Ideaalisesti skaalakerroin on noin:
 *
 *   V_ADC ≈ 0.1 V / ohm
 *
 * jolloin karkea intuitio on:
 *
 *   0.01 V -> 0.1 Ω
 *   0.02 V -> 0.2 Ω
 *   0.10 V -> 1 Ω
 *   1.00 V -> 10 Ω
 *   2.00 V -> 20 Ω
 *   3.00 V -> 30 Ω
 *
 * Siksi:
 * - hyvin pieni jännitealue on offset-, johdinresistanssi- ja kohinadominoitu
 * - lähellä ADC:n ylärajaa ollaan jo suuren vastuksen / amplifier headroomin
 *   tai virtalähdehaaran compliance-rajan lähellä
 */
#define MEASUREMENT_CORE_KELVIN_MIN_VALID_VOLTAGE_V         0.001f
#define MEASUREMENT_CORE_KELVIN_MAX_VALID_VOLTAGE_V         3.100f
#define MEASUREMENT_CORE_KELVIN_LOW_CONFIDENCE_VOLTAGE_V    0.020f
#define MEASUREMENT_CORE_KELVIN_NEAR_ADC_RAIL_MARGIN_V      0.100f

/*
 * Open-slot hajakapasitanssi.
 */
#define MEASUREMENT_CORE_CAP_OPEN_OFFSET_PF                25.75f

/*
 * Diodiproben kynnysarvot.
 *
 * Nämä on pidetty saman hengen mukaisina kuin aiemmassa rungossa, mutta
 * niitä kannattaa myöhemmin säätää uuden kolmialueisen VDIVS-verkon
 * bench-testien perusteella.
 *
 * Malli perustuu tällä hetkellä:
 * - forward vs reverse -polariteetin eroon
 * - erityisesti LOW-haaran käyttäytymiseen
 * - haarojen väliseen epälineaarisuuteen
 */
#define MEASUREMENT_CORE_DIODE_MAX_CONDUCTION_V             2.8f
#define MEASUREMENT_CORE_DIODE_RAIL_MARGIN_V                0.20f
#define MEASUREMENT_CORE_DIODE_LOW_MARGIN_V                 0.05f
#define MEASUREMENT_CORE_DIODE_RESISTOR_SUM_MARGIN_V        0.20f
#define MEASUREMENT_CORE_DIODE_NONLINEAR_MARGIN_V           0.05f
#define MEASUREMENT_CORE_DIODE_LED_NODE_THRESHOLD_V         1.15f
#define MEASUREMENT_CORE_DIODE_RATIO_EPSILON                0.001f

/*
 * ============================================================================
 * Sisäiset funktioprototyypit
 * ============================================================================
 */

static float measurement_core_convert_raw_to_voltage(uint16_t raw_value);

static bool measurement_core_estimate_resistance_from_divider(
    float vdivs_voltage,
    float top_resistor_ohms,
    float *result_ohms);

static bool measurement_core_estimate_resistance_from_kelvin_voltage(
    float kelvin_voltage,
    float *result_ohms);

static float measurement_core_apply_cap_open_offset(float raw_cap_pf);

static component_type_t measurement_core_classify_diode_probe(
    const measurement_data_t *data);

static float measurement_core_get_diode_conduction_voltage(
    const measurement_data_t *data);

static float measurement_core_get_max3(float a, float b, float c);

static float measurement_core_get_three_point_spread(
    float v1,
    float v2,
    float v3);

static HAL_StatusTypeDef measurement_core_apply_topology_and_log(
    measurement_topology_mode_t mode);

static HAL_StatusTypeDef measurement_core_read_vdivs_average(
    uint16_t *raw_value,
    float *voltage_value);

static HAL_StatusTypeDef measurement_core_read_kelvin_average(
    uint16_t *raw_value,
    float *voltage_value);

static HAL_StatusTypeDef measurement_core_measure_safe(measurement_data_t *data);
static HAL_StatusTypeDef measurement_core_measure_res_high(measurement_data_t *data);
static HAL_StatusTypeDef measurement_core_measure_res_mid(measurement_data_t *data);
static HAL_StatusTypeDef measurement_core_measure_res_low(measurement_data_t *data);
static HAL_StatusTypeDef measurement_core_measure_capacitance(measurement_data_t *data);
static HAL_StatusTypeDef measurement_core_measure_diode_mode(
    measurement_topology_mode_t mode,
    const char *label,
    float *voltage_result);

/*
 * ============================================================================
 * Perushelperit
 * ============================================================================
 */

static float measurement_core_convert_raw_to_voltage(uint16_t raw_value)
{
    return measurement_adc_convert_raw_to_voltage(
        raw_value,
        MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V);
}

static bool measurement_core_estimate_resistance_from_divider(
    float vdivs_voltage,
    float top_resistor_ohms,
    float *result_ohms)
{
    if (result_ohms == NULL)
    {
        return false;
    }

    if (vdivs_voltage <= MEASUREMENT_CORE_MIN_VALID_DIVIDER_VOLTAGE_V)
    {
        return false;
    }

    if (vdivs_voltage >=
        (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V - MEASUREMENT_CORE_MAX_VALID_MARGIN_V))
    {
        return false;
    }

    *result_ohms =
        (top_resistor_ohms * vdivs_voltage) /
        (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V - vdivs_voltage);

    return true;
}

static bool measurement_core_estimate_resistance_from_kelvin_voltage(
    float kelvin_voltage,
    float *result_ohms)
{
    float corrected_voltage;

    if (result_ohms == NULL)
    {
        return false;
    }

    if (kelvin_voltage <= MEASUREMENT_CORE_KELVIN_MIN_VALID_VOLTAGE_V)
    {
        return false;
    }

    if (kelvin_voltage >= MEASUREMENT_CORE_KELVIN_MAX_VALID_VOLTAGE_V)
    {
        return false;
    }

    corrected_voltage = kelvin_voltage - MEASUREMENT_CORE_KELVIN_ADC_OFFSET_V;
    if (corrected_voltage <= 0.0f)
    {
        return false;
    }

    *result_ohms =
        corrected_voltage / MEASUREMENT_CORE_KELVIN_TRANSFER_V_PER_OHM;

    return true;
}

static float measurement_core_apply_cap_open_offset(float raw_cap_pf)
{
    float corrected_pf;

    if (raw_cap_pf <= 0.0f)
    {
        return -1.0f;
    }

    corrected_pf = raw_cap_pf - MEASUREMENT_CORE_CAP_OPEN_OFFSET_PF;

    if (corrected_pf < 0.0f)
    {
        corrected_pf = 0.0f;
    }

    return corrected_pf;
}

static float measurement_core_get_max3(float a, float b, float c)
{
    float max_value = a;

    if (b > max_value)
    {
        max_value = b;
    }

    if (c > max_value)
    {
        max_value = c;
    }

    return max_value;
}

static float measurement_core_get_three_point_spread(
    float v1,
    float v2,
    float v3)
{
    const float d12 = fabsf(v1 - v2);
    const float d23 = fabsf(v2 - v3);
    const float d13 = fabsf(v1 - v3);

    return measurement_core_get_max3(d12, d23, d13);
}

/*
 * ============================================================================
 * Diodiproben helperit
 * ============================================================================
 */

static float measurement_core_get_diode_conduction_voltage(
    const measurement_data_t *data)
{
    float vf;
    float vr;
    int open_like_fwd;
    int open_like_rev;
    int low_like_fwd;
    int low_like_rev;

    if (data == NULL)
    {
        return -1.0f;
    }

    vf = data->diode_forward_low_voltage_v;
    vr = data->diode_reverse_low_voltage_v;

    open_like_fwd = (vf > (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V -
                           MEASUREMENT_CORE_DIODE_RAIL_MARGIN_V));
    open_like_rev = (vr > (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V -
                           MEASUREMENT_CORE_DIODE_RAIL_MARGIN_V));

    low_like_fwd = (vf < MEASUREMENT_CORE_DIODE_LOW_MARGIN_V);
    low_like_rev = (vr < MEASUREMENT_CORE_DIODE_LOW_MARGIN_V);

    if (!open_like_fwd && open_like_rev)
    {
        return vf;
    }

    if (!open_like_rev && open_like_fwd)
    {
        return vr;
    }

    if (low_like_fwd && !low_like_rev)
    {
        return vr;
    }

    if (low_like_rev && !low_like_fwd)
    {
        return vf;
    }

    return (vf < vr) ? vf : vr;
}

static component_type_t measurement_core_classify_diode_probe(
    const measurement_data_t *data)
{
    float vf;
    float vr;
    float v_cond;
    int open_like_fwd;
    int open_like_rev;
    int low_like_fwd;
    int low_like_rev;
    int resistor_like;
    int one_way_fwd;
    int one_way_rev;
    int clamp_like_fwd;
    int clamp_like_rev;
    int nonlinear_signature;

    if (data == NULL)
    {
        return COMPONENT_UNKNOWN;
    }

    vf = data->diode_forward_low_voltage_v;
    vr = data->diode_reverse_low_voltage_v;

    open_like_fwd = (vf > (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V -
                           MEASUREMENT_CORE_DIODE_RAIL_MARGIN_V));
    open_like_rev = (vr > (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V -
                           MEASUREMENT_CORE_DIODE_RAIL_MARGIN_V));

    low_like_fwd = (vf < MEASUREMENT_CORE_DIODE_LOW_MARGIN_V);
    low_like_rev = (vr < MEASUREMENT_CORE_DIODE_LOW_MARGIN_V);

    resistor_like =
        (fabsf((vf + vr) - MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V) <
         MEASUREMENT_CORE_DIODE_RESISTOR_SUM_MARGIN_V);

    one_way_fwd =
        (vf < MEASUREMENT_CORE_DIODE_MAX_CONDUCTION_V) &&
        open_like_rev;

    one_way_rev =
        (vr < MEASUREMENT_CORE_DIODE_MAX_CONDUCTION_V) &&
        open_like_fwd &&
        !low_like_rev;

    clamp_like_fwd =
        low_like_fwd &&
        !open_like_rev &&
        !resistor_like;

    clamp_like_rev =
        low_like_rev &&
        !open_like_fwd &&
        !resistor_like;

    nonlinear_signature =
        (data->diode_nonlinearity_forward_v > MEASUREMENT_CORE_DIODE_NONLINEAR_MARGIN_V) ||
        (data->diode_nonlinearity_reverse_v > MEASUREMENT_CORE_DIODE_NONLINEAR_MARGIN_V);

    if (open_like_fwd && low_like_rev)
    {
        return COMPONENT_OPEN;
    }

    if (open_like_rev && low_like_fwd)
    {
        return COMPONENT_OPEN;
    }

    v_cond = measurement_core_get_diode_conduction_voltage(data);

    if (one_way_fwd || one_way_rev || clamp_like_fwd || clamp_like_rev)
    {
        if (v_cond > MEASUREMENT_CORE_DIODE_LED_NODE_THRESHOLD_V)
        {
            return COMPONENT_LED;
        }
        else
        {
            return COMPONENT_DIODE;
        }
    }

    if (resistor_like)
    {
        return COMPONENT_RESISTOR;
    }

    if (nonlinear_signature)
    {
        if (v_cond > MEASUREMENT_CORE_DIODE_LED_NODE_THRESHOLD_V)
        {
            return COMPONENT_LED;
        }
        else
        {
            return COMPONENT_DIODE;
        }
    }

    return COMPONENT_UNKNOWN;
}

/*
 * ============================================================================
 * Topologian vaihto + ADC-helperit
 * ============================================================================
 */

static HAL_StatusTypeDef measurement_core_apply_topology_and_log(
    measurement_topology_mode_t mode)
{
    HAL_StatusTypeDef status;

    LOG_INFO(
        MEASUREMENT_CORE_LOG_TAG,
        "TOPO Request -> %s",
        measurement_topology_get_mode_name(mode));

    status = measurement_topology_apply(mode);
    if (status != HAL_OK)
    {
        LOG_ERROR(
            MEASUREMENT_CORE_LOG_TAG,
            "Topology apply FAILED: %s",
            measurement_topology_get_mode_name(mode));
        return status;
    }

    LOG_INFO(
        MEASUREMENT_CORE_LOG_TAG,
        "TOPO Active -> %s",
        measurement_topology_get_mode_name(
            measurement_topology_get_current_mode()));

    return HAL_OK;
}

static HAL_StatusTypeDef measurement_core_read_vdivs_average(
    uint16_t *raw_value,
    float *voltage_value)
{
    HAL_StatusTypeDef status;

    if ((raw_value == NULL) || (voltage_value == NULL))
    {
        return HAL_ERROR;
    }

    status = measurement_adc_read_average(
        MEASUREMENT_ADC_INPUT_VDIVS,
        MEASUREMENT_CORE_DISCARD_SAMPLES,
        MEASUREMENT_CORE_KEEP_SAMPLES,
        raw_value);

    if (status != HAL_OK)
    {
        return status;
    }

    *voltage_value = measurement_core_convert_raw_to_voltage(*raw_value);
    return HAL_OK;
}

static HAL_StatusTypeDef measurement_core_read_kelvin_average(
    uint16_t *raw_value,
    float *voltage_value)
{
    HAL_StatusTypeDef status;

    if ((raw_value == NULL) || (voltage_value == NULL))
    {
        return HAL_ERROR;
    }

    status = measurement_adc_read_average(
        MEASUREMENT_ADC_INPUT_KELVIN,
        MEASUREMENT_CORE_DISCARD_SAMPLES,
        MEASUREMENT_CORE_KEEP_SAMPLES,
        raw_value);

    if (status != HAL_OK)
    {
        return status;
    }

    *voltage_value = measurement_core_convert_raw_to_voltage(*raw_value);
    return HAL_OK;
}

/*
 * ============================================================================
 * Yksittäiset mittausvaiheet
 * ============================================================================
 */

static HAL_StatusTypeDef measurement_core_measure_safe(measurement_data_t *data)
{
    HAL_StatusTypeDef status;

    if (data == NULL)
    {
        return HAL_ERROR;
    }

    status = measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    status = measurement_core_read_vdivs_average(
        &data->safe_vdivs_adc_raw,
        &data->safe_voltage_v);

    if (status != HAL_OK)
    {
        LOG_ERROR(MEASUREMENT_CORE_LOG_TAG, "SAFE VDIVS read FAILED");
        return status;
    }

    LOG_DEBUG(
        MEASUREMENT_CORE_LOG_TAG,
        "SAFE: raw=%u V=%.4f",
        data->safe_vdivs_adc_raw,
        (double)data->safe_voltage_v);

    return HAL_OK;
}

static HAL_StatusTypeDef measurement_core_measure_res_high(measurement_data_t *data)
{
    HAL_StatusTypeDef status;

    if (data == NULL)
    {
        return HAL_ERROR;
    }

    status = measurement_core_apply_topology_and_log(
        MEASUREMENT_TOPOLOGY_RESISTANCE_HIGH_RANGE);
    if (status != HAL_OK)
    {
        return status;
    }

    LOG_INFO(MEASUREMENT_CORE_LOG_TAG, "WAIT Set switches for RES_HIGH");
    HAL_Delay(MEASUREMENT_CORE_MANUAL_SWITCH_DELAY_MS);
    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    status = measurement_core_read_vdivs_average(
        &data->res_high_vdivs_adc_raw,
        &data->res_high_voltage_v);

    if (status != HAL_OK)
    {
        LOG_ERROR(MEASUREMENT_CORE_LOG_TAG, "RES_HIGH VDIVS read FAILED");
        return status;
    }

    data->res_high_resistance_valid =
        measurement_core_estimate_resistance_from_divider(
            data->res_high_voltage_v,
            MEASUREMENT_CORE_HIGH_RANGE_TOP_RESISTOR_OHMS,
            &data->res_high_estimated_resistance_ohms);

    if (data->res_high_resistance_valid)
    {
        LOG_DEBUG(
            MEASUREMENT_CORE_LOG_TAG,
            "RES_HIGH: raw=%u V=%.4f R=%.1f ohm",
            data->res_high_vdivs_adc_raw,
            (double)data->res_high_voltage_v,
            (double)data->res_high_estimated_resistance_ohms);
    }
    else
    {
        LOG_DEBUG(
            MEASUREMENT_CORE_LOG_TAG,
            "RES_HIGH: raw=%u V=%.4f R=N/A",
            data->res_high_vdivs_adc_raw,
            (double)data->res_high_voltage_v);
    }

    return HAL_OK;
}

static HAL_StatusTypeDef measurement_core_measure_res_mid(measurement_data_t *data)
{
    HAL_StatusTypeDef status;

    if (data == NULL)
    {
        return HAL_ERROR;
    }

    status = measurement_core_apply_topology_and_log(
        MEASUREMENT_TOPOLOGY_RESISTANCE_MID_RANGE);
    if (status != HAL_OK)
    {
        return status;
    }

    LOG_INFO(MEASUREMENT_CORE_LOG_TAG, "WAIT Set switches for RES_MID");
    HAL_Delay(MEASUREMENT_CORE_MANUAL_SWITCH_DELAY_MS);
    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    status = measurement_core_read_vdivs_average(
        &data->res_mid_vdivs_adc_raw,
        &data->res_mid_voltage_v);

    if (status != HAL_OK)
    {
        LOG_ERROR(MEASUREMENT_CORE_LOG_TAG, "RES_MID VDIVS read FAILED");
        return status;
    }

    data->res_mid_resistance_valid =
        measurement_core_estimate_resistance_from_divider(
            data->res_mid_voltage_v,
            MEASUREMENT_CORE_MID_RANGE_TOP_RESISTOR_OHMS,
            &data->res_mid_estimated_resistance_ohms);

    if (data->res_mid_resistance_valid)
    {
        LOG_DEBUG(
            MEASUREMENT_CORE_LOG_TAG,
            "RES_MID: raw=%u V=%.4f R=%.1f ohm",
            data->res_mid_vdivs_adc_raw,
            (double)data->res_mid_voltage_v,
            (double)data->res_mid_estimated_resistance_ohms);
    }
    else
    {
        LOG_DEBUG(
            MEASUREMENT_CORE_LOG_TAG,
            "RES_MID: raw=%u V=%.4f R=N/A",
            data->res_mid_vdivs_adc_raw,
            (double)data->res_mid_voltage_v);
    }

    return HAL_OK;
}

static HAL_StatusTypeDef measurement_core_measure_res_low(measurement_data_t *data)
{
    HAL_StatusTypeDef status;

    if (data == NULL)
    {
        return HAL_ERROR;
    }

    status = measurement_core_apply_topology_and_log(
        MEASUREMENT_TOPOLOGY_RESISTANCE_LOW_RANGE);
    if (status != HAL_OK)
    {
        return status;
    }

    LOG_INFO(MEASUREMENT_CORE_LOG_TAG, "WAIT Set switches for RES_LOW");
    HAL_Delay(MEASUREMENT_CORE_MANUAL_SWITCH_DELAY_MS);
    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    status = measurement_core_read_vdivs_average(
        &data->res_low_vdivs_adc_raw,
        &data->res_low_voltage_v);

    if (status != HAL_OK)
    {
        LOG_ERROR(MEASUREMENT_CORE_LOG_TAG, "RES_LOW VDIVS read FAILED");
        return status;
    }

    data->res_low_resistance_valid =
        measurement_core_estimate_resistance_from_divider(
            data->res_low_voltage_v,
            MEASUREMENT_CORE_LOW_RANGE_TOP_RESISTOR_OHMS,
            &data->res_low_estimated_resistance_ohms);

    if (data->res_low_resistance_valid)
    {
        LOG_DEBUG(
            MEASUREMENT_CORE_LOG_TAG,
            "RES_LOW: raw=%u V=%.4f R=%.1f ohm",
            data->res_low_vdivs_adc_raw,
            (double)data->res_low_voltage_v,
            (double)data->res_low_estimated_resistance_ohms);
    }
    else
    {
        LOG_DEBUG(
            MEASUREMENT_CORE_LOG_TAG,
            "RES_LOW: raw=%u V=%.4f R=N/A",
            data->res_low_vdivs_adc_raw,
            (double)data->res_low_voltage_v);
    }

    return HAL_OK;
}

static HAL_StatusTypeDef measurement_core_measure_capacitance(measurement_data_t *data)
{
    HAL_StatusTypeDef status;
    measurement_capacitance_result_t cap_result;

    if (data == NULL)
    {
        return HAL_ERROR;
    }

    status = measurement_core_apply_topology_and_log(
        MEASUREMENT_TOPOLOGY_CAPACITANCE);
    if (status != HAL_OK)
    {
        return status;
    }

    LOG_INFO(MEASUREMENT_CORE_LOG_TAG, "WAIT Set switches for CAPACITANCE");
    HAL_Delay(MEASUREMENT_CORE_MANUAL_SWITCH_DELAY_MS);
    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    status = measurement_capacitance_measure(&cap_result);
    if (status != HAL_OK)
    {
        LOG_WARN(MEASUREMENT_CORE_LOG_TAG, "CAP measurement failed");

        data->cap_valid = false;
        data->cap_raw_pf = -1.0f;
        data->cap_corrected_pf = -1.0f;
        data->cap_frequency_hz = -1.0f;
        data->cap_pulse_high_ns = 0U;
        data->cap_pulse_low_ns = 0U;

        (void)measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
        return HAL_OK;
    }

    data->cap_pulse_high_ns = cap_result.pulse_high_ns;
    data->cap_pulse_low_ns = cap_result.pulse_low_ns;
    data->cap_frequency_hz = cap_result.frequency_hz;
    data->cap_raw_pf = cap_result.capacitance_pf;
    data->cap_corrected_pf = measurement_core_apply_cap_open_offset(cap_result.capacitance_pf);
    data->cap_valid = (cap_result.capacitance_pf > 0.0f);

    LOG_INFO(
        MEASUREMENT_CORE_LOG_TAG,
        "CAP: tH=%lu ns tL=%lu ns f=%.2f Hz raw=%.2f pF corrected=%.2f pF",
        (unsigned long)data->cap_pulse_high_ns,
        (unsigned long)data->cap_pulse_low_ns,
        (double)data->cap_frequency_hz,
        (double)data->cap_raw_pf,
        (double)data->cap_corrected_pf);

    status = measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    return HAL_OK;
}

static HAL_StatusTypeDef measurement_core_measure_diode_mode(
    measurement_topology_mode_t mode,
    const char *label,
    float *voltage_result)
{
    HAL_StatusTypeDef status;
    uint16_t raw_value = 0U;

    if ((label == NULL) || (voltage_result == NULL))
    {
        return HAL_ERROR;
    }

    status = measurement_core_apply_topology_and_log(mode);
    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    status = measurement_core_read_vdivs_average(&raw_value, voltage_result);
    if (status != HAL_OK)
    {
        LOG_ERROR(MEASUREMENT_CORE_LOG_TAG, "%s VDIVS read FAILED", label);
        return status;
    }

    LOG_INFO(
        MEASUREMENT_CORE_LOG_TAG,
        "%s: raw=%u V=%.5f",
        label,
        raw_value,
        (double)*voltage_result);

    return HAL_OK;
}

HAL_StatusTypeDef measurement_core_run_resistor_baseline(measurement_data_t *data)
{
    HAL_StatusTypeDef status;

    if (data == NULL)
    {
        return HAL_ERROR;
    }

    memset(data, 0, sizeof(*data));

    data->res_high_estimated_resistance_ohms = -1.0f;
    data->res_mid_estimated_resistance_ohms = -1.0f;
    data->res_low_estimated_resistance_ohms = -1.0f;
    data->kelvin_estimated_resistance_ohms = -1.0f;
    data->cap_raw_pf = -1.0f;
    data->cap_corrected_pf = -1.0f;
    data->cap_frequency_hz = -1.0f;

    data->diode_forward_low_voltage_v = -1.0f;
    data->diode_forward_mid_voltage_v = -1.0f;
    data->diode_forward_high_voltage_v = -1.0f;
    data->diode_reverse_low_voltage_v = -1.0f;
    data->diode_reverse_mid_voltage_v = -1.0f;
    data->diode_reverse_high_voltage_v = -1.0f;
    data->diode_nonlinearity_forward_v = -1.0f;
    data->diode_nonlinearity_reverse_v = -1.0f;
    data->diode_asymmetry_ratio = -1.0f;
    data->diode_probe_type = COMPONENT_UNKNOWN;
    data->diode_valid = false;

    status = measurement_core_measure_safe(data);
    if (status != HAL_OK)
    {
        return status;
    }

    status = measurement_core_measure_res_high(data);
    if (status != HAL_OK)
    {
        return status;
    }

    status = measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    status = measurement_core_measure_res_mid(data);
    if (status != HAL_OK)
    {
        return status;
    }

    status = measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    status = measurement_core_measure_res_low(data);
    if (status != HAL_OK)
    {
        return status;
    }

    status = measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    LOG_INFO(
        MEASUREMENT_CORE_LOG_TAG,
        "SUMMARY SAFE(raw=%u,V=%.4f) "
        "RES_HIGH(raw=%u,V=%.4f,R=%.1f) "
        "RES_MID(raw=%u,V=%.4f,R=%.1f) "
        "RES_LOW(raw=%u,V=%.4f,R=%.1f)",
        data->safe_vdivs_adc_raw,
        (double)data->safe_voltage_v,
        data->res_high_vdivs_adc_raw,
        (double)data->res_high_voltage_v,
        (double)(data->res_high_resistance_valid ?
            data->res_high_estimated_resistance_ohms : -1.0f),
        data->res_mid_vdivs_adc_raw,
        (double)data->res_mid_voltage_v,
        (double)(data->res_mid_resistance_valid ?
            data->res_mid_estimated_resistance_ohms : -1.0f),
        data->res_low_vdivs_adc_raw,
        (double)data->res_low_voltage_v,
        (double)(data->res_low_resistance_valid ?
            data->res_low_estimated_resistance_ohms : -1.0f));

    return HAL_OK;
}

HAL_StatusTypeDef measurement_core_run_kelvin_probe(measurement_data_t *data)
{
    HAL_StatusTypeDef status;

    if (data == NULL)
    {
        return HAL_ERROR;
    }

    data->kelvin_adc_raw = 0U;
    data->kelvin_voltage_v = 0.0f;
    data->kelvin_estimated_resistance_ohms = -1.0f;
    data->kelvin_valid = false;
    data->kelvin_resistance_valid = false;

    status = measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_KELVIN);
    if (status != HAL_OK)
    {
        return status;
    }

    LOG_INFO(MEASUREMENT_CORE_LOG_TAG, "WAIT Set switches for KELVIN");
    HAL_Delay(MEASUREMENT_CORE_MANUAL_SWITCH_DELAY_MS);
    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    status = measurement_core_read_kelvin_average(
        &data->kelvin_adc_raw,
        &data->kelvin_voltage_v);

    if (status != HAL_OK)
    {
        LOG_ERROR(MEASUREMENT_CORE_LOG_TAG, "KELVIN ADC read FAILED");
        (void)measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
        return status;
    }

    data->kelvin_valid = true;

    if (data->kelvin_voltage_v >=
        (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V - MEASUREMENT_CORE_KELVIN_NEAR_ADC_RAIL_MARGIN_V))
    {
        LOG_WARN(
            MEASUREMENT_CORE_LOG_TAG,
            "KELVIN near ADC rail / possible amplifier saturation, compliance limit or out-of-range DUT");
    }

    if (data->kelvin_voltage_v < MEASUREMENT_CORE_KELVIN_LOW_CONFIDENCE_VOLTAGE_V)
    {
        LOG_WARN(
            MEASUREMENT_CORE_LOG_TAG,
            "KELVIN very low-level / possible near-short, lead resistance or offset-dominated region");
    }

    data->kelvin_resistance_valid =
        measurement_core_estimate_resistance_from_kelvin_voltage(
            data->kelvin_voltage_v,
            &data->kelvin_estimated_resistance_ohms);

    if (data->kelvin_resistance_valid)
    {
        LOG_INFO(
            MEASUREMENT_CORE_LOG_TAG,
            "KELVIN: raw=%u Vadc=%.5f G=%.2f I_nom=%.5f A CC_REF=%.3fV RREF1=%.1f ohm R_est=%.3f ohm",
            data->kelvin_adc_raw,
            (double)data->kelvin_voltage_v,
            (double)MEASUREMENT_CORE_KELVIN_MEAS_GAIN_V_PER_V,
            (double)MEASUREMENT_CORE_KELVIN_NOMINAL_TEST_CURRENT_A,
            (double)MEASUREMENT_CORE_KELVIN_CC_REF_VOLTAGE_V,
            (double)MEASUREMENT_CORE_KELVIN_RREF1_OHMS,
            (double)data->kelvin_estimated_resistance_ohms);
    }
    else
    {
        LOG_INFO(
            MEASUREMENT_CORE_LOG_TAG,
            "KELVIN: raw=%u Vadc=%.5f G=%.2f I_nom=%.5f A CC_REF=%.3fV RREF1=%.1f ohm R_est=N/A",
            data->kelvin_adc_raw,
            (double)data->kelvin_voltage_v,
            (double)MEASUREMENT_CORE_KELVIN_MEAS_GAIN_V_PER_V,
            (double)MEASUREMENT_CORE_KELVIN_NOMINAL_TEST_CURRENT_A,
            (double)MEASUREMENT_CORE_KELVIN_CC_REF_VOLTAGE_V,
            (double)MEASUREMENT_CORE_KELVIN_RREF1_OHMS);
    }

    status = measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);

    return HAL_OK;
}

HAL_StatusTypeDef measurement_core_run_capacitance_probe(measurement_data_t *data)
{
    if (data == NULL)
    {
        return HAL_ERROR;
    }

    data->cap_pulse_high_ns = 0U;
    data->cap_pulse_low_ns = 0U;
    data->cap_frequency_hz = -1.0f;
    data->cap_raw_pf = -1.0f;
    data->cap_corrected_pf = -1.0f;
    data->cap_valid = false;

    return measurement_core_measure_capacitance(data);
}

HAL_StatusTypeDef measurement_core_run_diode_probe(measurement_data_t *data)
{
    HAL_StatusTypeDef status;

    if (data == NULL)
    {
        return HAL_ERROR;
    }

    data->diode_forward_low_voltage_v = -1.0f;
    data->diode_forward_mid_voltage_v = -1.0f;
    data->diode_forward_high_voltage_v = -1.0f;
    data->diode_reverse_low_voltage_v = -1.0f;
    data->diode_reverse_mid_voltage_v = -1.0f;
    data->diode_reverse_high_voltage_v = -1.0f;
    data->diode_nonlinearity_forward_v = -1.0f;
    data->diode_nonlinearity_reverse_v = -1.0f;
    data->diode_asymmetry_ratio = -1.0f;
    data->diode_valid = false;
    data->diode_probe_type = COMPONENT_UNKNOWN;

    LOG_INFO(MEASUREMENT_CORE_LOG_TAG, "WAIT Set switches for DIODE");
    HAL_Delay(MEASUREMENT_CORE_MANUAL_SWITCH_DELAY_MS);

    status = measurement_core_measure_diode_mode(
        MEASUREMENT_TOPOLOGY_DIODE_FORWARD_LOW_RANGE,
        "DIODE FWD LOW",
        &data->diode_forward_low_voltage_v);
    if (status != HAL_OK)
    {
        (void)measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
        return status;
    }

    status = measurement_core_measure_diode_mode(
        MEASUREMENT_TOPOLOGY_DIODE_FORWARD_MID_RANGE,
        "DIODE FWD MID",
        &data->diode_forward_mid_voltage_v);
    if (status != HAL_OK)
    {
        (void)measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
        return status;
    }

    status = measurement_core_measure_diode_mode(
        MEASUREMENT_TOPOLOGY_DIODE_FORWARD_HIGH_RANGE,
        "DIODE FWD HIGH",
        &data->diode_forward_high_voltage_v);
    if (status != HAL_OK)
    {
        (void)measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
        return status;
    }

    status = measurement_core_measure_diode_mode(
        MEASUREMENT_TOPOLOGY_DIODE_REVERSE_LOW_RANGE,
        "DIODE REV LOW",
        &data->diode_reverse_low_voltage_v);
    if (status != HAL_OK)
    {
        (void)measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
        return status;
    }

    status = measurement_core_measure_diode_mode(
        MEASUREMENT_TOPOLOGY_DIODE_REVERSE_MID_RANGE,
        "DIODE REV MID",
        &data->diode_reverse_mid_voltage_v);
    if (status != HAL_OK)
    {
        (void)measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
        return status;
    }

    status = measurement_core_measure_diode_mode(
        MEASUREMENT_TOPOLOGY_DIODE_REVERSE_HIGH_RANGE,
        "DIODE REV HIGH",
        &data->diode_reverse_high_voltage_v);
    if (status != HAL_OK)
    {
        (void)measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
        return status;
    }

    data->diode_nonlinearity_forward_v =
        measurement_core_get_three_point_spread(
            data->diode_forward_low_voltage_v,
            data->diode_forward_mid_voltage_v,
            data->diode_forward_high_voltage_v);

    data->diode_nonlinearity_reverse_v =
        measurement_core_get_three_point_spread(
            data->diode_reverse_low_voltage_v,
            data->diode_reverse_mid_voltage_v,
            data->diode_reverse_high_voltage_v);

    data->diode_asymmetry_ratio =
        data->diode_nonlinearity_reverse_v /
        (data->diode_nonlinearity_forward_v + MEASUREMENT_CORE_DIODE_RATIO_EPSILON);

    data->diode_probe_type = measurement_core_classify_diode_probe(data);
    data->diode_valid = true;

    LOG_INFO(
        MEASUREMENT_CORE_LOG_TAG,
        "DIODE SUMMARY "
        "FWD_LOW=%.4f FWD_MID=%.4f FWD_HIGH=%.4f "
        "REV_LOW=%.4f REV_MID=%.4f REV_HIGH=%.4f "
        "dF=%.4f dR=%.4f ratio=%.3f type=%s",
        (double)data->diode_forward_low_voltage_v,
        (double)data->diode_forward_mid_voltage_v,
        (double)data->diode_forward_high_voltage_v,
        (double)data->diode_reverse_low_voltage_v,
        (double)data->diode_reverse_mid_voltage_v,
        (double)data->diode_reverse_high_voltage_v,
        (double)data->diode_nonlinearity_forward_v,
        (double)data->diode_nonlinearity_reverse_v,
        (double)data->diode_asymmetry_ratio,
        measurement_core_get_component_type_name(data->diode_probe_type));

    status = measurement_core_apply_topology_and_log(MEASUREMENT_TOPOLOGY_SAFE);
    if (status != HAL_OK)
    {
        return status;
    }

    HAL_Delay(MEASUREMENT_CORE_TOPOLOGY_SETTLE_DELAY_MS);
    return HAL_OK;
}

component_type_t measurement_core_detect_component(const measurement_data_t *data)
{
    /*
     * Huom:
     * Tämä on edelleen kevyt väliaikainen detektio, eikä se käytä vielä
     * Kelvin-, cap- tai diode-proben lopullista päätöslogiikkaa.
     *
     * Nykyinen baseline-päätös:
     * - jos mikä tahansa haaroista antaa validin resistanssiestimaatin,
     *   palautetaan RESISTOR
     * - jos kaikki haarat näyttävät railin lähelle, palautetaan OPEN
     * - muuten UNKNOWN
     */
    if (data == NULL)
    {
        return COMPONENT_UNKNOWN;
    }

    if (data->res_low_resistance_valid ||
        data->res_mid_resistance_valid ||
        data->res_high_resistance_valid)
    {
        return COMPONENT_RESISTOR;
    }

    if ((data->res_high_voltage_v >=
            (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V - MEASUREMENT_CORE_OPEN_MARGIN_V)) &&
        (data->res_mid_voltage_v >=
            (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V - MEASUREMENT_CORE_OPEN_MARGIN_V)) &&
        (data->res_low_voltage_v >=
            (MEASUREMENT_CORE_ADC_REFERENCE_VOLTAGE_V - MEASUREMENT_CORE_OPEN_MARGIN_V)))
    {
        return COMPONENT_OPEN;
    }

    return COMPONENT_UNKNOWN;
}

const char *measurement_core_get_component_type_name(component_type_t type)
{
    switch (type)
    {
        case COMPONENT_RESISTOR:
            return "RESISTOR";

        case COMPONENT_DIODE:
            return "DIODE";

        case COMPONENT_LED:
            return "LED";

        case COMPONENT_CAPACITOR:
            return "CAPACITOR";

        case COMPONENT_OPEN:
            return "OPEN";

        case COMPONENT_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}