#include "measurement_topology.h"

#include "debug_logging.h"

#define MEASUREMENT_TOPOLOGY_LOG_TAG "TOPO"

/*
 * ============================================================================
 * Projektin tämänhetkinen pinout
 * ============================================================================
 *
 * Tämä tiedosto kuvaa korkean tason mittaustopologiat ja niiden
 * vastaavuuden fyysisiin GPIO-pinneihin.
 *
 * Tämänhetkinen STM32duino / proto-pinout:
 *
 *   PA3  = VDIVS low-range drive
 *   PA12 = VDIVS mid-range drive
 *   PB1  = VDIVS high-range drive
 *   PB4  = DUT1 control
 *   PB6  = relay / switch K1 indication
 *   PB7  = relay / switch K2 indication
 *
 * Lisäksi projektissa on muita pinnejä, joita tämä topologiakerros EI ohjaa:
 *
 *   PA4  = REF_1V_DAC1_OUT1
 *   PA7  = HIGH_R_ADC1_IN12
 *   PB0  = LOW_R_ADC1_IN15
 *   PA9  = TFT_BL_TIM1_CH2
 *
 * Tärkeä topologinen kokonaiskuva:
 *
 * - DUT1: firmware-ohjattu mittaterminaalin toinen puoli
 * - DUT2: reititetään K1/K2-logiikalla eri mittaushaaroihin
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
 * Käytännön seuraukset:
 *
 * - RESISTANCE_* käyttää:
 *     K1 = right
 *     K2 = left
 *     DUT2 -> VDIVS
 *     DUT1 = LOW
 *
 * - KELVIN käyttää:
 *     K1 = right
 *     K2 = right
 *     DUT2 -> KELVIN
 *     DUT1 = HIGH
 *
 * - CAPACITANCE käyttää:
 *     K1 = left
 *     K2 = don't care
 *     DUT2 -> CAPACITANCE
 *     DUT1 = LOW
 *
 * - DIODE_* käyttää:
 *     K1 = right
 *     K2 = left
 *     DUT2 -> VDIVS
 *     DUT1_CTRL valitsee forward/reverse-suunnan
 *
 * Huom:
 * PB6 / PB7 ovat tässä protovaiheessa releiden / kytkinasentojen
 * firmware-indikaatiot. Käsikytkimet simuloivat varsinaista relelogiikkaa.
 */

#define PIN_VDIVS_LOW               GPIO_PIN_3
#define PORT_VDIVS_LOW              GPIOA

#define PIN_VDIVS_MID               GPIO_PIN_12
#define PORT_VDIVS_MID              GPIOA

#define PIN_VDIVS_HIGH              GPIO_PIN_1
#define PORT_VDIVS_HIGH             GPIOB

#define PIN_DUT1_CONTROL            GPIO_PIN_4
#define PORT_DUT1_CONTROL           GPIOB

#define PIN_RELAY_1                 GPIO_PIN_6
#define PORT_RELAY_1                GPIOB

#define PIN_RELAY_2                 GPIO_PIN_7
#define PORT_RELAY_2                GPIOB

/*
 * ============================================================================
 * Diodimoodin tämänhetkiset bench-oletukset
 * ============================================================================
 *
 * Diodimittauksen tämänhetkisessä versiossa oletetaan:
 *
 * - DUT2 reititetään VDIVS-haaraan:
 *     K1 = right
 *     K2 = left
 *
 * - forward / reverse -suunta tehdään DUT1_CTRL-pinnillä:
 *     FORWARD -> DUT1 = LOW
 *     REVERSE -> DUT1 = HIGH
 *
 * - VDIVS-lähde voidaan valita LOW / MID / HIGH -alueista
 *
 * Jos bench-testit myöhemmin osoittavat, että:
 * - FORWARD/REVERSE ovat loogisesti toisin päin
 * - tai jokin range ei ole hyödyllinen diodiproben kannalta
 *
 * silloin korjaus onnistuu vaihtamalla vain näitä makroja tai
 * state map -taulukon rivejä.
 */

#define MEASUREMENT_TOPOLOGY_DIODE_FORWARD_DUT1_MODE \
    MEASUREMENT_DUT1_CONTROL_DRIVE_LOW

#define MEASUREMENT_TOPOLOGY_DIODE_REVERSE_DUT1_MODE \
    MEASUREMENT_DUT1_CONTROL_DRIVE_HIGH

/*
 * ============================================================================
 * Sisäinen topologiatilan kuvaus
 * ============================================================================
 *
 * measurement_topology_state_t kuvaa yhden korkean tason mittausmoodin
 * fyysisen GPIO- ja kytkinasetuksen.
 *
 * Kentät:
 *
 * relay1_enabled:
 *   false -> K1 left  -> CAPACITANCE
 *   true  -> K1 right -> K2 common
 *
 * relay2_enabled:
 *   false -> K2 right -> KELVIN
 *   true  -> K2 left  -> VDIVS
 *
 * low_range_enabled:
 *   PA3 syöttää VDIVS low-range vastushaaraa
 *
 * mid_range_enabled:
 *   PA12 syöttää VDIVS mid-range vastushaaraa
 *
 * high_range_enabled:
 *   PB1 syöttää VDIVS high-range vastushaaraa
 *
 * dut1_control_mode:
 *   PB4 tila valitussa topologiassa
 *
 * read_path:
 *   mitä lukureittiä ylempi koodi normaalisti käyttää
 */
typedef struct
{
    bool relay1_enabled;
    bool relay2_enabled;
    bool low_range_enabled;
    bool mid_range_enabled;
    bool high_range_enabled;
    measurement_dut1_control_mode_t dut1_control_mode;
    measurement_read_path_t read_path;
} measurement_topology_state_t;

/*
 * ============================================================================
 * Sisäinen tila (statuskyselyitä ja debugia varten)
 * ============================================================================
 */

static measurement_dut1_control_mode_t current_dut1_control_mode =
    MEASUREMENT_DUT1_CONTROL_HIGH_IMPEDANCE;

static measurement_topology_mode_t current_topology_mode =
    MEASUREMENT_TOPOLOGY_SAFE;

/*
 * ============================================================================
 * Sisäiset GPIO-helperit
 * ============================================================================
 *
 * Tässä topologiassa PA3 / PA12 / PB1 eivät ole tavallisia "on/off output"
 * -pinnejä, vaan ne syöttävät VDIVS-solmua vastusten kautta.
 *
 * Siksi "disabled" EI saa tarkoittaa GPIO LOW, vaan linjan vapauttamista.
 *
 * Käytännössä:
 *   enabled  -> output HIGH
 *   disabled -> analog / Hi-Z
 *
 * Sama ajatus toimii myös DUT1 control -pinnille:
 * sitä voidaan ajaa HIGH/LOW tai vapauttaa Hi-Z-tilaan.
 */

/*
 * Asettaa annetun pinnin analog-tilaan.
 *
 * Tässä projektissa analog-tila toimii käytännössä Hi-Z-tilana:
 * pinni ei enää aktiivisesti ohjaa linjaa ylös eikä alas.
 */
static void measurement_topology_set_pin_high_impedance(
    GPIO_TypeDef *port,
    uint16_t pin)
{
    GPIO_InitTypeDef gpio_configuration = {0};

    gpio_configuration.Pin = pin;
    gpio_configuration.Mode = GPIO_MODE_ANALOG;
    gpio_configuration.Pull = GPIO_NOPULL;

    HAL_GPIO_Init(port, &gpio_configuration);
}

/*
 * Asettaa annetun pinnin push-pull-lähdöksi ja ajaa sen korkeaksi.
 */
static void measurement_topology_set_pin_output_high(
    GPIO_TypeDef *port,
    uint16_t pin)
{
    GPIO_InitTypeDef gpio_configuration = {0};

    gpio_configuration.Pin = pin;
    gpio_configuration.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_configuration.Pull = GPIO_NOPULL;
    gpio_configuration.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(port, &gpio_configuration);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
}

/*
 * Asettaa annetun pinnin push-pull-lähdöksi ja ajaa sen matalaksi.
 */
static void measurement_topology_set_pin_output_low(
    GPIO_TypeDef *port,
    uint16_t pin)
{
    GPIO_InitTypeDef gpio_configuration = {0};

    gpio_configuration.Pin = pin;
    gpio_configuration.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_configuration.Pull = GPIO_NOPULL;
    gpio_configuration.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(port, &gpio_configuration);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
}

/*
 * Asettaa annetun pinnin push-pull-lähdöksi ja ajaa sille annetun tason.
 *
 * Tätä helperiä käytetään erityisesti K1/K2-indikaatiopinnejä varten.
 */
static void measurement_topology_set_pin_output_state(
    GPIO_TypeDef *port,
    uint16_t pin,
    GPIO_PinState pin_state)
{
    GPIO_InitTypeDef gpio_configuration = {0};

    gpio_configuration.Pin = pin;
    gpio_configuration.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_configuration.Pull = GPIO_NOPULL;
    gpio_configuration.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(port, &gpio_configuration);
    HAL_GPIO_WritePin(port, pin, pin_state);
}

/*
 * Kytkee käyttöön ne GPIO-porttien kellot, joita topologiakerros tarvitsee.
 */
static void measurement_topology_enable_gpio_clocks(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
}

/*
 * ============================================================================
 * Topologioiden state map
 * ============================================================================
 *
 * Tämä taulukko on koko topologiakerroksen “source of truth”.
 *
 * DUT1-ohjaus:
 *   SAFE                         -> Hi-Z
 *   WAKE_SETTLE                  -> Hi-Z
 *   RESISTANCE_HIGH_RANGE        -> LOW
 *   RESISTANCE_MID_RANGE         -> LOW
 *   RESISTANCE_LOW_RANGE         -> LOW
 *   KELVIN                       -> HIGH
 *   CAPACITANCE                  -> LOW
 *   DIODE_FORWARD_*              -> LOW
 *   DIODE_REVERSE_*              -> HIGH
 *
 * Kytkinreitti:
 *
 * RESISTANCE_*:
 *   K1 -> right
 *   K2 -> left
 *   DUT2 -> VDIVS
 *
 * KELVIN:
 *   K1 -> right
 *   K2 -> right
 *   DUT2 -> KELVIN
 *
 * CAPACITANCE:
 *   K1 -> left
 *   DUT2 -> CAPACITANCE
 *
 * DIODE_*:
 *   K1 -> right
 *   K2 -> left
 *   DUT2 -> VDIVS
 *   DUT1 ohjaa forward/reverse-suunnan
 */
static const measurement_topology_state_t
g_measurement_topology_states[MEASUREMENT_TOPOLOGY_COUNT] =
{
    [MEASUREMENT_TOPOLOGY_SAFE] =
    {
        .relay1_enabled = false,
        .relay2_enabled = false,
        .low_range_enabled = false,
        .mid_range_enabled = false,
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_DUT1_CONTROL_HIGH_IMPEDANCE,
        .read_path = MEASUREMENT_READ_PATH_NONE
    },

    [MEASUREMENT_TOPOLOGY_WAKE_SETTLE] =
    {
        .relay1_enabled = false,
        .relay2_enabled = false,
        .low_range_enabled = false,
        .mid_range_enabled = false,
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_DUT1_CONTROL_HIGH_IMPEDANCE,
        .read_path = MEASUREMENT_READ_PATH_NONE
    },

    [MEASUREMENT_TOPOLOGY_RESISTANCE_HIGH_RANGE] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = true,      /* K2 -> left  -> VDIVS */
        .low_range_enabled = false,
        .mid_range_enabled = false,
        .high_range_enabled = true,  /* PB1 HIGH -> high-range resistor -> VDIVS */
        .dut1_control_mode = MEASUREMENT_DUT1_CONTROL_DRIVE_LOW,
        .read_path = MEASUREMENT_READ_PATH_VDIVS_ADC
    },

    [MEASUREMENT_TOPOLOGY_RESISTANCE_MID_RANGE] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = true,      /* K2 -> left  -> VDIVS */
        .low_range_enabled = false,
        .mid_range_enabled = true,   /* PA12 HIGH -> mid-range resistor -> VDIVS */
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_DUT1_CONTROL_DRIVE_LOW,
        .read_path = MEASUREMENT_READ_PATH_VDIVS_ADC
    },

    [MEASUREMENT_TOPOLOGY_RESISTANCE_LOW_RANGE] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = true,      /* K2 -> left  -> VDIVS */
        .low_range_enabled = true,   /* PA3 HIGH -> low-range resistor -> VDIVS */
        .mid_range_enabled = false,
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_DUT1_CONTROL_DRIVE_LOW,
        .read_path = MEASUREMENT_READ_PATH_VDIVS_ADC
    },

    [MEASUREMENT_TOPOLOGY_KELVIN] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = false,     /* K2 -> right -> KELVIN */
        .low_range_enabled = false,
        .mid_range_enabled = false,
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_DUT1_CONTROL_DRIVE_HIGH,
        .read_path = MEASUREMENT_READ_PATH_KELVIN_ADC
    },

    [MEASUREMENT_TOPOLOGY_CAPACITANCE] =
    {
        .relay1_enabled = false,     /* K1 -> left -> CAPACITANCE */
        .relay2_enabled = false,     /* don't care, pidetään tunnetussa tilassa */
        .low_range_enabled = false,
        .mid_range_enabled = false,
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_DUT1_CONTROL_DRIVE_LOW,
        .read_path = MEASUREMENT_READ_PATH_TIM2_CH1
    },

    [MEASUREMENT_TOPOLOGY_DIODE_FORWARD_LOW_RANGE] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = true,      /* K2 -> left  -> VDIVS */
        .low_range_enabled = true,   /* low-range source resistor active */
        .mid_range_enabled = false,
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_TOPOLOGY_DIODE_FORWARD_DUT1_MODE,
        .read_path = MEASUREMENT_READ_PATH_VDIVS_ADC
    },

    [MEASUREMENT_TOPOLOGY_DIODE_FORWARD_MID_RANGE] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = true,      /* K2 -> left  -> VDIVS */
        .low_range_enabled = false,
        .mid_range_enabled = true,   /* mid-range source resistor active */
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_TOPOLOGY_DIODE_FORWARD_DUT1_MODE,
        .read_path = MEASUREMENT_READ_PATH_VDIVS_ADC
    },

    [MEASUREMENT_TOPOLOGY_DIODE_FORWARD_HIGH_RANGE] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = true,      /* K2 -> left  -> VDIVS */
        .low_range_enabled = false,
        .mid_range_enabled = false,
        .high_range_enabled = true,  /* high-range source resistor active */
        .dut1_control_mode = MEASUREMENT_TOPOLOGY_DIODE_FORWARD_DUT1_MODE,
        .read_path = MEASUREMENT_READ_PATH_VDIVS_ADC
    },

    [MEASUREMENT_TOPOLOGY_DIODE_REVERSE_LOW_RANGE] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = true,      /* K2 -> left  -> VDIVS */
        .low_range_enabled = true,
        .mid_range_enabled = false,
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_TOPOLOGY_DIODE_REVERSE_DUT1_MODE,
        .read_path = MEASUREMENT_READ_PATH_VDIVS_ADC
    },

    [MEASUREMENT_TOPOLOGY_DIODE_REVERSE_MID_RANGE] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = true,      /* K2 -> left  -> VDIVS */
        .low_range_enabled = false,
        .mid_range_enabled = true,
        .high_range_enabled = false,
        .dut1_control_mode = MEASUREMENT_TOPOLOGY_DIODE_REVERSE_DUT1_MODE,
        .read_path = MEASUREMENT_READ_PATH_VDIVS_ADC
    },

    [MEASUREMENT_TOPOLOGY_DIODE_REVERSE_HIGH_RANGE] =
    {
        .relay1_enabled = true,      /* K1 -> right -> K2 */
        .relay2_enabled = true,      /* K2 -> left  -> VDIVS */
        .low_range_enabled = false,
        .mid_range_enabled = false,
        .high_range_enabled = true,
        .dut1_control_mode = MEASUREMENT_TOPOLOGY_DIODE_REVERSE_DUT1_MODE,
        .read_path = MEASUREMENT_READ_PATH_VDIVS_ADC
    }
};

/*
 * ============================================================================
 * Julkinen low-level API
 * ============================================================================
 */

void measurement_topology_set_low_range_enabled(bool enabled)
{
    if (enabled)
    {
        measurement_topology_set_pin_output_high(PORT_VDIVS_LOW, PIN_VDIVS_LOW);
    }
    else
    {
        measurement_topology_set_pin_high_impedance(PORT_VDIVS_LOW, PIN_VDIVS_LOW);
    }
}

void measurement_topology_set_mid_range_enabled(bool enabled)
{
    if (enabled)
    {
        measurement_topology_set_pin_output_high(PORT_VDIVS_MID, PIN_VDIVS_MID);
    }
    else
    {
        measurement_topology_set_pin_high_impedance(PORT_VDIVS_MID, PIN_VDIVS_MID);
    }
}

void measurement_topology_set_high_range_enabled(bool enabled)
{
    if (enabled)
    {
        measurement_topology_set_pin_output_high(PORT_VDIVS_HIGH, PIN_VDIVS_HIGH);
    }
    else
    {
        measurement_topology_set_pin_high_impedance(PORT_VDIVS_HIGH, PIN_VDIVS_HIGH);
    }
}

void measurement_topology_set_relay_1_enabled(bool enabled)
{
    measurement_topology_set_pin_output_state(
        PORT_RELAY_1,
        PIN_RELAY_1,
        enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void measurement_topology_set_relay_2_enabled(bool enabled)
{
    measurement_topology_set_pin_output_state(
        PORT_RELAY_2,
        PIN_RELAY_2,
        enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void measurement_topology_set_dut1_control_mode(measurement_dut1_control_mode_t mode)
{
    switch (mode)
    {
        case MEASUREMENT_DUT1_CONTROL_HIGH_IMPEDANCE:
            measurement_topology_set_pin_high_impedance(
                PORT_DUT1_CONTROL,
                PIN_DUT1_CONTROL);
            current_dut1_control_mode = MEASUREMENT_DUT1_CONTROL_HIGH_IMPEDANCE;
            break;

        case MEASUREMENT_DUT1_CONTROL_DRIVE_LOW:
            measurement_topology_set_pin_output_low(
                PORT_DUT1_CONTROL,
                PIN_DUT1_CONTROL);
            current_dut1_control_mode = MEASUREMENT_DUT1_CONTROL_DRIVE_LOW;
            break;

        case MEASUREMENT_DUT1_CONTROL_DRIVE_HIGH:
            measurement_topology_set_pin_output_high(
                PORT_DUT1_CONTROL,
                PIN_DUT1_CONTROL);
            current_dut1_control_mode = MEASUREMENT_DUT1_CONTROL_DRIVE_HIGH;
            break;

        default:
            measurement_topology_set_pin_high_impedance(
                PORT_DUT1_CONTROL,
                PIN_DUT1_CONTROL);
            current_dut1_control_mode = MEASUREMENT_DUT1_CONTROL_HIGH_IMPEDANCE;
            break;
    }
}

measurement_dut1_control_mode_t measurement_topology_get_dut1_control_mode(void)
{
    return current_dut1_control_mode;
}

/*
 * ============================================================================
 * Julkinen korkean tason API
 * ============================================================================
 */

void measurement_topology_initialize_safe_state(void)
{
    measurement_topology_enable_gpio_clocks();
    (void)measurement_topology_apply(MEASUREMENT_TOPOLOGY_SAFE);
}

HAL_StatusTypeDef measurement_topology_apply(measurement_topology_mode_t mode)
{
    const measurement_topology_state_t *state;

    /*
     * Varmistetaan GPIO-kellot myös tässä, jotta apply() toimii turvallisesti
     * vaikka initialize_safe_state() olisi jäänyt jossain polussa kutsumatta.
     */
    measurement_topology_enable_gpio_clocks();

    if ((uint32_t)mode >= (uint32_t)MEASUREMENT_TOPOLOGY_COUNT)
    {
        LOG_ERROR(MEASUREMENT_TOPOLOGY_LOG_TAG, "Invalid mode=%u", (unsigned int)mode);
        return HAL_ERROR;
    }

    state = &g_measurement_topology_states[mode];

    /*
     * Passiivinen välitila:
     * - kaikki VDIVS-ajurit Hi-Z
     * - DUT1 vapautetaan
     * - rele/kytkinindikaatiot tunnettuun tilaan
     *
     * Tämä vähentää riskiä, että topologian vaihto aiheuttaisi hetkellisiä
     * ristiohjauksia tai lataisi väärän mittaushaaran.
     */
    measurement_topology_set_low_range_enabled(false);
    measurement_topology_set_mid_range_enabled(false);
    measurement_topology_set_high_range_enabled(false);
    measurement_topology_set_dut1_control_mode(MEASUREMENT_DUT1_CONTROL_HIGH_IMPEDANCE);
    measurement_topology_set_relay_1_enabled(false);
    measurement_topology_set_relay_2_enabled(false);

    /*
     * Kohdetilan asetus state mapin mukaan.
     *
     * Järjestys:
     *  1) reititys tunnettuun kohdeasentoon
     *  2) DUT1-ohjaus kohdetilaan
     *  3) aktiivinen VDIVS-range päälle
     *
     * Näin aktiiviset lähteet kytkeytyvät vasta sen jälkeen, kun reitti on
     * jo valittu.
     */
    measurement_topology_set_relay_1_enabled(state->relay1_enabled);
    measurement_topology_set_relay_2_enabled(state->relay2_enabled);
    measurement_topology_set_dut1_control_mode(state->dut1_control_mode);
    measurement_topology_set_low_range_enabled(state->low_range_enabled);
    measurement_topology_set_mid_range_enabled(state->mid_range_enabled);
    measurement_topology_set_high_range_enabled(state->high_range_enabled);

    current_topology_mode = mode;

    LOG_DEBUG(
        MEASUREMENT_TOPOLOGY_LOG_TAG,
        "APPLY mode=%s relay1=%u relay2=%u low=%u mid=%u high=%u dut1=%u read=%u",
        measurement_topology_get_mode_name(current_topology_mode),
        state->relay1_enabled ? 1U : 0U,
        state->relay2_enabled ? 1U : 0U,
        state->low_range_enabled ? 1U : 0U,
        state->mid_range_enabled ? 1U : 0U,
        state->high_range_enabled ? 1U : 0U,
        (unsigned int)state->dut1_control_mode,
        (unsigned int)state->read_path);

    return HAL_OK;
}

measurement_read_path_t measurement_topology_get_read_path(measurement_topology_mode_t mode)
{
    if ((uint32_t)mode >= (uint32_t)MEASUREMENT_TOPOLOGY_COUNT)
    {
        return MEASUREMENT_READ_PATH_NONE;
    }

    return g_measurement_topology_states[mode].read_path;
}

measurement_topology_mode_t measurement_topology_get_current_mode(void)
{
    return current_topology_mode;
}

const char *measurement_topology_get_mode_name(measurement_topology_mode_t mode)
{
    switch (mode)
    {
        case MEASUREMENT_TOPOLOGY_SAFE:
            return "SAFE";

        case MEASUREMENT_TOPOLOGY_WAKE_SETTLE:
            return "WAKE_SETTLE";

        case MEASUREMENT_TOPOLOGY_RESISTANCE_HIGH_RANGE:
            return "RESISTANCE_HIGH_RANGE";

        case MEASUREMENT_TOPOLOGY_RESISTANCE_MID_RANGE:
            return "RESISTANCE_MID_RANGE";

        case MEASUREMENT_TOPOLOGY_RESISTANCE_LOW_RANGE:
            return "RESISTANCE_LOW_RANGE";

        case MEASUREMENT_TOPOLOGY_KELVIN:
            return "KELVIN";

        case MEASUREMENT_TOPOLOGY_CAPACITANCE:
            return "CAPACITANCE";

        case MEASUREMENT_TOPOLOGY_DIODE_FORWARD_LOW_RANGE:
            return "DIODE_FORWARD_LOW_RANGE";

        case MEASUREMENT_TOPOLOGY_DIODE_FORWARD_MID_RANGE:
            return "DIODE_FORWARD_MID_RANGE";

        case MEASUREMENT_TOPOLOGY_DIODE_FORWARD_HIGH_RANGE:
            return "DIODE_FORWARD_HIGH_RANGE";

        case MEASUREMENT_TOPOLOGY_DIODE_REVERSE_LOW_RANGE:
            return "DIODE_REVERSE_LOW_RANGE";

        case MEASUREMENT_TOPOLOGY_DIODE_REVERSE_MID_RANGE:
            return "DIODE_REVERSE_MID_RANGE";

        case MEASUREMENT_TOPOLOGY_DIODE_REVERSE_HIGH_RANGE:
            return "DIODE_REVERSE_HIGH_RANGE";

        default:
            return "UNKNOWN";
    }
}