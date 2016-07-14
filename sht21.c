/*
 * Driver for the SHT2x temperature and humidity sensor.
 *
 * Licensed under the Apache License, Version 2.0, January 2004 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *      http://www.apache.org/licenses/
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS WITH THE SOFTWARE.
 *
 */

#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <esp/uart.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "i2c/i2c.h"

#include <espressif/esp_misc.h>
#include "espressif/esp8266/gpio_register.h"

#include "buffer.h"
#include "leds.h"



/* Sensor commands */
typedef enum
{
    TRIG_T_MEASUREMENT_HM    = 0xE3, /* Trigger temp measurement - hold master. */
    TRIG_RH_MEASUREMENT_HM   = 0xE5, /* Trigger humidity measurement - hold master. */
    TRIG_T_MEASUREMENT_POLL  = 0xF3, /* Trigger temp measurement - no hold master. */
    TRIG_RH_MEASUREMENT_POLL = 0xF5, /* Trigger humidity measurement - no hold master. */
    USER_REG_W               = 0xE6, /* Writing user register. */
    USER_REG_R               = 0xE7, /* Reading user register. */
    SOFT_RESET               = 0xFE  /* Soft reset. */
} sht2x_command;

typedef enum
{
    I2C_ADR_W                = 128,   /* Sensor I2C address + write bit. */
    I2C_ADR_R                = 129    /* Sensor I2C address + read bit. */
} sht2x_addr;


#define SCL_PIN GPIO_ID_PIN((0))      /* Nodemcu pin D3 */
#define SDA_PIN GPIO_ID_PIN((2))      /* Nodemcu pin D4 */

static uint8_t sht2x_check_crc(uint8_t data[], uint8_t num_bytes, uint8_t checksum)
{
  uint8_t crc = 0;
  uint8_t i;

  for (i = 0; i < num_bytes; ++i) {
      crc ^= (data[i]);
      for (uint8_t bit = 8; bit > 0; --bit) {
          if (crc & 0x80)
              crc = (crc << 1) ^ 0x131;
          else
              crc = (crc << 1);
      }
  }
  return (crc != checksum) ? 1 : 0;
}

/*
 * Measure the temperature if temp_rh is 0 and the relative humidity
 * if temp_rh is 1. Return 0 on success and 1 if there is an error.
 */
static uint8_t sht2x_measure_poll(int temp_rh, uint8_t data[])
{
    i2c_start();
    int res = i2c_write(I2C_ADR_W);

    if (!i2c_write(temp_rh ? TRIG_RH_MEASUREMENT_POLL : TRIG_T_MEASUREMENT_POLL)) {
        i2c_stop();
        return 1;
    }

    int i = 0;
    do {
        i2c_start();
        sdk_os_delay_us(10000);
        res = i2c_write(I2C_ADR_R);
        if (i++ >= 20) {
            i2c_stop();
            return 1;
        }
    } while (res == 0);

    data[0] = i2c_read(0);
    data[1] = i2c_read(0);
    uint8_t checksum = i2c_read(1);
    res = sht2x_check_crc(data, 2, checksum);
    i2c_stop();
    return res;
}

static void sht2x_read_task(void *pvParameters)
{
    /* Delta encoding state. */
    uint32_t last_index = 0;
    uint16_t last_temp = 0;
    uint16_t last_rh = 0;

    for (;;) {
        vTaskDelay(10000 / portTICK_RATE_MS);

        uint8_t data[4];

        if (sht2x_measure_poll(0, data)) {
            blink_red();
            continue;
        }

        uint16_t temp = ((uint16_t) data[0]) << 8 | data[1];
        temp >>= 2; /* Strip the two low status bits */

        if (sht2x_measure_poll(1, &data[2])) {
            blink_red();
            continue;
        }

        uint16_t rh = ((uint16_t) data[2]) << 8 | data[3];
        rh >>= 2; /* Stip the two low status bits */

        while (1) {
            uint8_t outbuf[4];
            outbuf[0] = temp;
            outbuf[1] = temp >> 8;
            outbuf[2] = rh;
            outbuf[3] = rh >> 8;
            int len = sizeof(outbuf);
            int32_t code = DBUF_EVENT_SHT2X_TEMP_HUM;
            uint32_t new_index = dbuf_append(last_index, code, outbuf, len, 1, 0);
            if (new_index == last_index)
                break;

            /* Moved on to a new buffer. Reset the delta encoding state and
             * retry. */
            last_index = new_index;
            last_temp = 0;
            last_rh = 0;
        };

        blink_green();
        
        /* Commit the values logged. Note this is the only task accessing this
         * state so these updates are synchronized with the last event of this
         * class append. */
        last_temp = temp;
        last_rh = rh;
    }
}

void init_sht2x()
{
    i2c_init(SCL_PIN, SDA_PIN);

    xTaskCreate(&sht2x_read_task, (signed char *)"sht2x_read_task", 256, NULL, 2, NULL);
}
