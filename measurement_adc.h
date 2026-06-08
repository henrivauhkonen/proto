#ifndef MEASUREMENT_ADC_H
#define MEASUREMENT_ADC_H

#include <stdint.h>
#include "stm32l4xx_hal.h"

/*
 * Aktiivisesti käytettävät ADC-tulot tässä projektissa.
 *
 * VDIVS:
 *   PA7 -> ADC1_IN12
 *
 * KELVIN:
 *   PB0 -> ADC1_IN15
 *
 * Huom:
 * VDIVS on yhteinen lukusolmu kaikille VDIVS-haaroille:
 * - LOW  = 820 Ω
 * - MID  = 56 kΩ
 * - HIGH = 470 kΩ
 *
 * Topologiakerros päättää mikä haara on aktiivinen.
 * Tämä ADC-kerros lukee vain kyseisen solmun jännitteen.
 */
typedef enum
{
    MEASUREMENT_ADC_INPUT_VDIVS = 0,
    MEASUREMENT_ADC_INPUT_KELVIN
} measurement_adc_input_t;

/*
 * Alustaa ADC:n mittaussovellusta varten.
 *
 * Tässä vaiheessa:
 * - käynnistetään yhden päätteisen (single-ended) ADC-kalibrointi
 * - oletetaan, että board_adc_hal_initialize() on kutsuttu aiemmin
 */
HAL_StatusTypeDef measurement_adc_initialize(void);

/*
 * Lukee yhden näytteen valitulta ADC-tulolta.
 *
 * Parametrit:
 * - input:
 *     valittava ADC-lähde (VDIVS tai KELVIN)
 *
 * - result:
 *     osoitin 12-bit raakakoodin talletukseen
 */
HAL_StatusTypeDef measurement_adc_read_single(
    measurement_adc_input_t input,
    uint16_t *result);

/*
 * Lukee useita näytteitä valitulta ADC-tulolta ja palauttaa keskiarvon.
 *
 * Parametrit:
 * - input:
 *     valittava ADC-lähde
 *
 * - discard_samples:
 *     hylättävien alkuarvojen määrä
 *
 * - keep_samples:
 *     keskiarvoon käytettävien näytteiden määrä
 *
 * - result:
 *     pyöristetty keskiarvo raakakoodeina
 *
 * Tarkoitus:
 * - vähentää yksittäisen näytteen kohinaa
 * - antaa vakaampi lukema VDIVS- ja Kelvin-mittauksille
 */
HAL_StatusTypeDef measurement_adc_read_average(
    measurement_adc_input_t input,
    uint16_t discard_samples,
    uint16_t keep_samples,
    uint16_t *result);

/*
 * Muuntaa raakakoodin jännitteeksi annetulla referenssijännitteellä.
 *
 * Tässä vaiheessa käytetään 12-bit skaalaa:
 *   0 ... 4095
 */
float measurement_adc_convert_raw_to_voltage(
    uint16_t raw_value,
    float reference_voltage);

#endif /* MEASUREMENT_ADC_H */