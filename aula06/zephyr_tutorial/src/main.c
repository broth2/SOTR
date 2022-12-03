/*
 * Joao Castanheira, Nuno Fahla, 2022/11
 * Based on teachers examples
 * Zephyr: Distance Detector System
 * 
 * One of the tasks is periodc, the other two are activated via a semaphore. Data communicated via sharem memory 
 *
 * 
 */


#include <zephyr.h>
#include <device.h>
#include <drivers/gpio.h>
#include <sys/printk.h>
#include <sys/__assert.h>
#include <string.h>
#include <timing/timing.h>
#include <stdlib.h>
#include <stdio.h>

#include <drivers/adc.h>




/* Size of stack area used by each thread (can be thread specific, if necessary)*/
#define STACK_SIZE 1024

/* Thread scheduling priority */
#define sensor_thread_prio 1
#define movAvg_thread_prio 1
#define output_thread_prio 1
#define reset_thread_prio 1

/* Therad periodicity (in ms)*/
#define SAMP_PERIOD_MS  1000
#define STOP_TIME_MS    5000

#define BUFFY_SIZE 10
/* Create thread stack space */
K_THREAD_STACK_DEFINE(sensor_thread_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(movAvg_thread_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(output_thread_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(reset_thread_stack, STACK_SIZE);
  
/* Create variables for thread data */
struct k_thread sensor_thread_data;
struct k_thread movAvg_thread_data;
struct k_thread output_thread_data;
struct k_thread reset_thread_data;

/* Create task IDs */
k_tid_t sensor_thread_tid;
k_tid_t movAvg_thread_tid;
k_tid_t output_thread_tid;
k_tid_t reset_thread_tid;

/* Global vars (shared memory between tasks A/B and B/C, resp) */
int sampleData;
int filteredData = 0;
int samples[10];
int cnt = 0;
int arrNumbers[10] = {0};
int pos = 0;
int newAvg = 0;
int sum = 0;
int len = BUFFY_SIZE;
int reset_flag = 0;
    


/* Semaphores for task synch */
struct k_sem sem_samplemovAvg;
struct k_sem sem_movAvgoutput;
struct k_sem sem_resetButton;

/* Thread code prototypes */
void thread_A_code(void *argA, void *argB, void *argC);
void thread_B_code(void *argA, void *argB, void *argC);
void thread_C_code(void *argA, void *argB, void *argC);
void thread_Reset_code(void *argA, void *argB, void *argC);


#define LED0_NODE DT_NODELABEL(led0)
#define LED2_NODE DT_NODELABEL(led1)
#define LED3_NODE DT_NODELABEL(led2)
#define LED4_NODE DT_NODELABEL(led3)
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(LED2_NODE, gpios);
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(LED3_NODE, gpios);
static const struct gpio_dt_spec led4 = GPIO_DT_SPEC_GET(LED4_NODE, gpios);

// set up button 1
#define SW0_NODE DT_NODELABEL(button0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;


/*ADC definitions and includes*/
#include <hal/nrf_saadc.h>
#define ADC_RESOLUTION 10
#define ADC_GAIN ADC_GAIN_1_4
#define ADC_REFERENCE ADC_REF_VDD_1_4
#define ADC_ACQUISITION_TIME ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40)
#define ADC_CHANNEL_ID 1  

#define ADC_CHANNEL_INPUT NRF_SAADC_INPUT_AIN1 

#define BUFFER_SIZE 1
static uint16_t adc_sample_buffer[BUFFER_SIZE];
#define ADC_NODE DT_NODELABEL(adc)  
const struct device *adc_dev = NULL;

/* Other defines */
#define TIMER_INTERVAL_MSEC 1000 /* Interval between ADC samples */

#define LED_TOGGLE_RATE_MS  500

/* ADC channel configuration */
static const struct adc_channel_cfg my_channel_cfg = {
	.gain = ADC_GAIN,
	.reference = ADC_REFERENCE,
	.acquisition_time = ADC_ACQUISITION_TIME,
	.channel_id = ADC_CHANNEL_ID,
	.input_positive = ADC_CHANNEL_INPUT
};

/* Takes one sample */
static int adc_sample(void)
{
	int ret;
	const struct adc_sequence sequence = {
		.channels = BIT(ADC_CHANNEL_ID),
		.buffer = adc_sample_buffer,
		.buffer_size = sizeof(adc_sample_buffer),
		.resolution = ADC_RESOLUTION,
	};

	if (adc_dev == NULL) {
            printk("adc_sample(): error, must bind to adc first \n\r");
            return -1;
	}

	ret = adc_read(adc_dev, &sequence);
	if (ret) {
            printk("adc_read() failed with code %d\n", ret);
	}	

	return ret;
}

void test_button(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{

    k_sem_give(&sem_resetButton);

}

int movingAvg(int *ptrArrNumbers, int *ptrSum, int pos, int len, int nextNum)
{
  //Subtract the oldest number from the prev sum, add the new number
  *ptrSum = *ptrSum - ptrArrNumbers[pos] + nextNum;
  //Assign the nextNum to the position in the array
  ptrArrNumbers[pos] = nextNum;
  //return the average
  return *ptrSum / len;
}


/* Main function */
void main(void) {

    
    /* Welcome message */
    printf("\n\r Distance Detector System\n\r");
    
    /* Create and init semaphores */
    k_sem_init(&sem_samplemovAvg, 0, 1);
    k_sem_init(&sem_movAvgoutput, 0, 1);
    k_sem_init(&sem_resetButton, 0, 1);

    /* set leds pins as output*/
    gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led3, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led4, GPIO_OUTPUT_INACTIVE);

    /* set button 1 as input */
    gpio_pin_configure_dt(&button, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE );
    gpio_init_callback(&button_cb_data, test_button, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);
    
    /* Create tasks */
    sensor_thread_tid = k_thread_create(&sensor_thread_data, sensor_thread_stack,
        K_THREAD_STACK_SIZEOF(sensor_thread_stack), thread_A_code,
        NULL, NULL, NULL, sensor_thread_prio, 0, K_NO_WAIT);

    movAvg_thread_tid = k_thread_create(&movAvg_thread_data, movAvg_thread_stack,
        K_THREAD_STACK_SIZEOF(movAvg_thread_stack), thread_B_code,
        NULL, NULL, NULL, movAvg_thread_prio, 0, K_NO_WAIT); 
        

    output_thread_tid = k_thread_create(&output_thread_data, output_thread_stack,
        K_THREAD_STACK_SIZEOF(output_thread_stack), thread_C_code,
        NULL, NULL, NULL, output_thread_prio, 0, K_NO_WAIT); 

    reset_thread_tid = k_thread_create(&reset_thread_data, reset_thread_stack,
        K_THREAD_STACK_SIZEOF(reset_thread_stack), thread_Reset_code,
        NULL, NULL, NULL, reset_thread_prio, 0, K_NO_WAIT); 

    
    return;

} 

/* Thread code implementation */
void thread_A_code(void *argA , void *argB, void *argC)
{
    /* Timing variables to control task periodicity */
    int64_t fin_time=0, release_time=0;

    /* Other variables */
    long int nact = 0;
    int err=0;
    printk("A) Sensor Thread init (periodic)\n");

    adc_dev = device_get_binding(DT_LABEL(ADC_NODE));

    err = adc_channel_setup(adc_dev, &my_channel_cfg);

    /* Compute next release instant */
    release_time = k_uptime_get() + SAMP_PERIOD_MS;
    NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
    /* Thread loop */
    while(1) {
        printk("thread a instance\n\r");
        /* Do the workload */          
        printk("\n\nSensorThread instance %ld released at time: %lld (ms). \n",++nact, k_uptime_get());  
        
        //recolher sampleData do potenciometro
        //fazer calculos da adc
        err = adc_sample();

        if(err) {
            printk("adc_sample() failed with error code %d\n\r",err);
        }
        else {
            if(adc_sample_buffer[0] > 1023) {
                printk("adc reading out of range\n\r\n");
            }
            else {
            /* ADC is set to use gain of 1/4 and reference VDD/4, so input range is 0...VDD (3 V), with 10 bit resolution */
                //printk("adc reading: raw:%4u / %4u mV: \n\r",adc_sample_buffer[0],(uint16_t)(1000*adc_sample_buffer[0]*((float)3/1023)));
            
                sampleData = (uint16_t)(1000*adc_sample_buffer[0]*((float)3/1023))/100;
                printk("Sensor read a value of: %d \n",sampleData);
                

                if(cnt  < 10){
                    samples[cnt] = sampleData;
                }else if(cnt >= 10){
                    samples[cnt % 10] = sampleData;
                }
                if(!err){
                    printk("give ab\n");
                    k_sem_give(&sem_samplemovAvg);
                }
                
                cnt++;
            }
        }

        if (reset_flag){
            release_time += STOP_TIME_MS;
            reset_flag=0;
        }
        
        /* Wait for next release instant */ 
        fin_time = k_uptime_get();
        if( fin_time < release_time) {
            k_msleep(release_time - fin_time);
            release_time += SAMP_PERIOD_MS;
        }
    }

}

void thread_B_code(void *argA , void *argB, void *argC)
{
    /* Other variables */
    long int nact = 0;
    printk("B) Mov Average Thread init (sporadic)\n\r");
    
    while(1) {
        printk("take ab\n");
        k_sem_take(&sem_samplemovAvg,  K_FOREVER);
        printk("thread b instance\n\r");
        printk("MovAvgThread instance %ld released at time: %lld (ms). \n",++nact, k_uptime_get());
        //fazer os calculos da moving Average com o que vem no valor sampleData, que é global
        filteredData = movingAvg(arrNumbers, &sum, pos, len, samples[pos]);
        printf("Distance: %d\n", filteredData);
        pos++;
        if (pos >= len){
            pos = 0;
        }
        if(cnt > 10){
            printk("give bc\n");
            k_sem_give(&sem_movAvgoutput);
        }
        
    }

        

}



void thread_C_code(void *argA , void *argB, void *argC)
{
    // Other variables
    long int nact = 0;  

    printk("C) Output Thread init (sporadic)\n");
    while(1) {
        printk("take bc\n");
        k_sem_take(&sem_movAvgoutput, K_FOREVER);
        printk("thread c instance\n\r\n\r");  
        printk("Thread C instance %ld released at time: %lld (ms). \n", ++nact, k_uptime_get()); 
        if(filteredData >= 30){
            //LED 1 On
            gpio_pin_set_dt(&led1, 1);
            gpio_pin_set_dt(&led2, 0);
            gpio_pin_set_dt(&led3, 0);
            gpio_pin_set_dt(&led4, 0);
        }else if(20 <= filteredData && filteredData < 30){
            //LED 1,2 ON
            gpio_pin_set_dt(&led1, 1);
            gpio_pin_set_dt(&led2, 1);
            gpio_pin_set_dt(&led3, 0);
            gpio_pin_set_dt(&led4, 0);
        }else if(10 <= filteredData && filteredData < 20){
            //LED 1,2,3 ON
            gpio_pin_set_dt(&led1, 1);
            gpio_pin_set_dt(&led2, 1);
            gpio_pin_set_dt(&led3, 1);
            gpio_pin_set_dt(&led4, 0);
        }else if(filteredData < 10 && filteredData >= 0){
            //LED 1,2,3,4 ON
            gpio_pin_set_dt(&led1, 1);
            gpio_pin_set_dt(&led2, 1);
            gpio_pin_set_dt(&led3, 1);
            gpio_pin_set_dt(&led4, 1);
        }else{
            printk("something else that shouldnt occurr\n\r");}
        

        
  }
}

void thread_Reset_code(void *argA , void *argB, void *argC){
    printk("[RESET] Button Thread init");
    while(1){
        k_sem_take(&sem_resetButton, K_FOREVER);

        printk("\nreset thread instance\n\r");
        k_thread_suspend(sensor_thread_tid);
        reset_flag = 1;
        //k_thread_suspend(movAvg_thread_tid);
        //k_thread_suspend(output_thread_tid);

        gpio_pin_set_dt(&led1, 1);
        gpio_pin_set_dt(&led2, 1);
        gpio_pin_set_dt(&led3, 1);
        gpio_pin_set_dt(&led4, 1);


        for (int j=0; j<10; j++) {
            gpio_pin_toggle_dt(&led1);
            gpio_pin_toggle_dt(&led2);
            gpio_pin_toggle_dt(&led3);
            gpio_pin_toggle_dt(&led4);
         	k_msleep(LED_TOGGLE_RATE_MS);
        }
        
        //k_thread_resume(output_thread_tid);
        //k_thread_resume(movAvg_thread_tid);
        k_thread_resume(sensor_thread_tid);
    }
}