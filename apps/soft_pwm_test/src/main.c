/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include "sysinit/sysinit.h"
#include <os/os.h>
#include <pwm/pwm.h>
#include <bsp/bsp.h>
#include <soft_pwm/soft_pwm.h>

static struct os_dev dev;
struct pwm_dev *pwm;
uint16_t max_val;

int
main(int argc, char **argv)
{
    sysinit();
    struct pwm_chan_cfg chan_conf = {
        .pin = LED_1,
        .inverted = true,
        .data = NULL
    };
    uint32_t base_freq;

    os_dev_create(&dev,
                  "spwm",
                  OS_DEV_INIT_KERNEL,
                  OS_DEV_INIT_PRIO_DEFAULT,
                  soft_pwm_dev_init,
                  NULL);
    pwm = (struct pwm_dev *) os_dev_open("spwm", 0, NULL);

    base_freq = pwm_set_frequency(pwm, 600);
    max_val = (uint16_t) (base_freq / 600);
    pwm_chan_config(pwm, 0, &chan_conf);
    pwm_enable_duty_cycle(pwm, 0, max_val/2);

    /* chan_conf.pin = LED_2; */
    /* pwm_chan_config(pwm, 1, &chan_conf); */
    /* pwm_enable_duty_cycle(pwm, 1, value/10); */

    //changing frequency while playing


    /* chan_conf.pin = LED_3; */
    /* pwm_chan_config(pwm, 2, &chan_conf); */
    /* pwm_enable_duty_cycle(pwm, 2, value/100); */

    /* chan_conf.pin = LED_4; */
    /* pwm_chan_config(pwm, 3, &chan_conf); */
    /* pwm_enable_duty_cycle(pwm, 3, value/1000); */

    while (1) {
        os_eventq_run(os_eventq_dflt_get());
    }
    assert(0);
    return(0);
}
