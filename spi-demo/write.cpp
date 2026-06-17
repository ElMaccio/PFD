#include <stdio.h>
#include <stdlib.h>
#include <pigpio.h>
#include <cstring>

int main(){

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
   
    printf("Started transmision\n");

    float c = 0.1f;
    char buf[6] = {2, 0};
    memcpy(&buf[2], &c, sizeof(c));
    
//    for(int i = 0; i < 10000; i++)
    while(true) {
        spiWrite(spi_handle, buf, sizeof(buf));
    }

    printf("Finished transmision\n");

    return 0;
}
