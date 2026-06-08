#include <stdio.h>
#include <stdlib.h>
#include <pigpio.h>
#include <cstring>

#define BUFFER_SIZE 64
#define NUM_INSTRUMENTS 5

struct InstrumentData
{
    uint8_t idWithInop;
    float value;
} __attribute__((packed));

int main() {
    	if (gpioInitialise() < 0) {
        	fprintf(stderr, "pigpio initialization failed\n");
        	return 1;
    	}

    	int spi_handle = spiOpen(0, 1000000, 0);
    	if (spi_handle < 0) {
        	fprintf(stderr, "Failed to open SPI channel\n");
        	gpioTerminate();
        	return 1;
    	}

	char tx_buf[BUFFER_SIZE];
	char rx_buf[BUFFER_SIZE];
	
	for(int i=0;i<BUFFER_SIZE;i++) 
		tx_buf[i]=0xFF;

	spiXfer(spi_handle, tx_buf, rx_buf, BUFFER_SIZE);

    	struct InstrumentData data;
	for (int i = 0; i < NUM_INSTRUMENTS; i++)
	{
    		data.idWithInop = rx_buf[i * 5];

    		memcpy(&data.value, &rx_buf[i * 5 + 1], sizeof(float));

    		printf("ID: 0x%02X, Value: %f\n", data.idWithInop, data.value);
	}

    spiClose(spi_handle);
    gpioTerminate();

    return 0;
}
