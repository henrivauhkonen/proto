#ifndef MEASUREMENT_CORE_H
#define MEASUREMENT_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32l4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ensimmäisen vaiheen komponenttityypit.
 *
 * Huom:
 * Lopullista komponenttitunnistusta ei vielä tehdä tässä vaiheessa.
 * Tämä enum saa silti jäädä käyttöön debugging- ja probe-klassifiointia varten.
 */
typedef enum
{
    COMPONENT_UNKNOWN = 0,
    COMPONENT_RESISTOR,
    COMPONENT_DIODE,
    COMPONENT_LED,
    COMPONENT_CAPACITOR,
    COMPONENT_OPEN
} component_type_t;

/*
 * Yhden mittausajon tulokset.
 *
 * Tässä vaiheessa mukana on:
 * - resistiivisen VDIVS-polun baseline-data
 * - erillinen Kelvin-proben raakadata + alustava ohmiestimaatti
 * - erillinen kapasitanssiprobe TIM2:lla
 * - erillinen diode-probe VDIVS + diode direction -mallilla
 *
 * Huom:
 * - Kelvin ei vielä osallistu lopulliseen komponenttitunnistukseen
 * - capacitance ei vielä osallistu lopulliseen komponenttitunnistukseen
 * - diode_probe_type on diode-proben oma luokitus, ei vielä koko järjestelmän
 *   lopullinen komponenttipäätös
 */
typedef struct
{
    /*
     * VDIVS / baseline raakadata.
     */
    uint16_t safe_vdivs_adc_raw;
    uint16_t res_high_vdivs_adc_raw;
    uint16_t res_low_vdivs_adc_raw;

    /*
     * Kelvin raakadata.
     */
    uint16_t kelvin_adc_raw;

    /*
     * Kapasitanssimittauksen pulssiajat.
     */
    uint32_t cap_pulse_high_ns;
    uint32_t cap_pulse_low_ns;

    /*
     * Jännitteet.
     */
    float safe_voltage_v;
    float res_high_voltage_v;
    float res_low_voltage_v;
    float kelvin_voltage_v;

    /*
     * Kapasitanssimittauksen johdetut suureet.
     */
    float cap_frequency_hz;
    float cap_raw_pf;
    float cap_corrected_pf;

    /*
     * Resistanssiarviot.
     */
    float res_high_estimated_resistance_ohms;
    float res_low_estimated_resistance_ohms;
    float kelvin_estimated_resistance_ohms;

    /*
     * Diodiproben raakajännitteet.
     *
     * forward_low:
     *   diode direction = FORWARD, low-range source aktiivinen
     *
     * forward_high:
     *   diode direction = FORWARD, high-range source aktiivinen
     *
     * reverse_low:
     *   diode direction = REVERSE, low-range source aktiivinen
     *
     * reverse_high:
     *   diode direction = REVERSE, high-range source aktiivinen
     */
    float diode_forward_low_voltage_v;
    float diode_forward_high_voltage_v;
    float diode_reverse_low_voltage_v;
    float diode_reverse_high_voltage_v;

    /*
     * Diodiproben johdetut suureet.
     *
     * nonlinearity_forward_v:
     *   |forward_low - forward_high|
     *
     * nonlinearity_reverse_v:
     *   |reverse_low - reverse_high|
     *
     * asymmetry_ratio:
     *   reverse / forward epälineaarisuuden suhde; debugia varten
     */
    float diode_nonlinearity_forward_v;
    float diode_nonlinearity_reverse_v;
    float diode_asymmetry_ratio;

    /*
     * Validiteetit.
     */
    bool res_high_resistance_valid;
    bool res_low_resistance_valid;

    bool kelvin_valid;
    bool kelvin_resistance_valid;

    bool cap_valid;

    /*
     * Diodiproben validi- ja tyyppitulos.
     */
    bool diode_valid;
    component_type_t diode_probe_type;
} measurement_data_t;

/*
 * Ajaa nykyisen resistiivisen baseline-mittauksen:
 *
 * SAFE -> RES_HIGH -> SAFE -> RES_LOW -> SAFE -> SUMMARY
 */
HAL_StatusTypeDef measurement_core_run_resistor_baseline(measurement_data_t *data);

/*
 * Ajaa erillisen Kelvin-proben:
 *
 * KELVIN -> ADC read -> SAFE
 */
HAL_StatusTypeDef measurement_core_run_kelvin_probe(measurement_data_t *data);

/*
 * Ajaa erillisen kapasitanssiprobe-mittauksen:
 *
 * CAPACITANCE -> TIM2 pulse measurement -> SAFE
 */
HAL_StatusTypeDef measurement_core_run_capacitance_probe(measurement_data_t *data);

/*
 * Ajaa erillisen diode-proben:
 *
 * FORWARD_LOW  -> read
 * FORWARD_HIGH -> read
 * REVERSE_LOW  -> read
 * REVERSE_HIGH -> read
 * SAFE
 *
 * Lopuksi laskee diode-proben oman luokituksen:
 * - DIODE
 * - LED
 * - RESISTOR
 * - OPEN
 * - UNKNOWN
 */
HAL_StatusTypeDef measurement_core_run_diode_probe(measurement_data_t *data);

/*
 * Palauttaa karkean komponenttityypin baseline-datan perusteella.
 *
 * Huom:
 * Tämä on edelleen kevyt väliaikainen detektio.
 * Capacitance-, Kelvin- ja diode-proben dataa ei vielä käytetä lopulliseen
 * päätökseen.
 */
component_type_t measurement_core_detect_component(const measurement_data_t *data);

/*
 * Palauttaa komponenttityypin nimen debugia varten.
 */
const char *measurement_core_get_component_type_name(component_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* MEASUREMENT_CORE_H */
