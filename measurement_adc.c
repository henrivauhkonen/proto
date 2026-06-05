
#include "measurement_adc.h"
#include "board_adc_hal.h"


#define MEASUREMENT_ADC_TIMEOUT_MS      10U
#define MEASUREMENT_ADC_MAX_RAW_12BIT   4095.0f

static HAL_StatusTypeDef measurement_adc_select_channel(measurement_adc_input_t input);

HAL_StatusTypeDef measurement_adc_initialize(void)
{
    return HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
}

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

        if (index >= (uint32_t)discard_samples)
        {
            sum += raw_value;
        }
    }

    *result = (uint16_t)((sum + ((uint32_t)keep_samples / 2U)) / (uint32_t)keep_samples);

    return HAL_OK;
}

float measurement_adc_convert_raw_to_voltage(uint16_t raw_value, float reference_voltage)
{
    return ((float)raw_value * reference_voltage) / MEASUREMENT_ADC_MAX_RAW_12BIT;
}

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
