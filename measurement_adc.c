#include "measurement_adc.h"
#include "board_adc_hal.h"

/*
 * ============================================================================
 * ADC-mittauskerroksen toteutus
 * ============================================================================
 *
 * Tämän tiedoston vastuu on tarkoituksella rajattu:
 *
 * - valita oikea ADC-kanava (VDIVS tai KELVIN)
 * - lukea yksi näyte tai usean näytteen keskiarvo
 * - tarjota raakakoodin -> jännite -muunnos
 *
 * Tämä tiedosto EI:
 * - päätä mikä VDIVS-haara on aktiivinen
 * - ohjaa DUT1-tilaa
 * - ohjaa releitä / K1 / K2 -reititystä
 *
 * Nämä korkeamman tason päätökset kuuluvat topologia- ja measurement_core-
 * kerroksille.
 *
 * Projektin aktiiviset ADC-tulot:
 *
 *   VDIVS  -> PA7 -> ADC1_IN12
 *   KELVIN -> PB0 -> ADC1_IN15
 *
 *Tässä tulee **`measurement_adc.c` kokonaan uudestaan** kattavasti kommentoituna ja nykyiseen malliin sovitettuna.

Tämä versio on linjassa sen kanssa, että:

- **VDIVS-solmu** luetaan kanavasta  
  **PA7 -> ADC1_IN12**
- **KELVIN-solmu** luetaan kanavasta  
  **PB0 -> ADC1_IN15**
- **ADC-kerros ei ohjaa topologiaa**, vaan:
  - topologiakerros valitsee sähköisen reitin
  - ADC-kerros vain lukee valitun analogiasolmun

Kommentit on pidetty samalla tasolla kuin muissakin tiedostoissa.

---

## `measurement_adc.c`

```c
#include "measurement_adc.h"
#include "board_adc_hal.h"

/*
 * ============================================================================
 * ADC-mittauskerroksen yleiset asetukset
 * ============================================================================
 *
 * Tämä tiedosto rakentaa board_adc_hal-kerroksen päälle yksinkertaisen
 * mittausrajapinnan:
 *
 * - yhden näytteen luku valitulta mittaussolmulta
 * - usean näytteen keskiarvoluku kohinan pienentämiseksi
 * - raakakoodin muunto jännitteeksi
 *
 * Tämän kerroksen tarkoitus on erottaa:
 *
 *   1) board_adc_hal:
 *      - ADC1:n fyysinen/perifeerinen alustus
 *      - GPIO-pinnit analog-tilaan
 *
 *   2) measurement_adc:
 *      - mitä ADC-tuloa kulloinkin luetaan
 *      - miten näytteet otetaan
 *      - miten tulos palautetaan ylemmälle mittauslogiikalle
 *
 * Tämä jako pitää mittauslogiikan siistinä:
 * measurement_core ei tarvitse tietää HAL/ADC-konfiguraation yksityiskohtia.
 */

#define MEASUREMENT_ADC_TIMEOUT_MS      10U
#define MEASUREMENT_ADC_MAX_RAW_12BIT   4095.0f

/*
 * Sisäinen helperi aktiivisen ADC-kanavan valintaan.
 *
 * Tämä ei ole julkinen API, koska ylemmän kerroksen ei tarvitse tietää
 * käytettyjä kanavanumeroita. Ylempi koodi puhuu vain:
 *
 *   - MEASUREMENT_ADC_INPUT_VDIVS
 *   - MEASUREMENT_ADC_INPUT_KELVIN
 */
static HAL_StatusTypeDef measurement_adc_select_channel(measurement_adc_input_t input);

/*
 * ============================================================================
 * Julkinen API
 * ============================================================================
 */

/*
 * Alustaa ADC:n mittaussovellusta varten.
 *
 * Tässä vaiheessa tämä tarkoittaa käytännössä ADC:n self-calibration-ajon
 * käynnistämistä single-ended-tilassa.
 *
 * Oletus:
 * - board_adc_hal_initialize() on jo kutsuttu aiemmin
 * - hadc1 on siis alustettu ja käyttövalmis
 *
 * Miksi kalibrointi tehdään täällä:
 * - board_adc_hal vastaa raudan alustuksesta
 * - measurement_adc vastaa mittauskäyttöön valmistelusta
 *
 * Tämä jako pitää vastuut selkeinä.
 */
HAL_StatusTypeDef measurement_adc_initialize(void)
{
    return HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
}

/*
 * Lukee yhden näytteen valitulta ADC-tulolta.
 *
 * Työvaiheet:
 *   1) valitaan oikea ADC-kanava
 *   2) käynnistetään muunnos
 *   3) odotetaan EOC (end of conversion)
 *   4) luetaan raakakoodi
 *   5) pysäytetään ADC
 *
 * Huom:
 * Tämä funktio palauttaa 12-bittisen raakakoodin.
 * Jännitemuunnos tehdään erillisellä helperillä.
 */
HAL_StatusTypeDef measurement_adc_read_single(
    measurement_adc_input_t input,
    uint16_t *result)
{
    HAL_StatusTypeDef status;
    uint32_t raw_value;

    if (result == NULL)
    {
        return HAL_ERROR;
    }

    /*
     * Valitaan mitattava analogiasolmu.
     *
     * Tämä on puhtaasti ADC:n sisäinen kanavavalinta.
     * Topologia eli se, mitä fyysisessä mittausverkossa tapahtuu, on jo
     * päätetty aiemmin measurement_topology-kerroksessa.
     */
    status = measurement_adc_select_channel(input);
    if (status != HAL_OK)
    {
        return status;
    }

    status = HAL_ADC_Start(&hadc1);
    if (status != HAL_OK)
    {
        return status;
    }

    status = HAL_ADC_PollForConversion(&hadc1, MEASUREMENT_ADC_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        (void)HAL_ADC_Stop(&hadc1);
        return status;
    }

    raw_value = HAL_ADC_GetValue(&hadc1);

    status = HAL_ADC_Stop(&hadc1);
    if (status != HAL_OK)
    {
        return status;
    }

    *result = (uint16_t)raw_value;
    return HAL_OK;
}

/*
 * Lukee useita näytteitä valitulta ADC-tulolta ja palauttaa niiden
 * pyöristetyn keskiarvon.
 *
 * Parametrit:
 * - input:
 *     mitä analogiasolmua luetaan
 *
 * - discard_samples:
 *     montako alkuarvoa hylätään
 *
 * - keep_samples:
 *     montako arvoa otetaan mukaan keskiarvoon
 *
 * - result:
 *     lopullinen raakakoodi
 *
 * Miksi tätä käytetään:
 * - yksittäinen ADC-luku voi heilahdella hieman
 * - topologian vaihdon jälkeen aivan ensimmäiset näytteet voivat olla
 *   vähemmän stabiileja
 * - keskiarvo antaa measurement_core:lle rauhallisemman jännitelukeman
 *
 * Toteutus:
 * - kanava valitaan kerran ennen näytesarjaa
 * - jokainen näyte otetaan start/poll/get/stop -sekvenssillä
 * - discard_samples kappaletta hylätään
 * - keep_samples kappaletta summataan
 * - lopuksi lasketaan pyöristetty kokonaislukukeskiarvo
 */
HAL_StatusTypeDef measurement_adc_read_average(
    measurement_adc_input_t input,
    uint16_t discard_samples,
    uint16_t keep_samples,
    uint16_t *result)
{
    HAL_StatusTypeDef status;
    uint32_t sum = 0U;
    uint32_t total_samples;
    uint32_t index;
    uint32_t raw_value;

    if (result == NULL)
    {
        return HAL_ERROR;
    }

    if (keep_samples == 0U)
    {
        return HAL_ERROR;
    }

    total_samples = (uint32_t)discard_samples + (uint32_t)keep_samples;

    /*
     * Valitaan ADC-kanava kerran ennen koko näytesarjaa.
     *
     * Tämä on sekä tehokasta että loogisesti selkeää:
     * kaikki tämän funktion aikana kerättävät näytteet tulevat samasta
     * analogiasolmusta.
     */
    status = measurement_adc_select_channel(input);
    if (status != HAL_OK)
    {
        return status;
    }

    for (index = 0U; index < total_samples; ++index)
    {
        status = HAL_ADC_Start(&hadc1);
        if (status != HAL_OK)
        {
            return status;
        }

        status = HAL_ADC_PollForConversion(&hadc1, MEASUREMENT_ADC_TIMEOUT_MS);
        if (status != HAL_OK)
        {
            (void)HAL_ADC_Stop(&hadc1);
            return status;
        }

        raw_value = HAL_ADC_GetValue(&hadc1);

        status = HAL_ADC_Stop(&hadc1);
        if (status != HAL_OK)
        {
            return status;
        }

        /*
         * Ensimmäiset näytteet voidaan haluttaessa hylätä.
         *
         * Tämä on hyödyllistä erityisesti silloin, kun:
         * - topologia on juuri vaihtunut
         * - ADC:n sampling capacitor asettuu uuteen lähde-impedanssiin
         * - halutaan vähentää transienttien vaikutusta
         */
        if (index >= (uint32_t)discard_samples)
        {
            sum += raw_value;
        }
    }

    /*
     * Pyöristetty keskiarvo:
     * lisätään keep_samples/2 ennen jakoa, jotta saadaan lähimpään
     * kokonaislukuun pyöristyvä tulos eikä pelkkä truncation.
     */
    *result = (uint16_t)((sum + ((uint32_t)keep_samples / 2U)) / (uint32_t)keep_samples);

    return HAL_OK;
}

/*
 * Muuntaa 12-bittisen raakakoodin jännitteeksi.
 *
 * Kaava:
 *
 *   V = raw * Vref / 4095
 *
 * Tässä ei oleteta mitään tiettyä referenssijännitettä, vaan se annetaan
 * parametrina. Näin sama helperi voidaan käyttää joustavasti mittauskerroksen
 * eri kohdissa.
 */
float measurement_adc_convert_raw_to_voltage(uint16_t raw_value, float reference_voltage)
{
    return ((float)raw_value * reference_voltage) / MEASUREMENT_ADC_MAX_RAW_12BIT;
}

/*
 * ============================================================================
 * Sisäinen kanavavalinta
 * ============================================================================
 */

/*
 * Valitsee ADC:n aktiivisen kanavan projektin mittaustarpeiden mukaan.
 *
 * Tämän projektin aktiiviset ADC-tulot:
 *
 *   VDIVS  -> PA7 -> ADC1_IN12
 *   KELVIN -> PB0 -> ADC1_IN15
 *
 * SamplingTime:
 * Käytössä on pitkä sampling time (640.5 cycles), koska se on tässä
 * bring-up-vaiheessa turvallinen valinta:
 *
 * - VDIVS-solmun lähde-impedanssi vaihtelee eri topologioissa
 * - Kelvin-haaran käyttäytyminen voi myös riippua analogisesta etuasteesta
 * - pidempi sample-aika usein stabiloi lukemaa paremmin kuin hyvin lyhyt
 *
 * Tätä asetusta voidaan myöhemmin optimoida, jos:
 * - mittausnopeutta halutaan nostaa
 * - tiedetään tarkemmin eri haarojen lähde-impedanssit
 * - bench-testeissä havaitaan, että jokin lyhyempi sample-aika toimii
 *   yhtä hyvin
 */
static HAL_StatusTypeDef measurement_adc_select_channel(measurement_adc_input_t input)
{
    ADC_ChannelConfTypeDef channel_configuration = {0};

    channel_configuration.Rank = ADC_REGULAR_RANK_1;
    channel_configuration.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
    channel_configuration.SingleDiff = ADC_SINGLE_ENDED;
    channel_configuration.OffsetNumber = ADC_OFFSET_NONE;
    channel_configuration.Offset = 0U;

    switch (input)
    {
        case MEASUREMENT_ADC_INPUT_VDIVS:
            channel_configuration.Channel = ADC_CHANNEL_12;  /* PA7 */
            break;

        case MEASUREMENT_ADC_INPUT_KELVIN:
            channel_configuration.Channel = ADC_CHANNEL_15;  /* PB0 */
            break;

        default:
            return HAL_ERROR;
    }

    return HAL_ADC_ConfigChannel(&hadc1, &channel_configuration);
}