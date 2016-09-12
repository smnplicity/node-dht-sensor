#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <bcm2835.h>
#include <sched.h>
#include <unistd.h>

#define BCM2708_PERI_BASE   0x20000000
#define GPIO_BASE           (BCM2708_PERI_BASE + 0x200000)
#define MAXTIMINGS          100
#define DHT11               11
#define DHT22               22
#define AM2302              22

int initialized = 0;
unsigned long long last_read[32] = {};
float last_temperature[32] = {};
float last_humidity[32] = {};
int data[MAXTIMINGS];

void set_default_priority(void) {
	struct sched_param sched;
	memset(&sched, 0, sizeof(sched));
	// Go back to default scheduler with default 0 priority.
	sched.sched_priority = 0;
	sched_setscheduler(0, SCHED_OTHER, &sched);
	munlockall();
}

inline void set_max_priority(void) {
	struct sched_param sched;
	memset(&sched, 0, sizeof(sched));
	// Use FIFO scheduler with highest priority for the lowest chance
	// of the kernel context switching.
	sched.sched_priority = sched_get_priority_max(SCHED_FIFO);
	sched_setscheduler(0, SCHED_FIFO, &sched);
	mlockall(MCL_CURRENT | MCL_FUTURE);
}

unsigned long long getTime()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  unsigned long long time = (unsigned long long)(tv.tv_sec)*1000 +
                            (unsigned long long)(tv.tv_usec)/1000;
  return time;
}

long readDHT(int type, int pin, float &temperature, float &humidity)
{
	int j = 0;
	int timeout;
	int bits[MAXTIMINGS];
  data[0] = data[1] = data[2] = data[3] = data[4] = 0;

	#ifdef VERBOSE
	FILE *pTrace = fopen("dht-sensor.log", "a");
	if (pTrace == NULL) {
		puts("WARNING: unable to initialize trace file.");
	}
	#endif

  unsigned long long now = getTime();
  if (now - last_read[pin] < 2000) {
		#ifdef VERBOSE
		fprintf(pTrace, "too early to read again pin %d: %llu\n", pin, now - last_read[pin]);
		#endif
		temperature = last_temperature[pin];
		humidity = last_humidity[pin];
		return 0;
  } else {
     last_read[pin] = now + 420;
  }

	// set up as real-time scheduling as possible
	set_max_priority();
	usleep(1000);

	bcm2835_gpio_fsel(pin, BCM2835_GPIO_FSEL_OUTP);
	bcm2835_gpio_write(pin, HIGH);
	usleep(10000);
	bcm2835_gpio_write(pin, LOW);
	usleep(type == 11 ? 2500 : 600);
	bcm2835_gpio_write(pin, HIGH);

  bcm2835_gpio_fsel(pin, BCM2835_GPIO_FSEL_INPT);

	for (timeout = 0; timeout < 1000000 && bcm2835_gpio_lev(pin) == LOW; ++timeout);
	if (timeout >= 100000) { set_default_priority(); return -3; }
	for (timeout = 0; timeout < 1000000 && bcm2835_gpio_lev(pin) == HIGH; ++timeout);
	if (timeout >= 100000) { set_default_priority(); return -3; }

	// read data!
	for (j = 0; j < MAXTIMINGS; ++j) {
		for (timeout = 0; bcm2835_gpio_lev(pin) == LOW && timeout < 50000; ++timeout);
		for (timeout = 0; bcm2835_gpio_lev(pin) == HIGH && timeout < 50000; ++timeout);
		bits[j] = timeout;
		if (timeout >= 50000) break;
	}

	set_default_priority();

	int peak = bits[1];
	#ifdef VERBOSE
	fprintf(pTrace, "init peak: %d\n", bits[1]);
	#endif
	for (int i = 2; i < j; ++i) {
		if (peak < bits[i]) {
			peak = bits[i];
			#ifdef VERBOSE
				fprintf(pTrace, "update peak: %d (%d)\n", i, bits[i]);
			#endif
		}
	}

	#ifdef VERBOSE
	fprintf(pTrace, "j=%d, peak=%d:\n", j, peak);
	#endif
	int k = 0;
	for (int i = 1; i < j; ++i) {
		data[k] <<= 1;
		if ((2*bits[i] - peak) > 0) {
			data[k] |= 1;
			#ifdef VERBOSE
			fprintf(pTrace, "1 (%03d) ", bits[i]);
		} else {
			fprintf(pTrace, "0 (%03d) ", bits[i]);
			#endif
		}
		if (i % 8 == 0) {
			k++;
			#ifdef VERBOSE
			fprintf(pTrace, "\n");
			#endif
		}
	}

	#ifdef VERBOSE
	int crc = ((data[0] + data[1] + data[2] + data[3]) & 0xff);
	fprintf(pTrace, "\n=> %x %x %x %x (%x/%x) : %s\n",
		data[0], data[1], data[2], data[3], data[4],
		crc, (data[4] == crc) ? "OK" : "ERR");
	#endif

  if ((j == 41) && (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xff)))
  {
		#ifdef VERBOSE
    fprintf(pTrace, "[Sensor type = %d] ", type);
		#endif

    if (type == DHT11) {
			#ifdef VERBOSE
      printf("Temp = %d C, Hum = %d %%\n", data[2], data[0]);
			#endif
      temperature = data[2];
      humidity = data[0];
    }
    else if (type == DHT22)
    {
      float f, h;
      h = data[0] * 256 + data[1];
      h /= 10;

      f = (data[2] & 0x7F) * 256 + data[3];
      f /= 10.0;
      if (data[2] & 0x80) f *= -1;

			#ifdef VERBOSE
      fprintf(pTrace, "Temp = %.1f C, Hum = %.1f %%\n", f, h);
			#endif
	    temperature = f;
	    humidity = h;
	  }
		else
		{
			#ifdef VERBOSE
			fclose(pTrace);
			#endif
	  	return -2;
	  }
	}
	else
	{
		#ifdef VERBOSE
    fprintf(pTrace, "Unexpected data: bits=%d: %d != %d + %d + %d + %d\n",
     j, data[4], data[0], data[1], data[2], data[3]);
		fclose(pTrace);
		#endif
    return -1;
  }

	#ifdef VERBOSE
  fprintf(pTrace, "Obtained readout successfully.\n");
	fclose(pTrace);
	#endif

	// update last readout
	last_temperature[pin] = temperature;
	last_humidity[pin] = humidity;

	return 0;
}

int initialize()
{
  if (!bcm2835_init())
  {
		#ifdef VERBOSE
    printf("BCM2835 initialization failed.\n");
		#endif
    return 1;
  }
  else
  {
		#ifdef VERBOSE
    printf("BCM2835 initialized.\n");
		#endif
    initialized = 1;
    memset(last_read, 0, sizeof(unsigned long long)*32);
		memset(last_temperature, 0, sizeof(float)*32);
		memset(last_humidity, 0, sizeof(float)*32);
    return 0;
  }
}