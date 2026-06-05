#ifndef MEASUREMENT_TOPOLOGY_H
#define MEASUREMENT_TOPOLOGY_H

#include <stdbool.h>
#include "stm32l4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Diodisuunnan ohjauspinnin toimintatila.
 *
 * HIGH_IMPEDANCE:
 *   Pinni asetetaan analog-tilaan, jolloin se toimii käytännössä Hi-Z-tilassa.
 *
 * DRIVE_LOW:
 *   Pinni asetetaan digitaaliseksi lähtöpiniksi ja ajetaan matalaksi.
 *
 * DRIVE_HIGH:
 *   Pinni asetetaan digitaaliseksi lähtöpiniksi ja ajetaan korkeaksi.
 */
typedef enum
{
    DIODE_DIRECTION_HIGH_IMPEDANCE = 0,
    DIODE_DIRECTION_DRIVE_LOW,
    DIODE_DIRECTION_DRIVE_HIGH
} diode_direction_mode_t;

/*
 * DUT1-ohjauspinnin toimintatila.
 *
 * DUT1 on tässä projektissa firmware-ohjattu mittausterminaalin toinen puoli.
 * Toistaiseksi DUT1-ohjauspinniksi valitaan PB4.
 *
 * HIGH_IMPEDANCE:
 *   DUT1 vapautetaan (Hi-Z).
 *
 * DRIVE_LOW:
 *   DUT1 vedetään matalaksi.
 *
 * DRIVE_HIGH:
 *   DUT1 vedetään korkeaksi.
 *
 * Huom:
 * Tässä protoversiossa KELVIN käyttää DUT1 = HIGH -tilaa, koska PB4
 * on osa Kelvin-kytkennän ohjausta juuri tämänhetkisen KiCad-/proto-
 * toteutuksen mukaisesti.
 */
typedef enum
{
    MEASUREMENT_DUT1_CONTROL_HIGH_IMPEDANCE = 0,
    MEASUREMENT_DUT1_CONTROL_DRIVE_LOW,
    MEASUREMENT_DUT1_CONTROL_DRIVE_HIGH
} measurement_dut1_control_mode_t;

/*
 * Yhteisen DUT-paikan korkeamman tason topologiat.
 *
 * Tämänhetkinen topologinen malli:
 *
 * - DUT1 ohjataan pinnillä (PB4)
 * - DUT2 reititetään kahdella kytkimellä / releellä
 *
 * K1:
 *   common = DUT2
 *   left   = CAPACITANCE
 *   right  = K2 common
 *
 * K2:
 *   common = K1 right
 *   left   = VDIVS
 *   right  = KELVIN
 *
 * Lisäksi:
 * - RESISTANCE_* käyttää VDIVS-solmua
 * - KELVIN käyttää Kelvin-ADC-polkuja
 * - CAPACITANCE käyttää TIM2_CH1 / PA0 -polkua
 * - Diodimittaus käyttää VDIVS-solmua + diode direction -pinniä
 *
 * Huom:
 * PB6 / PB7 ovat tässä protovaiheessa releiden / kytkinasentojen
 * firmware-indikaatiot. Käsikytkimet simuloivat varsinaista relelogiikkaa.
 */

typedef enum
{
    MEASUREMENT_TOPOLOGY_SAFE = 0,
    MEASUREMENT_TOPOLOGY_WAKE_SETTLE,
    MEASUREMENT_TOPOLOGY_RESISTANCE_HIGH_RANGE,
    MEASUREMENT_TOPOLOGY_RESISTANCE_LOW_RANGE,
    MEASUREMENT_TOPOLOGY_KELVIN,
    MEASUREMENT_TOPOLOGY_CAPACITANCE,
    MEASUREMENT_TOPOLOGY_DIODE_FORWARD_LOW_RANGE,
    MEASUREMENT_TOPOLOGY_DIODE_FORWARD_HIGH_RANGE,
    MEASUREMENT_TOPOLOGY_DIODE_REVERSE_LOW_RANGE,
    MEASUREMENT_TOPOLOGY_DIODE_REVERSE_HIGH_RANGE,
    MEASUREMENT_TOPOLOGY_COUNT
} measurement_topology_mode_t;


/*
 * Kertoo mitä lukupolkua topologia normaalisti käyttää.
 */
typedef enum
{
    MEASUREMENT_READ_PATH_NONE = 0,
    MEASUREMENT_READ_PATH_VDIVS_ADC,
    MEASUREMENT_READ_PATH_KELVIN_ADC,
    MEASUREMENT_READ_PATH_TIM2_CH1
} measurement_read_path_t;

/*
 * Alustaa mittaustopologian turvalliseen oletustilaan.
 *
 * Tämä kannattaa kutsua kerran käynnistyksessä ennen varsinaista mittausajoa.
 */
void measurement_topology_initialize_safe_state(void);

/*
 * Valitsee halutun topologian.
 *
 * Palauttaa HAL_OK jos topologia onnistuttiin asettamaan.
 */
HAL_StatusTypeDef measurement_topology_apply(measurement_topology_mode_t mode);

/*
 * Palauttaa annetun topologian oletuslukupolun.
 */
measurement_read_path_t measurement_topology_get_read_path(measurement_topology_mode_t mode);

/*
 * Palauttaa viimeksi onnistuneesti asetetun topologian.
 */
measurement_topology_mode_t measurement_topology_get_current_mode(void);

/*
 * Palauttaa topologian nimen debugia varten.
 */
const char *measurement_topology_get_mode_name(measurement_topology_mode_t mode);

/*
 * Low-level API jää myös näkyviin.
 *
 * Huom:
 * VDIVS low/high "enabled" tarkoittaa tässä projektissa:
 *   enabled  -> output HIGH
 *   disabled -> analog / Hi-Z
 */
void measurement_topology_set_low_range_enabled(bool enabled);
void measurement_topology_set_high_range_enabled(bool enabled);

/*
 * K1 / K2 indikaatio-ohjaus.
 *
 * K1 semantics:
 *   relay1_enabled = false -> K1 left  -> CAPACITANCE
 *   relay1_enabled = true  -> K1 right -> K2 common
 *
 * K2 semantics:
 *   relay2_enabled = false -> K2 right -> KELVIN
 *   relay2_enabled = true  -> K2 left  -> VDIVS
 *
 * Huom:
 * Nimet "enabled" säilytetään yhteensopivuuden vuoksi, vaikka logiikka
 * kuvaa käytännössä kytkimen asentoa.
 */
void measurement_topology_set_relay_1_enabled(bool enabled);
void measurement_topology_set_relay_2_enabled(bool enabled);

void measurement_topology_set_diode_direction(diode_direction_mode_t mode);
diode_direction_mode_t measurement_topology_get_diode_direction(void);

void measurement_topology_set_dut1_control_mode(measurement_dut1_control_mode_t mode);
measurement_dut1_control_mode_t measurement_topology_get_dut1_control_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* MEASUREMENT_TOPOLOGY_H */
