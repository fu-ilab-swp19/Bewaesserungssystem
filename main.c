#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "xtimer.h"
#include "msg.h"
#include "thread.h"
#include "fmt.h"
#include "periph/rtc.h"
#include "periph/adc.h"     //sensor
#include "periph/gpio.h"    //pump
#include "net/loramac.h"      //lora
#include "semtech_loramac.h"  //lora

#define PERIOD              (18000U)      //sending messages period in s
#define THRESHOLD	          1000                 //humidity threshold

#define SENDER_PRIO         (THREAD_PRIORITY_MAIN - 1)
static kernel_pid_t sender_pid;
static char sender_stack[THREAD_STACKSIZE_MAIN / 2];

semtech_loramac_t loramac;

static uint8_t deveui[LORAMAC_DEVEUI_LEN];
static uint8_t appeui[LORAMAC_APPEUI_LEN];
static uint8_t appkey[LORAMAC_APPKEY_LEN];
static int watered = 0;

static void rtc_cb(void *arg)
{
    (void) arg;
    msg_t msg;
    msg_send(&msg, sender_pid);
}

static void _prepare_next_alarm(void)
{
    struct tm time;
    rtc_get_time(&time);
    /* set initial alarm */
    time.tm_sec += PERIOD;
    mktime(&time);
    rtc_set_alarm(&time, rtc_cb, NULL);
}

static void watering(void) {
    puts("watering\n");
    watered = 1;

    gpio_t pin = GPIO_PIN(1,2);
    gpio_init(pin ,GPIO_OUT);
    gpio_set(pin);
    xtimer_sleep(2);
    gpio_clear(pin);
}

static void _send_message(void)
{
    int analog0 = adc_sample(ADC_LINE(0), ADC_RES_12BIT);
    char message[128];
    sprintf(message,"%d,%d",analog0,watered);       // example: message = 1023,0
    printf("humidity: %d,\nwatered: %d\n",analog0,watered);
    watered = 0;

    /* Try to send the message */
    uint8_t ret = semtech_loramac_send(&loramac,
                                       (uint8_t *)message, strlen(message));
    if (ret != SEMTECH_LORAMAC_TX_OK) {
        printf("Cannot send message '%s', ret code: %d\n", message, ret);
        return;
    }

    if(analog0 < THRESHOLD){
          watering();
    }
    /* The send was successfully scheduled, now wait until the send cycle has
       completed and a reply is received from the MAC */
    semtech_loramac_recv(&loramac);
}


static void *sender(void *arg)
{
    (void)arg;

    msg_t msg;
    msg_t msg_queue[8];
    msg_init_queue(msg_queue, 8);

    while (1) {
        msg_receive(&msg);

        /* Trigger the message send */
        _send_message();

        /* Schedule the next wake-up alarm */
        _prepare_next_alarm();
    }

    /* this should never be reached */
    return NULL;
}

static void init_lora(void) {

  // Convert identifiers and application key
  fmt_hex_bytes(deveui, DEVEUI);
  fmt_hex_bytes(appeui, APPEUI);
  fmt_hex_bytes(appkey, APPKEY);

  // Initialize the loramac stack
  semtech_loramac_init(&loramac);
  semtech_loramac_set_deveui(&loramac, deveui);
  semtech_loramac_set_appeui(&loramac, appeui);
  semtech_loramac_set_appkey(&loramac, appkey);

  // Use a fast datarate, e.g. BW125/SF7 in EU868
  semtech_loramac_set_dr(&loramac, LORAMAC_DR_5);
}


int main(void)
{
    puts("Start");
    puts("=====================================");

    init_lora();

    /* Start the Over-The-Air Activation (OTAA) procedure to retrieve the
     * generated device address and to get the network and application session
     * keys.*/
    puts("Starting join procedure");
    while (semtech_loramac_join(&loramac, LORAMAC_JOIN_OTAA) != SEMTECH_LORAMAC_JOIN_SUCCEEDED) {
        puts("Join procedure failed");

        //watering if not connected to te network
        int analog0 = adc_sample(ADC_LINE(0), ADC_RES_12BIT);
        printf("humidity: %d,\nwatered: %d\n",analog0,watered);
        if(analog0 < THRESHOLD){
              watering();
        }

        xtimer_sleep(2);
    }
    puts("Join procedure succeeded");

    // start the sender thread
    sender_pid = thread_create(sender_stack, sizeof(sender_stack),
                               SENDER_PRIO, 0, sender, NULL, "sender");

    // trigger the first send
    msg_t msg;
    msg_send(&msg, sender_pid);

    return 0;
}
