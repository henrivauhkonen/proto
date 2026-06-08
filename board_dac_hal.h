#ifndef BOARD_DAC_HAL_H
#define BOARD_DAC_HAL_H

#include <stdint.h>
#include "stm32l4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Projektikohtaiset DAC-asetukset
 * ============================================================================
 *
 * Näitä makroja voi tarvittaessa yliajaa ennen tämän headerin includea,
 * jos projektissa halutaan myöhemmin eri oletusjännite, eri kalibrointikerroin
 * tai muu konfiguraatio.
 */

/*
 * DAC-ulostulon oletusvertailujännite.
 *
 * Yleensä sama kuin analoginen käyttöjännite, esim. 3.3 V.
 */
#ifndef BOARD_DAC_DEFAULT_REFERENCE_VOLTAGE_V
#define BOARD_DAC_DEFAULT_REFERENCE_VOLTAGE_V           3.3f
#endif

/*
 * Kelvin-mittauksen tavoitereferenssi.
 *
 * Tämä on looginen tavoitetaso, jonka käyttäjä / sovellus haluaa.
 * Mahdollinen kalibrointikerroin sovelletaan tämän päälle toteutuksessa.
 */
#ifndef BOARD_DAC_KELVIN_REFERENCE_TARGET_VOLTAGE_V
#define BOARD_DAC_KELVIN_REFERENCE_TARGET_VOLTAGE_V     1.0f
#endif

/*
 * Yksipistekalibroinnin gain-korjaus DAC-ulostulolle.
 *
 * Esimerkkitilanne:
 * - pyydetty 1.000 V
 * - mitattu noin 0.993 V
 *
 * Korjauskerroin:
 *   1.000 / 0.993 ≈ 1.00705
 *
 * Tällä korjauksella board_dac_hal_set_voltage(1.0f, 3.3f)
 * tuottaa käytännössä hyvin lähelle 1.000 V ulostulon.
 *
 * Huom:
 * Tämä on prototasoinen kalibrointi nykyiselle laitteelle / kuormalle /
 * käyttöjännitteelle. Lopullinen tuotantokalibrointi voidaan myöhemmin
 * tehdä tarkemmin.
 */
#ifndef BOARD_DAC_OUTPUT_GAIN_CORRECTION
#define BOARD_DAC_OUTPUT_GAIN_CORRECTION                1.00705f
#endif

/*
 * DAC1-handle projektin analogista referenssiulostuloa varten.
 *
 * Tämänhetkinen käyttö:
 *
 *   PA4 -> DAC1_OUT1 -> REF_1V_DAC1_OUT1 / CC_REF
 *
 * Tarkoitus:
 * - tuottaa vakioitu referenssijännite analogiaetupäälle
 * - mahdollistaa Kelvin-polun hallittu referenssi ilman ulkoista
 *   jännitelähdettä tai erillistä jännitteenjakajaa
 *
 * Huom:
 * Tämä on matalan tason HAL-kerros. Se ei päätä, milloin tai miksi
 * referenssiä käytetään, vaan tarjoaa hallitun tavan alustaa ja ohjata DAC:ia.
 */
extern DAC_HandleTypeDef hdac1;

/*
 * Alustaa DAC1-periferian ja siihen liittyvän PA4-ulostulon.
 *
 * Tämä funktio:
 * - ottaa DAC1-kellon käyttöön
 * - konfiguroi PA4:n analog-tilaan
 * - alustaa DAC1:n
 * - konfiguroi kanavan 1 ohjelmallisesti ohjatuksi ilman triggeriä
 * - käynnistää kanavan
 *
 * Huom:
 * Tässä vaiheessa DAC:n ulostuloarvo asetetaan erillisellä
 * board_dac_hal_set_*()-funktiolla.
 */
HAL_StatusTypeDef board_dac_hal_initialize(void);

/*
 * Vapauttaa DAC1-periferian ja tähän kerrokseen kuuluvan PA4-konfiguraation.
 */
HAL_StatusTypeDef board_dac_hal_deinitialize(void);

/*
 * Asettaa DAC-kanavan 12-bittisen raakaarvon.
 *
 * raw_value:
 *   0 ... 4095
 *
 * Tätä voi käyttää silloin, kun halutaan täysi kontrolli ilman
 * jännitehelperin pyöristyksiä.
 */
HAL_StatusTypeDef board_dac_hal_set_raw_12bit(uint16_t raw_value);

/*
 * Asettaa DAC-ulostulon halutuksi jännitteeksi.
 *
 * voltage_v:
 *   käyttäjän tai sovelluksen pyytämä looginen tavoitejännite
 *
 * reference_voltage_v:
 *   DAC:n skaalausreferenssi, tyypillisesti analoginen käyttöjännite,
 *   esim. 3.3 V
 *
 * Funktio:
 * - soveltaa projektin kalibrointikerrointa
 * - rajaa korjatun jännitteen välille [0, reference_voltage_v]
 * - muuntaa sen 12-bittiseksi arvoksi
 * - kirjoittaa arvon DAC:lle
 *
 * Huom:
 * board_dac_hal_get_last_voltage() palauttaa loogisen pyydetyn jännitteen,
 * ei sisäistä korjattua arvoa.
 */
HAL_StatusTypeDef board_dac_hal_set_voltage(
    float voltage_v,
    float reference_voltage_v);

/*
 * Kätevä helperi Kelvin-referenssin asettamiseen projektin oletusarvoilla.
 *
 * Käyttää:
 * - BOARD_DAC_KELVIN_REFERENCE_TARGET_VOLTAGE_V
 * - BOARD_DAC_DEFAULT_REFERENCE_VOLTAGE_V
 */
HAL_StatusTypeDef board_dac_hal_set_kelvin_reference_default(void);

/*
 * Palauttaa viimeksi asetetun 12-bittisen raakaarvon.
 *
 * Huom:
 * tämä on ohjelmiston muistama arvo, ei DAC-rekisterin takaisinluku.
 */
uint16_t board_dac_hal_get_last_raw_12bit(void);

/*
 * Palauttaa viimeksi pyydetyn jännitteen.
 *
 * Huom:
 * tämä on ohjelmiston muistama looginen tavoitejännite,
 * ei mitattu fyysinen ulostulojännite.
 */
float board_dac_hal_get_last_voltage(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_DAC_HAL_H */