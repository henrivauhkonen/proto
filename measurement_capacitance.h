#ifndef MEASUREMENT_CAPACITANCE_H
#define MEASUREMENT_CAPACITANCE_H

#include <stdint.h>
#include "stm32l4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Kapasitanssimittauksen tulosrakenne.
 *
 * Tässä vaiheessa halutaan talteen:
 * - yhden HIGH-pulssin pituus
 * - yhden LOW-pulssin pituus
 * - laskettu raakafrekvenssi
 * - laskettu kapasitanssi pikofaradeina
 *
 * Tätä dataa käytetään bring-upiin ja myöhemmin mahdolliseen
 * komponenttien tunnistukseen.
 */
typedef struct
{
    uint32_t pulse_high_ns;
    uint32_t pulse_low_ns;
    float frequency_hz;
    float capacitance_pf;
} measurement_capacitance_result_t;

/*
 * Alustaa TIM2-pohjaisen kapasitanssimittauksen.
 *
 * Tämä funktio on turvallista kutsua useita kertoja; varsinainen
 * laitteistoalustus tehdään vain kerran.
 */
HAL_StatusTypeDef measurement_capacitance_initialize(void);

/*
 * Mittaa yhden pulssin pituuden nanosekunteina.
 *
 * state == GPIO_PIN_SET:
 *   mitataan HIGH-pulssin leveys
 *
 * state == GPIO_PIN_RESET:
 *   mitataan LOW-pulssin leveys
 */
uint32_t measurement_capacitance_pulse_in_ns(GPIO_PinState state);

/*
 * Ajaa yhden täydellisen kapasitanssiprobe-mittauksen.
 *
 * Palauttaa:
 * - tH
 * - tL
 * - raakafrekvenssin
 * - kapasitanssin pikofaradeina
 */
HAL_StatusTypeDef measurement_capacitance_measure(
    measurement_capacitance_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* MEASUREMENT_CAPACITANCE_H */
