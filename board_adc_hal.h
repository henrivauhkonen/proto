#ifndef BOARD_ADC_HAL_H
#define BOARD_ADC_HAL_H

#include "stm32l4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ADC1-handle mittauskerrosten käyttöön.
 *
 * Tämän projektin aktiiviset ADC-tulot:
 *
 *   PA7 -> ADC1_IN12 -> VDIVS-solmun mittaus
 *   PB0 -> ADC1_IN15 -> KELVIN-solmun mittaus
 *
 * Huom:
 * VDIVS-solmu on yhteinen ADC-lukupiste riippumatta siitä, mikä VDIVS-haara
 * (LOW / MID / HIGH) on kulloinkin aktiivinen.
 *
 * Huom 2:
 * PA4 = DAC-ref kuuluu analogia-arkkitehtuuriin, mutta EI ADC HAL -kerrokseen.
 * Se alustetaan myöhemmin erillisessä DAC-moduulissa.
 */
extern ADC_HandleTypeDef hadc1;

/*
 * Alustaa ADC1-periferian tätä mittausprojektia varten.
 *
 * Alustus sisältää:
 * - ADC-kellon konfiguroinnin
 * - tarvittavien GPIO-pinnejä analog-tilaan
 * - ADC1:n perusasetukset
 * - yhden oletuskanTotta — tässä tulee nyt pyytämäsi tiedostot kokonaan:

1. **`board_adc_hal.h`**  
2. **`measurement_adc.h`**  
3. **`measurement_adc.c`**

Päivitin kommentit ja kuvaukset vastaamaan nykyistä mallia:

- **PA7 = ADC1_IN12 = VDIVS-solmun ADC**
- **PB0 = ADC1_IN15 = KELVIN-solmun ADC**
- VDIVS on yhteinen ADC-lukupiste riippumatta siitä, mikä haara on aktiivinen:
  - **LOW = 820 Ω**
  - **MID = 56 kΩ**
  - **HIGH = 470 kΩ**
- ADC-kerros ei tiedä topologiasta muuta kuin sen, että se lukee joko
  - VDIVS-solmua
  - tai Kelvin-solmua

Toiminnallisesti `measurement_adc.c` pysyy hyvin lähellä aiempaa versiota, mikä on tässä hyvä asia:  
**topologiakerros valitsee sähköisen reitin, ADC-kerros vain lukee valittua analogiasolmua.**

---

# `board_adc_hal.h`

```c
#ifndef BOARD_ADC_HAL_H
#define BOARD_ADC_HAL_H

#include "stm32l4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ADC1-handle mittauskerrosten käyttöön.
 *
 * Tämän projektin aktiiviset ADC-tulot:
 *
 *   PA7 -> ADC1_IN12 -> VDIVS-solmun mittaus
 *   PB0 -> ADC1_IN15 -> KELVIN-solmun mittaus
 *
 * Huom:
 * VDIVS-solmu on yhteinen ADC-lukupiste riippumatta siitä, mikä VDIVS-haara
 * (LOW / MID / HIGH) on kulloinkin aktiivinen.
 *
 * Huom 2:
 * PA4 = DAC-ref kuuluu analogia-arkkitehtuuriin, mutta EI ADC HAL -kerrokseen.
 * Se alustetaan myöhemmin erillisessä DAC-moduulissa.
 */
extern ADC_HandleTypeDef hadc1;

/*
 * Alustaa ADC1-periferian tätä mittausprojektia varten.
 *
 * Alustus sisältää:
 * - ADC-kellon konfiguroinnin
 * - tarvittavien GPIO-pinnejä analog-tilaan
 * - ADC1:n perusasetukset
 * - yhden oletuskanavan konfiguroinnin, jotta init-polku on ehjä
 *
 * Varsinainen mittauskerros (measurement_adc.c) vaihtaa aktiivista kanavaa
 * myöhemmin tarpeen mukaan.
 */
HAL_StatusTypeDef board_adc_hal_initialize(void);

/*
 * Vapauttaa ADC1-periferian ja tähän kerrokseen kuuluvat GPIO-konfiguraatiot.
 */
HAL_StatusTypeDef board_adc_hal_deinitialize(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_ADC_HAL_H */
