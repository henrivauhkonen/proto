#ifndef MEASUREMENT_ADC_H
#define MEASUREMENT_ADC_H

#include <stdint.h>
#include "stm32l4xx_hal.h"

/*
 * Aktiivisesti käytettävät ADC-tulot tässä projektissa.
 *
 * VDIVS  -> PA7 -> ADC1_IN12
 * KELVIN -> PB0 -> ADC1_IN15
 */
typedef enum
{
    MEASUREMENT_ADC_INPUT_VDIVS = 0,
    MEASUREMENT_ADC_INPUT_KELVIN
} measurement_adc_input_t;

/*
 * Alustaa ADC:n mittaussovellusta varten.
 */
HAL_StatusTypeDef measurement_adc_initialize(void);

/*
 * Lukee yhden näytteen valitulta ADC-tulolta.
 */
HAL_StatusTypeDef measurement_adc_read_single(
    measurement_adc_input_t input,
    uint16_t *result);

/*
 * Lukee useita näytteitä valitulta ADC-tulolta.
 * discard_samples = hylättävien alkuarvojen määrä
 * keep_samples    = keskiarvoon käytettävien näytteiden määrä
 */
HAL_StatusTypeDef measurement_adc_read_average(
    measurement_adc_input_t input,
    uint16_t discard_samples,
    uint16_t keep_samples,
    uint16_t *result);

/*
 * Muuntaa raakakoodin jännitteeksi annetulla referenssijännitteellä.
 * Tässä vaiheessa käytetään 12-bit skaalaa (0...4095).
 */
float measurement_adc_convert_raw_to_voltage(uint16_t raw_value, float reference_voltage);

#endif /* MEASUREMENT_ADC_H */
