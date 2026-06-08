#ifndef MEASUREMENT_TOPOLOGY_H
#define MEASUREMENT_TOPOLOGY_H

#include <stdbool.h>
#include "stm32l4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DUT1-ohjauspinnin toimintatila.
 *
 * DUT1 on tässä projektissa firmware-ohjattu mittausterminaalin toinen puoli.
 * Tämänhetkisessä protoversiossa DUT1_CTRL on erillinen digitaalinen ohjaus-
 * pinni, jolla voidaan:
 *
 * - vapauttaa DUT1 (Hi-Z)
 * - vetää DUT1 matalaksi
 * - vetää DUT1 korkeaksi
 *
 * Tämä sama DUT1_CTRL-pinni palvelee nyt useita topologioita:
 *
 *   SAFE         -> Hi-Z
 *   RESISTANCE_* -> LOW
 *   KELVIN       -> HIGH
 *   CAPACITANCE  -> LOW
 *   DIODE_*      -> LOW tai HIGH mittaussuunnasta riippuen
 *
 * Huom:
 * Aiempi erillinen "diode direction" -pinni on poistunut käytöstä.
 * Diodisuunta toteutetaan nykyisessä mallissa DUT1_CTRL-pinnillä.
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
 * - DUT1 ohjataan erillisellä DUT1_CTRL-pinnillä
 * - DUT2 reititetään kahdella kytkimellä / releellä
 * - VDIVS-jakoverkko on kolmialueinen: LOW / MID / HIGH
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
 * - Diodimittaus käyttää VDIVS-solmua + DUT1_CTRL-ohjausta
 *
 * Huom:
 * PB6 / PB7 ovat tässä protovaiheessa releiden / kytkinasentojen
 * firmware-indikaatiot. Käsikytkimet simuloivat varsinaista relelogiikkaa.
 *
 * Huom 2:
 * Nyt kun VDIVS-lähdeverkko on kolmialueinen, topologialuetteloon on lisätty
 * MID-alue sekä resistanssi- että diodimittaukselle.
 */
typedef enum
{
    MEASUREMENT_TOPOLOGY_SAFE = 0,
    MEASUREMENT_TOPOLOGY_WAKE_SETTLE,

    MEASUREMENT_TOPOLOGY_RESISTANCE_HIGH_RANGE,
    MEASUREMENT_TOPOLOGY_RESISTANCE_MID_RANGE,
    MEASUREMENT_TOPOLOGY_RESISTANCE_LOW_RANGE,

    MEASUREMENT_TOPOLOGY_KELVIN,
    MEASUREMENT_TOPOLOGY_CAPACITANCE,

    MEASUREMENT_TOPOLOGY_DIODE_FORWARD_LOW_RANGE,
    MEASUREMENT_TOPOLOGY_DIODE_FORWARD_MID_RANGE,
    MEASUREMENT_TOPOLOGY_DIODE_FORWARD_HIGH_RANGE,

    MEASUREMENT_TOPOLOGY_DIODE_REVERSE_LOW_RANGE,
    MEASUREMENT_TOPOLOGY_DIODE_REVERSE_MID_RANGE,
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
 * VDIVS low/mid/high "enabled" tarkoittaa tässä projektissa:
 *   enabled  -> output HIGH
 *   disabled -> analog / Hi-Z
 *
 * Näitä pinnejä ei sammuteta ajamalla LOW-tilaan, vaan vapauttamalla linja.
 * Tämä vähentää riskiä kuormittaa VDIVS-solmua väärin topologian vaihdon aikana.
 */
void measurement_topology_set_low_range_enabled(bool enabled);
void measurement_topology_set_mid_range_enabled(bool enabled);
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

/*
 * DUT1-ohjauksen low-level API.
 */
void measurement_topology_set_dut1_control_mode(measurement_dut1_control_mode_t mode);
measurement_dut1_control_mode_t measurement_topology_get_dut1_control_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* MEASUREMENT_TOPOLOGY_H */
