#ifndef BOARD_ADC_HAL_H
#define BOARD_ADC_HAL_H

#include "stm32l4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

extern ADC_HandleTypeDef hadc1;

HAL_StatusTypeDef board_adc_hal_initialize(void);
HAL_StatusTypeDef board_adc_hal_deinitialize(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_ADC_HAL_H */
